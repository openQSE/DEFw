/*
 * Do events arrive whole and in order, and does a sink that stops answering
 * cost only itself?
 *
 * One process holds two runtimes over na+sm: a sender with publishers, and
 * a receiver serving sinks. A second process, this program run again with
 * --sink, serves one more sink and is then stopped with SIGSTOP. That is
 * the stalled client of openQSE/QFw issue 64, alive and holding its
 * connection but answering nothing, and the test holds the publisher to
 * what v1 could not do: every other sink still gets its events at once, no
 * publish waits on the network, and the stalled one fails at its time limit.
 *
 *	defw2_event_smoke [telemetry-dir]
 *	defw2_event_smoke --sink	serve a sink, print its address, and
 *					stop when stdin closes
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <defw2/defw2_qpm.h>

#define STALL_LIMIT_MS	500
/* Every other delivery's limit, long enough for a cold first one. */
#define PATIENT_MS	10000
#define TARGET_DEPTH	64
#define FAST_EVENTS	50
#define BIG_EXTRA	(200u * 1024u)
#define MAX_RECORDS	512

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static bool same(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a, b) == 0;
}

/* --- what is sent ---------------------------------------------------- */

/*
 * The task sent as event i. Every field varies with i, and absent and empty
 * strings alternate, so a field dropped or mangled on the way shows up.
 */
struct fixture {
	char			cid[32];
	char			extra[128];
	defw2_qpm_task_t	task;
};

static void fixture(struct fixture *f, unsigned i)
{
	memset(f, 0, sizeof(*f));
	snprintf(f->cid, sizeof(f->cid), "cid-%u", i);
	snprintf(f->extra, sizeof(f->extra),
		 "{\"counts\":{\"0000\":%u},\"i\":%u}", 1024 - i % 1024, i);
	f->task.outcome = i % 7 == 3 ? DEFW2_QPM_FAILED : DEFW2_QPM_COMPLETED;
	f->task.lifecycle_state = "completed";
	f->task.cid = f->cid;
	f->task.qtask_id = 1000 + i;
	f->task.reservation_id = 77;
	f->task.reason = i % 2 ? NULL : "";
	f->task.message = i % 3 ? "done" : NULL;
	f->task.completion_ready = true;
	f->task.extra = f->extra;
	defw2_tensor_vector(&f->task.statevector, DEFW2_DTYPE_C128,
			    1ull << (i % 21));
	/* A sender's claim, which an event must not repeat. */
	f->task.statevector_delivered = true;
}

/* Does a received task match what was sent as event i? */
static bool task_matches(const defw2_qpm_task_t *got, unsigned i)
{
	struct fixture f;
	uint32_t d;

	fixture(&f, i);
	if (got == NULL || !same(got->outcome, f.task.outcome) ||
	    !same(got->lifecycle_state, f.task.lifecycle_state) ||
	    !same(got->cid, f.task.cid) || !same(got->reason, f.task.reason) ||
	    !same(got->message, f.task.message) ||
	    !same(got->extra, f.task.extra) ||
	    got->qtask_id != f.task.qtask_id ||
	    got->reservation_id != f.task.reservation_id ||
	    got->completion_ready != f.task.completion_ready ||
	    got->statevector.dtype != f.task.statevector.dtype ||
	    got->statevector.rank != f.task.statevector.rank ||
	    got->statevector.nbytes != f.task.statevector.nbytes ||
	    got->statevector_delivered || got->arena != NULL)
		return false;
	for (d = 0; d < got->statevector.rank; d++)
		if (got->statevector.shape[d] != f.task.statevector.shape[d])
			return false;
	return true;
}

/* --- what arrives ---------------------------------------------------- */

/*
 * What a callback saw of each event. An event lasts only as long as its
 * callback, so the checks run there and only their outcome is kept.
 */
struct record {
	char		tag[16];
	uint64_t	seq;
	unsigned	index;
	bool		task_ok;
	bool		type_ok;
	bool		source_ok;
	bool		trace_ok;
};

struct collector {
	pthread_mutex_t		lock;
	pthread_cond_t		changed;
	struct record		records[MAX_RECORDS];
	unsigned		count;
	const char		*source;	/* the sender's runtime */
	const char		*trace_id;	/* every event's trace */
	/* A gate the callback waits at while it is shut. */
	bool			gated;
	bool			gate_open;
};

static void collector_init(struct collector *c, const char *source,
			   const char *trace_id)
{
	memset(c, 0, sizeof(*c));
	pthread_mutex_init(&c->lock, NULL);
	pthread_cond_init(&c->changed, NULL);
	c->source = source;
	c->trace_id = trace_id;
}

static void collect(const defw2_event_t *event, void *arg)
{
	struct collector *c = arg;
	const defw2_qpm_task_t *task = defw2_qpm_event_task(event);
	struct record r;

	memset(&r, 0, sizeof(r));
	snprintf(r.tag, sizeof(r.tag), "%s", event->tag ? event->tag : "");
	r.seq = event->seq;
	r.index = 0;
	if (task != NULL && task->cid != NULL)
		sscanf(task->cid, "cid-%u", &r.index);
	r.task_ok = task_matches(task, r.index);
	r.type_ok = same(event->type, DEFW2_QPM_EVENT_COMPLETION);
	r.source_ok = same(event->source, c->source);
	/* The trace id is the second field of a traceparent. */
	r.trace_ok = event->traceparent != NULL &&
		     strlen(event->traceparent) == 55 &&
		     strncmp(event->traceparent + 3, c->trace_id, 32) == 0;

	pthread_mutex_lock(&c->lock);
	if (c->count < MAX_RECORDS)
		c->records[c->count] = r;
	c->count++;
	pthread_cond_broadcast(&c->changed);
	if (c->gated) {
		while (!c->gate_open)
			pthread_cond_wait(&c->changed, &c->lock);
	}
	pthread_mutex_unlock(&c->lock);
}

static bool wait_count(struct collector *c, unsigned n, uint32_t ms)
{
	uint64_t deadline = now_ms() + ms;
	bool reached;

	pthread_mutex_lock(&c->lock);
	while (c->count < n && now_ms() < deadline) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 10 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&c->changed, &c->lock, &ts);
	}
	reached = c->count >= n;
	pthread_mutex_unlock(&c->lock);
	return reached;
}

static unsigned count_of(struct collector *c)
{
	unsigned n;

	pthread_mutex_lock(&c->lock);
	n = c->count;
	pthread_mutex_unlock(&c->lock);
	return n;
}

/* Wait until a publisher has an answer for at least n deliveries. */
static bool wait_settled(defw2_event_publisher_t *pub, uint64_t n,
			 uint32_t ms)
{
	uint64_t deadline = now_ms() + ms;
	defw2_event_publisher_stats_t s;

	do {
		defw2_event_publisher_stats(pub, &s);
		if (s.delivered + s.refused + s.failed >= n)
			return true;
		usleep(1000);
	} while (now_ms() < deadline);
	return false;
}

/*
 * Wait until nothing is queued or on its way. A sink has an event before
 * its sender has counted the answer, so a phase that compares counts
 * starts from here.
 */
static bool quiet(defw2_event_publisher_t *pub)
{
	uint64_t deadline = now_ms() + 5000;
	defw2_event_publisher_stats_t s;

	do {
		defw2_event_publisher_stats(pub, &s);
		if (s.waiting == 0)
			return true;
		usleep(1000);
	} while (now_ms() < deadline);
	return false;
}

static bool wait_failed(defw2_event_publisher_t *pub, uint64_t n,
			uint32_t ms)
{
	uint64_t deadline = now_ms() + ms;
	defw2_event_publisher_stats_t s;

	do {
		defw2_event_publisher_stats(pub, &s);
		if (s.failed >= n)
			return true;
		usleep(1000);
	} while (now_ms() < deadline);
	return false;
}

/* --- the parts ------------------------------------------------------- */

#define TRACE_ID	"0af7651916cd43dd8448eb211c80319c"
#define TRACEPARENT	"00-" TRACE_ID "-b7ad6b7169203331-01"

static defw2_rc_t send_task(defw2_event_publisher_t *pub,
			    const defw2_event_target_t *target, unsigned i)
{
	struct fixture f;

	fixture(&f, i);
	return defw2_qpm_publish_completion(pub, target,
					    DEFW2_QPM_EVENT_COMPLETION,
					    &f.task, TRACEPARENT);
}

/* Every record for tag arrived in order, from 1, and checked out. */
static bool records_good(struct collector *c, const char *tag,
			 unsigned expected)
{
	unsigned i, seen = 0;
	uint64_t seq = 0;
	bool good = true;

	pthread_mutex_lock(&c->lock);
	for (i = 0; i < c->count && i < MAX_RECORDS; i++) {
		struct record *r = &c->records[i];

		if (strcmp(r->tag, tag) != 0)
			continue;
		seen++;
		good = good && r->seq == ++seq && r->task_ok && r->type_ok &&
		       r->source_ok && r->trace_ok;
	}
	pthread_mutex_unlock(&c->lock);
	return good && seen == expected;
}

static void creation(defw2_rt_t *receiver, defw2_rt_t *sender)
{
	defw2_event_sink_t *sink = NULL;

	check("a sink needs a runtime that listens",
	      defw2_event_sink_create(sender, DEFW2_PROVIDER_EVENT, NULL,
				      &sink) == DEFW2_ERR_CONFIG &&
	      sink == NULL);
	check("and may not take the directory's provider",
	      defw2_event_sink_create(receiver, 0, NULL, &sink) ==
	      DEFW2_ERR_INVALID);
}

static void round_trip(defw2_event_publisher_t *pub,
		       const defw2_event_target_t *to_a,
		       struct collector *a)
{
	defw2_event_target_t x = *to_a, y = *to_a;
	defw2_event_publisher_stats_t s;
	bool sent = true, settled;
	unsigned i;

	for (i = 0; i < 100; i++)
		sent = sent && send_task(pub, to_a, i) == DEFW2_OK;
	check("a hundred completions are published", sent);
	check("and a callback sees all of them", wait_count(a, 100, 10000));
	check("in order, each one whole", records_good(a, to_a->tag, 100));
	/* The publisher counts an answer after the sink has queued the
	 * event, so its count can trail the callback's for a moment. */
	settled = wait_settled(pub, 100, 5000);
	defw2_event_publisher_stats(pub, &s);
	check("the publisher counts each one delivered",
	      settled && s.published == 100 && s.delivered == 100 &&
	      s.failed == 0 && s.waiting == 0);

	/* One sink, two registrations: each has its own order. */
	x.tag = "x";
	y.tag = "y";
	for (i = 0; i < 20; i++)
		sent = sent && send_task(pub, i % 2 ? &x : &y, i) == DEFW2_OK;
	check("two tags on one sink are told apart",
	      sent && wait_count(a, 120, 10000) && records_good(a, "x", 10) &&
	      records_good(a, "y", 10));
}

static void pulling(defw2_rt_t *receiver, defw2_event_publisher_t *pub)
{
	defw2_event_target_t target = { NULL, DEFW2_PROVIDER_EVENT + 1, "b" };
	defw2_event_sink_t *b = NULL, *again = NULL;
	defw2_event_t event = { 0 };
	const defw2_qpm_task_t *task;
	bool good = true;
	unsigned i;
	defw2_rc_t rc;

	check("a sink without a callback is served",
	      defw2_event_sink_create(receiver, target.provider_id, NULL,
				      &b) == DEFW2_OK &&
	      defw2_qpm_event_accept(b) == DEFW2_OK);
	check("and a second one on its provider is refused",
	      defw2_event_sink_create(receiver, target.provider_id, NULL,
				      &again) == DEFW2_ERR_BUSY);
	target.address = defw2_event_sink_address(b);
	for (i = 0; i < 3; i++)
		good = good && send_task(pub, &target, 500 + i) == DEFW2_OK;
	for (i = 0; i < 3; i++) {
		rc = defw2_event_sink_next(b, 5000, &event);
		task = defw2_qpm_event_task(&event);
		good = good && rc == DEFW2_OK && event.seq == i + 1 &&
		       task_matches(task, 500 + i) && same(event.tag, "b");
		defw2_event_free(&event);
	}
	check("its owner takes the events in order", good);
	check("and is told when nothing more arrives",
	      defw2_event_sink_next(b, 100, &event) == DEFW2_ERR_TIMEOUT);

	/*
	 * A destroyed sink is gone to its senders. It refuses the next event,
	 * and the publish after that says the target is gone.
	 */
	defw2_event_sink_destroy(b);
	check("a destroyed sink says so to its reader",
	      defw2_event_sink_next(b, 100, &event) == DEFW2_ERR_NOT_FOUND);
	rc = send_task(pub, &target, 600);
	check("an event to a destroyed sink still queues", rc == DEFW2_OK);
	check("and its delivery fails", wait_failed(pub, 1, 5000));
	rc = send_task(pub, &target, 601);
	check("so the next publish says the target is gone",
	      rc == DEFW2_ERR_NOT_FOUND && defw2_event_target_gone(rc));

	/* The same provider serves again, and the target is tried afresh. */
	check("a sink can be created again on its provider",
	      defw2_event_sink_create(receiver, target.provider_id, NULL,
				      &b) == DEFW2_OK &&
	      defw2_qpm_event_accept(b) == DEFW2_OK);
	rc = send_task(pub, &target, 602);
	good = rc == DEFW2_OK &&
	       defw2_event_sink_next(b, 5000, &event) == DEFW2_OK &&
	       task_matches(defw2_qpm_event_task(&event), 602);
	defw2_event_free(&event);
	check("and its events arrive again", good);
	defw2_event_sink_destroy(b);
}

static void refusals(defw2_rt_t *receiver, defw2_event_publisher_t *pub,
		     const defw2_event_target_t *to_a, struct collector *a)
{
	defw2_event_target_t target = { NULL, DEFW2_PROVIDER_EVENT + 2, "c" };
	defw2_event_publisher_stats_t before, after;
	defw2_event_sink_t *c = NULL;
	struct fixture f;
	unsigned count;
	char *big;
	defw2_rc_t rc;

	/* An extra too long for the wire is refused, and only it. */
	big = malloc(DEFW2_EAGER_MAX + 1);
	memset(big, 'x', DEFW2_EAGER_MAX);
	big[DEFW2_EAGER_MAX] = '\0';
	fixture(&f, 0);
	f.task.extra = big;
	check("an event with a field too long for the wire is refused",
	      defw2_qpm_publish_completion(pub, to_a, "completion", &f.task,
					   NULL) == DEFW2_ERR_INVALID);

	/* A large one that fits travels whole. */
	memset(big, 'e', BIG_EXTRA);
	big[BIG_EXTRA] = '\0';
	check("the publisher has nothing on its way", quiet(pub));
	count = count_of(a);
	defw2_event_publisher_stats(pub, &before);
	rc = defw2_qpm_publish_completion(pub, to_a, "completion", &f.task,
					  NULL);
	check("while a 200 KiB extra is delivered",
	      rc == DEFW2_OK && wait_settled(pub, before.delivered +
					     before.refused + before.failed +
					     1, 5000));
	defw2_event_publisher_stats(pub, &after);
	check("to the same target", after.delivered == before.delivered + 1 &&
	      wait_count(a, count + 1, 5000));
	free(big);

	/* A sink that does not take QPM events. */
	check("a sink that accepts nothing is served",
	      defw2_event_sink_create(receiver, target.provider_id, NULL,
				      &c) == DEFW2_OK);
	target.address = defw2_event_sink_address(c);
	defw2_event_publisher_stats(pub, &before);
	rc = send_task(pub, &target, 700);
	check("an event it does not take is published",
	      rc == DEFW2_OK && wait_failed(pub, before.failed + 1, 5000));
	rc = send_task(pub, &target, 701);
	check("and then reported as gone",
	      rc == DEFW2_ERR_NOT_FOUND && defw2_event_target_gone(rc));
	defw2_event_sink_destroy(c);
}

/*
 * A full sink is backpressure: it turns events away, and its sender counts
 * them lost, but nothing about the sink is gone.
 */
static void backpressure(defw2_rt_t *receiver, defw2_event_publisher_t *pub,
			 const char *source)
{
	defw2_event_sink_opts_t opts = { 0 };
	defw2_event_target_t target = { NULL, DEFW2_PROVIDER_EVENT + 3, "d" };
	defw2_event_publisher_stats_t before, after;
	defw2_event_sink_stats_t sink_stats;
	defw2_event_sink_t *d = NULL;
	struct collector gate;
	uint64_t accepted;
	bool sent = true;
	unsigned i;

	collector_init(&gate, source, TRACE_ID);
	gate.gated = true;
	opts.callback = collect;
	opts.arg = &gate;
	opts.depth = 4;
	check("a sink four deep is served",
	      defw2_event_sink_create(receiver, target.provider_id, &opts,
				      &d) == DEFW2_OK &&
	      defw2_qpm_event_accept(d) == DEFW2_OK);
	target.address = defw2_event_sink_address(d);

	check("the publisher has nothing on its way", quiet(pub));
	defw2_event_publisher_stats(pub, &before);
	/*
	 * The first event alone, until the stuck callback holds it. Until the
	 * sink's thread has handed it over it takes a place in the queue, so
	 * the rest would find four places or five depending on that race, and
	 * one freed later would let in an event the test expects refused.
	 */
	check("its callback is stuck with the first event",
	      send_task(pub, &target, 800) == DEFW2_OK &&
	      wait_count(&gate, 1, 5000));
	for (i = 1; i < 20; i++)
		sent = sent && send_task(pub, &target, 800 + i) == DEFW2_OK;
	check("twenty events are published to it", sent);
	check("and each is answered",
	      wait_settled(pub, before.delivered + before.refused +
			   before.failed + 20, 5000));
	defw2_event_publisher_stats(pub, &after);
	accepted = after.delivered - before.delivered;
	/* One with the callback and four waiting. */
	check("a full sink turns the rest away",
	      accepted == 5 &&
	      after.refused - before.refused == 20 - accepted &&
	      after.failed == before.failed);
	defw2_event_sink_stats(d, &sink_stats);
	check("and counts them as the sender does",
	      sink_stats.refused == 20 - accepted &&
	      sink_stats.received == accepted);
	/* Still full, so the next is turned away too, and that is all. */
	check("but is not gone",
	      send_task(pub, &target, 900) == DEFW2_OK &&
	      wait_settled(pub, before.delivered + before.refused +
			   before.failed + 21, 5000));
	defw2_event_publisher_stats(pub, &after);
	check("and stays full while its callback is stuck",
	      after.refused - before.refused == 21 - accepted &&
	      after.failed == before.failed);

	pthread_mutex_lock(&gate.lock);
	gate.gate_open = true;
	pthread_cond_broadcast(&gate.changed);
	pthread_mutex_unlock(&gate.lock);
	check("its callback gets what it queued once it moves",
	      wait_count(&gate, (unsigned)accepted, 5000));
	defw2_event_sink_destroy(d);
	check("and no more", count_of(&gate) == accepted);
	pthread_cond_destroy(&gate.changed);
	pthread_mutex_destroy(&gate.lock);
}

/*
 * openQSE/QFw issue 64. A sink in a stopped process holds its sender up
 * for one time limit, and nobody else for any of it.
 */
static void stalled(defw2_rt_t *sender, const defw2_event_target_t *to_a,
		    struct collector *a, const char *child_address,
		    pid_t child)
{
	defw2_event_publisher_opts_t opts = { 0 };
	defw2_event_publisher_opts_t patient = { .timeout_ms = PATIENT_MS };
	defw2_event_target_t slow = { child_address, DEFW2_PROVIDER_EVENT,
				      "stalled" };
	defw2_event_target_t fast = *to_a;
	defw2_event_publisher_t *pub = NULL, *first = NULL;
	defw2_event_publisher_stats_t s;
	uint64_t start, waited, longest = 0, t0;
	unsigned i, queued = 0, count;
	bool sent = true;
	int status = 0;
	defw2_rc_t rc;

	/*
	 * The first event to a process that has just started can be slow, so
	 * it goes through a publisher with a limit to match. Only the stall
	 * itself is held to the short one.
	 */
	if (defw2_event_publisher_create(sender, &patient, &first) !=
	    DEFW2_OK) {
		check("a publisher starts", false);
		return;
	}
	check("the other process's sink answers before it is stopped",
	      send_task(first, &slow, 0) == DEFW2_OK &&
	      wait_settled(first, 1, PATIENT_MS * 2));
	defw2_event_publisher_stats(first, &s);
	check("and takes the event", s.delivered == 1);
	defw2_event_publisher_destroy(first);

	opts.timeout_ms = STALL_LIMIT_MS;
	opts.depth = TARGET_DEPTH;
	if (defw2_event_publisher_create(sender, &opts, &pub) != DEFW2_OK) {
		check("a publisher with a short time limit starts", false);
		return;
	}
	fast.tag = "fast";

	/* Stopped for certain before anything is sent, since a signal takes
	 * effect only when the kernel next schedules the process. */
	kill(child, SIGSTOP);
	check("the other process stops",
	      waitpid(child, &status, WUNTRACED) == child &&
	      WIFSTOPPED(status));
	start = now_ms();
	/* One on its way, then the queue fills, then one is refused at
	 * once rather than waited for. */
	rc = send_task(pub, &slow, 1);
	while (rc == DEFW2_OK && queued < 2 * TARGET_DEPTH) {
		queued++;
		rc = send_task(pub, &slow, 1 + queued);
	}
	check("a stalled target's queue fills, then refuses at once",
	      rc == DEFW2_ERR_BUSY &&
	      (queued == TARGET_DEPTH || queued == TARGET_DEPTH + 1));

	count = count_of(a);
	for (i = 0; i < FAST_EVENTS; i++) {
		t0 = now_us();
		sent = sent && send_task(pub, &fast, i) == DEFW2_OK;
		if (now_us() - t0 > longest)
			longest = now_us() - t0;
	}
	check("fifty events reach a healthy sink meanwhile",
	      sent && wait_count(a, count + FAST_EVENTS, 5000) &&
	      records_good(a, "fast", FAST_EVENTS));
	waited = now_ms() - start;
	printf("    healthy sink served in %llu ms, longest publish %llu us\n",
	       (unsigned long long)waited, (unsigned long long)longest);
	check("before the stalled delivery reaches its time limit",
	      waited < STALL_LIMIT_MS * 9 / 10);
	check("and no publish waited on the network", longest < 50000);

	check("the stalled delivery fails at its time limit",
	      wait_failed(pub, 1, STALL_LIMIT_MS * 6));
	waited = now_ms() - start;
	printf("    stalled delivery failed after %llu ms\n",
	       (unsigned long long)waited);
	check("not before it",
	      waited >= STALL_LIMIT_MS * 9 / 10 &&
	      waited < STALL_LIMIT_MS * 5);
	defw2_event_publisher_stats(pub, &s);
	check("and the queue behind it is dropped, not tried one by one",
	      s.failed == 1 && s.dropped >= queued && s.waiting == 0);
	rc = send_task(pub, &slow, 50);
	check("the next publish says the target is gone",
	      rc == DEFW2_ERR_TIMEOUT && defw2_event_target_gone(rc));
	check("and the one after is tried afresh",
	      send_task(pub, &slow, 51) == DEFW2_OK);

	t0 = now_ms();
	defw2_event_publisher_destroy(pub);
	waited = now_ms() - t0;
	printf("    publisher closed in %llu ms\n", (unsigned long long)waited);
	check("closing waits for that delivery, and no longer",
	      waited < STALL_LIMIT_MS * 3);
	kill(child, SIGCONT);
}

/* A sink whose process has gone is reported gone too. */
static void departed(defw2_rt_t *sender, const char *child_address,
		     FILE *child_in, pid_t child)
{
	defw2_event_publisher_opts_t opts = { .timeout_ms = STALL_LIMIT_MS };
	defw2_event_target_t gone = { child_address, DEFW2_PROVIDER_EVENT,
				      "departed" };
	defw2_event_publisher_t *pub = NULL;
	int status = 0;
	defw2_rc_t rc;

	fclose(child_in);
	check("the other process stops when told to",
	      waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	      WEXITSTATUS(status) == 0);
	if (defw2_event_publisher_create(sender, &opts, &pub) != DEFW2_OK) {
		check("a publisher starts", false);
		return;
	}
	rc = send_task(pub, &gone, 0);
	check("an event to a process that has gone fails",
	      rc == DEFW2_OK && wait_failed(pub, 1, STALL_LIMIT_MS * 6));
	rc = send_task(pub, &gone, 1);
	check("and the target is reported gone", defw2_event_target_gone(rc));
	defw2_event_publisher_destroy(pub);
}

/* --- the other process ----------------------------------------------- */

static int serve_sink(void)
{
	defw2_event_sink_t *sink = NULL;
	defw2_rt_t *rt = NULL;
	defw2_config_t cfg;
	char discard[256];

	memset(&cfg, 0, sizeof(cfg));
	cfg.address = "na+sm://";
	cfg.node_name = "stalled-sink";
	cfg.role = DEFW2_ROLE_SERVER;
	cfg.log_level = DEFW2_LOG_ERROR;
	cfg.rpc_thread_count = 1;
	if (defw2_init(&cfg, &rt) != DEFW2_OK ||
	    defw2_event_sink_create(rt, DEFW2_PROVIDER_EVENT, NULL, &sink) !=
	    DEFW2_OK || defw2_qpm_event_accept(sink) != DEFW2_OK) {
		fprintf(stderr, "the other process cannot serve a sink\n");
		return EXIT_FAILURE;
	}
	printf("%s\n", defw2_event_sink_address(sink));
	fflush(stdout);
	while (fread(discard, 1, sizeof(discard), stdin) > 0)
		;
	defw2_event_sink_destroy(sink);
	defw2_finalize(rt);
	return EXIT_SUCCESS;
}

/*
 * Start this program again as the other process. Done before either
 * runtime here starts, so the fork copies no Margo state.
 */
static pid_t spawn_sink(FILE **in, char *address, size_t len)
{
	int down[2], up[2];
	FILE *out;
	pid_t pid;

	if (pipe(down) != 0 || pipe(up) != 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		dup2(down[0], STDIN_FILENO);
		dup2(up[1], STDOUT_FILENO);
		close(down[0]);
		close(down[1]);
		close(up[0]);
		close(up[1]);
		execl("/proc/self/exe", "defw2_event_smoke", "--sink",
		      (char *)NULL);
		_exit(127);
	}
	close(down[0]);
	close(up[1]);
	*in = fdopen(down[1], "w");
	out = fdopen(up[0], "r");
	if (*in == NULL || out == NULL || fgets(address, (int)len, out) == NULL)
		return -1;
	address[strcspn(address, "\n")] = '\0';
	fclose(out);
	return pid;
}

int main(int argc, char **argv)
{
	defw2_event_target_t to_a = { NULL, DEFW2_PROVIDER_EVENT, "a" };
	defw2_event_publisher_opts_t patient = { .timeout_ms = PATIENT_MS };
	defw2_event_sink_opts_t a_opts = { 0 };
	defw2_rt_t *receiver = NULL, *sender = NULL;
	defw2_event_publisher_t *pub = NULL, *lingering = NULL;
	defw2_event_sink_t *a = NULL, *left_open = NULL;
	defw2_config_t receiver_cfg, sender_cfg;
	const char *telemetry = NULL;
	char child_address[256];
	struct collector seen;
	FILE *child_in = NULL;
	defw2_event_t other;
	pid_t child;

	if (argc > 1 && strcmp(argv[1], "--sink") == 0)
		return serve_sink();
	if (argc > 1)
		telemetry = argv[1];
	if (telemetry != NULL)
		setenv("DEFW2_TELEMETRY_DIR", telemetry, 1);

	child = spawn_sink(&child_in, child_address, sizeof(child_address));
	check("another process serves a sink", child > 0);
	if (child <= 0)
		return EXIT_FAILURE;

	memset(&receiver_cfg, 0, sizeof(receiver_cfg));
	receiver_cfg.address = "na+sm://";
	receiver_cfg.node_name = "event-receiver";
	receiver_cfg.role = DEFW2_ROLE_SERVER;
	receiver_cfg.log_level = DEFW2_LOG_ERROR;
	receiver_cfg.rpc_thread_count = 2;
	receiver_cfg.profile = telemetry != NULL;
	/* A publisher needs no listener, so the sender is a plain client. */
	memset(&sender_cfg, 0, sizeof(sender_cfg));
	sender_cfg.address = "na+sm://";
	sender_cfg.node_name = "event-sender";
	sender_cfg.role = DEFW2_ROLE_CLIENT;
	sender_cfg.log_level = DEFW2_LOG_ERROR;
	sender_cfg.profile = telemetry != NULL;
	if (defw2_init(&receiver_cfg, &receiver) != DEFW2_OK ||
	    defw2_init(&sender_cfg, &sender) != DEFW2_OK) {
		fprintf(stderr, "cannot start the runtimes\n");
		return EXIT_FAILURE;
	}

	creation(receiver, sender);

	collector_init(&seen, defw2_runtime_id(sender), TRACE_ID);
	a_opts.callback = collect;
	a_opts.arg = &seen;
	check("a sink with a callback is served",
	      defw2_event_sink_create(receiver, DEFW2_PROVIDER_EVENT,
				      &a_opts, &a) == DEFW2_OK &&
	      defw2_qpm_event_accept(a) == DEFW2_OK &&
	      defw2_qpm_event_accept(a) == DEFW2_OK);
	to_a.address = defw2_event_sink_address(a);
	check("and a publisher starts",
	      defw2_event_publisher_create(sender, &patient, &pub) ==
	      DEFW2_OK);

	memset(&other, 0, sizeof(other));
	other.api = "qfw.something.else";
	other.name = DEFW2_QPM_EVENT_COMPLETION;
	check("another API's event is not taken for a completion",
	      defw2_qpm_event_task(&other) == NULL);

	if (failures == 0) {
		round_trip(pub, &to_a, &seen);
		pulling(receiver, pub);
		refusals(receiver, pub, &to_a, &seen);
		backpressure(receiver, pub, defw2_runtime_id(sender));
		stalled(sender, &to_a, &seen, child_address, child);
	}
	departed(sender, child_address, child_in, child);
	defw2_event_publisher_destroy(pub);

	/*
	 * Left open on purpose: stopping the runtimes closes a sink and a
	 * publisher, the publisher with an event still on its way, and the
	 * publisher can be destroyed afterwards.
	 */
	check("a sink and a publisher left open start",
	      defw2_event_sink_create(receiver, DEFW2_PROVIDER_EVENT + 4, NULL,
				      &left_open) == DEFW2_OK &&
	      defw2_qpm_event_accept(left_open) == DEFW2_OK &&
	      defw2_event_publisher_create(sender, NULL, &lingering) ==
	      DEFW2_OK);
	to_a.tag = "lingering";
	check("and an event is on its way as the runtimes stop",
	      send_task(lingering, &to_a, 0) == DEFW2_OK);
	defw2_finalize(sender);
	defw2_event_publisher_destroy(lingering);
	defw2_finalize(receiver);
	check("both runtimes stop with them open", true);

	pthread_cond_destroy(&seen.changed);
	pthread_mutex_destroy(&seen.lock);
	printf("\n%s\n", failures == 0 ? "event smoke passed"
				       : "event smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
