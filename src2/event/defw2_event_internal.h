/*
 * Events, shared inside libdefw2 and not installed.
 *
 * One RPC carries every event, defw2.event.deliver. Its request is an
 * envelope, which every event has, followed by a payload, which belongs to
 * the API that sent it. The envelope names the API and the event, and the
 * payload is that API's own wire structure, encoded by that API's own proc.
 * So a payload is as typed as any answer, and a sink decodes it with the
 * same checked procs, in the same pass as the envelope.
 *
 * What lets one RPC carry every API's events is the kind: what an API
 * supplies to say how its events travel. A publisher is handed the kind by
 * the API's publish call, and a sink decodes only the kinds an API's accept
 * call gave it. The framework itself knows no event by name.
 */
#ifndef DEFW2_EVENT_INTERNAL_H
#define DEFW2_EVENT_INTERNAL_H

#include <defw2/defw2_event.h>

#include "../rpc/defw2_typed.h"

#define DEFW2_RPC_EVENT_DELIVER	"defw2.event.deliver"

/* The most kinds of event one sink takes. */
#define DEFW2_EVENT_KINDS_MAX	8

/* How long a publisher remembers a target it has nothing for. */
#define DEFW2_EVENT_IDLE_MS	60000

struct defw2_event_kind {
	/*
	 * The API and the event's name, which travel in the envelope, and the
	 * API's version, which travels in the header. A delivery's spans are
	 * named for them too. rpc is always DEFW2_RPC_EVENT_DELIVER.
	 */
	struct defw2_method	method;
	hg_proc_cb_t		proc;		/* the payload's wire form */
	size_t			wire_size;	/* and its size */
	/*
	 * A sink's half: copy a decoded payload into its public form, all of
	 * it in arena.
	 */
	defw2_rc_t		(*take)(struct defw2_arena *arena,
					const void *wire, const void **payload);
	/*
	 * A publisher's half: copy the caller's public form into a wire
	 * structure whose strings live in arena, and count roughly the bytes
	 * it holds. Refuses with DEFW2_ERR_INVALID what would not encode.
	 */
	defw2_rc_t		(*make)(struct defw2_arena *arena,
					const void *payload, void **wire,
					uint64_t *bytes);
};

/*
 * The request. The envelope and the payload travel. The rest belongs to
 * whoever encodes or decodes it and travels nowhere: kind says how to
 * handle the payload, and accepted is what a sink takes, set before the
 * decode so the decode can refuse a kind the sink does not know. unknown
 * says that is why a decode failed.
 */
typedef struct {
	defw2_hdr_t			hdr;
	defw2_str_t			tag;
	defw2_str_t			type;
	hg_uint64_t			seq;
	defw2_str_t			api;
	defw2_str_t			name;
	void				*payload;
	const struct defw2_event_kind	*kind;
	const struct defw2_event_kind	*accepted[DEFW2_EVENT_KINDS_MAX];
	unsigned			naccepted;
	bool				unknown;
} defw2_event_in_t;

hg_return_t hg_proc_defw2_event_in_t(hg_proc_t proc, void *arg);

/* A sink's answer is whether it queued the event. */
MERCURY_GEN_PROC(defw2_event_out_t,
	((defw2_wire_status_t)(status)))

/*
 * Have sink take events of kind, which must be static, since a handler may
 * still be using it after the sink closes.
 */
defw2_rc_t defw2_event_sink_accept(defw2_event_sink_t *sink,
				   const struct defw2_event_kind *kind);

/*
 * Queue payload for target, as an event of kind. Returns what
 * defw2_event.h says an API's publish function returns.
 */
defw2_rc_t defw2_event_publish(defw2_event_publisher_t *pub,
			       const defw2_event_target_t *target,
			       const struct defw2_event_kind *kind,
			       const char *type, const void *payload,
			       const char *traceparent);

#endif /* DEFW2_EVENT_INTERNAL_H */
