/*
 * Does the binding cache resolve once and notice when that stops being true?
 *
 * Three things matter here. A second lookup of the same query must not go to
 * the directory, or the cache is doing nothing. A different query must get
 * its own entry, or two services would share one binding. And a reported
 * failure must make the next lookup resolve again, because that is the only
 * invalidation signal there is: Mercury reports failures per call, not per
 * peer, so nothing but the caller can know.
 *
 * The cached binding is also used for a real echo call, since a binding that
 * cannot carry an RPC is not worth caching.
 *
 * A directory on one runtime, an echo service on another, and a client on a
 * third. Separate runtimes because one Margo instance cannot both serve an
 * API and look the same RPC names up as a client.
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
	defw2_config_t dir_cfg, svc_cfg, cli_cfg;
	defw2_rt_t *dir_rt = NULL, *svc_rt = NULL, *cli_rt = NULL;
	defw2_service_t *dirsvc = NULL, *echo_svc = NULL;
	defw2_dir_store_opts_t store_opts;
	defw2_dir_agent_t *agent = NULL;
	defw2_dir_cache_t *cache = NULL;
	defw2_dir_t *dir = NULL;
	defw2_dir_record_t record;
	defw2_dir_query_t query, other;
	defw2_binding_t *first = NULL, *again = NULL, *narrow = NULL;
	defw2_status_t status;
	defw2_call_opts_t opts;
	defw2_buffer_t reply;
	const defw2_dir_binding_t bindings[] = {
		{ "echo", DEFW2_API_ECHO, 1, DEFW2_PROVIDER_ECHO },
	};

	memset(&dir_cfg, 0, sizeof(dir_cfg));
	memset(&svc_cfg, 0, sizeof(svc_cfg));
	memset(&cli_cfg, 0, sizeof(cli_cfg));
	memset(&status, 0, sizeof(status));
	memset(&opts, 0, sizeof(opts));
	opts.timeout_ms = 5000;

	dir_cfg.address = "na+sm://";
	dir_cfg.node_name = "cache-dirsvc";
	dir_cfg.role = DEFW2_ROLE_SERVER;
	dir_cfg.log_level = DEFW2_LOG_ERROR;
	dir_cfg.rpc_thread_count = 2;
	svc_cfg = dir_cfg;
	svc_cfg.node_name = "cache-echo";
	cli_cfg.address = "na+sm://";
	cli_cfg.node_name = "cache-client";
	cli_cfg.role = DEFW2_ROLE_CLIENT;
	cli_cfg.log_level = DEFW2_LOG_ERROR;

	if (defw2_init(&dir_cfg, &dir_rt) != DEFW2_OK ||
	    defw2_init(&svc_cfg, &svc_rt) != DEFW2_OK ||
	    defw2_init(&cli_cfg, &cli_rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the runtimes\n");
		return EXIT_FAILURE;
	}

	memset(&store_opts, 0, sizeof(store_opts));
	store_opts.heartbeat_timeout_ms = 60000;
	store_opts.scan_interval_ms = 5000;
	check("the directory binds",
	      defw2_service_create(dir_rt, "dirsvc-1", DEFW2_API_DIR,
				   DEFW2_PROVIDER_DIR, &dirsvc) == DEFW2_OK &&
	      defw2_dir_bind(dirsvc, &store_opts) == DEFW2_OK);
	check("an echo service binds",
	      defw2_service_create(svc_rt, "qpm-cache", "qfw.qpm",
				   DEFW2_PROVIDER_ECHO,
				   &echo_svc) == DEFW2_OK &&
	      defw2_echo_bind(echo_svc, NULL) == DEFW2_OK);

	memset(&record, 0, sizeof(record));
	record.service_id = "qpm-cache";
	record.service_type = "qfw.qpm";
	record.selector.name = "cache-qpu";
	record.bindings = bindings;
	record.binding_count = 1;
	check("it registers itself",
	      defw2_dir_agent_start(echo_svc,
				    defw2_service_address(dirsvc), &record,
				    30000, &agent) == DEFW2_OK);

	check("a client opens the directory and a cache",
	      defw2_dir_open(cli_rt, defw2_service_address(dirsvc), &dir) ==
	      DEFW2_OK &&
	      defw2_dir_cache_create(dir, &cache) == DEFW2_OK);
	check("a new cache is empty", defw2_dir_cache_size(cache) == 0);

	memset(&query, 0, sizeof(query));
	query.service_type = "qfw.qpm";
	query.binding_name = "echo";

	check("the first lookup resolves",
	      defw2_dir_cache_binding(cache, &query, &opts, &first,
				      &status) == DEFW2_OK && first != NULL);
	check("and is cached", defw2_dir_cache_size(cache) == 1);
	check("the binding points at the echo service",
	      strcmp(defw2_binding_address(first),
		     defw2_service_address(echo_svc)) == 0 &&
	      defw2_binding_provider_id(first) == DEFW2_PROVIDER_ECHO);

	/* The point of the whole thing: the same query, the same binding. */
	check("a second lookup returns the very same binding",
	      defw2_dir_cache_binding(cache, &query, &opts, &again,
				      &status) == DEFW2_OK && again == first);
	check("and did not add an entry", defw2_dir_cache_size(cache) == 1);

	/* A cached binding has to be able to carry a call. */
	memset(&reply, 0, sizeof(reply));
	check("the cached binding carries a real echo",
	      defw2_echo(first, "cache", 5, &opts, &reply, &status) ==
	      DEFW2_OK && reply.len == 5 &&
	      memcmp(reply.data, "cache", 5) == 0);
	defw2_buffer_free(&reply);

	/* A different query is a different entry, not a shared one. */
	memset(&other, 0, sizeof(other));
	other.selector_name = "cache-qpu";
	check("a different query gets its own entry",
	      defw2_dir_cache_binding(cache, &other, &opts, &narrow,
				      &status) == DEFW2_OK &&
	      defw2_dir_cache_size(cache) == 2);
	check("even though it resolves to the same service",
	      narrow != NULL && narrow != first &&
	      strcmp(defw2_binding_address(narrow),
		     defw2_binding_address(first)) == 0);

	/* Invalidation, which is the caller's to report. */
	defw2_dir_cache_failed(cache, first);
	check("a reported failure drops that entry",
	      defw2_dir_cache_size(cache) == 1);
	check("and the next lookup resolves afresh",
	      defw2_dir_cache_binding(cache, &query, &opts, &again,
				      &status) == DEFW2_OK &&
	      again != NULL && defw2_dir_cache_size(cache) == 2);
	/* Reporting a binding the cache never had must be harmless. */
	defw2_dir_cache_failed(cache, narrow);
	defw2_dir_cache_failed(cache, narrow);
	check("reporting an unknown binding is harmless",
	      defw2_dir_cache_size(cache) == 1);

	/* Nothing matching is not cached, so the service can still arrive. */
	{
		defw2_dir_query_t missing;
		defw2_binding_t *nothing = NULL;
		size_t before = defw2_dir_cache_size(cache);

		memset(&missing, 0, sizeof(missing));
		missing.service_type = "qfw.nothing-serves-this";
		check("a query that resolves to nothing is NOT_FOUND",
		      defw2_dir_cache_binding(cache, &missing, &opts,
					      &nothing, &status) ==
		      DEFW2_ERR_NOT_FOUND && nothing == NULL);
		check("and is not remembered as an absence",
		      defw2_dir_cache_size(cache) == before);
	}

	/* revalidate: unchanged generations keep their entries. */
	check("revalidate keeps a service that has not restarted",
	      defw2_dir_cache_revalidate(cache, &opts) == 0 &&
	      defw2_dir_cache_size(cache) == 1);

	/*
	 * Then restart it. A second agent takes the generation to 2, which is
	 * what revalidate is for: noticing without having to fail a call.
	 */
	defw2_dir_agent_stop(agent);
	agent = NULL;
	check("after a restart revalidate drops the stale entry",
	      defw2_dir_agent_start(echo_svc,
				    defw2_service_address(dirsvc), &record,
				    30000, &agent) == DEFW2_OK &&
	      defw2_dir_agent_generation(agent) == 2 &&
	      defw2_dir_cache_revalidate(cache, &opts) == 1 &&
	      defw2_dir_cache_size(cache) == 0);

	/* clear, for a client that has reason to distrust everything. */
	defw2_dir_cache_binding(cache, &query, &opts, &first, &status);
	defw2_dir_cache_clear(cache);
	check("clear empties the cache", defw2_dir_cache_size(cache) == 0);

	defw2_status_free(&status);
	defw2_dir_cache_destroy(cache);
	defw2_dir_close(dir);
	defw2_finalize(cli_rt);
	defw2_dir_agent_stop(agent);
	defw2_service_shutdown(echo_svc);
	defw2_service_destroy(echo_svc);
	defw2_finalize(svc_rt);
	defw2_service_shutdown(dirsvc);
	defw2_service_destroy(dirsvc);
	defw2_finalize(dir_rt);

	printf("\n%s\n", failures == 0 ? "directory cache smoke passed"
				       : "directory cache smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
