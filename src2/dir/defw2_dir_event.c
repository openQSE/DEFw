/*
 * The directory's events: a service coming and going, told to the sinks
 * that subscribed to it.
 *
 * The store calls defw2_dir_events_changed under its own lock each time a
 * record becomes UP or stops being UP, and that publishes the change to every
 * subscription it matches before the store lets go. Publishing only copies
 * the event, so the copy is all the store waits for, and each subscriber
 * hears of the changes in the order the store made them. The publisher's own
 * execution stream does the delivering.
 *
 * A subscription goes when its owner cancels it or when the publisher says
 * its sink is gone, which is how v1 dropped a registration whose put failed.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_dir_internal.h"
#include "defw2_dir_wire.h"
#include "../event/defw2_event_internal.h"

/* How many subscriptions one directory keeps. */
#define DIR_SUBSCRIPTIONS_MAX	4096

struct dir_subscription {
	uint64_t		id;
	char			*address;
	uint16_t		provider_id;
	char			*tag;		/* NULL for none */
	char			*service_id;	/* NULL for any */
	char			*service_type;	/* NULL for any */
	uint32_t		changes;
	bool			gone;		/* its sink is, drop it */
	struct dir_subscription	*next;
};

struct defw2_dir_events {
	struct defw2_rt			*rt;
	defw2_event_publisher_t		*pub;
	pthread_mutex_t			lock;
	struct dir_subscription		*subs;
	size_t				count;
	uint64_t			next_id;
};

/* --- the event's payload --------------------------------------------- */

/*
 * Copies into the event's arena, with "" for absent as the directory's wire
 * has it. The first failure sticks, so a run of copies needs one check.
 */
struct maker {
	struct defw2_arena	*arena;
	uint64_t		bytes;
	defw2_rc_t		rc;
};

static defw2_str_t copy(struct maker *m, const char *src)
{
	char *dst;

	src = dir_str_out(src);
	if (m->rc != DEFW2_OK)
		return "";
	if (strnlen(src, DEFW2_STR_MAX) >= DEFW2_STR_MAX) {
		m->rc = DEFW2_ERR_INVALID;
		return "";
	}
	dst = defw2_arena_strdup(m->arena, src);
	if (dst == NULL) {
		m->rc = DEFW2_ERR_NOMEM;
		return "";
	}
	m->bytes += strlen(src) + 1;
	return dst;
}

static void *grab(struct maker *m, size_t count, size_t size)
{
	void *p;

	if (m->rc != DEFW2_OK || count == 0)
		return NULL;
	p = defw2_arena_alloc(m->arena, count * size);
	if (p == NULL)
		m->rc = DEFW2_ERR_NOMEM;
	m->bytes += count * size;
	return p;
}

/*
 * The change onto the wire, as a copy, since the record is the store's and
 * the event outlives the call. A list longer than the wire carries makes
 * the event one that cannot be sent.
 */
static defw2_rc_t change_make(struct defw2_arena *arena, const void *payload,
			      void **wire, uint64_t *bytes)
{
	const defw2_dir_change_t *change = payload;
	const defw2_dir_record_t *r = &change->record;
	struct maker m = { arena, 0, DEFW2_OK };
	defw2_dir_wire_change_t *w;
	defw2_wire_record_t *wr;
	size_t i;

	if (r->selector.alias_count > DEFW2_DIR_LIST_MAX ||
	    r->selector.resource_count > DEFW2_DIR_LIST_MAX ||
	    r->binding_count > DEFW2_DIR_LIST_MAX ||
	    r->property_count > DEFW2_DIR_LIST_MAX)
		return DEFW2_ERR_INVALID;
	w = grab(&m, 1, sizeof(*w));
	if (w == NULL)
		return DEFW2_ERR_NOMEM;
	w->connected = change->connected ? 1 : 0;
	w->reason = copy(&m, change->reason);
	wr = &w->record;
	wr->service_id = copy(&m, r->service_id);
	wr->service_type = copy(&m, r->service_type);
	wr->runtime_id = copy(&m, r->runtime_id);
	wr->generation = r->generation;
	wr->state = (hg_uint32_t)r->state;
	wr->address = copy(&m, r->address);
	wr->node_name = copy(&m, r->endpoint.node_name);
	wr->hostname = copy(&m, r->endpoint.hostname);
	wr->pid = r->endpoint.pid;
	wr->selector_name = copy(&m, r->selector.name);
	wr->registered_at_ns = r->registered_at_ns;
	wr->last_heartbeat_ns = r->last_heartbeat_ns;
	wr->retention_deadline_ns = r->retention_deadline_ns;

	wr->aliases.items = grab(&m, r->selector.alias_count,
				 sizeof(*wr->aliases.items));
	for (i = 0; wr->aliases.items != NULL &&
		    i < r->selector.alias_count; i++)
		wr->aliases.items[i] = copy(&m, r->selector.aliases[i]);
	wr->aliases.count = wr->aliases.items != NULL ?
		(hg_uint32_t)r->selector.alias_count : 0;

	wr->resources.items = grab(&m, r->selector.resource_count,
				   sizeof(*wr->resources.items));
	for (i = 0; wr->resources.items != NULL &&
		    i < r->selector.resource_count; i++)
		wr->resources.items[i] = copy(&m, r->selector.resources[i]);
	wr->resources.count = wr->resources.items != NULL ?
		(hg_uint32_t)r->selector.resource_count : 0;

	wr->bindings.items = grab(&m, r->binding_count,
				  sizeof(*wr->bindings.items));
	for (i = 0; wr->bindings.items != NULL && i < r->binding_count; i++) {
		defw2_wire_binding_t *b = &wr->bindings.items[i];

		b->binding_name = copy(&m, r->bindings[i].binding_name);
		b->api_id = copy(&m, r->bindings[i].api_id);
		b->api_version = r->bindings[i].api_version;
		b->provider_id = r->bindings[i].provider_id;
	}
	wr->bindings.count = wr->bindings.items != NULL ?
		(hg_uint32_t)r->binding_count : 0;

	wr->properties.items = grab(&m, r->property_count,
				    sizeof(*wr->properties.items));
	for (i = 0; wr->properties.items != NULL &&
		    i < r->property_count; i++) {
		wr->properties.items[i].name =
			copy(&m, r->properties[i].name);
		wr->properties.items[i].value =
			copy(&m, r->properties[i].value);
	}
	wr->properties.count = wr->properties.items != NULL ?
		(hg_uint32_t)r->property_count : 0;

	if (m.rc != DEFW2_OK)
		return m.rc;
	*wire = w;
	*bytes = m.bytes;
	return DEFW2_OK;
}

/* A decoded change into the event's arena, the way a resolve answer is. */
static defw2_rc_t change_take(struct defw2_arena *arena, const void *wire,
			      const void **payload)
{
	const defw2_dir_wire_change_t *w = wire;
	const char *reason = dir_str_in(w->reason);
	defw2_dir_change_t *change;
	defw2_dir_entry_t entry;

	change = defw2_arena_alloc(arena, sizeof(*change));
	if (change == NULL)
		return DEFW2_ERR_NOMEM;
	change->connected = w->connected != 0;
	change->reason = defw2_arena_strdup(arena, reason);
	if ((reason != NULL && change->reason == NULL) ||
	    !defw2_dir_entry_from_wire(arena, &w->record,
				       DEFW2_DIR_NO_BINDING, &entry))
		return DEFW2_ERR_NOMEM;
	change->record = entry.record;
	*payload = change;
	return DEFW2_OK;
}

const struct defw2_event_kind defw2_dir_change_kind = {
	.method = {
		DEFW2_API_DIR, DEFW2_DIR_EVENT_SERVICE,
		DEFW2_RPC_EVENT_DELIVER, DEFW2_API_VERSION,
		hg_proc_defw2_event_in_t, hg_proc_defw2_event_out_t,
	},
	.proc = hg_proc_defw2_dir_wire_change_t,
	.wire_size = sizeof(defw2_dir_wire_change_t),
	.take = change_take,
	.make = change_make,
};

defw2_rc_t defw2_dir_event_accept(defw2_event_sink_t *sink)
{
	return defw2_event_sink_accept(sink, &defw2_dir_change_kind);
}

const defw2_dir_change_t *defw2_dir_event_change(const defw2_event_t *event)
{
	if (event == NULL || event->api == NULL || event->name == NULL ||
	    strcmp(event->api, DEFW2_API_DIR) != 0 ||
	    strcmp(event->name, DEFW2_DIR_EVENT_SERVICE) != 0)
		return NULL;
	return event->payload;
}

/* --- subscriptions --------------------------------------------------- */

static void subscription_free(struct dir_subscription *s)
{
	if (s == NULL)
		return;
	free(s->address);
	free(s->tag);
	free(s->service_id);
	free(s->service_type);
	free(s);
}

static bool same(const char *a, const char *b)
{
	return (a == NULL && b == NULL) ||
	       (a != NULL && b != NULL && strcmp(a, b) == 0);
}

/* A filter that is set has to match exactly. */
static bool narrows_out(const char *filter, const char *value)
{
	return filter != NULL && (value == NULL || strcmp(filter, value) != 0);
}

static bool matches(const struct dir_subscription *s,
		    const defw2_dir_record_t *record, bool connected)
{
	uint32_t change = connected ? DEFW2_DIR_CONNECTED
				    : DEFW2_DIR_DISCONNECTED;

	if (s->changes != 0 && (s->changes & change) == 0)
		return false;
	return !narrows_out(s->service_id, record->service_id) &&
	       !narrows_out(s->service_type, record->service_type);
}

/*
 * Mark every subscription whose sink is this one. The publisher reports a
 * gone sink once, to whichever publish comes next, so each subscription
 * sharing it has to go now rather than wait to be told. Caller holds the
 * lock.
 */
static void mark_gone_locked(struct defw2_dir_events *events,
			     const struct dir_subscription *gone)
{
	struct dir_subscription *s;

	for (s = events->subs; s != NULL; s = s->next)
		if (s->provider_id == gone->provider_id &&
		    same(s->address, gone->address) && same(s->tag, gone->tag))
			s->gone = true;
}

static void sweep_gone_locked(struct defw2_dir_events *events)
{
	struct dir_subscription **link = &events->subs, *s;

	while ((s = *link) != NULL) {
		if (!s->gone) {
			link = &s->next;
			continue;
		}
		defw2_log(events->rt, DEFW2_LOG_MESSAGE,
			  "directory: subscription %llu's sink at %s is gone",
			  (unsigned long long)s->id, s->address);
		*link = s->next;
		events->count--;
		subscription_free(s);
	}
}

void defw2_dir_events_changed(void *arg, const defw2_dir_record_t *record,
			      bool connected, const char *reason)
{
	struct defw2_dir_events *events = arg;
	struct dir_subscription *s;
	defw2_event_target_t target;
	defw2_dir_change_t change;
	bool gone = false;
	defw2_rc_t rc;

	change.connected = connected;
	change.reason = reason;
	change.record = *record;

	pthread_mutex_lock(&events->lock);
	for (s = events->subs; s != NULL; s = s->next) {
		if (s->gone || !matches(s, record, connected))
			continue;
		target.address = s->address;
		target.provider_id = s->provider_id;
		target.tag = s->tag;
		rc = defw2_event_publish(events->pub, &target,
					 &defw2_dir_change_kind,
					 connected ?
						 DEFW2_DIR_SERVICE_CONNECTED :
						 DEFW2_DIR_SERVICE_DISCONNECTED,
					 &change, NULL);
		if (defw2_event_target_gone(rc)) {
			mark_gone_locked(events, s);
			gone = true;
		}
	}
	if (gone)
		sweep_gone_locked(events);
	pthread_mutex_unlock(&events->lock);
}

static defw2_rc_t subscription_add(struct defw2_dir_events *events,
				   const defw2_dir_subscribe_in_t *in,
				   uint64_t *id)
{
	struct dir_subscription *s, **tail;
	bool copied;

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return DEFW2_ERR_NOMEM;
	s->address = strdup(dir_str_in(in->address));
	s->tag = dir_str_in(in->tag) ? strdup(in->tag) : NULL;
	s->service_id = dir_str_in(in->service_id) ?
		strdup(in->service_id) : NULL;
	s->service_type = dir_str_in(in->service_type) ?
		strdup(in->service_type) : NULL;
	copied = s->address != NULL &&
		 (dir_str_in(in->tag) == NULL || s->tag != NULL) &&
		 (dir_str_in(in->service_id) == NULL ||
		  s->service_id != NULL) &&
		 (dir_str_in(in->service_type) == NULL ||
		  s->service_type != NULL);
	if (!copied) {
		subscription_free(s);
		return DEFW2_ERR_NOMEM;
	}
	s->provider_id = in->provider_id;
	s->changes = in->changes;

	pthread_mutex_lock(&events->lock);
	if (events->count >= DIR_SUBSCRIPTIONS_MAX) {
		pthread_mutex_unlock(&events->lock);
		subscription_free(s);
		return DEFW2_ERR_BUSY;
	}
	s->id = ++events->next_id;
	/* Kept in the order they came, which is the order they are told. */
	for (tail = &events->subs; *tail != NULL; tail = &(*tail)->next)
		;
	*tail = s;
	events->count++;
	*id = s->id;
	pthread_mutex_unlock(&events->lock);
	return DEFW2_OK;
}

static bool subscription_remove(struct defw2_dir_events *events, uint64_t id)
{
	struct dir_subscription **link, *s;

	pthread_mutex_lock(&events->lock);
	for (link = &events->subs; (s = *link) != NULL; link = &s->next) {
		if (s->id != id)
			continue;
		*link = s->next;
		events->count--;
		pthread_mutex_unlock(&events->lock);
		subscription_free(s);
		return true;
	}
	pthread_mutex_unlock(&events->lock);
	return false;
}

/* --- the events' life ------------------------------------------------ */

defw2_rc_t defw2_dir_events_create(struct defw2_rt *rt,
				   defw2_dir_events_t **out)
{
	struct defw2_dir_events *events;
	defw2_rc_t rc;

	events = calloc(1, sizeof(*events));
	if (events == NULL)
		return DEFW2_ERR_NOMEM;
	if (pthread_mutex_init(&events->lock, NULL) != 0) {
		free(events);
		return DEFW2_ERR_INTERNAL;
	}
	events->rt = rt;
	rc = defw2_event_publisher_create(rt, NULL, &events->pub);
	if (rc != DEFW2_OK) {
		pthread_mutex_destroy(&events->lock);
		free(events);
		return rc;
	}
	*out = events;
	return DEFW2_OK;
}

void defw2_dir_events_destroy(defw2_dir_events_t *events)
{
	struct dir_subscription *s, *next;

	if (events == NULL)
		return;
	defw2_event_publisher_destroy(events->pub);
	for (s = events->subs; s != NULL; s = next) {
		next = s->next;
		subscription_free(s);
	}
	pthread_mutex_destroy(&events->lock);
	free(events);
}

/* --- subscribe, unsubscribe and get_runtime_id ----------------------- */

/*
 * What the three methods find on their registration. Margo frees it with
 * free, and the events it points at belong to the directory, which frees
 * them as the runtime finalizes, after the last call.
 */
struct dir_events_bound {
	struct defw2_bound	base;
	defw2_dir_events_t	*events;
};

/*
 * A subscription has to name a sink that events can reach: an address a
 * publisher will take, on a provider other than the directory's own.
 */
static void serve_subscribe(struct defw2_served *served, void *vin,
			    void *vout)
{
	struct dir_events_bound *b = (struct dir_events_bound *)served->bound;
	defw2_dir_subscribe_in_t *in = vin;
	defw2_dir_subscribe_out_t *out = vout;
	const char *address = dir_str_in(in->address);
	defw2_rc_t rc;

	if (address == NULL || strnlen(address, DEFW2_NAME_MAX) >=
	    DEFW2_NAME_MAX || in->provider_id == DEFW2_PROVIDER_DIR) {
		defw2_served_fail(served, DEFW2_ERR_INVALID,
				  DEFW2_CAT_INVALID_ARGUMENT,
				  "a subscription needs a sink's address and "
				  "provider");
		return;
	}
	rc = subscription_add(b->events, in, &out->subscription_id);
	if (rc == DEFW2_ERR_BUSY)
		defw2_served_fail(served, rc, DEFW2_CAT_PENDING_CAPACITY,
				  "the directory keeps no more subscriptions");
	else if (rc != DEFW2_OK)
		defw2_served_fail(served, rc, DEFW2_CAT_PROVIDER_FAILURE,
				  "the directory could not keep the "
				  "subscription");
}

static void serve_unsubscribe(struct defw2_served *served, void *vin,
			      void *vout)
{
	struct dir_events_bound *b = (struct dir_events_bound *)served->bound;
	defw2_dir_unsubscribe_in_t *in = vin;

	(void)vout;
	if (!subscription_remove(b->events, in->subscription_id))
		defw2_served_fail(served, DEFW2_ERR_NOT_FOUND,
				  DEFW2_CAT_NOT_FOUND,
				  "the directory has no such subscription");
}

/* The string is the runtime's, so it outlives the respond. */
static void serve_runtime_id(struct defw2_served *served, void *vin,
			     void *vout)
{
	defw2_dir_runtime_id_out_t *out = vout;

	(void)vin;
	out->runtime_id = defw2_runtime_id(served->rt);
}

static void defw2_dir_subscribe_ult(hg_handle_t handle)
{
	defw2_dir_subscribe_in_t in;
	defw2_dir_subscribe_out_t out;

	defw2_typed_serve(handle, &defw2_dir_m_subscribe, &in, sizeof(in),
			  &out, sizeof(out), serve_subscribe);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_subscribe_ult)

static void defw2_dir_unsubscribe_ult(hg_handle_t handle)
{
	defw2_dir_unsubscribe_in_t in;
	defw2_dir_lease_out_t out;

	defw2_typed_serve(handle, &defw2_dir_m_unsubscribe, &in, sizeof(in),
			  &out, sizeof(out), serve_unsubscribe);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_unsubscribe_ult)

static void defw2_dir_runtime_id_ult(hg_handle_t handle)
{
	defw2_dir_runtime_id_in_t in;
	defw2_dir_runtime_id_out_t out;

	defw2_typed_serve(handle, &defw2_dir_m_get_runtime_id, &in,
			  sizeof(in), &out, sizeof(out), serve_runtime_id);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_runtime_id_ult)

defw2_rc_t defw2_dir_events_bind(defw2_service_t *svc,
				 defw2_dir_events_t *events)
{
	static const struct defw2_typed_entry entries[] = {
		{ &defw2_dir_m_subscribe,
		  _handler_for_defw2_dir_subscribe_ult },
		{ &defw2_dir_m_unsubscribe,
		  _handler_for_defw2_dir_unsubscribe_ult },
		{ &defw2_dir_m_get_runtime_id,
		  _handler_for_defw2_dir_runtime_id_ult },
	};
	struct dir_events_bound *bound;

	if (svc == NULL || events == NULL)
		return DEFW2_ERR_INVALID;
	bound = calloc(1, sizeof(*bound));
	if (bound == NULL)
		return DEFW2_ERR_NOMEM;
	bound->events = events;
	return defw2_typed_bind(svc, entries,
				sizeof(entries) / sizeof(entries[0]),
				&bound->base);
}
