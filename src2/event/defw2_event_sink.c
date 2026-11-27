/*
 * Sinks: where a caller's events arrive.
 *
 * A sink is one provider. Its handler decodes an event, queues it and
 * answers at once, so a sender waits only for the network, never for
 * whoever reads the event. The reader is either the sink's own thread,
 * which runs the owner's callback, or the owner itself through
 * defw2_event_sink_next. Neither is a Margo thread, which is the rule that
 * keeps a slow consumer, or a foreign runtime such as Python, away from the
 * network.
 *
 * The sink is the data its provider's registration carries, so it lives
 * until Margo frees it when the runtime finalizes. That is what makes
 * destroying a sink safe with a handler still running: the handler finds it
 * closed rather than freed. Creating a sink again on the same provider
 * reopens the same one.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "defw2_event_internal.h"
#include "defw2_trace.h"

struct sink_event {
	struct sink_event	*next;
	defw2_event_t		event;
	uint64_t		bytes;
};

struct defw2_event_sink {
	struct defw2_rt			*rt;
	uint16_t			provider_id;
	pthread_mutex_t			lock;
	pthread_cond_t			arrived;
	bool				open;
	const struct defw2_event_kind	*kinds[DEFW2_EVENT_KINDS_MAX];
	unsigned			nkinds;
	struct sink_event		*head;
	struct sink_event		*tail;
	unsigned			length;
	unsigned			depth;
	uint64_t			bytes;
	uint64_t			max_bytes;
	uint64_t			received;
	uint64_t			refused;
	defw2_event_cb_t		callback;
	void				*arg;
	pthread_t			thread;
	bool				threaded;
};

static void deadline_after(struct timespec *when, uint32_t timeout_ms)
{
	clock_gettime(CLOCK_REALTIME, when);
	when->tv_sec += timeout_ms / 1000;
	when->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (when->tv_nsec >= 1000000000L) {
		when->tv_sec++;
		when->tv_nsec -= 1000000000L;
	}
}

static void event_drop(struct sink_event *e)
{
	defw2_event_free(&e->event);
	free(e);
}

/* Take the oldest event. The caller holds the lock and knows there is one. */
static struct sink_event *pop_locked(struct defw2_event_sink *sink)
{
	struct sink_event *e = sink->head;

	sink->head = e->next;
	if (sink->head == NULL)
		sink->tail = NULL;
	sink->length--;
	sink->bytes -= e->bytes;
	e->next = NULL;
	return e;
}

/* --- receiving ------------------------------------------------------- */

static bool copy_str(struct defw2_arena *arena, const char *src,
		     const char **dst)
{
	*dst = NULL;
	if (src == NULL)
		return true;
	*dst = defw2_arena_strdup(arena, src);
	return *dst != NULL;
}

/*
 * Copy a decoded request into an event the sink owns, since the request is
 * Mercury's and goes once the handler answers. api and name come from the
 * kind, which is static, so they need no copy.
 */
static struct sink_event *take_event(const defw2_event_in_t *in,
				     const char *traceparent, uint64_t bytes)
{
	struct defw2_arena *arena;
	struct sink_event *e;

	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return NULL;
	arena = calloc(1, sizeof(*arena));
	if (arena == NULL) {
		free(e);
		return NULL;
	}
	e->event.arena = arena;
	e->event.api = in->kind->method.api;
	e->event.name = in->kind->method.name;
	e->event.seq = in->seq;
	e->bytes = bytes;
	if (traceparent != NULL && traceparent[0] == '\0')
		traceparent = NULL;
	if (!copy_str(arena, in->tag, &e->event.tag) ||
	    !copy_str(arena, in->type, &e->event.type) ||
	    !copy_str(arena, in->hdr.runtime_id, &e->event.source) ||
	    !copy_str(arena, traceparent, &e->event.traceparent) ||
	    in->kind->take(arena, in->payload, &e->event.payload) !=
	    DEFW2_OK) {
		event_drop(e);
		return NULL;
	}
	return e;
}

/*
 * Queue an event, or say why not. A full sink is backpressure, which the
 * sender counts as a lost event rather than as a sink that is gone.
 */
static void queue_event(struct defw2_event_sink *sink, struct sink_event *e,
			defw2_event_out_t *out)
{
	pthread_mutex_lock(&sink->lock);
	if (!sink->open) {
		pthread_mutex_unlock(&sink->lock);
		defw2_wire_status_set(&out->status, DEFW2_ERR_NOT_FOUND,
				      DEFW2_CAT_NOT_FOUND,
				      "the sink is closed");
		event_drop(e);
		return;
	}
	if (sink->length >= sink->depth ||
	    sink->bytes + e->bytes > sink->max_bytes) {
		sink->refused++;
		pthread_mutex_unlock(&sink->lock);
		defw2_wire_status_set(&out->status, DEFW2_ERR_BUSY,
				      DEFW2_CAT_PENDING_CAPACITY,
				      "the sink is full");
		event_drop(e);
		return;
	}
	if (sink->tail == NULL)
		sink->head = e;
	else
		sink->tail->next = e;
	sink->tail = e;
	sink->length++;
	sink->bytes += e->bytes;
	sink->received++;
	pthread_cond_signal(&sink->arrived);
	pthread_mutex_unlock(&sink->lock);
}

static void deliver_ult(hg_handle_t handle)
{
	uint64_t arrived_wall = 0, arrived_mono = 0, mark = 0;
	struct defw2_event_sink *sink = NULL;
	struct defw2_trace trace = { 0 };
	const struct hg_info *info;
	struct sink_event *e;
	margo_instance_id mid;
	defw2_event_out_t out;
	defw2_event_in_t in;
	bool decoded = false;
	hg_return_t hret;
	bool open;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	defw2_wire_status_ok(&out.status);
	mid = margo_hg_handle_get_instance(handle);
	info = margo_get_info(handle);
	if (info != NULL)
		sink = margo_registered_data(mid, info->id);
	if (sink == NULL) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_NOT_FOUND,
				      DEFW2_CAT_NOT_FOUND,
				      "nothing takes events on this provider");
		goto respond;
	}
	if (defw2_profiling(sink->rt)) {
		arrived_wall = defw2_wall_ns();
		arrived_mono = defw2_mono_ns();
	}

	/*
	 * What the sink accepts goes into the request before the decode,
	 * which is how the decode refuses any other kind before allocating
	 * for it. A copy, so a sink closing meanwhile changes nothing here.
	 */
	pthread_mutex_lock(&sink->lock);
	open = sink->open;
	memcpy(in.accepted, sink->kinds, sizeof(in.accepted));
	in.naccepted = sink->nkinds;
	pthread_mutex_unlock(&sink->lock);
	if (!open) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_NOT_FOUND,
				      DEFW2_CAT_NOT_FOUND,
				      "the sink is closed");
		goto respond;
	}

	hret = margo_get_input(handle, &in);
	if (hret != HG_SUCCESS) {
		/* defw2_free_partial says why this is not margo_free_input. */
		defw2_free_partial(mid, hg_proc_defw2_event_in_t, &in);
		if (in.unknown)
			defw2_wire_status_set(&out.status, DEFW2_ERR_NOT_FOUND,
					      DEFW2_CAT_NOT_FOUND,
					      "this sink does not take that "
					      "kind of event");
		else
			defw2_wire_status_set(&out.status, DEFW2_ERR_INVALID,
					      DEFW2_CAT_INVALID_ARGUMENT,
					      "cannot decode request");
		goto respond;
	}
	decoded = true;

	if (arrived_mono != 0) {
		defw2_trace_begin(sink->rt, &trace, DEFW2_SPAN_SERVER,
				  in.hdr.traceparent);
		defw2_trace_backdate(&trace, arrived_wall, arrived_mono);
		mark = defw2_mono_ns();
		trace.span.decode_ns = mark - arrived_mono;
	}

	if (!defw2_hdr_compatible(&in.hdr, in.kind->method.version)) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_VERSION,
				      DEFW2_CAT_VERSION_MISMATCH,
				      "unsupported api version");
		goto respond;
	}

	/*
	 * The event belongs under this span when it is recorded, and under
	 * the sender's otherwise, so what its reader records joins the trace.
	 */
	e = take_event(&in, trace.recording ? trace.traceparent
					    : in.hdr.traceparent,
		       HG_Get_input_payload_size(handle));
	if (e == NULL)
		defw2_wire_status_set(&out.status, DEFW2_ERR_NOMEM,
				      DEFW2_CAT_PROVIDER_FAILURE,
				      "no memory for the event");
	else
		queue_event(sink, e, &out);

respond:
	if (trace.recording) {
		trace.span.handler_ns = defw2_mono_ns() - mark;
		mark = defw2_mono_ns();
	}
	hret = margo_respond(handle, &out);
	if (hret != HG_SUCCESS && sink != NULL)
		defw2_log(sink->rt, DEFW2_LOG_ERROR, "event respond: %s",
			  HG_Error_to_string(hret));
	if (trace.recording) {
		trace.span.encode_ns = defw2_mono_ns() - mark;
		trace.span.request_bytes = HG_Get_input_payload_size(handle);
		trace.span.response_bytes = HG_Get_output_payload_size(handle);
		trace.span.api = in.kind->method.api;
		trace.span.method = in.kind->method.name;
		trace.span.tier = DEFW2_TIER_TYPED;
		trace.span.category = out.status.category;
		trace.span.code = out.status.code;
		defw2_trace_end(sink->rt, &trace);
	}
	if (decoded)
		margo_free_input(handle, &in);
	margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(deliver_ult)

/* --- reading --------------------------------------------------------- */

static void *dispatch(void *arg)
{
	struct defw2_event_sink *sink = arg;
	struct sink_event *e;

	for (;;) {
		pthread_mutex_lock(&sink->lock);
		while (sink->open && sink->head == NULL)
			pthread_cond_wait(&sink->arrived, &sink->lock);
		if (!sink->open) {
			pthread_mutex_unlock(&sink->lock);
			return NULL;
		}
		e = pop_locked(sink);
		pthread_mutex_unlock(&sink->lock);
		sink->callback(&e->event, sink->arg);
		event_drop(e);
	}
}

defw2_rc_t defw2_event_sink_next(defw2_event_sink_t *sink,
				 uint32_t timeout_ms, defw2_event_t *event)
{
	struct sink_event *e = NULL;
	struct timespec deadline;
	defw2_rc_t rc;

	if (sink == NULL || event == NULL)
		return DEFW2_ERR_INVALID;
	memset(event, 0, sizeof(*event));
	if (sink->callback != NULL)
		return DEFW2_ERR_INVALID;
	deadline_after(&deadline, timeout_ms);

	pthread_mutex_lock(&sink->lock);
	while (sink->open && sink->head == NULL) {
		if (pthread_cond_timedwait(&sink->arrived, &sink->lock,
					   &deadline) == ETIMEDOUT)
			break;
	}
	if (sink->head != NULL) {
		e = pop_locked(sink);
		rc = DEFW2_OK;
	} else {
		/* A closed sink is how a reading loop learns to stop. */
		rc = sink->open ? DEFW2_ERR_TIMEOUT : DEFW2_ERR_NOT_FOUND;
	}
	pthread_mutex_unlock(&sink->lock);

	if (e != NULL) {
		*event = e->event;
		free(e);
	}
	return rc;
}

/* --- the sink's life ------------------------------------------------- */

/*
 * Close the sink: refuse what arrives from now on, drop what waits, and
 * wait for the callback. Safe to call again.
 */
static void sink_close(struct defw2_event_sink *sink)
{
	struct sink_event *e, *next;
	bool threaded;

	pthread_mutex_lock(&sink->lock);
	if (!sink->open) {
		pthread_mutex_unlock(&sink->lock);
		return;
	}
	sink->open = false;
	e = sink->head;
	sink->head = NULL;
	sink->tail = NULL;
	sink->length = 0;
	sink->bytes = 0;
	threaded = sink->threaded;
	sink->threaded = false;
	pthread_cond_broadcast(&sink->arrived);
	pthread_mutex_unlock(&sink->lock);

	for (; e != NULL; e = next) {
		next = e->next;
		event_drop(e);
	}
	if (!threaded)
		return;
	/* A callback may destroy its own sink, and cannot wait for itself. */
	if (pthread_equal(pthread_self(), sink->thread))
		pthread_detach(sink->thread);
	else
		pthread_join(sink->thread, NULL);
}

/*
 * The runtime is stopping. Closing here, while Margo is still up, means the
 * callback finishes before the network goes, rather than in the middle of
 * Margo's teardown.
 */
static void sink_prefinalize(void *arg)
{
	sink_close(arg);
}

/* Margo releases the registration's data as the runtime finalizes. */
static void sink_free(void *arg)
{
	struct defw2_event_sink *sink = arg;

	sink_close(sink);
	pthread_cond_destroy(&sink->arrived);
	pthread_mutex_destroy(&sink->lock);
	free(sink);
}

defw2_rc_t defw2_event_sink_create(defw2_rt_t *rt, uint16_t provider_id,
				   const defw2_event_sink_opts_t *opts,
				   defw2_event_sink_t **out)
{
	struct defw2_event_sink *sink = NULL;
	hg_bool_t flag = HG_FALSE;
	hg_id_t id = 0;

	if (rt == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	*out = NULL;
	/*
	 * Provider 0 is the directory's, and a publisher in the same process
	 * registers this RPC there to call it, so a sink there would collide.
	 */
	if (provider_id == 0)
		return DEFW2_ERR_INVALID;
	/* A client-mode Margo instance does not listen, so no event could
	 * reach a sink on it. Say so rather than let every sender time out. */
	if (rt->role != DEFW2_ROLE_SERVER) {
		defw2_log(rt, DEFW2_LOG_ERROR,
			  "an event sink needs a server runtime, set "
			  "DEFW_AGENT_TYPE");
		return DEFW2_ERR_CONFIG;
	}
	if (__atomic_load_n(&rt->stopping, __ATOMIC_ACQUIRE) != 0)
		return DEFW2_ERR_CANCELLED;

	if (margo_provider_registered_name(rt->mid, DEFW2_RPC_EVENT_DELIVER,
					   provider_id, &id, &flag) !=
	    HG_SUCCESS)
		flag = HG_FALSE;
	if (flag) {
		sink = margo_registered_data(rt->mid, id);
		if (sink == NULL)
			return DEFW2_ERR_INTERNAL;
		pthread_mutex_lock(&sink->lock);
		if (sink->open) {
			pthread_mutex_unlock(&sink->lock);
			return DEFW2_ERR_BUSY;
		}
		sink->nkinds = 0;
		sink->received = 0;
		sink->refused = 0;
		pthread_mutex_unlock(&sink->lock);
	} else {
		sink = calloc(1, sizeof(*sink));
		if (sink == NULL)
			return DEFW2_ERR_NOMEM;
		pthread_mutex_init(&sink->lock, NULL);
		pthread_cond_init(&sink->arrived, NULL);
		sink->rt = rt;
		sink->provider_id = provider_id;
		id = margo_provider_register_name(rt->mid,
						  DEFW2_RPC_EVENT_DELIVER,
						  hg_proc_defw2_event_in_t,
						  hg_proc_defw2_event_out_t,
						  _handler_for_deliver_ult,
						  provider_id, ABT_POOL_NULL);
		if (id == 0) {
			defw2_log(rt, DEFW2_LOG_ERROR,
				  "cannot register an event sink on provider "
				  "%u", provider_id);
			pthread_cond_destroy(&sink->arrived);
			pthread_mutex_destroy(&sink->lock);
			free(sink);
			return DEFW2_ERR_INTERNAL;
		}
		margo_register_data(rt->mid, id, sink, sink_free);
	}

	sink->depth = opts != NULL && opts->depth > 0 ? opts->depth
						     : DEFW2_EVENT_SINK_DEPTH;
	sink->max_bytes = opts != NULL && opts->max_bytes > 0 ?
				  opts->max_bytes : DEFW2_EVENT_SINK_BYTES;
	sink->callback = opts != NULL ? opts->callback : NULL;
	sink->arg = opts != NULL ? opts->arg : NULL;
	sink->open = true;
	if (sink->callback != NULL) {
		if (pthread_create(&sink->thread, NULL, dispatch, sink) != 0) {
			sink->open = false;
			return DEFW2_ERR_INTERNAL;
		}
		sink->threaded = true;
	}
	margo_provider_push_prefinalize_callback(rt->mid, sink,
						 sink_prefinalize, sink);

	defw2_log(rt, DEFW2_LOG_MESSAGE, "event sink on provider %u at %s",
		  provider_id, rt->address);
	*out = sink;
	return DEFW2_OK;
}

void defw2_event_sink_destroy(defw2_event_sink_t *sink)
{
	if (sink == NULL)
		return;
	/* The runtime need not close it now. Its memory stays until Margo
	 * frees it, in case a handler is still running. */
	margo_provider_pop_prefinalize_callback(sink->rt->mid, sink);
	sink_close(sink);
}

defw2_rc_t defw2_event_sink_accept(defw2_event_sink_t *sink,
				   const struct defw2_event_kind *kind)
{
	defw2_rc_t rc = DEFW2_OK;
	unsigned i;

	if (sink == NULL || kind == NULL)
		return DEFW2_ERR_INVALID;
	pthread_mutex_lock(&sink->lock);
	for (i = 0; i < sink->nkinds; i++)
		if (sink->kinds[i] == kind)
			goto out;
	if (!sink->open)
		rc = DEFW2_ERR_NOT_FOUND;
	else if (sink->nkinds == DEFW2_EVENT_KINDS_MAX)
		rc = DEFW2_ERR_BUSY;
	else
		sink->kinds[sink->nkinds++] = kind;
out:
	pthread_mutex_unlock(&sink->lock);
	return rc;
}

const char *defw2_event_sink_address(const defw2_event_sink_t *sink)
{
	return sink != NULL ? sink->rt->address : NULL;
}

uint16_t defw2_event_sink_provider_id(const defw2_event_sink_t *sink)
{
	return sink != NULL ? sink->provider_id : 0;
}

void defw2_event_sink_stats(defw2_event_sink_t *sink,
			    defw2_event_sink_stats_t *stats)
{
	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	if (sink == NULL)
		return;
	pthread_mutex_lock(&sink->lock);
	stats->received = sink->received;
	stats->refused = sink->refused;
	stats->waiting = sink->length;
	pthread_mutex_unlock(&sink->lock);
}
