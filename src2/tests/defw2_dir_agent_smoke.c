/*
 * Does a service stay registered without its author doing anything?
 *
 * The store and RPC tests both show a record timing out. This one shows the
 * opposite, which is the case only the agent can demonstrate: a service that
 * does nothing but exist stays UP well past the heartbeat timeout, because
 * the agent's timer is beating for it.
 *
 * Then it shows the other half of phase 1's exit criterion at the level a
 * service actually experiences it. The agent stops, which deregisters, and a
 * second agent for the same service_id comes back as a new generation without
 * any service code having asked for one.
 *
 * The directory gets a runtime of its own, and the registering service
 * another, both over na+sm in this one process. They could share one now that
 * defw2_rpc_lookup reuses a registration the process already has, which
 * defw2_self_call_smoke covers; a runtime each is kept because it is what
 * production does, the directory being its own process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <defw2/defw2.h>
#include <defw2/defw2_dir.h>
#include <defw2/defw2_echo.h>

/* Short enough that the test finishes, long enough to be survivable. */
#define DIR_TIMEOUT_MS	300u
#define DIR_SCAN_MS	40u
#define BEAT_MS		60u

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L,
	};

	nanosleep(&ts, NULL);
}

/* How many UP records the directory has for this service_id. */
static size_t up_count(defw2_dir_t *dir, const char *service_id,
		       defw2_dir_state_t *state, uint64_t *generation)
{
	defw2_dir_query_t query;
	defw2_dir_result_t result;
	defw2_status_t status;
	defw2_call_opts_t opts;
	size_t count;

	memset(&query, 0, sizeof(query));
	memset(&result, 0, sizeof(result));
	memset(&status, 0, sizeof(status));
	memset(&opts, 0, sizeof(opts));
	opts.timeout_ms = 5000;
	query.service_id = service_id;
	query.include_inactive = state != NULL;

	if (state != NULL)
		defw2_dir_query(dir, &query, &opts, &result, &status);
	else
		defw2_dir_resolve(dir, &query, &opts, &result, &status);
	count = result.entry_count;
	if (count > 0) {
		if (state != NULL)
			*state = result.entries[0].record.state;
		if (generation != NULL)
			*generation = result.entries[0].record.generation;
	}
	defw2_dir_result_free(&result);
	defw2_status_free(&status);
	return count;
}

int main(void)
{
	defw2_config_t dir_cfg, svc_cfg, client_cfg;
	defw2_rt_t *dir_rt = NULL, *svc_rt = NULL, *client_rt = NULL;
	defw2_service_t *dirsvc = NULL, *svc = NULL;
	defw2_dir_store_opts_t store_opts;
	defw2_dir_agent_t *agent = NULL;
	defw2_dir_t *dir = NULL;
	defw2_dir_record_t record;
	defw2_dir_state_t state = DEFW2_DIR_STATE_UP;
	uint64_t generation = 0;
	const defw2_dir_binding_t bindings[] = {
		{ "echo", DEFW2_API_ECHO, 1, 1 },
	};
	const defw2_dir_property_t properties[] = {
		{ "vendor", "test" },
	};

	memset(&dir_cfg, 0, sizeof(dir_cfg));
	memset(&svc_cfg, 0, sizeof(svc_cfg));
	memset(&client_cfg, 0, sizeof(client_cfg));

	dir_cfg.address = "na+sm://";
	dir_cfg.node_name = "dirsvc-host";
	dir_cfg.role = DEFW2_ROLE_SERVER;
	dir_cfg.log_level = DEFW2_LOG_ERROR;
	dir_cfg.rpc_thread_count = 2;
	if (defw2_init(&dir_cfg, &dir_rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the directory runtime\n");
		return EXIT_FAILURE;
	}

	svc_cfg.address = "na+sm://";
	svc_cfg.node_name = "agent-host";
	svc_cfg.role = DEFW2_ROLE_SERVER;
	svc_cfg.log_level = DEFW2_LOG_ERROR;
	svc_cfg.rpc_thread_count = 4;
	if (defw2_init(&svc_cfg, &svc_rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the service runtime\n");
		return EXIT_FAILURE;
	}

	memset(&store_opts, 0, sizeof(store_opts));
	store_opts.heartbeat_timeout_ms = DIR_TIMEOUT_MS;
	store_opts.scan_interval_ms = DIR_SCAN_MS;
	store_opts.retention_ms = 60000;

	check("the directory binds on provider 0",
	      defw2_service_create(dir_rt, "dirsvc-1", DEFW2_API_DIR,
				   DEFW2_PROVIDER_DIR, &dirsvc) == DEFW2_OK &&
	      defw2_dir_bind(dirsvc, &store_opts) == DEFW2_OK);

	/* The service that will register itself. */
	check("the echo service binds on provider 1",
	      defw2_service_create(svc_rt, "qpm-agent", "qfw.qpm",
				   DEFW2_PROVIDER_ECHO, &svc) == DEFW2_OK &&
	      defw2_echo_bind(svc, NULL) == DEFW2_OK);

	client_cfg.address = "na+sm://";
	client_cfg.node_name = "agent-observer";
	client_cfg.role = DEFW2_ROLE_CLIENT;
	client_cfg.log_level = DEFW2_LOG_ERROR;
	if (defw2_init(&client_cfg, &client_rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the client runtime\n");
		return EXIT_FAILURE;
	}
	check("an observer opens the directory",
	      defw2_dir_open(client_rt, defw2_service_address(dirsvc), &dir) ==
	      DEFW2_OK);

	/*
	 * What a service author writes: the parts only they know. No address,
	 * no runtime_id, no endpoint, no timer.
	 */
	memset(&record, 0, sizeof(record));
	record.service_id = "qpm-agent";
	record.service_type = "qfw.qpm";
	record.selector.name = "test-qpu";
	record.bindings = bindings;
	record.binding_count = 1;
	record.properties = properties;
	record.property_count = 1;

	check("the agent starts",
	      defw2_dir_agent_start(svc, defw2_service_address(dirsvc),
				    &record, BEAT_MS, &agent) == DEFW2_OK);
	check("and registered as generation 1",
	      defw2_dir_agent_generation(agent) == 1);
	check("the agent took the runtime's own identity",
	      defw2_dir_agent_runtime_id(agent) != NULL &&
	      strcmp(defw2_dir_agent_runtime_id(agent),
		     defw2_runtime_id(svc_rt)) == 0);
	check("the service resolves straight away",
	      up_count(dir, "qpm-agent", NULL, NULL) == 1);

	/*
	 * The address and endpoint the agent filled in, which a caller never
	 * supplied. Without these a resolve would hand out nowhere.
	 */
	{
		defw2_dir_query_t query;
		defw2_dir_result_t result;
		defw2_status_t status;
		defw2_call_opts_t opts;

		memset(&query, 0, sizeof(query));
		memset(&result, 0, sizeof(result));
		memset(&status, 0, sizeof(status));
		memset(&opts, 0, sizeof(opts));
		opts.timeout_ms = 5000;
		query.service_id = "qpm-agent";
		defw2_dir_resolve(dir, &query, &opts, &result, &status);
		check("the agent filled in the service's own address",
		      result.entry_count == 1 &&
		      result.entries[0].record.address != NULL &&
		      strcmp(result.entries[0].record.address,
			     defw2_service_address(svc)) == 0);
		check("and the endpoint, from the runtime",
		      result.entry_count == 1 &&
		      result.entries[0].record.endpoint.pid ==
		      defw2_pid(svc_rt));
		check("what the author did supply survived",
		      result.entry_count == 1 &&
		      result.entries[0].record.binding_count == 1 &&
		      strcmp(result.entries[0].record.selector.name,
			     "test-qpu") == 0);
		defw2_dir_result_free(&result);
		defw2_status_free(&status);
	}

	/*
	 * The point of the agent. Wait several times the directory's timeout
	 * and several scan intervals. A service with no agent would be
	 * TIMED_OUT long before this returns.
	 */
	sleep_ms(DIR_TIMEOUT_MS * 3);
	check("heartbeats keep it UP well past the timeout",
	      up_count(dir, "qpm-agent", NULL, NULL) == 1);
	check("and it is still generation 1, so it never re-registered",
	      defw2_dir_agent_generation(agent) == 1);

	/* Stopping the agent deregisters, which is the clean shutdown path. */
	defw2_dir_agent_stop(agent);
	agent = NULL;
	check("stopping the agent deregisters",
	      up_count(dir, "qpm-agent", NULL, NULL) == 0);
	state = DEFW2_DIR_STATE_UP;
	check("and leaves a DEREGISTERED record for the operator",
	      up_count(dir, "qpm-agent", &state, NULL) == 1 &&
	      state == DEFW2_DIR_STATE_DEREGISTERED);

	/* The restart: a second agent, no service code involved. */
	check("a second agent starts",
	      defw2_dir_agent_start(svc, defw2_service_address(dirsvc),
				    &record, BEAT_MS, &agent) == DEFW2_OK);
	generation = 0;
	check("the restart is generation 2, automatically",
	      defw2_dir_agent_generation(agent) == 2 &&
	      up_count(dir, "qpm-agent", NULL, &generation) == 1 &&
	      generation == 2);

	defw2_dir_agent_stop(agent);

	defw2_dir_close(dir);
	defw2_finalize(client_rt);
	defw2_service_shutdown(svc);
	defw2_service_destroy(svc);
	defw2_finalize(svc_rt);
	defw2_service_shutdown(dirsvc);
	defw2_service_destroy(dirsvc);
	defw2_finalize(dir_rt);

	printf("\n%s\n", failures == 0 ? "directory agent smoke passed"
				       : "directory agent smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
