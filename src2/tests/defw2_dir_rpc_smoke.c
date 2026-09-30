/*
 * Does the directory work over a real RPC?
 *
 * The store test covers the lifecycle rules in process. This one puts Margo
 * between the caller and those rules, so what it checks is the part the store
 * test cannot: that a record survives the wire intact, that the six methods
 * are registered and answer, and that resolve and query differ in exactly
 * the way the registration decides rather than the request.
 *
 * It runs a directory and a client in one process over na+sm, so it needs no
 * network, no port and no second process. Two runtimes, because a server and
 * a client are different Margo configurations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <defw2/defw2.h>
#include <defw2/defw2_dir.h>

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-54s %s\n", what, ok ? "ok" : "FAILED");
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

int main(void)
{
	defw2_config_t server_cfg, client_cfg;
	defw2_rt_t *server_rt = NULL, *client_rt = NULL;
	defw2_service_t *svc = NULL;
	defw2_dir_t *dir = NULL;
	defw2_dir_store_opts_t store_opts;
	defw2_dir_record_t record;
	defw2_dir_query_t query;
	defw2_dir_filter_t filter;
	defw2_dir_result_t result;
	defw2_status_t status;
	defw2_call_opts_t opts;
	uint64_t generation = 0;
	const char *aliases[] = { "ornl-iqm-20q" };
	const char *resources[] = { "IQM-20q" };
	const defw2_dir_binding_t bindings[] = {
		{ "execution", "qfw.qpm.execution", 1, 7 },
		{ "telemetry", "qfw.qpm.telemetry", 2, 8 },
	};
	const defw2_dir_property_t properties[] = {
		{ "qpm_capabilities", "0x0d" },
		{ "vendor", "iqm" },
	};

	memset(&server_cfg, 0, sizeof(server_cfg));
	memset(&client_cfg, 0, sizeof(client_cfg));
	memset(&status, 0, sizeof(status));
	memset(&opts, 0, sizeof(opts));
	opts.timeout_ms = 5000;

	server_cfg.address = "na+sm://";
	server_cfg.node_name = "dirsvc";
	server_cfg.role = DEFW2_ROLE_SERVER;
	server_cfg.log_level = DEFW2_LOG_ERROR;
	server_cfg.rpc_thread_count = 2;
	if (defw2_init(&server_cfg, &server_rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the directory runtime\n");
		return EXIT_FAILURE;
	}

	/* A short timeout, so the liveness case does not take 15 seconds. */
	memset(&store_opts, 0, sizeof(store_opts));
	store_opts.heartbeat_timeout_ms = 200;
	store_opts.scan_interval_ms = 50;
	store_opts.retention_ms = 60000;

	check("the directory service is created",
	      defw2_service_create(server_rt, "dirsvc-1", DEFW2_API_DIR,
				   DEFW2_PROVIDER_DIR, &svc) == DEFW2_OK);
	check("qfw.directory binds",
	      defw2_dir_bind(svc, &store_opts) == DEFW2_OK);

	client_cfg.address = "na+sm://";
	client_cfg.node_name = "dir-client";
	client_cfg.role = DEFW2_ROLE_CLIENT;
	client_cfg.log_level = DEFW2_LOG_ERROR;
	if (defw2_init(&client_cfg, &client_rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the client runtime\n");
		return EXIT_FAILURE;
	}

	check("a client opens the directory",
	      defw2_dir_open(client_rt, defw2_service_address(svc), &dir) ==
	      DEFW2_OK);
	/* The bootstrap's own failure mode: nobody told us where it is. */
	{
		defw2_dir_t *nowhere = NULL;

		check("opening a directory with no address is a config error",
		      defw2_dir_open(client_rt, NULL, &nowhere) ==
		      DEFW2_ERR_CONFIG);
	}

	/* --- register over the wire, with every field populated --- */
	memset(&record, 0, sizeof(record));
	record.service_id = "qpm-rpc";
	record.service_type = "qfw.qpm";
	record.runtime_id = "runtime-a";
	record.address = "na+sm://999-0";
	record.endpoint.node_name = "qpm_rpc";
	record.endpoint.hostname = "rpc-host";
	record.endpoint.pid = 1234;
	record.selector.name = "IQM-20q";
	record.selector.aliases = aliases;
	record.selector.alias_count = 1;
	record.selector.resources = resources;
	record.selector.resource_count = 1;
	record.bindings = bindings;
	record.binding_count = 2;
	record.properties = properties;
	record.property_count = 2;

	check("register_service answers",
	      defw2_dir_register(dir, &record, &opts, &generation, &status) ==
	      DEFW2_OK);
	check("and reports OK", status.code == DEFW2_OK);
	check("with generation 1", generation == 1);

	/* --- resolve, and check the record crossed intact --- */
	memset(&query, 0, sizeof(query));
	query.service_type = "qfw.qpm";
	query.binding_name = "telemetry";
	memset(&result, 0, sizeof(result));
	check("resolve_services answers",
	      defw2_dir_resolve(dir, &query, &opts, &result, &status) ==
	      DEFW2_OK);
	check("one record came back", result.entry_count == 1);
	if (result.entry_count == 1) {
		const defw2_dir_entry_t *e = &result.entries[0];

		check("service_id survived the wire",
		      strcmp(e->record.service_id, "qpm-rpc") == 0);
		check("the address survived",
		      strcmp(e->record.address, "na+sm://999-0") == 0);
		check("the endpoint survived",
		      e->record.endpoint.pid == 1234 &&
		      strcmp(e->record.endpoint.hostname, "rpc-host") == 0);
		check("the generation and state survived",
		      e->record.generation == 1 &&
		      e->record.state == DEFW2_DIR_STATE_UP);
		check("the alias list survived",
		      e->record.selector.alias_count == 1 &&
		      strcmp(e->record.selector.aliases[0],
			     "ornl-iqm-20q") == 0);
		check("the resource list survived",
		      e->record.selector.resource_count == 1 &&
		      strcmp(e->record.selector.resources[0],
			     "IQM-20q") == 0);
		check("both bindings survived",
		      e->record.binding_count == 2 &&
		      strcmp(e->record.bindings[1].api_id,
			     "qfw.qpm.telemetry") == 0);
		check("the properties survived",
		      e->record.property_count == 2 &&
		      strcmp(e->record.properties[0].value, "0x0d") == 0);
		/* The selection the server made, not one the client redid. */
		check("the selected binding is the one asked for",
		      e->binding.provider_id == 8 &&
		      strcmp(e->binding.binding_name, "telemetry") == 0);
		check("the timestamps the directory owns are set",
		      e->record.registered_at_ns > 0 &&
		      e->record.last_heartbeat_ns > 0);
	}
	defw2_dir_result_free(&result);

	/* A bitmask filter, over the wire this time. */
	memset(&query, 0, sizeof(query));
	memset(&filter, 0, sizeof(filter));
	filter.name = "qpm_capabilities";
	filter.value = "0x05";
	filter.match = DEFW2_DIR_MATCH_BITS_ALL;
	query.filters = &filter;
	query.filter_count = 1;
	memset(&result, 0, sizeof(result));
	defw2_dir_resolve(dir, &query, &opts, &result, &status);
	check("a bitmask filter crosses the wire",
	      result.entry_count == 1);
	defw2_dir_result_free(&result);

	filter.value = "0x02";
	memset(&result, 0, sizeof(result));
	defw2_dir_resolve(dir, &query, &opts, &result, &status);
	check("and still excludes what it should",
	      result.entry_count == 0);
	defw2_dir_result_free(&result);

	/* --- heartbeat and get_generation --- */
	check("heartbeat answers",
	      defw2_dir_heartbeat(dir, "qpm-rpc", "runtime-a", 1, &opts,
				  &status) == DEFW2_OK);
	check("and is accepted", status.code == DEFW2_OK);
	check("a wrong runtime's heartbeat is refused over the wire",
	      defw2_dir_heartbeat(dir, "qpm-rpc", "runtime-z", 1, &opts,
				  &status) == DEFW2_OK &&
	      status.code == DEFW2_ERR_NOT_FOUND);

	generation = 0;
	check("get_generation answers",
	      defw2_dir_generation(dir, "qpm-rpc", &opts, &generation,
				   &status) == DEFW2_OK);
	check("with the current generation",
	      status.code == DEFW2_OK && generation == 1);

	/* --- the liveness timer, which only exists in the served form --- */
	sleep_ms(500);
	memset(&query, 0, sizeof(query));
	memset(&result, 0, sizeof(result));
	defw2_dir_resolve(dir, &query, &opts, &result, &status);
	check("the service's own timer timed the record out",
	      result.entry_count == 0);
	defw2_dir_result_free(&result);

	/* query_directory sees what resolve_services will not. */
	memset(&result, 0, sizeof(result));
	defw2_dir_query(dir, &query, &opts, &result, &status);
	check("query_directory still shows it",
	      result.entry_count == 1 &&
	      result.entries[0].record.state == DEFW2_DIR_STATE_TIMED_OUT);
	defw2_dir_result_free(&result);

	/*
	 * A client cannot reach the operator view by setting the flag: the
	 * registration decides, so resolve_services ignores it.
	 */
	memset(&query, 0, sizeof(query));
	query.include_inactive = true;
	memset(&result, 0, sizeof(result));
	defw2_dir_resolve(dir, &query, &opts, &result, &status);
	check("resolve_services ignores a caller's include_inactive",
	      result.entry_count == 0);
	defw2_dir_result_free(&result);

	/* --- restart, over the wire --- */
	record.runtime_id = "runtime-b";
	generation = 0;
	defw2_dir_register(dir, &record, &opts, &generation, &status);
	check("a restart over the wire gets a new generation",
	      status.code == DEFW2_OK && generation == 2);

	/* --- deregister --- */
	check("deregister_service answers",
	      defw2_dir_deregister(dir, "qpm-rpc", "runtime-b", 2, &opts,
				   &status) == DEFW2_OK);
	check("and is accepted", status.code == DEFW2_OK);
	memset(&query, 0, sizeof(query));
	memset(&result, 0, sizeof(result));
	defw2_dir_query(dir, &query, &opts, &result, &status);
	check("a deregistered record has no address",
	      result.entry_count == 1 &&
	      result.entries[0].record.state ==
	      DEFW2_DIR_STATE_DEREGISTERED &&
	      result.entries[0].record.address == NULL);
	defw2_dir_result_free(&result);

	/* An empty answer is success, not an error. */
	memset(&query, 0, sizeof(query));
	query.service_id = "nobody-here";
	memset(&result, 0, sizeof(result));
	check("resolving nothing is still OK",
	      defw2_dir_resolve(dir, &query, &opts, &result, &status) ==
	      DEFW2_OK && result.entry_count == 0 &&
	      status.code == DEFW2_OK);
	defw2_dir_result_free(&result);

	defw2_status_free(&status);
	defw2_dir_close(dir);
	defw2_finalize(client_rt);
	defw2_service_shutdown(svc);
	defw2_service_destroy(svc);
	defw2_finalize(server_rt);

	printf("\n%s\n", failures == 0 ? "directory rpc smoke passed"
				       : "directory rpc smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
