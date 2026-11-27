/*
 * Typed methods. Shared inside libdefw2 and not installed.
 *
 * Every typed method does the same things around its own conversions. A
 * caller fills the header, forwards with a timeout, copies what came back out
 * of Mercury's buffers and records a span. A provider decodes, checks the
 * version, runs the service, pushes any bulk result, encodes the answer and
 * records a span. Those steps live here once, so a method costs only its
 * conversions, and an API defined on top of DEFw gets exactly what DEFw's own
 * APIs get.
 *
 * One layout rule makes it work. Every wire request starts with a defw2_hdr_t
 * and every wire response starts with a defw2_wire_status_t. C puts a
 * structure's first member at the structure's own address, so the shared
 * code reaches both without knowing the rest of either.
 */
#ifndef DEFW2_TYPED_H
#define DEFW2_TYPED_H

#include "defw2_host.h"
#include "defw2_trace.h"
#include "defw2_wire.h"

/* What both sides know about one method. */
struct defw2_method {
	const char	*api;		/* "qfw.qpm.execution" */
	const char	*name;		/* "async_run" */
	const char	*rpc;		/* "defw2.qfw.qpm.execution.async_run" */
	uint32_t	version;	/* the API's, carried in the header */
	hg_proc_cb_t	in_proc;
	hg_proc_cb_t	out_proc;
	uint8_t		tier;		/* DEFW2_TIER_*, for the span */
};

/* --- calling --------------------------------------------------------- */

struct defw2_typed_call;

/*
 * Copy what the caller keeps out of a decoded response. It runs only when
 * the call came back, while the response is still Mercury's, and its return
 * code becomes the call's.
 */
typedef defw2_rc_t (*defw2_take_fn)(struct defw2_typed_call *call);

struct defw2_typed_call {
	defw2_binding_t			*binding;
	const struct defw2_method	*method;
	const defw2_call_opts_t		*opts;
	void				*in;	/* starts with defw2_hdr_t */
	void				*out;	/* starts with defw2_wire_status_t */
	size_t				out_size;
	defw2_take_fn			take;	/* may be NULL */
	void				*arg;	/* for take */
	uint64_t			bulk_bytes; /* take sets it, for the span */
};

/*
 * Forward one call and hand the response to take. The return code is the
 * transport outcome and status the service's own, as for every stub.
 */
defw2_rc_t defw2_typed_call(struct defw2_typed_call *call,
			    defw2_status_t *status);

/* --- serving --------------------------------------------------------- */

/*
 * The data a typed API attaches to its registered methods. It starts with
 * the service it belongs to, which is how the shared code finds the runtime
 * without knowing the API.
 */
struct defw2_bound {
	struct defw2_service	*svc;
};

/* One typed call on the provider side, from decode to destroy. */
struct defw2_served {
	struct defw2_rt			*rt;
	margo_instance_id		mid;
	hg_handle_t			handle;
	const struct hg_info		*info;
	const struct defw2_method	*method;
	struct defw2_bound		*bound;
	struct defw2_call		call;
	defw2_wire_status_t		*status;	/* the answer's own */
	uint64_t			bulk_bytes;	/* pushed, for the span */
	uint64_t			queue_ns;	/* waited, for the span */
};

/*
 * A method's own part: convert the decoded request, run the service, fill
 * the answer. It runs only once the request decoded, the provider found its
 * service, and the caller's version matched.
 */
typedef void (*defw2_serve_fn)(struct defw2_served *served, void *in,
			       void *out);

/*
 * Serve one call. in and out are the handler's own wire structures. They are
 * zeroed here, so a decode that fails part way frees only what it decoded.
 */
void defw2_typed_serve(hg_handle_t handle, const struct defw2_method *method,
		       void *in, size_t in_size, void *out, size_t out_size,
		       defw2_serve_fn serve);

/* Answer with a failure. message is borrowed and must be a literal. */
void defw2_served_fail(struct defw2_served *served, defw2_rc_t code,
		       uint32_t category, const char *message);

/*
 * Does this call's service answer from its queue? A service in another
 * language does, and then the call goes to defw2_served_queue rather than
 * to an operations table.
 */
bool defw2_served_queued(const struct defw2_served *served);

/*
 * Hand the call to the service's queue and park until a consumer answers
 * it. The consumer answers into the call's own request, answer and storage,
 * so nothing is copied either way. Returns what defw2_served_finish takes:
 * the consumer's code, or why the queue would not take the call, with the
 * status set to say which.
 */
defw2_rc_t defw2_served_queue(struct defw2_served *served);

/*
 * Turn what the service returned into the answer's status. A status the
 * service set through the call wins. A failure without one is reported as a
 * provider failure. Returns true when the service answered, so the method
 * goes on to encode the answer.
 */
bool defw2_served_finish(struct defw2_served *served, defw2_rc_t rc);

/*
 * Push the call's bulk reply into the buffer the caller lent. Returns true
 * when it was delivered. Nothing is pushed when there is no reply, when the
 * caller lent nothing, or when the reply is larger than what was lent. The
 * caller then learns the size from the descriptor in the answer.
 */
bool defw2_served_push(struct defw2_served *served, hg_bulk_t sink);

/* One method an API registers, and the handler Margo runs for it. */
struct defw2_typed_entry {
	const struct defw2_method	*method;
	hg_rpc_cb_t			handler;
};

/*
 * Register an API's methods on the service's provider and attach bound to
 * every one of them. bound must come from malloc: Margo frees it when the
 * runtime finalizes, which is what makes destroying a service with a call in
 * flight safe. On failure bound is freed here, and a method that did
 * register answers DEFW2_CAT_NOT_FOUND, as one with no service would.
 */
defw2_rc_t defw2_typed_bind(struct defw2_service *svc,
			    const struct defw2_typed_entry *entries,
			    size_t count, struct defw2_bound *bound);

#endif /* DEFW2_TYPED_H */
