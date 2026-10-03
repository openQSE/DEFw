/*
 * Publishers: how a service sends events.
 *
 * Publishing copies the event onto its target's queue and returns, so the
 * service's own thread never waits on the network. Delivery happens on an
 * execution stream the publisher adds to Margo for itself. Each target with
 * something queued has one ULT there, which sends that target's events one
 * at a time, in order, each within the time limit. A ULT waiting on one
 * target's answer leaves the stream to the others, so a target that has
 * stopped answering costs its own events and nobody else's.
 *
 * A delivery that fails drops whatever else waits for that target, and the
 * next publish to it reports the failure, so the service drops the
 * registration as v1 dropped one whose put failed. A target nobody
 * publishes to for a while is forgotten, along with its address.
 */
#include <stdlib.h>
#include <string.h>

#include <margo-config.h>

#include "defw2_event_internal.h"
#include "defw2_trace.h"

struct pub_event {
	struct pub_event		*next;
	const struct defw2_event_kind	*kind;
	struct defw2_arena		arena;
	const char			*type;
	const char			*traceparent;
	void				*wire;
	uint64_t			seq;
	uint64_t			bytes;
};

struct pub_target {
	struct pub_target		*next;
	struct defw2_event_publisher	*pub;
	char				*address;
	uint16_t			provider_id;
	char				*tag;
	defw2_binding_t			*binding;	/* on first delivery */
	struct pub_event		*head;
	struct pub_event		*tail;
	unsigned			length;
	uint64_t			seq;
	bool				running;	/* a ULT owns it */
	defw2_rc_t			failed;		/* unreported failure */
	uint64_t			idle_since_ns;
};

struct defw2_event_publisher {
	struct defw2_rt			*rt;
	char				name[64];	/* of pool and stream */
	ABT_pool			pool;
	pthread_mutex_t			lock;
	pthread_cond_t			idle;		/* a ULT has ended */
	struct pub_target		*targets;
	unsigned			ntargets;
	unsigned			running;	/* delivery ULTs */
	bool				closed;
	bool				stopped;	/* by the runtime */
	uint32_t			timeout_ms;
	unsigned			depth;
	uint64_t			max_bytes;
	uint64_t			bytes;
	defw2_event_publisher_stats_t	stats;
};

static void event_free(struct pub_event *e)
{
	defw2_arena_free(&e->arena);
	free(e);
}

static bool fits(const char *s, size_t max)
{
	return s == NULL || strnlen(s, max) < max;
}

static bool same_tag(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a, b) == 0;
}

/* Drop what waits for a target, counting it. The caller holds the lock. */
static void drop_queue_locked(struct defw2_event_publisher *pub,
			      struct pub_target *t)
{
	struct pub_event *e, *next;

	for (e = t->head; e != NULL; e = next) {
		next = e->next;
		pub->bytes -= e->bytes;
		pub->stats.waiting--;
		pub->stats.dropped++;
		event_free(e);
	}
	t->head = NULL;
	t->tail = NULL;
	t->length = 0;
}

static void target_free(struct pub_target *t)
{
	defw2_binding_free(t->binding);
	free(t->address);
	free(t->tag);
	free(t);
}

/*
 * Forget targets that have had nothing for a while, and their addresses. A
 * failure nobody came back for goes too. The caller holds the lock.
 */
static void sweep_locked(struct defw2_event_publisher *pub, uint64_t now)
{
	struct pub_target **at = &pub->targets;
	struct pub_target *t;

	while ((t = *at) != NULL) {
		if (!t->running && t->head == NULL &&
		    now - t->idle_since_ns > DEFW2_EVENT_IDLE_MS * 1000000ull) {
			*at = t->next;
			pub->ntargets--;
			target_free(t);
			continue;
		}
		at = &t->next;
	}
}

static struct pub_target *find_locked(struct defw2_event_publisher *pub,
				      const defw2_event_target_t *target)
{
	struct pub_target *t;

	for (t = pub->targets; t != NULL; t = t->next)
		if (t->provider_id == target->provider_id &&
		    strcmp(t->address, target->address) == 0 &&
		    same_tag(t->tag, target->tag))
			return t;
	return NULL;
}

static struct pub_target *target_new(struct defw2_event_publisher *pub,
				     const defw2_event_target_t *target)
{
	struct pub_target *t = calloc(1, sizeof(*t));

	if (t == NULL)
		return NULL;
	t->pub = pub;
	t->provider_id = target->provider_id;
	t->address = strdup(target->address);
	t->tag = target->tag != NULL ? strdup(target->tag) : NULL;
	if (t->address == NULL || (target->tag != NULL && t->tag == NULL)) {
		target_free(t);
		return NULL;
	}
	return t;
}

/* --- delivering ------------------------------------------------------ */

/*
 * Send one event and read the sink's answer. Returns DEFW2_OK when the sink
 * queued it, DEFW2_ERR_BUSY when the sink was full, and otherwise why the
 * target is gone.
 */
static defw2_rc_t deliver_one(struct defw2_event_publisher *pub,
			      struct pub_target *t, struct pub_event *e)
{
	struct defw2_typed_call call;
	defw2_status_t status = { 0 };
	defw2_call_opts_t opts;
	defw2_event_out_t out;
	defw2_event_in_t in;
	defw2_rc_t rc;

	if (t->binding == NULL &&
	    defw2_binding_create(pub->rt, t->address, t->provider_id,
				 &t->binding) != DEFW2_OK)
		return DEFW2_ERR_TRANSPORT;

	memset(&in, 0, sizeof(in));
	in.tag = t->tag;
	in.type = e->type;
	in.seq = e->seq;
	in.api = e->kind->method.api;
	in.name = e->kind->method.name;
	in.kind = e->kind;
	in.payload = e->wire;

	memset(&opts, 0, sizeof(opts));
	opts.timeout_ms = pub->timeout_ms;
	opts.traceparent = e->traceparent;

	memset(&call, 0, sizeof(call));
	call.binding = t->binding;
	call.method = &e->kind->method;
	call.opts = &opts;
	call.in = &in;
	call.out = &out;
	call.out_size = sizeof(out);

	rc = defw2_typed_call(&call, &status);
	if (rc == DEFW2_OK) {
		switch (status.code) {
		case DEFW2_OK:
			break;
		case DEFW2_ERR_BUSY:
		case DEFW2_ERR_NOMEM:
			rc = DEFW2_ERR_BUSY;
			break;
		case DEFW2_ERR_VERSION:
			rc = DEFW2_ERR_VERSION;
			break;
		default:
			/* No sink, a closed one, or one that does not take
			 * this kind: all mean nobody there wants it. */
			rc = DEFW2_ERR_NOT_FOUND;
			break;
		}
	}
	if (rc != DEFW2_OK && rc != DEFW2_ERR_BUSY)
		defw2_log(pub->rt, DEFW2_LOG_MESSAGE,
			  "%s %s events to %s provider %u tag %s stopped: %s",
			  e->kind->method.api, e->kind->method.name,
			  t->address, t->provider_id,
			  t->tag != NULL ? t->tag : "(none)",
			  status.message != NULL ? status.message
						 : defw2_strerror(rc));
	defw2_status_free(&status);
	return rc;
}

/*
 * One per target with something queued. It sends until the queue is empty,
 * then leaves, so an idle target costs no ULT.
 */
static void deliver_ult(void *arg)
{
	struct pub_target *t = arg;
	struct defw2_event_publisher *pub = t->pub;
	struct pub_event *e;
	defw2_rc_t rc;

	for (;;) {
		pthread_mutex_lock(&pub->lock);
		e = t->head;
		if (e == NULL || pub->closed) {
			drop_queue_locked(pub, t);
			t->running = false;
			t->idle_since_ns = defw2_mono_ns();
			if (--pub->running == 0)
				pthread_cond_broadcast(&pub->idle);
			pthread_mutex_unlock(&pub->lock);
			return;
		}
		t->head = e->next;
		if (t->head == NULL)
			t->tail = NULL;
		t->length--;
		pthread_mutex_unlock(&pub->lock);

		rc = deliver_one(pub, t, e);

		pthread_mutex_lock(&pub->lock);
		pub->bytes -= e->bytes;
		pub->stats.waiting--;
		if (rc == DEFW2_OK) {
			pub->stats.delivered++;
		} else if (rc == DEFW2_ERR_BUSY) {
			pub->stats.refused++;
		} else {
			pub->stats.failed++;
			t->failed = rc;
			drop_queue_locked(pub, t);
		}
		pthread_mutex_unlock(&pub->lock);
		event_free(e);
	}
}

/* --- publishing ------------------------------------------------------ */

static struct pub_event *event_new(const struct defw2_event_kind *kind,
				   const char *type, const void *payload,
				   const char *traceparent, defw2_rc_t *rc)
{
	struct pub_event *e = calloc(1, sizeof(*e));

	*rc = DEFW2_ERR_NOMEM;
	if (e == NULL)
		return NULL;
	e->kind = kind;
	*rc = kind->make(&e->arena, payload, &e->wire, &e->bytes);
	if (*rc != DEFW2_OK) {
		event_free(e);
		return NULL;
	}
	if (traceparent != NULL && traceparent[0] == '\0')
		traceparent = NULL;
	e->type = defw2_arena_strdup(&e->arena, type);
	e->traceparent = defw2_arena_strdup(&e->arena, traceparent);
	if ((type != NULL && e->type == NULL) ||
	    (traceparent != NULL && e->traceparent == NULL)) {
		*rc = DEFW2_ERR_NOMEM;
		event_free(e);
		return NULL;
	}
	e->bytes += sizeof(*e) + (type != NULL ? strlen(type) : 0) +
		    (traceparent != NULL ? strlen(traceparent) : 0);
	return e;
}

defw2_rc_t defw2_event_publish(defw2_event_publisher_t *pub,
			       const defw2_event_target_t *target,
			       const struct defw2_event_kind *kind,
			       const char *type, const void *payload,
			       const char *traceparent)
{
	struct pub_target *t;
	struct pub_event *e;
	defw2_rc_t rc;
	uint64_t now;

	if (pub == NULL || target == NULL || target->address == NULL ||
	    kind == NULL || payload == NULL)
		return DEFW2_ERR_INVALID;
	if (!fits(target->address, DEFW2_NAME_MAX) ||
	    !fits(target->tag, DEFW2_STR_MAX) || !fits(type, DEFW2_STR_MAX) ||
	    !fits(traceparent, DEFW2_STR_MAX))
		return DEFW2_ERR_INVALID;

	/* Copied before the lock, so a large event costs only its caller. */
	e = event_new(kind, type, payload, traceparent, &rc);
	if (e == NULL)
		return rc;

	pthread_mutex_lock(&pub->lock);
	if (pub->closed ||
	    __atomic_load_n(&pub->rt->stopping, __ATOMIC_ACQUIRE) != 0) {
		rc = DEFW2_ERR_CANCELLED;
		goto refuse;
	}
	now = defw2_mono_ns();
	sweep_locked(pub, now);
	t = find_locked(pub, target);
	if (t == NULL) {
		t = target_new(pub, target);
		if (t == NULL) {
			rc = DEFW2_ERR_NOMEM;
			goto refuse;
		}
		t->idle_since_ns = now;
		t->next = pub->targets;
		pub->targets = t;
		pub->ntargets++;
	}
	if (t->failed != DEFW2_OK) {
		/* Reported once. The service drops the registration, and the
		 * next event to this target is tried afresh. */
		rc = t->failed;
		t->failed = DEFW2_OK;
		t->idle_since_ns = now;
		goto refuse;
	}
	if (t->length >= pub->depth ||
	    pub->bytes + e->bytes > pub->max_bytes) {
		rc = DEFW2_ERR_BUSY;
		goto refuse;
	}

	e->seq = ++t->seq;
	if (t->tail == NULL)
		t->head = e;
	else
		t->tail->next = e;
	t->tail = e;
	t->length++;
	pub->bytes += e->bytes;
	pub->stats.waiting++;
	pub->stats.published++;

	/*
	 * A target with no ULT has an empty queue, since a ULT leaves only
	 * once its queue is, so this event is the only one waiting.
	 */
	if (!t->running) {
		t->running = true;
		pub->running++;
		if (ABT_thread_create(pub->pool, deliver_ult, t,
				      ABT_THREAD_ATTR_NULL, NULL) !=
		    ABT_SUCCESS) {
			t->running = false;
			pub->running--;
			t->head = NULL;
			t->tail = NULL;
			t->length = 0;
			t->seq--;
			pub->bytes -= e->bytes;
			pub->stats.waiting--;
			pub->stats.published--;
			defw2_log(pub->rt, DEFW2_LOG_ERROR,
				  "no ULT to deliver events to %s",
				  t->address);
			rc = DEFW2_ERR_INTERNAL;
			goto refuse;
		}
	}
	pthread_mutex_unlock(&pub->lock);
	return DEFW2_OK;

refuse:
	pub->stats.dropped++;
	pthread_mutex_unlock(&pub->lock);
	event_free(e);
	return rc;
}

/* --- the publisher's life -------------------------------------------- */

/*
 * Drop what waits, wait for what is on its way, and give the execution
 * stream back. Safe to call again.
 */
static void pub_close(struct defw2_event_publisher *pub)
{
	struct pub_target *t, *next;

	pthread_mutex_lock(&pub->lock);
	if (pub->closed) {
		pthread_mutex_unlock(&pub->lock);
		return;
	}
	pub->closed = true;
	for (t = pub->targets; t != NULL; t = t->next)
		drop_queue_locked(pub, t);
	/* Bounded by the time limit: each ULT ends its delivery and finds
	 * nothing left. */
	while (pub->running > 0)
		pthread_cond_wait(&pub->idle, &pub->lock);
	for (t = pub->targets; t != NULL; t = next) {
		next = t->next;
		target_free(t);
	}
	pub->targets = NULL;
	pub->ntargets = 0;
	pthread_mutex_unlock(&pub->lock);

	if (margo_remove_xstream_by_name(pub->rt->mid, pub->name) !=
	    HG_SUCCESS ||
	    margo_remove_pool_by_name(pub->rt->mid, pub->name) != HG_SUCCESS)
		defw2_log(pub->rt, DEFW2_LOG_WARNING,
			  "could not give back the stream %s", pub->name);
}

/* The runtime is stopping, so close while Margo can still deliver. */
static void pub_prefinalize(void *arg)
{
	struct defw2_event_publisher *pub = arg;

	pub_close(pub);
	pub->stopped = true;
}

defw2_rc_t defw2_event_publisher_create(defw2_rt_t *rt,
					const defw2_event_publisher_opts_t *opts,
					defw2_event_publisher_t **out)
{
	static unsigned serial;
	struct margo_xstream_info stream;
	struct margo_pool_info pool;
	struct defw2_event_publisher *pub;
	char json[256];

	if (rt == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	*out = NULL;
	if (__atomic_load_n(&rt->stopping, __ATOMIC_ACQUIRE) != 0)
		return DEFW2_ERR_CANCELLED;
	pub = calloc(1, sizeof(*pub));
	if (pub == NULL)
		return DEFW2_ERR_NOMEM;
	pthread_mutex_init(&pub->lock, NULL);
	pthread_cond_init(&pub->idle, NULL);
	pub->rt = rt;
	pub->timeout_ms = opts != NULL && opts->timeout_ms > 0 ?
				  opts->timeout_ms : DEFW2_EVENT_TIMEOUT_MS;
	pub->depth = opts != NULL && opts->depth > 0 ? opts->depth
						    : DEFW2_EVENT_TARGET_DEPTH;
	pub->max_bytes = opts != NULL && opts->max_bytes > 0 ?
				 opts->max_bytes : DEFW2_EVENT_PUBLISHER_BYTES;
	snprintf(pub->name, sizeof(pub->name), "defw2_events_%u",
		 __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));

	/*
	 * A pool and a stream of its own, so delivery never runs on the
	 * caller's thread, nor competes with the handlers for theirs.
	 */
	snprintf(json, sizeof(json),
		 "{\"name\":\"%s\",\"kind\":\"fifo_wait\",\"access\":\"mpmc\"}",
		 pub->name);
	if (margo_add_pool_from_json(rt->mid, json, &pool) != HG_SUCCESS)
		goto fail;
	pub->pool = pool.pool;
	snprintf(json, sizeof(json),
		 "{\"name\":\"%s\",\"scheduler\":{\"type\":\"basic_wait\","
		 "\"pools\":[\"%s\"]}}", pub->name, pub->name);
	if (margo_add_xstream_from_json(rt->mid, json, &stream) !=
	    HG_SUCCESS) {
		margo_remove_pool_by_name(rt->mid, pub->name);
		goto fail;
	}
	margo_provider_push_prefinalize_callback(rt->mid, pub,
						 pub_prefinalize, pub);

	defw2_log(rt, DEFW2_LOG_MESSAGE,
		  "event publisher %s, %u ms a delivery", pub->name,
		  pub->timeout_ms);
	*out = pub;
	return DEFW2_OK;

fail:
	defw2_log(rt, DEFW2_LOG_ERROR, "no execution stream for %s",
		  pub->name);
	pthread_cond_destroy(&pub->idle);
	pthread_mutex_destroy(&pub->lock);
	free(pub);
	return DEFW2_ERR_INTERNAL;
}

void defw2_event_publisher_destroy(defw2_event_publisher_t *pub)
{
	if (pub == NULL)
		return;
	/* Once the runtime has stopped it, the runtime may be gone too, so
	 * only the memory is left to release. */
	if (!pub->stopped) {
		margo_provider_pop_prefinalize_callback(pub->rt->mid, pub);
		pub_close(pub);
	}
	pthread_cond_destroy(&pub->idle);
	pthread_mutex_destroy(&pub->lock);
	free(pub);
}

void defw2_event_publisher_stats(defw2_event_publisher_t *pub,
				 defw2_event_publisher_stats_t *stats)
{
	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	if (pub == NULL)
		return;
	pthread_mutex_lock(&pub->lock);
	*stats = pub->stats;
	stats->targets = pub->ntargets;
	pthread_mutex_unlock(&pub->lock);
}
