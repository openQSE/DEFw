/*
 * Can a process serve an API and call that same API?
 *
 * It could not, and the failure was silent. margo_register_name is
 * margo_provider_register_name with provider 0 and a NULL handler, so a
 * caller looking up an RPC its own process already served on provider 0
 * replaced that handler with nothing. The provider stopped answering its own
 * API, Mercury said "Overwriting RPC callback for a previously registered RPC
 * ID", and the only other symptom was every call failing.
 *
 * The directory is the case that matters, because it serves on provider 0 by
 * definition: its address is the one nobody can resolve, so it has to be at a
 * provider known in advance. A service on any other provider never collided,
 * which is why this survived phase 0.
 *
 * This test is the configuration that broke: one runtime serving
 * qfw.directory and using a directory client against itself. If
 * defw2_rpc_lookup stops asking Margo before registering, the registrations
 * here clobber the handlers and every check below fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2.h>
#include <defw2/defw2_dir.h>
#include <defw2/defw2_echo.h>

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

int main(void)
{
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;
	defw2_service_t *dirsvc = NULL, *echo_svc = NULL;
	defw2_dir_store_opts_t store_opts;
	defw2_dir_t *dir = NULL;
	defw2_dir_record_t record;
	defw2_dir_query_t query;
	defw2_dir_result_t result;
	defw2_status_t status;
	defw2_call_opts_t opts;
	defw2_binding_t *echo_binding = NULL;
	defw2_buffer_t reply;
	uint64_t generation = 0;

	memset(&cfg, 0, sizeof(cfg));
	memset(&status, 0, sizeof(status));
	memset(&opts, 0, sizeof(opts));
	opts.timeout_ms = 5000;

	cfg.address = "na+sm://";
	cfg.node_name = "self-call";
	cfg.role = DEFW2_ROLE_SERVER;
	cfg.log_level = DEFW2_LOG_ERROR;
	cfg.rpc_thread_count = 4;
	if (defw2_init(&cfg, &rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the runtime\n");
		return EXIT_FAILURE;
	}

	memset(&store_opts, 0, sizeof(store_opts));
	store_opts.heartbeat_timeout_ms = 60000;
	store_opts.scan_interval_ms = 10000;

	/* The directory, on provider 0, where the collision lived. */
	check("the directory binds on provider 0",
	      defw2_service_create(rt, "dirsvc-self", DEFW2_API_DIR,
				   DEFW2_PROVIDER_DIR, &dirsvc) == DEFW2_OK &&
	      defw2_dir_bind(dirsvc, &store_opts) == DEFW2_OK);
	/* And an echo service, so the same runtime serves two APIs. */
	check("an echo service binds on provider 1",
	      defw2_service_create(rt, "echo-self", DEFW2_API_ECHO,
				   DEFW2_PROVIDER_ECHO,
				   &echo_svc) == DEFW2_OK &&
	      defw2_echo_bind(echo_svc, NULL) == DEFW2_OK);

	/*
	 * Now call the directory from the process that serves it. Opening the
	 * handle is what registers the six directory RPCs as a caller, which
	 * is the moment the handlers used to be lost.
	 */
	check("the serving process opens a client on its own directory",
	      defw2_dir_open(rt, defw2_service_address(dirsvc), &dir) ==
	      DEFW2_OK);

	memset(&record, 0, sizeof(record));
	record.service_id = "registered-by-self";
	record.service_type = "qfw.qpm";
	record.runtime_id = "runtime-self";
	record.address = defw2_service_address(echo_svc);
	check("register_service still answers",
	      defw2_dir_register(dir, &record, &opts, &generation, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK && generation == 1);

	memset(&query, 0, sizeof(query));
	query.service_type = "qfw.qpm";
	memset(&result, 0, sizeof(result));
	check("resolve_services still answers",
	      defw2_dir_resolve(dir, &query, &opts, &result, &status) ==
	      DEFW2_OK && result.entry_count == 1);
	defw2_dir_result_free(&result);

	check("heartbeat still answers",
	      defw2_dir_heartbeat(dir, "registered-by-self", "runtime-self", 1,
				  &opts, &status) == DEFW2_OK &&
	      status.code == DEFW2_OK);

	generation = 0;
	check("get_generation still answers",
	      defw2_dir_generation(dir, "registered-by-self", &opts,
				   &generation, &status) == DEFW2_OK &&
	      status.code == DEFW2_OK && generation == 1);

	memset(&result, 0, sizeof(result));
	check("query_directory still answers",
	      defw2_dir_query(dir, &query, &opts, &result, &status) ==
	      DEFW2_OK && result.entry_count == 1);
	defw2_dir_result_free(&result);

	check("deregister_service still answers",
	      defw2_dir_deregister(dir, "registered-by-self", "runtime-self",
				   1, &opts, &status) == DEFW2_OK &&
	      status.code == DEFW2_OK);

	/*
	 * The other API on the same runtime has to be unharmed too: a caller
	 * registering qfw.echo here must not disturb the echo service bound
	 * on provider 1.
	 */
	memset(&reply, 0, sizeof(reply));
	check("and the process can call its own echo service",
	      defw2_binding_create(rt, defw2_service_address(echo_svc),
				   DEFW2_PROVIDER_ECHO,
				   &echo_binding) == DEFW2_OK &&
	      defw2_echo(echo_binding, "self", 4, &opts, &reply,
			 &status) == DEFW2_OK &&
	      reply.len == 4 && memcmp(reply.data, "self", 4) == 0);
	defw2_buffer_free(&reply);
	defw2_binding_free(echo_binding);

	defw2_status_free(&status);
	defw2_dir_close(dir);
	defw2_service_shutdown(echo_svc);
	defw2_service_destroy(echo_svc);
	defw2_service_shutdown(dirsvc);
	defw2_service_destroy(dirsvc);
	defw2_finalize(rt);

	printf("\n%s\n", failures == 0 ? "self-call smoke passed"
				       : "self-call smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
