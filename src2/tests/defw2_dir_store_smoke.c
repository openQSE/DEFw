/*
 * Does the directory store keep the lifecycle model?
 *
 * This is phase 1's exit criterion taken apart: a service that stops
 * heartbeating becomes TIMED_OUT within the timeout, and its restart gets a
 * new generation. The rest of the file covers the matching rules and the
 * cases where the store has to say no.
 *
 * It drives the store directly with no runtime and no Margo, so it needs no
 * network, no port and no directory process, and a timeout is 50 ms rather
 * than 15 seconds. The service, the wire and the agent are tested separately;
 * what is checked here is the state machine they all depend on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../dir/defw2_dir_internal.h"

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-52s %s\n", what, ok ? "ok" : "FAILED");
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

/* A record with just enough in it to be registrable. */
static defw2_dir_record_t basic_record(const char *service_id,
				       const char *runtime_id)
{
	defw2_dir_record_t record;

	memset(&record, 0, sizeof(record));
	record.service_id = service_id;
	record.service_type = "qfw.qpm";
	record.runtime_id = runtime_id;
	record.address = "na+sm://1-0";
	record.endpoint.node_name = "qpm_test";
	record.endpoint.hostname = "test-host";
	record.endpoint.pid = 4242;
	return record;
}

static size_t resolve_count(defw2_dir_store_t *store,
			    const defw2_dir_query_t *query)
{
	defw2_dir_result_t result;
	defw2_status_t status;
	size_t count;

	memset(&result, 0, sizeof(result));
	memset(&status, 0, sizeof(status));
	if (defw2_dir_store_resolve(store, query, &result, &status) != DEFW2_OK)
		return (size_t)-1;
	count = result.entry_count;
	defw2_dir_result_free(&result);
	defw2_status_free(&status);
	return count;
}

/*
 * The exit criterion. A registered service that goes quiet is TIMED_OUT by
 * the scan, disappears from resolve while still being queryable, and its
 * replacement gets a higher generation.
 */
static void test_timeout_and_restart(void)
{
	defw2_dir_store_opts_t opts;
	defw2_dir_store_t *store = NULL;
	defw2_dir_record_t record;
	defw2_dir_query_t all;
	defw2_dir_query_t inactive;
	defw2_status_t status;
	uint64_t generation = 0;

	memset(&opts, 0, sizeof(opts));
	opts.heartbeat_timeout_ms = 50;
	opts.retention_ms = 60000;
	memset(&all, 0, sizeof(all));
	memset(&inactive, 0, sizeof(inactive));
	inactive.include_inactive = true;
	memset(&status, 0, sizeof(status));

	check("store creates",
	      defw2_dir_store_create(NULL, &opts, &store) == DEFW2_OK);

	record = basic_record("qpm-1", "runtime-a");
	defw2_dir_store_register(store, &record, &generation, &status);
	check("first registration is generation 1", generation == 1);
	check("registration reports OK", status.code == DEFW2_OK);
	check("a registered service resolves", resolve_count(store, &all) == 1);

	/* A heartbeat inside the timeout keeps it up. */
	sleep_ms(20);
	defw2_dir_store_heartbeat(store, "qpm-1", "runtime-a", 1, &status);
	check("heartbeat inside the timeout is accepted",
	      status.code == DEFW2_OK);
	check("no scan casualty while heartbeating",
	      defw2_dir_store_scan(store) == 0);
	check("still resolves", resolve_count(store, &all) == 1);

	/* Then stop, and let the scan notice. */
	sleep_ms(70);
	check("the scan times out a silent service",
	      defw2_dir_store_scan(store) == 1);
	check("a timed-out service does not resolve",
	      resolve_count(store, &all) == 0);
	check("but an operator can still see it",
	      resolve_count(store, &inactive) == 1);

	/* The restart. A new process, so a new runtime_id. */
	record = basic_record("qpm-1", "runtime-b");
	generation = 0;
	defw2_dir_store_register(store, &record, &generation, &status);
	check("a restart gets a new generation", generation == 2);
	check("the restarted service resolves",
	      resolve_count(store, &all) == 1);
	check("only one record survives the restart",
	      resolve_count(store, &inactive) == 1);

	/* A heartbeat from the process it replaced must not revive anything. */
	defw2_dir_store_heartbeat(store, "qpm-1", "runtime-a", 1, &status);
	check("a superseded runtime's heartbeat is refused",
	      status.code == DEFW2_ERR_NOT_FOUND);
	defw2_dir_store_heartbeat(store, "qpm-1", "runtime-b", 1, &status);
	check("a stale generation's heartbeat is refused",
	      status.code == DEFW2_ERR_NOT_FOUND);
	defw2_dir_store_heartbeat(store, "qpm-1", "runtime-b", 2, &status);
	check("the current runtime's heartbeat is accepted",
	      status.code == DEFW2_OK);

	defw2_status_free(&status);
	defw2_dir_store_destroy(store);
}

/*
 * A heartbeat that arrives late, after the scan gave up, brings the record
 * back rather than forcing a re-registration. The process never left.
 */
static void test_late_heartbeat_revives(void)
{
	defw2_dir_store_opts_t opts;
	defw2_dir_store_t *store = NULL;
	defw2_dir_record_t record;
	defw2_dir_query_t all;
	defw2_status_t status;

	memset(&opts, 0, sizeof(opts));
	opts.heartbeat_timeout_ms = 30;
	opts.retention_ms = 60000;
	memset(&all, 0, sizeof(all));
	memset(&status, 0, sizeof(status));
	defw2_dir_store_create(NULL, &opts, &store);

	record = basic_record("qpm-2", "runtime-a");
	defw2_dir_store_register(store, &record, NULL, &status);
	sleep_ms(50);
	defw2_dir_store_scan(store);
	check("gone quiet, so not resolvable", resolve_count(store, &all) == 0);

	defw2_dir_store_heartbeat(store, "qpm-2", "runtime-a", 1, &status);
	check("a late heartbeat is accepted", status.code == DEFW2_OK);
	check("and the record is back", resolve_count(store, &all) == 1);

	defw2_status_free(&status);
	defw2_dir_store_destroy(store);
}

static void test_conflict_and_deregister(void)
{
	defw2_dir_store_t *store = NULL;
	defw2_dir_record_t record;
	defw2_dir_query_t all;
	defw2_dir_query_t inactive;
	defw2_status_t status;
	uint64_t generation = 0;

	memset(&all, 0, sizeof(all));
	memset(&inactive, 0, sizeof(inactive));
	inactive.include_inactive = true;
	memset(&status, 0, sizeof(status));
	defw2_dir_store_create(NULL, NULL, &store);

	record = basic_record("qpm-3", "runtime-a");
	defw2_dir_store_register(store, &record, &generation, &status);

	/* A second live process claiming the same id is a mistake, not a restart. */
	record = basic_record("qpm-3", "runtime-b");
	defw2_dir_store_register(store, &record, NULL, &status);
	check("a live conflicting runtime is refused",
	      status.code == DEFW2_ERR_BUSY);
	check("the incumbent is untouched", resolve_count(store, &all) == 1);

	/* The same process re-registering is fine, and counts as a restart. */
	record = basic_record("qpm-3", "runtime-a");
	generation = 0;
	defw2_dir_store_register(store, &record, &generation, &status);
	check("the same runtime may re-register", generation == 2);

	defw2_dir_store_deregister(store, "qpm-3", "runtime-a", 2, &status);
	check("deregister reports OK", status.code == DEFW2_OK);
	check("a deregistered service does not resolve",
	      resolve_count(store, &all) == 0);
	check("but is retained for the operator",
	      resolve_count(store, &inactive) == 1);

	/* A record with no service_id is not a record. */
	memset(&record, 0, sizeof(record));
	record.runtime_id = "runtime-c";
	defw2_dir_store_register(store, &record, NULL, &status);
	check("a record without a service_id is refused",
	      status.code == DEFW2_ERR_INVALID);

	defw2_status_free(&status);
	defw2_dir_store_destroy(store);
}

/* Retention is what finally removes an inactive record. */
static void test_retention_drops(void)
{
	defw2_dir_store_opts_t opts;
	defw2_dir_store_t *store = NULL;
	defw2_dir_record_t record;
	defw2_dir_query_t inactive;
	defw2_status_t status;

	memset(&opts, 0, sizeof(opts));
	opts.heartbeat_timeout_ms = 20;
	opts.retention_ms = 40;
	memset(&inactive, 0, sizeof(inactive));
	inactive.include_inactive = true;
	memset(&status, 0, sizeof(status));
	defw2_dir_store_create(NULL, &opts, &store);

	record = basic_record("qpm-4", "runtime-a");
	defw2_dir_store_register(store, &record, NULL, &status);
	sleep_ms(35);
	defw2_dir_store_scan(store);
	check("timed out but retained", resolve_count(store, &inactive) == 1);
	sleep_ms(60);
	defw2_dir_store_scan(store);
	check("dropped once retention passed",
	      resolve_count(store, &inactive) == 0);

	defw2_status_free(&status);
	defw2_dir_store_destroy(store);
}

/*
 * Matching. The interesting case is the bitmask, because it is the one thing
 * v1 hard-coded for qpm_type and qpm_capabilities and the reason the filter
 * carries its own mode.
 */
static void test_matching(void)
{
	defw2_dir_store_t *store = NULL;
	defw2_dir_record_t record;
	defw2_dir_query_t query;
	defw2_dir_filter_t filter;
	defw2_status_t status;
	const char *aliases[] = { "ornl-iqm-20q" };
	const char *resources[] = { "IQM-20q" };
	const defw2_dir_binding_t bindings[] = {
		{ "execution", "qfw.qpm.execution", 1, 7 },
		{ "telemetry", "qfw.qpm.telemetry", 2, 8 },
	};
	const defw2_dir_property_t properties[] = {
		{ "qpm_capabilities", "0x0d" },
		{ "vendor", "iqm" },
		{ "not_a_number", "banana" },
	};
	defw2_dir_result_t result;

	memset(&status, 0, sizeof(status));
	defw2_dir_store_create(NULL, NULL, &store);

	record = basic_record("qpm-5", "runtime-a");
	record.selector.name = "IQM-20q";
	record.selector.aliases = aliases;
	record.selector.alias_count = 1;
	record.selector.resources = resources;
	record.selector.resource_count = 1;
	record.bindings = bindings;
	record.binding_count = 2;
	record.properties = properties;
	record.property_count = 3;
	defw2_dir_store_register(store, &record, NULL, &status);

	memset(&query, 0, sizeof(query));
	query.service_type = "qfw.qpm";
	check("service_type matches", resolve_count(store, &query) == 1);
	query.service_type = "qfw.other";
	check("a wrong service_type does not",
	      resolve_count(store, &query) == 0);

	memset(&query, 0, sizeof(query));
	query.selector_name = "ornl-iqm-20q";
	check("an alias matches the selector",
	      resolve_count(store, &query) == 1);

	memset(&query, 0, sizeof(query));
	query.resource = "IQM-20q";
	check("a resource matches", resolve_count(store, &query) == 1);
	query.resource = "IQM-5q";
	check("a wrong resource does not", resolve_count(store, &query) == 0);

	/* The selected binding is what a client actually needs back. */
	memset(&query, 0, sizeof(query));
	query.binding_name = "telemetry";
	memset(&result, 0, sizeof(result));
	defw2_dir_store_resolve(store, &query, &result, &status);
	check("the named binding is selected",
	      result.entry_count == 1 && result.entries[0].binding.provider_id == 8);
	check("and its record still carries both bindings",
	      result.entry_count == 1 && result.entries[0].record.binding_count == 2);
	defw2_dir_result_free(&result);

	memset(&query, 0, sizeof(query));
	query.binding_name = "execution";
	query.api_version = 2;
	check("a binding with the wrong version does not match",
	      resolve_count(store, &query) == 0);

	/* Properties: equality, then the bitmask modes. */
	memset(&query, 0, sizeof(query));
	memset(&filter, 0, sizeof(filter));
	query.filters = &filter;
	query.filter_count = 1;

	filter.name = "vendor";
	filter.value = "iqm";
	filter.match = DEFW2_DIR_MATCH_EQUAL;
	check("an equal property matches", resolve_count(store, &query) == 1);
	filter.value = "ibm";
	check("an unequal one does not", resolve_count(store, &query) == 0);

	/* 0x0d is 1101, so 0x05 (101) is present and 0x02 (010) is not. */
	filter.name = "qpm_capabilities";
	filter.match = DEFW2_DIR_MATCH_BITS_ALL;
	filter.value = "0x05";
	check("BITS_ALL matches when every bit is set",
	      resolve_count(store, &query) == 1);
	filter.value = "0x07";
	check("BITS_ALL fails when one bit is missing",
	      resolve_count(store, &query) == 0);
	filter.match = DEFW2_DIR_MATCH_BITS_ANY;
	filter.value = "0x06";
	check("BITS_ANY matches on one shared bit",
	      resolve_count(store, &query) == 1);
	filter.value = "0x02";
	check("BITS_ANY fails with no shared bit",
	      resolve_count(store, &query) == 0);

	/* A value that will not parse is a mismatch, not a failed request. */
	filter.name = "not_a_number";
	filter.match = DEFW2_DIR_MATCH_BITS_ALL;
	filter.value = "0x01";
	check("an unparsable property value is a mismatch",
	      resolve_count(store, &query) == 0);

	filter.name = "absent";
	filter.match = DEFW2_DIR_MATCH_EQUAL;
	filter.value = "anything";
	check("a property the record lacks is a mismatch",
	      resolve_count(store, &query) == 0);

	/* The copy a caller gets must survive the store being emptied. */
	memset(&query, 0, sizeof(query));
	memset(&result, 0, sizeof(result));
	defw2_dir_store_resolve(store, &query, &result, &status);
	defw2_dir_store_deregister(store, "qpm-5", "runtime-a", 1, &status);
	check("a resolved record outlives the registration",
	      result.entry_count == 1 &&
	      strcmp(result.entries[0].record.service_id, "qpm-5") == 0 &&
	      strcmp(result.entries[0].record.selector.aliases[0],
		     "ornl-iqm-20q") == 0);
	defw2_dir_result_free(&result);

	defw2_status_free(&status);
	defw2_dir_store_destroy(store);
}

static void test_generation_lookup(void)
{
	defw2_dir_store_t *store = NULL;
	defw2_dir_record_t record;
	defw2_status_t status;
	uint64_t generation = 0;

	memset(&status, 0, sizeof(status));
	defw2_dir_store_create(NULL, NULL, &store);

	defw2_dir_store_generation(store, "nobody", &generation, &status);
	check("an unknown service_id has no generation",
	      status.code == DEFW2_ERR_NOT_FOUND);

	record = basic_record("qpm-6", "runtime-a");
	defw2_dir_store_register(store, &record, NULL, &status);
	defw2_dir_store_generation(store, "qpm-6", &generation, &status);
	check("a known one reports its generation",
	      status.code == DEFW2_OK && generation == 1);

	defw2_status_free(&status);
	defw2_dir_store_destroy(store);
}

/*
 * The snapshot. It is never read back, so what matters is only that it is
 * written, replaced atomically, and well formed even when a value contains
 * the characters that would break it. Whether it parses is checked by the
 * caller that passes a path, since C has no JSON reader here.
 */
static void test_snapshot(const char *path)
{
	defw2_dir_store_opts_t opts;
	defw2_dir_store_t *store = NULL;
	defw2_dir_record_t record;
	defw2_status_t status;
	const defw2_dir_property_t hostile[] = {
		{ "quote\"and\\backslash", "line\nbreak\ttab" },
		{ "plain", "value" },
	};
	FILE *check_file;

	memset(&opts, 0, sizeof(opts));
	opts.snapshot_path = path;
	memset(&status, 0, sizeof(status));
	defw2_dir_store_create(NULL, &opts, &store);

	record = basic_record("qpm-snap", "runtime-a");
	record.properties = hostile;
	record.property_count = 2;
	defw2_dir_store_register(store, &record, NULL, &status);

	check_file = fopen(path, "r");
	check("the snapshot is written", check_file != NULL);
	if (check_file != NULL)
		fclose(check_file);

	defw2_status_free(&status);
	defw2_dir_store_destroy(store);
}

int main(int argc, char **argv)
{
	test_timeout_and_restart();
	test_late_heartbeat_revives();
	test_conflict_and_deregister();
	test_retention_drops();
	test_matching();
	test_generation_lookup();
	/* The snapshot needs somewhere to write, so it runs only when asked. */
	if (argc > 1)
		test_snapshot(argv[1]);

	printf("\n%s\n", failures == 0 ? "directory store smoke passed"
				       : "directory store smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
