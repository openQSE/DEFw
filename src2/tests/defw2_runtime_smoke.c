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
	failures += status_helpers();

	printf("%s\n", failures ? "SMOKE FAILED" : "SMOKE PASSED");
	return failures ? 1 : 0;
}
