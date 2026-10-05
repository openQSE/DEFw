/*
 * The environment contract from the v2 design.
 *
 * v2 reads the DEFW_ names v1 already sets where they still mean something,
 * and adds a small DEFW2_ set of its own. An empty environment is a valid
 * client configuration.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>	/* strcasecmp */

#include "defw2_internal.h"

/*
 * Composed strings live here rather than in the caller's struct, so that a
 * config stays a set of pointers. defw2_config_from_env is a startup call
 * made once per process, which is what makes that safe.
 */
static char composed_address[DEFW2_NAME_MAX];
static char composed_dirsvc[DEFW2_NAME_MAX];

static const char *env_or(const char *name, const char *fallback)
{
	const char *value = getenv(name);

	return (value != NULL && value[0] != '\0') ? value : fallback;
}

static bool env_flag(const char *name, bool fallback)
{
	const char *value = env_or(name, NULL);

	if (value == NULL)
		return fallback;
	return strcmp(value, "1") == 0 || strcasecmp(value, "yes") == 0 ||
	       strcasecmp(value, "true") == 0 || strcasecmp(value, "on") == 0;
}

static defw2_rc_t env_int(const char *name, int fallback, int *out)
{
	const char *value = env_or(name, NULL);
	char *end = NULL;
	long parsed;

	if (value == NULL) {
		*out = fallback;
		return DEFW2_OK;
	}
	parsed = strtol(value, &end, 10);
	if (end == value || *end != '\0' || parsed < 0 || parsed > INT16_MAX)
		return DEFW2_ERR_CONFIG;
	*out = (int)parsed;
	return DEFW2_OK;
}

static defw2_role_t role_from_env(void)
{
	const char *type = env_or("DEFW_AGENT_TYPE", "agent");

	/* v1's types: agent, client, service, dirsvc. The last two serve
	 * RPCs, so they need handler threads. */
	if (strcasecmp(type, "service") == 0 || strcasecmp(type, "dirsvc") == 0)
		return DEFW2_ROLE_SERVER;
	return DEFW2_ROLE_CLIENT;
}

static defw2_log_level_t log_level_from_env(void)
{
	const char *level = env_or("DEFW_LOG_LEVEL", "error");

	if (strcasecmp(level, "all") == 0)
		return DEFW2_LOG_ALL;
	if (strcasecmp(level, "debug") == 0)
		return DEFW2_LOG_DEBUG;
	if (strcasecmp(level, "message") == 0 || strcasecmp(level, "msg") == 0)
		return DEFW2_LOG_MESSAGE;
	if (strcasecmp(level, "warning") == 0)
		return DEFW2_LOG_WARNING;
	return DEFW2_LOG_ERROR;
}

/*
 * DEFW_LISTEN_PORT names the port v1 would have listened on. It only means
 * something for an IP provider, so it is appended for ofi+tcp and ignored
 * otherwise, as the design's environment table says.
 */
static const char *address_from_env(void)
{
	const char *address = env_or("DEFW2_ADDRESS", DEFW2_DEFAULT_ADDRESS);
	const char *port = env_or("DEFW_LISTEN_PORT", NULL);

	if (port == NULL || strcmp(port, "0") == 0)
		return address;
	if (strncmp(address, "ofi+tcp://", strlen("ofi+tcp://")) != 0)
		return address;
	if (strlen(address) > strlen("ofi+tcp://"))
		return address;	/* already carries a host or a port */

	snprintf(composed_address, sizeof(composed_address),
		 "ofi+tcp://:%s", port);
	return composed_address;
}

/*
 * Where the directory service is. DEFW2_DIRSVC wins. Otherwise v1's parent
 * tuple is composed into an address, which is what a v1 deployment already
 * passes to every process it starts.
 */
static const char *dirsvc_from_env(void)
{
	const char *dirsvc = env_or("DEFW2_DIRSVC", NULL);
	const char *host;
	const char *port;

	if (dirsvc != NULL)
		return dirsvc;
	if (env_flag("DEFW_DISABLE_DIRSVC", false))
		return NULL;

	host = env_or("DEFW_PARENT_ADDR", env_or("DEFW_PARENT_HOSTNAME", NULL));
	port = env_or("DEFW_PARENT_PORT", NULL);
	if (host == NULL || port == NULL || strcmp(port, "0") == 0 ||
	    strcasecmp(host, "none") == 0)
		return NULL;

	snprintf(composed_dirsvc, sizeof(composed_dirsvc),
		 "ofi+tcp://%s:%s", host, port);
	return composed_dirsvc;
}

defw2_rc_t defw2_config_from_env(defw2_config_t *cfg)
{
	defw2_rc_t rc;

	if (cfg == NULL)
		return DEFW2_ERR_INVALID;

	memset(cfg, 0, sizeof(*cfg));
	cfg->address = address_from_env();
	cfg->dirsvc = dirsvc_from_env();
	cfg->margo_config = env_or("DEFW2_MARGO_CONFIG", NULL);
	cfg->node_name = env_or("DEFW_AGENT_NAME", NULL);
	cfg->log_dir = env_or("DEFW_LOG_DIR", NULL);
	cfg->role = role_from_env();
	cfg->log_level = log_level_from_env();
	cfg->profile = env_flag("DEFW2_PROFILE", false);

	rc = env_int("DEFW2_RPC_THREADS",
		     cfg->role == DEFW2_ROLE_SERVER ?
			     DEFW2_DEFAULT_RPC_THREADS : 0,
		     &cfg->rpc_thread_count);
	if (rc != DEFW2_OK) {
		defw2_log(NULL, DEFW2_LOG_ERROR,
			  "DEFW2_RPC_THREADS is not a thread count: %s",
			  getenv("DEFW2_RPC_THREADS"));
		return rc;
	}

	/* Unset leaves it to the role, which defw2_init decides. */
	rc = env_int("DEFW2_PROGRESS_SPINDOWN_MS", -1,
		     &cfg->progress_spindown_ms);
	if (rc != DEFW2_OK) {
		defw2_log(NULL, DEFW2_LOG_ERROR,
			  "DEFW2_PROGRESS_SPINDOWN_MS is not a count of "
			  "milliseconds: %s",
			  getenv("DEFW2_PROGRESS_SPINDOWN_MS"));
		return rc;
	}
	cfg->has_progress_spindown = cfg->progress_spindown_ms >= 0;
	if (!cfg->has_progress_spindown)
		cfg->progress_spindown_ms = 0;
	return DEFW2_OK;
}
