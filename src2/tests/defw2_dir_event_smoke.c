/*
 * Does the directory tell a sink about a service coming and going?
 *
 * Two runtimes in one process over na+sm: a directory, and a subscriber
 * whose sinks take its events. The subscriber also registers, heartbeats
 * and deregisters records by hand, so each change happens when the test
 * says. It checks what defw2_dir.h promises:
 *
 * - every change reaches each subscription it matches, with v1's name as
 *   its type, its reason, and the record as the directory now holds it;
 * - a subscriber hears of the changes in the order the directory made them;
 * - service_id, service_type and the changes mask each narrow a
 *   subscription;
 * - a record whose heartbeats stop goes and, when they resume, comes back;
 * - unsubscribe ends a subscription, and an unknown one is not found;
 * - a subscription whose sink is gone is dropped, without holding up a live
 *   one;
 * - a subscription that names no sink, or the directory's provider, is
 *   refused.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <defw2/defw2.h>
#include <defw2/defw2_dir.h>
#include <defw2/defw2_event.h>

/*
 * Long enough that no record times out while the test works, short enough
 * that the one that is meant to does so quickly.
 */
#define TIMEOUT_MS	1000
#define WAIT_MS		10000
#define BURST		50
#define GONE_CHANGES	10

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

static bool eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L,
	};

	nanosleep(&ts, NULL);
}

static defw2_call_opts_t opts = { .timeout_ms = 5000 };

struct world {
	defw2_rt_t		*dir_rt;
	defw2_rt_t		*rt;		/* the subscriber's */
	defw2_service_t		*svc;
	defw2_dir_t		*dir;
};

/* A record of a service the test plays, with one binding and property. */
static void record_of(defw2_dir_record_t *record, const char *service_id,
		      const char *service_type, const char *runtime_id)
{
	static const defw2_dir_binding_t binding = {
		"execution", "qfw.qpm.execution", 1, 4,
	};
	static const defw2_dir_property_t property = { "vendor", "fake" };
	static const char *resources[] = { "FAKE-20q" };

	memset(record, 0, sizeof(*record));
	record->service_id = service_id;
	record->service_type = service_type;
	record->runtime_id = runtime_id;
	record->address = "na+sm://4242-0";
	record->endpoint.node_name = "node-1";
	record->endpoint.hostname = "host-1";
	record->endpoint.pid = 4242;
	record->selector.name = "fake-20q";
	record->selector.resources = resources;
	record->selector.resource_count = 1;
	record->bindings = &binding;
	record->binding_count = 1;
	record->properties = &property;
	record->property_count = 1;
}

static uint64_t do_register(struct world *w, const char *service_id,
			    const char *service_type, const char *runtime_id)
{
	defw2_status_t status = { 0 };
	defw2_dir_record_t record;
	uint64_t generation = 0;

	record_of(&record, service_id, service_type, runtime_id);
	if (defw2_dir_register(w->dir, &record, &opts, &generation,
			       &status) != DEFW2_OK || status.code != DEFW2_OK)
		generation = 0;
	defw2_status_free(&status);
	return generation;
}

static bool do_deregister(struct world *w, const char *service_id,
			  const char *runtime_id, uint64_t generation)
{
	defw2_status_t status = { 0 };
	bool ok;

	ok = defw2_dir_deregister(w->dir, service_id, runtime_id, generation,
				  &opts, &status) == DEFW2_OK &&
	     status.code == DEFW2_OK;
	defw2_status_free(&status);
	return ok;
}

static defw2_event_sink_t *sink_on(struct world *w, uint16_t provider)
{
	defw2_event_sink_t *sink = NULL;

	if (defw2_event_sink_create(w->rt, provider, NULL, &sink) != DEFW2_OK ||
	    defw2_dir_event_accept(sink) != DEFW2_OK) {
		defw2_event_sink_destroy(sink);
		return NULL;
	}
	return sink;
}

static uint64_t subscribe(struct world *w, defw2_event_sink_t *sink,
			  const char *tag, const char *service_id,
			  const char *service_type, uint32_t changes)
{
	defw2_dir_subscribe_req_t req = {
		.target = { defw2_event_sink_address(sink),
			    defw2_event_sink_provider_id(sink), tag },
		.service_id = service_id,
		.service_type = service_type,
		.changes = changes,
	};
	defw2_status_t status = { 0 };
	uint64_t id = 0;

	if (defw2_dir_subscribe(w->dir, &req, &opts, &id, &status) !=
	    DEFW2_OK || status.code != DEFW2_OK)
		id = 0;
	defw2_status_free(&status);
	return id;
}

/*
 * The next change a sink takes, checked against what is expected of it.
 * reason NULL skips the reason, and generation 0 skips the generation.
 */
static bool next_is(defw2_event_sink_t *sink, bool connected,
		    const char *service_id, const char *reason,
		    uint64_t generation)
{
	const defw2_dir_change_t *change;
	defw2_event_t event;
	bool ok;

	if (defw2_event_sink_next(sink, WAIT_MS, &event) != DEFW2_OK)
		return false;
	change = defw2_dir_event_change(&event);
	ok = change != NULL && change->connected == connected &&
	     eq(event.type, connected ? DEFW2_DIR_SERVICE_CONNECTED
				      : DEFW2_DIR_SERVICE_DISCONNECTED) &&
	     eq(change->record.service_id, service_id) &&
	     (reason == NULL || eq(change->reason, reason)) &&
	     (generation == 0 || change->record.generation == generation);
	if (!ok && change != NULL)
		printf("    took %s %s %s generation %" PRIu64 "\n",
		       event.type, change->record.service_id, change->reason,
		       change->record.generation);
	defw2_event_free(&event);
	return ok;
}

static bool nothing_more(defw2_event_sink_t *sink)
{
	defw2_event_t event;

	if (defw2_event_sink_next(sink, 300, &event) == DEFW2_ERR_TIMEOUT)
		return true;
	defw2_event_free(&event);
	return false;
}

/* The first event in full: every field the record carried, and the envelope. */
static void first_change(struct world *w, defw2_event_sink_t *sink)
{
	const defw2_dir_change_t *change;
	const defw2_dir_record_t *r;
	defw2_event_t event;

	check("a registration reaches a subscriber",
	      defw2_event_sink_next(sink, WAIT_MS, &event) == DEFW2_OK);
	change = defw2_dir_event_change(&event);
	r = change != NULL ? &change->record : NULL;
	check("as a directory event from the directory's runtime",
	      eq(event.api, DEFW2_API_DIR) &&
	      eq(event.name, DEFW2_DIR_EVENT_SERVICE) &&
	      eq(event.source, defw2_runtime_id(w->dir_rt)) &&
	      eq(event.tag, "all") && event.seq == 1);
	check("named SERVICE_CONNECTED, as v1 named it",
	      change != NULL && change->connected &&
	      eq(event.type, DEFW2_DIR_SERVICE_CONNECTED) &&
	      eq(change->reason, "registered"));
	check("carrying the record as the directory now holds it",
	      r != NULL && eq(r->service_id, "svc-a") &&
	      eq(r->service_type, "type-a") && eq(r->runtime_id, "rt-a") &&
	      r->generation == 1 && r->state == DEFW2_DIR_STATE_UP &&
	      eq(r->address, "na+sm://4242-0") &&
	      eq(r->endpoint.hostname, "host-1") && r->endpoint.pid == 4242 &&
	      eq(r->selector.name, "fake-20q") &&
	      r->selector.resource_count == 1 &&
	      eq(r->selector.resources[0], "FAKE-20q") &&
	      r->binding_count == 1 &&
	      eq(r->bindings[0].api_id, "qfw.qpm.execution") &&
	      r->bindings[0].provider_id == 4 && r->property_count == 1 &&
	      eq(r->properties[0].value, "fake") &&
	      r->registered_at_ns != 0);
	defw2_event_free(&event);
}

static bool start(struct world *w)
{
	defw2_dir_store_opts_t store_opts = {
		.heartbeat_timeout_ms = TIMEOUT_MS,
		.scan_interval_ms = 50,
		.retention_ms = 60000,
	};
	defw2_config_t cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.address = "na+sm://";
	cfg.node_name = "dirsvc";
	cfg.role = DEFW2_ROLE_SERVER;
	cfg.log_level = DEFW2_LOG_ERROR;
	cfg.rpc_thread_count = 2;
	if (defw2_init(&cfg, &w->dir_rt) != DEFW2_OK ||
	    defw2_service_create(w->dir_rt, "dirsvc-events", DEFW2_API_DIR,
				 DEFW2_PROVIDER_DIR, &w->svc) != DEFW2_OK ||
	    defw2_dir_bind(w->svc, &store_opts) != DEFW2_OK)
		return false;

	/* A server, because its sinks are providers. */
	cfg.node_name = "subscriber";
	if (defw2_init(&cfg, &w->rt) != DEFW2_OK)
		return false;
	return defw2_dir_open(w->rt, defw2_service_address(w->svc),
			      &w->dir) == DEFW2_OK;
}

int main(void)
{
	defw2_event_sink_t *all, *only_a, *only_b, *ups, *gone;
	uint64_t id_all, id_a, id_b, id_ups, id_gone;
	struct world w = { 0 };
	defw2_status_t status = { 0 };
	uint64_t generation = 0, gen_b = 0;
	bool in_order = true, ups_in_order = true;
	int i;

	check("a directory and a subscriber start", start(&w));
	if (failures != 0)
		return EXIT_FAILURE;

	all = sink_on(&w, 5);
	only_a = sink_on(&w, 6);
	only_b = sink_on(&w, 7);
	ups = sink_on(&w, 8);
	check("four sinks take directory events",
	      all != NULL && only_a != NULL && only_b != NULL && ups != NULL);
	if (failures != 0)
		return EXIT_FAILURE;

	id_all = subscribe(&w, all, "all", NULL, NULL, 0);
	id_a = subscribe(&w, only_a, "a", "svc-a", NULL, 0);
	id_b = subscribe(&w, only_b, "b", NULL, "type-b", 0);
	id_ups = subscribe(&w, ups, "ups", NULL, NULL, DEFW2_DIR_CONNECTED);
	check("each subscribes, and the directory names each one",
	      id_all != 0 && id_a != 0 && id_b != 0 && id_ups != 0 &&
	      id_all != id_a && id_a != id_b && id_b != id_ups);

	{
		defw2_dir_subscribe_req_t bad = {
			.target = { NULL, 5, "x" },
		};
		uint64_t id = 0;

		check("a subscription that names no sink is refused",
		      defw2_dir_subscribe(w.dir, &bad, &opts, &id, &status) ==
		      DEFW2_OK &&
		      status.category == DEFW2_CAT_INVALID_ARGUMENT && id == 0);
		bad.target.address = defw2_event_sink_address(all);
		bad.target.provider_id = DEFW2_PROVIDER_DIR;
		check("and so is one on the directory's provider",
		      defw2_dir_subscribe(w.dir, &bad, &opts, &id, &status) ==
		      DEFW2_OK &&
		      status.category == DEFW2_CAT_INVALID_ARGUMENT);
	}

	/* --- each change, to each subscription it matches --- */
	check("svc-a registers", do_register(&w, "svc-a", "type-a", "rt-a") ==
	      1);
	first_change(&w, all);
	check("a service_id subscription hears of its service",
	      next_is(only_a, true, "svc-a", "registered", 1));
	check("and so does one for every connect",
	      next_is(ups, true, "svc-a", "registered", 1));

	gen_b = do_register(&w, "svc-b", "type-b", "rt-b");
	check("svc-b registers under another type", gen_b == 1);
	check("a service_type subscription hears of that type",
	      next_is(only_b, true, "svc-b", "registered", 1));
	check("everything goes to the subscription for everything",
	      next_is(all, true, "svc-b", "registered", 1) &&
	      next_is(ups, true, "svc-b", "registered", 1));
	check("svc-a deregisters",
	      do_deregister(&w, "svc-a", "rt-a", 1));
	{
		const defw2_dir_change_t *change;
		defw2_event_t event;
		bool ok;

		ok = defw2_event_sink_next(all, WAIT_MS, &event) == DEFW2_OK;
		change = ok ? defw2_dir_event_change(&event) : NULL;
		check("a deregistration is SERVICE_DISCONNECTED",
		      change != NULL && !change->connected &&
		      eq(event.type, DEFW2_DIR_SERVICE_DISCONNECTED) &&
		      eq(change->reason, "deregistered") &&
		      eq(change->record.service_id, "svc-a"));
		check("with the record as it now stands, address gone",
		      change != NULL &&
		      change->record.state == DEFW2_DIR_STATE_DEREGISTERED &&
		      change->record.address == NULL &&
		      change->record.retention_deadline_ns != 0);
		if (ok)
			defw2_event_free(&event);
	}
	check("the service_id subscription hears of it too",
	      next_is(only_a, false, "svc-a", "deregistered", 1));

	/* svc-b's heartbeats stop, so the scan times it out. */
	check("a record whose heartbeats stop goes",
	      next_is(only_b, false, "svc-b", "heartbeat-timeout", 1) &&
	      next_is(all, false, "svc-b", "heartbeat-timeout", 1));
	check("and comes back when they resume",
	      defw2_dir_heartbeat(w.dir, "svc-b", "rt-b", gen_b, &opts,
				  &status) == DEFW2_OK &&
	      status.code == DEFW2_OK &&
	      next_is(only_b, true, "svc-b", "heartbeat-resumed", 1) &&
	      next_is(all, true, "svc-b", "heartbeat-resumed", 1) &&
	      next_is(ups, true, "svc-b", "heartbeat-resumed", 1));
	check("svc-b deregisters before it times out again",
	      do_deregister(&w, "svc-b", "rt-b", gen_b) &&
	      next_is(only_b, false, "svc-b", "deregistered", 1) &&
	      next_is(all, false, "svc-b", "deregistered", 1));

	check("nothing reaches a subscription that does not match",
	      nothing_more(only_a) && nothing_more(only_b) &&
	      nothing_more(ups));

	/* --- order --- */
	for (i = 0; i < BURST; i++) {
		generation = do_register(&w, "svc-c", "type-c", "rt-c");
		if (generation != (uint64_t)i + 1 ||
		    !do_deregister(&w, "svc-c", "rt-c", generation))
			in_order = false;
	}
	check("a service registers and deregisters fifty times", in_order);
	for (i = 0; i < BURST && in_order; i++) {
		in_order = next_is(all, true, "svc-c", "registered",
				   (uint64_t)i + 1) &&
			   next_is(all, false, "svc-c", "deregistered",
				   (uint64_t)i + 1);
		ups_in_order = ups_in_order &&
			       next_is(ups, true, "svc-c", "registered",
				       (uint64_t)i + 1);
	}
	check("every change arrives in the order the directory made it",
	      in_order);
	check("and a subscription for connects hears only those, in order",
	      ups_in_order && nothing_more(ups));

	/* --- unsubscribe --- */
	check("unsubscribe ends a subscription",
	      defw2_dir_unsubscribe(w.dir, id_ups, &opts, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK);
	generation = do_register(&w, "svc-c", "type-c", "rt-c");
	check("so its sink hears nothing more",
	      generation != 0 && next_is(all, true, "svc-c", "registered",
					 generation) &&
	      nothing_more(ups));
	check("an id the directory no longer holds is not found",
	      defw2_dir_unsubscribe(w.dir, id_ups, &opts, &status) ==
	      DEFW2_OK && status.category == DEFW2_CAT_NOT_FOUND);
	check("nor is one it never gave",
	      defw2_dir_unsubscribe(w.dir, 999999, &opts, &status) ==
	      DEFW2_OK && status.category == DEFW2_CAT_NOT_FOUND);

	/*
	 * --- a sink that goes ---
	 *
	 * Its first delivery fails, the next publish to it says so, and the
	 * directory drops the subscription. Spacing the changes out gives the
	 * failure time to arrive, however loaded the machine.
	 */
	gone = sink_on(&w, 9);
	id_gone = gone != NULL ? subscribe(&w, gone, "gone", NULL, NULL, 0)
			       : 0;
	check("a fifth sink subscribes", id_gone != 0);
	defw2_event_sink_destroy(gone);
	in_order = true;
	for (i = 0; i < GONE_CHANGES; i++) {
		bool up = i % 2 == 1;

		if (up)
			generation = do_register(&w, "svc-c", "type-c",
						 "rt-c");
		else if (!do_deregister(&w, "svc-c", "rt-c", generation))
			in_order = false;
		in_order = in_order &&
			   next_is(all, up, "svc-c", NULL, generation);
		sleep_ms(100);
	}
	/* Down again, so no timeout fires while the runtimes stop. */
	in_order = in_order &&
		   do_deregister(&w, "svc-c", "rt-c", generation) &&
		   next_is(all, false, "svc-c", "deregistered", generation);
	check("a dead sink does not hold up a live one", in_order);
	check("and the directory drops the dead one's subscription",
	      defw2_dir_unsubscribe(w.dir, id_gone, &opts, &status) ==
	      DEFW2_OK && status.category == DEFW2_CAT_NOT_FOUND);
	check("but keeps the live ones",
	      defw2_dir_unsubscribe(w.dir, id_a, &opts, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK);

	defw2_status_free(&status);
	defw2_event_sink_destroy(all);
	defw2_event_sink_destroy(only_a);
	defw2_event_sink_destroy(only_b);
	defw2_event_sink_destroy(ups);
	defw2_dir_close(w.dir);
	defw2_finalize(w.rt);
	defw2_service_shutdown(w.svc);
	defw2_service_destroy(w.svc);
	defw2_finalize(w.dir_rt);

	printf("\n%s\n", failures == 0 ? "dir event smoke passed"
				       : "dir event smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
