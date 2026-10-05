/*
 * Does the runtime come up and go down cleanly?
 *
 * The test joins as a client and as a server, checks that each has an
 * identity and a usable address, and finalizes. It uses na+sm, so it needs
 * no network and no port and says nothing about the machine it runs on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2.h>
#include <margo.h>

#include "defw2_internal.h"	/* the Margo instance, to read its config */

static int check(const char *what, bool ok)
{
	printf("%-28s %s\n", what, ok ? "ok" : "FAILED");
	return ok ? 0 : 1;
}

/*
 * Mercury fills in the rest of the address, so the check is that the
 * runtime came up on the provider that was asked for rather than on some
 * exact string. That keeps the test true whatever DEFW2_ADDRESS says.
 */
static bool address_matches(const char *requested, const char *actual)
{
	const char *separator = strstr(requested, "://");
	size_t prefix = separator ? (size_t)(separator - requested) + 3
				  : strlen(requested);

	return actual != NULL && strncmp(actual, requested, prefix) == 0;
}

static int round_trip(defw2_role_t role, const char *label)
{
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;
	int failures = 0;
	defw2_rc_t rc;

	rc = defw2_config_from_env(&cfg);
	failures += check("config from env", rc == DEFW2_OK);
	cfg.role = role;

	rc = defw2_init(&cfg, &rt);
	if (rc != DEFW2_OK) {
		printf("%-28s FAILED (%s)\n", label, defw2_strerror(rc));
		return 1;
	}

	failures += check("runtime id", defw2_runtime_id(rt) != NULL &&
				strlen(defw2_runtime_id(rt)) == 36);
	failures += check("address",
			  address_matches(cfg.address, defw2_address(rt)));
	failures += check("node name", defw2_node_name(rt) != NULL &&
				defw2_node_name(rt)[0] != '\0');
	failures += check("hostname", defw2_hostname(rt) != NULL);
	failures += check("pid", defw2_pid(rt) > 0);
	failures += check("role", defw2_role(rt) == role);
	printf("  %s at %s\n", label, defw2_address(rt));

	defw2_finalize(rt);
	return failures;
}

/*
 * How long a runtime's progress loop spins, as the configuration Margo
 * runs with says, or -1 when it does not say.
 */
static long spindown_of(defw2_rt_t *rt)
{
	char *json = margo_get_config(rt->mid);
	const char *at = NULL;
	long ms = -1;

	if (json != NULL)
		at = strstr(json, "\"progress_spindown_msec\"");
	if (at != NULL && (at = strchr(at, ':')) != NULL)
		ms = strtol(at + 1, NULL, 10);
	free(json);
	return ms;
}

/* A runtime of role from the environment, and its spindown. */
static long spindown_for(defw2_role_t role, int set_ms)
{
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;
	long ms = -2;

	if (defw2_config_from_env(&cfg) != DEFW2_OK)
		return -3;
	cfg.role = role;
	if (set_ms >= 0) {
		cfg.has_progress_spindown = true;
		cfg.progress_spindown_ms = set_ms;
	}
	if (defw2_init(&cfg, &rt) == DEFW2_OK) {
		ms = spindown_of(rt);
		defw2_finalize(rt);
	}
	return ms;
}

/*
 * A server does not spin unless told to, since a process that listens has
 * handlers often enough to spin all the time. A client keeps Margo's own
 * default, 10 ms in Margo 0.24. DEFW2_PROGRESS_SPINDOWN_MS sets both, and
 * the config's own field sets one runtime.
 */
static int spindown(void)
{
	defw2_config_t cfg;
	int failures = 0;

	unsetenv("DEFW2_PROGRESS_SPINDOWN_MS");
	failures += check("a server does not spin",
			  spindown_for(DEFW2_ROLE_SERVER, -1) == 0);
	failures += check("a client keeps Margo's spindown",
			  spindown_for(DEFW2_ROLE_CLIENT, -1) == 10);
	failures += check("a runtime takes the one it is given",
			  spindown_for(DEFW2_ROLE_SERVER, 7) == 7 &&
			  spindown_for(DEFW2_ROLE_CLIENT, 0) == 0);
	setenv("DEFW2_PROGRESS_SPINDOWN_MS", "3", 1);
	failures += check("DEFW2_PROGRESS_SPINDOWN_MS sets both",
			  spindown_for(DEFW2_ROLE_SERVER, -1) == 3 &&
			  spindown_for(DEFW2_ROLE_CLIENT, -1) == 3);
	setenv("DEFW2_PROGRESS_SPINDOWN_MS", "-1", 1);
	failures += check("and a spindown that is not one is refused",
			  defw2_config_from_env(&cfg) == DEFW2_ERR_CONFIG);
	unsetenv("DEFW2_PROGRESS_SPINDOWN_MS");
	return failures;
}

static int status_helpers(void)
{
	defw2_status_t status = {
		.code = DEFW2_ERR_TIMEOUT,
		.category = DEFW2_CAT_TIMEOUT,
		.message = strdup("took too long"),
	};
	int failures = 0;

	failures += check("category name",
			  strcmp(defw2_category_name(DEFW2_CAT_PENDING_CAPACITY),
				 "pending-capacity") == 0);
	failures += check("unknown category",
			  strcmp(defw2_category_name(4242), "unknown") == 0);
	failures += check("strerror",
			  strcmp(defw2_strerror(DEFW2_ERR_NOT_FOUND),
				 "not found") == 0);
	defw2_status_free(&status);
	failures += check("status free",
			  status.message == NULL &&
			  status.category == DEFW2_CAT_OK);
	return failures;
}

int main(void)
{
	int failures = 0;

	printf("defw2 version %s\n", defw2_version());
	failures += round_trip(DEFW2_ROLE_CLIENT, "client");
	failures += round_trip(DEFW2_ROLE_SERVER, "server");
	failures += spindown();
	failures += status_helpers();

	printf("%s\n", failures ? "SMOKE FAILED" : "SMOKE PASSED");
	return failures ? 1 : 0;
}
