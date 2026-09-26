/*
 * Joining and leaving a DEFw v2 deployment.
 *
 * defw2_init starts one Margo instance, generates the runtime identity and
 * installs the logging sink. The Margo instance is created through
 * margo_init_ext with a JSON configuration, so pools, execution streams and
 * the profiling switches are deployment settings rather than code. The
 * progress loop always gets its own execution stream. That is a design rule
 * and not a tuning choice, because it is what keeps a foreign runtime, such
 * as an embedded Python interpreter, away from the network.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>	/* strcasecmp */
#include <unistd.h>

#include <uuid/uuid.h>

#include "defw2_internal.h"
#include "defw2_trace.h"

#define DEFW2_JSON_MAX	4096

static char *read_file(const struct defw2_rt *rt, const char *path)
{
	long size;
	char *text;
	FILE *stream = fopen(path, "r");

	if (stream == NULL) {
		defw2_log(rt, DEFW2_LOG_ERROR, "cannot read %s", path);
		return NULL;
	}
	if (fseek(stream, 0, SEEK_END) != 0 || (size = ftell(stream)) < 0) {
		fclose(stream);
		defw2_log(rt, DEFW2_LOG_ERROR, "cannot size %s", path);
		return NULL;
	}
	rewind(stream);
	text = malloc((size_t)size + 1);
	if (text != NULL) {
		size_t got = fread(text, 1, (size_t)size, stream);

		text[got] = '\0';
	}
	fclose(stream);
	return text;
}

/*
 * Margo's own monitor is opt-in.
 *
 * It is the only source of Margo's per-RPC counts and call paths, so the
 * design wants it, but the default monitor in Margo 0.24.2 reads freed
 * memory in __margo_default_monitor_on_respond_cb and takes a service down
 * under concurrent load. Our own spans cover the same ground, so the
 * monitor waits behind DEFW2_MARGO_MONITOR until that is fixed upstream.
 * Filed as mochi-hpc/mochi-margo issue 322:
 * https://github.com/mochi-hpc/mochi-margo/issues/322
 */
static bool want_margo_monitor(const defw2_config_t *cfg)
{
	const char *value = getenv("DEFW2_MARGO_MONITOR");

	if (!cfg->profile || value == NULL)
		return false;
	return strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0 &&
	       value[0] != '\0';
}

/*
 * A server always gets at least one handler execution stream.
 *
 * Margo runs handlers in the primary pool when it is asked for no handler
 * threads, which means a server only serves while its main thread is donated
 * to Margo. v2 cannot promise that thread: a Python service holds it, and so
 * does anything embedding the runtime. So this is a rule rather than a
 * setting, the same way the progress loop always gets its own stream.
 */
static int handler_threads(const struct defw2_rt *rt,
			   const defw2_config_t *cfg)
{
	if (cfg->role != DEFW2_ROLE_SERVER || cfg->rpc_thread_count >= 1)
		return cfg->rpc_thread_count;

	defw2_log(rt, DEFW2_LOG_DEBUG,
		  "a server needs handler streams, raising %d to %d",
		  cfg->rpc_thread_count, DEFW2_DEFAULT_RPC_THREADS);
	return DEFW2_DEFAULT_RPC_THREADS;
}

/*
 * The built-in configuration. A deployment that needs more than this, such
 * as pinned execution streams, supplies its own through DEFW2_MARGO_CONFIG.
 */
static char *margo_json(const struct defw2_rt *rt, const defw2_config_t *cfg,
			const char *dir)
{
	char *json;

	if (cfg->margo_config != NULL)
		return read_file(rt, cfg->margo_config);

	json = malloc(DEFW2_JSON_MAX);
	if (json == NULL)
		return NULL;
	/*
	 * Margo's own view of every RPC comes from its monitor, which writes
	 * its statistics beside our spans. Margo 0.24 replaced the
	 * breadcrumb profiler the design's telemetry table names, so
	 * enable_profiling alone produces nothing.
	 */
	if (want_margo_monitor(cfg) && dir != NULL)
		snprintf(json, DEFW2_JSON_MAX,
			 "{\"use_progress_thread\":true,"
			 "\"rpc_thread_count\":%d,"
			 "\"monitoring\":{\"config\":"
			 "{\"filename_prefix\":\"%.300s/margo-%.150s\","
			 "\"enable_statistics\":true,"
			 "\"pretty_json\":true}}}",
			 handler_threads(rt, cfg), dir, rt->node_name);
	else
		snprintf(json, DEFW2_JSON_MAX,
			 "{\"use_progress_thread\":true,"
			 "\"rpc_thread_count\":%d}",
			 handler_threads(rt, cfg));
	return json;
}

static defw2_rc_t record_address(struct defw2_rt *rt)
{
	hg_addr_t self = HG_ADDR_NULL;
	hg_size_t size = 0;
	hg_return_t hret;

	hret = margo_addr_self(rt->mid, &self);
	if (hret != HG_SUCCESS) {
		defw2_log(rt, DEFW2_LOG_ERROR, "margo_addr_self: %s",
			  HG_Error_to_string(hret));
		return DEFW2_ERR_TRANSPORT;
	}
	hret = margo_addr_to_string(rt->mid, NULL, &size, self);
	if (hret == HG_SUCCESS) {
		rt->address = malloc(size);
		if (rt->address == NULL)
			hret = HG_NOMEM;
		else
			hret = margo_addr_to_string(rt->mid, rt->address,
						    &size, self);
	}
	margo_addr_free(rt->mid, self);
	if (hret != HG_SUCCESS) {
		defw2_log(rt, DEFW2_LOG_ERROR, "margo_addr_to_string: %s",
			  HG_Error_to_string(hret));
		return DEFW2_ERR_TRANSPORT;
	}
	return DEFW2_OK;
}

static void record_identity(struct defw2_rt *rt, const defw2_config_t *cfg)
{
	uuid_t raw;

	uuid_generate(raw);
	uuid_unparse_lower(raw, rt->runtime_id);

	rt->pid = (int)getpid();
	if (gethostname(rt->hostname, sizeof(rt->hostname)) != 0)
		snprintf(rt->hostname, sizeof(rt->hostname), "unknown");
	rt->hostname[sizeof(rt->hostname) - 1] = '\0';

	if (cfg->node_name != NULL)
		snprintf(rt->node_name, sizeof(rt->node_name), "%s",
			 cfg->node_name);
	else
		snprintf(rt->node_name, sizeof(rt->node_name), "%.200s-%d",
			 rt->hostname, rt->pid);
}

defw2_rc_t defw2_init(const defw2_config_t *cfg, defw2_rt_t **out)
{
	struct defw2_rt *rt;
	struct margo_init_info args = MARGO_INIT_INFO_INITIALIZER;
	char *json;
	defw2_rc_t rc;

	if (cfg == NULL || out == NULL || cfg->address == NULL)
		return DEFW2_ERR_INVALID;

	rt = calloc(1, sizeof(*rt));
	if (rt == NULL)
		return DEFW2_ERR_NOMEM;
	pthread_mutex_init(&rt->log_lock, NULL);
	pthread_mutex_init(&rt->rpc_lock, NULL);
	rt->role = cfg->role;
	rt->log_level = cfg->log_level;
	rt->profile = cfg->profile;
	defw2_log_open(rt, cfg->log_dir);
	record_identity(rt, cfg);

	json = margo_json(rt, cfg, defw2_telemetry_dir(cfg));
	if (json == NULL) {
		rc = cfg->margo_config ? DEFW2_ERR_CONFIG : DEFW2_ERR_NOMEM;
		goto fail;
	}
	args.json_config = json;
	if (want_margo_monitor(cfg))
		args.monitor = margo_default_monitor;

	/* Server mode listens, which is what a process serving RPCs or
	 * receiving events needs. A pure client does not. */
	rt->mid = margo_init_ext(cfg->address,
				 cfg->role == DEFW2_ROLE_SERVER ?
					 MARGO_SERVER_MODE : MARGO_CLIENT_MODE,
				 &args);
	free(json);
	if (rt->mid == MARGO_INSTANCE_NULL) {
		defw2_log(rt, DEFW2_LOG_ERROR,
			  "margo_init_ext failed for address %s",
			  cfg->address);
		rc = DEFW2_ERR_TRANSPORT;
		goto fail;
	}
	defw2_log_attach_margo(rt);

	rc = record_address(rt);
	if (rc != DEFW2_OK)
		goto fail_margo;

	if (cfg->dirsvc != NULL) {
		rt->dirsvc = strdup(cfg->dirsvc);
		if (rt->dirsvc == NULL) {
			rc = DEFW2_ERR_NOMEM;
			goto fail_margo;
		}
	}

	rc = defw2_telemetry_open(rt, cfg);
	if (rc != DEFW2_OK)
		goto fail_margo;

	defw2_log(rt, DEFW2_LOG_MESSAGE,
		  "defw2 %s up as %s at %s, runtime %s, role %s",
		  defw2_version(), rt->node_name, rt->address, rt->runtime_id,
		  rt->role == DEFW2_ROLE_SERVER ? "server" : "client");
	*out = rt;
	return DEFW2_OK;

fail_margo:
	defw2_telemetry_close(rt);
	margo_finalize(rt->mid);
fail:
	free(rt->address);
	defw2_log_close(rt);
	pthread_mutex_destroy(&rt->rpc_lock);
	pthread_mutex_destroy(&rt->log_lock);
	free(rt);
	return rc;
}

/*
 * margo_finalize waits for handlers that are already running and must not
 * run twice, so the first caller wins and the rest return.
 */
void defw2_runtime_stop(struct defw2_rt *rt)
{
	int idle = 0;

	if (rt == NULL || rt->mid == MARGO_INSTANCE_NULL)
		return;
	if (!__atomic_compare_exchange_n(&rt->stopping, &idle, 1, false,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
	margo_finalize(rt->mid);
}

void defw2_finalize(defw2_rt_t *rt)
{
	if (rt == NULL)
		return;

	defw2_log(rt, DEFW2_LOG_MESSAGE, "defw2 down, runtime %s",
		  rt->runtime_id);
	/* Written before the network goes away, so a run that then hangs in
	 * margo_finalize still leaves its measurements behind. */
	defw2_telemetry_close(rt);
	defw2_runtime_stop(rt);
	free(rt->address);
	free(rt->dirsvc);
	defw2_log_close(rt);
	pthread_mutex_destroy(&rt->rpc_lock);
	pthread_mutex_destroy(&rt->log_lock);
	free(rt);
}

const char *defw2_runtime_id(const defw2_rt_t *rt)
{
	return rt ? rt->runtime_id : NULL;
}

const char *defw2_address(const defw2_rt_t *rt)
{
	return rt ? rt->address : NULL;
}

const char *defw2_node_name(const defw2_rt_t *rt)
{
	return rt ? rt->node_name : NULL;
}

const char *defw2_hostname(const defw2_rt_t *rt)
{
	return rt ? rt->hostname : NULL;
}

int defw2_pid(const defw2_rt_t *rt)
{
	return rt ? rt->pid : 0;
}

defw2_role_t defw2_role(const defw2_rt_t *rt)
{
	return rt ? rt->role : DEFW2_ROLE_CLIENT;
}

void defw2_status_free(defw2_status_t *status)
{
	if (status == NULL)
		return;
	free(status->message);
	status->message = NULL;
	status->code = 0;
	status->category = DEFW2_CAT_OK;
}

const char *defw2_category_name(uint32_t category)
{
	switch (category) {
	case DEFW2_CAT_OK:
		return "ok";
	case DEFW2_CAT_TRANSPORT:
		return "transport";
	case DEFW2_CAT_TIMEOUT:
		return "timeout";
	case DEFW2_CAT_CANCELLED:
		return "cancelled";
	case DEFW2_CAT_NOT_FOUND:
		return "not-found";
	case DEFW2_CAT_VERSION_MISMATCH:
		return "version-mismatch";
	case DEFW2_CAT_INVALID_ARGUMENT:
		return "invalid-argument";
	case DEFW2_CAT_INVALID_RESERVATION:
		return "invalid-reservation";
	case DEFW2_CAT_INSUFFICIENT_ALLOWANCE:
		return "insufficient-allowance";
	case DEFW2_CAT_PENDING_CAPACITY:
		return "pending-capacity";
	case DEFW2_CAT_POLICY_DELAYED:
		return "policy-delayed";
	case DEFW2_CAT_EXPIRED_RESERVATION:
		return "expired-reservation";
	case DEFW2_CAT_SCHEDULER_FAILURE:
		return "scheduler-failure";
	case DEFW2_CAT_PROVIDER_FAILURE:
		return "provider-failure";
	default:
		return "unknown";
	}
}

const char *defw2_strerror(defw2_rc_t rc)
{
	switch (rc) {
	case DEFW2_OK:
		return "ok";
	case DEFW2_ERR_INVALID:
		return "invalid argument";
	case DEFW2_ERR_NOMEM:
		return "out of memory";
	case DEFW2_ERR_CONFIG:
		return "bad configuration";
	case DEFW2_ERR_TRANSPORT:
		return "transport failure";
	case DEFW2_ERR_TIMEOUT:
		return "timed out";
	case DEFW2_ERR_CANCELLED:
		return "cancelled";
	case DEFW2_ERR_NOT_FOUND:
		return "not found";
	case DEFW2_ERR_VERSION:
		return "version mismatch";
	case DEFW2_ERR_INTERNAL:
		return "internal error";
	default:
		return "unknown error";
	}
}

const char *defw2_version(void)
{
	return DEFW2_VERSION_STRING;
}
