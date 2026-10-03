/*
 * Events: a service telling a caller that something happened.
 *
 * A caller that wants events creates a sink. A sink is a provider in the
 * caller's own process, so that process must listen: its runtime is a
 * server. The caller gives the service the sink's address and provider, and
 * a tag of its own, through whatever registration the service's API offers,
 * and the service sends it events through a publisher.
 *
 *	the caller
 *	defw2_event_sink_opts_t opts = { .callback = on_event, .arg = me };
 *	defw2_event_sink_create(rt, DEFW2_PROVIDER_EVENT, &opts, &sink);
 *	defw2_qpm_event_accept(sink);
 *	// register defw2_event_sink_address(sink), the provider and a tag
 *
 *	the service
 *	defw2_event_publisher_create(rt, NULL, &pub);
 *	rc = defw2_qpm_publish_completion(pub, &target, type, &task, tp);
 *	if (defw2_event_target_gone(rc))
 *		// drop the registration
 *
 * Publishing copies the event and returns. The publisher's own execution
 * stream delivers it: to every target at once, each target's events in the
 * order they were published, and each delivery within a time limit. A sink
 * acknowledges an event as soon as it is queued, before anything has looked
 * at it, so a slow consumer never holds up the service. v1 did the opposite.
 * Its service waited on each client in turn, and one client decoding a large
 * result held up every client behind it (openQSE/QFw issue 64).
 *
 * Delivery is at most once. An event is lost when a sink is full, when a
 * target's queue is full, or when a target cannot be reached. The service's
 * own record, such as a completion queue, is how a caller recovers. seq
 * counts one sender's events to one target from 1, so a gap says something
 * was lost.
 *
 * A sink or a publisher still open when the runtime stops is closed then,
 * while Margo can still reach the network, so neither has to be destroyed
 * in a particular order.
 */
#ifndef DEFW2_EVENT_H
#define DEFW2_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#include <defw2/defw2.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where a process serves its sink unless it says otherwise. */
#define DEFW2_PROVIDER_EVENT		5

/* The defaults, which the options below override. */
#define DEFW2_EVENT_TIMEOUT_MS		1000
#define DEFW2_EVENT_TARGET_DEPTH	1024
#define DEFW2_EVENT_PUBLISHER_BYTES	(256ull * 1024ull * 1024ull)
#define DEFW2_EVENT_SINK_DEPTH		4096
#define DEFW2_EVENT_SINK_BYTES		(64ull * 1024ull * 1024ull)

/*
 * Where a service sends events: a sink's address and provider, and the tag
 * the sink's owner chose when it registered. The tag comes back on every
 * event, so one sink can tell its registrations apart.
 */
typedef struct {
	const char	*address;
	uint16_t	provider_id;
	const char	*tag;		/* may be NULL */
} defw2_event_target_t;

/*
 * An event as a sink hands it over. api and name say what it is, and the
 * header of the API that sent it says what payload points at, such as a
 * defw2_qpm_task_t for a QPM's completion.
 */
typedef struct {
	const char	*api;		/* "qfw.qpm.execution" */
	const char	*name;		/* "completion" */
	const char	*type;		/* as the registration asked */
	const char	*tag;		/* the target's, may be NULL */
	const char	*source;	/* the runtime that sent it */
	uint64_t	seq;		/* from 1 per sender and target */
	const char	*traceparent;	/* its work, may be NULL */
	const void	*payload;
	void		*arena;		/* internal */
} defw2_event_t;

/*
 * Release an event taken with defw2_event_sink_next, payload and all. The
 * payload belongs to the event, so it is never freed on its own.
 */
void defw2_event_free(defw2_event_t *event);

/* --- sinks ----------------------------------------------------------- */

typedef struct defw2_event_sink defw2_event_sink_t;

/*
 * Called on the sink's own thread, never on a Margo thread, one event at a
 * time in the order they arrived, so it may block. The event lasts until
 * the callback returns. A callback must not call into DEFw once the runtime
 * is stopping.
 */
typedef void (*defw2_event_cb_t)(const defw2_event_t *event, void *arg);

/* Zero initialise for the defaults. */
typedef struct {
	defw2_event_cb_t	callback;	/* NULL to use next */
	void			*arg;
	unsigned		depth;		/* events that may wait */
	uint64_t		max_bytes;	/* bytes that may wait */
} defw2_event_sink_opts_t;

/*
 * Serve a sink on provider_id, which may not be 0, the directory's. The
 * runtime must be a server, because a client-mode Margo instance does not
 * listen. A sink takes no events until an API's accept function names
 * them, such as defw2_qpm_event_accept. Fails with DEFW2_ERR_BUSY when a
 * sink already serves that provider. opts may be NULL.
 */
defw2_rc_t defw2_event_sink_create(defw2_rt_t *rt, uint16_t provider_id,
				   const defw2_event_sink_opts_t *opts,
				   defw2_event_sink_t **sink);

/*
 * Stop taking events, drop those still waiting, and wait for the callback
 * to return. A sender then hears that the sink is gone. Call it before
 * defw2_finalize, or not at all, since an open sink is closed when the
 * runtime stops.
 */
void defw2_event_sink_destroy(defw2_event_sink_t *sink);

/*
 * Take the next event, for a sink with no callback. Returns
 * DEFW2_ERR_TIMEOUT when none arrived in time and DEFW2_ERR_NOT_FOUND once
 * the sink is closed. It holds no lock while it waits, so a binding may
 * release its interpreter lock around it. Free the event with
 * defw2_event_free.
 */
defw2_rc_t defw2_event_sink_next(defw2_event_sink_t *sink,
				 uint32_t timeout_ms, defw2_event_t *event);

const char *defw2_event_sink_address(const defw2_event_sink_t *sink);
uint16_t defw2_event_sink_provider_id(const defw2_event_sink_t *sink);

typedef struct {
	uint64_t	received;	/* queued on arrival */
	uint64_t	refused;	/* turned away, full */
	uint64_t	waiting;	/* queued now */
} defw2_event_sink_stats_t;

void defw2_event_sink_stats(defw2_event_sink_t *sink,
			    defw2_event_sink_stats_t *stats);

/* --- publishers ------------------------------------------------------ */

typedef struct defw2_event_publisher defw2_event_publisher_t;

/* Zero initialise for the defaults. */
typedef struct {
	uint32_t	timeout_ms;	/* the limit on one delivery */
	unsigned	depth;		/* waiting per target */
	uint64_t	max_bytes;	/* bytes that may wait in all */
} defw2_event_publisher_opts_t;

/*
 * Start a publisher, with an execution stream of its own to deliver from.
 * opts may be NULL.
 */
defw2_rc_t defw2_event_publisher_create(defw2_rt_t *rt,
					const defw2_event_publisher_opts_t *opts,
					defw2_event_publisher_t **pub);

/*
 * Drop what has not been sent, wait for deliveries already on their way,
 * at most the time limit, and give the execution stream back. Safe once the
 * runtime has stopped, which closes an open publisher itself.
 */
void defw2_event_publisher_destroy(defw2_event_publisher_t *pub);

/*
 * What an API's publish function returns, such as
 * defw2_qpm_publish_completion:
 *
 *	DEFW2_OK		queued for delivery
 *	DEFW2_ERR_BUSY		the target's queue, or the publisher's, is
 *				full, and this event is dropped
 *	DEFW2_ERR_INVALID	this event cannot be sent, such as a field
 *				too long for the wire
 *	DEFW2_ERR_NOMEM, _INTERNAL	this event could not be queued
 *	DEFW2_ERR_CANCELLED	the publisher is closing
 *
 * and a code for which defw2_event_target_gone is true when an earlier
 * delivery to the same target failed: the target is unreachable, timed
 * out, has no sink, or does not take this kind of event. The caller should
 * drop its registration. This event was not queued. A failure is reported
 * once, and the next event to the same target is tried afresh.
 */
bool defw2_event_target_gone(defw2_rc_t rc);

typedef struct {
	uint64_t	published;	/* accepted for delivery */
	uint64_t	delivered;	/* acknowledged by a sink */
	uint64_t	refused;	/* turned away by a full sink */
	uint64_t	failed;		/* deliveries that failed */
	uint64_t	dropped;	/* never sent */
	uint64_t	waiting;	/* queued or on their way now */
	uint32_t	targets;	/* targets it remembers now */
} defw2_event_publisher_stats_t;

void defw2_event_publisher_stats(defw2_event_publisher_t *pub,
				 defw2_event_publisher_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_EVENT_H */
