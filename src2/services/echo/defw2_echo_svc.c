/*
 * The qfw.echo service.
 *
 * Echo is the reference service and the benchmarks' subject. Its own work is
 * nothing, so what W1, W2 and W3 measure is the framework: decode, dispatch,
 * the bulk path and encode. When the service is queued, echo is answered by
 * whatever language is draining the queue, and echo_bulk stays in C, since
 * handing a quarter of a gigabyte to an interpreter measures the
 * interpreter. The handlers are also the worked example of what
 * a typed handler looks like, which is why they check the header version and
 * the payload bounds before they touch anything.
 */
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2_echo.h>

#include "defw2_host.h"
#include "defw2_trace.h"
#include "defw2_wire.h"

struct defw2_echo_binding {
	defw2_service_t		*svc;
	defw2_echo_ops_t	ops;
};

static struct defw2_echo_binding *bound_for(hg_handle_t handle,
					    margo_instance_id mid)
{
	const struct hg_info *info = margo_get_info(handle);

	if (info == NULL)
		return NULL;
	return (struct defw2_echo_binding *)margo_registered_data(mid,
								  info->id);
}

static void defw2_echo_ult(hg_handle_t handle)
{
	margo_instance_id mid = margo_hg_handle_get_instance(handle);
	struct defw2_echo_binding *bound = bound_for(handle, mid);
	struct defw2_rt *rt = bound ? bound->svc->rt : NULL;
	uint64_t arrived_wall = 0, arrived_mono = 0, mark = 0;
	struct defw2_trace trace = { 0 };
	defw2_status_t answer = { 0 };
	uint64_t waited = 0;
	void *reply = NULL;
	size_t reply_len = 0;
	defw2_echo_in_t in;
	defw2_echo_out_t out;
	hg_return_t hret;

	if (defw2_profiling(rt)) {
		arrived_wall = defw2_wall_ns();
		arrived_mono = defw2_mono_ns();
	}
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	defw2_wire_status_ok(&out.status);

	hret = margo_get_input(handle, &in);
	if (hret != HG_SUCCESS) {
		/* A decode that fails part way still allocated the fields
		 * before the one it refused. defw2_free_partial says why
		 * this is not margo_free_input. */
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_echo_in_t, &in);
		/* Nothing usable decoded, so a status is the whole answer. */
		defw2_wire_status_set(&out.status, DEFW2_ERR_INVALID,
				      DEFW2_CAT_INVALID_ARGUMENT,
				      "cannot decode request");
		margo_respond(handle, &out);
		margo_destroy(handle);
		return;
	}

	/*
	 * The span joins the caller's trace, and starts where the work did
	 * rather than where the traceparent could first be read.
	 */
	if (arrived_mono != 0) {
		defw2_trace_begin(rt, &trace, DEFW2_SPAN_SERVER,
				  in.hdr.traceparent);
		defw2_trace_backdate(&trace, arrived_wall, arrived_mono);
		mark = defw2_mono_ns();
		trace.span.decode_ns = mark - arrived_mono;
		trace.span.request_bytes = in.payload.len;
	}

	if (bound == NULL) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_NOT_FOUND,
				      DEFW2_CAT_NOT_FOUND,
				      "no echo service on this provider");
	} else if (!defw2_hdr_compatible(&in.hdr, DEFW2_API_VERSION)) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_VERSION,
				      DEFW2_CAT_VERSION_MISMATCH,
				      "unsupported api version");
	} else if (defw2_service_queued(bound->svc)) {
		/*
		 * A service in another language answers from the queue.
		 * This ULT parks until it does, which costs a hand-off and
		 * leaves the execution stream free for other calls.
		 */
		defw2_rc_t rc = defw2_service_dispatch(bound->svc,
						       DEFW2_API_ECHO, "echo",
						       in.payload.data,
						       in.payload.len, &reply,
						       &reply_len, &answer,
						       &waited);

		if (rc == DEFW2_ERR_BUSY)
			defw2_wire_status_set(&out.status, rc,
					      DEFW2_CAT_PENDING_CAPACITY,
					      "the service is at capacity");
		else if (rc != DEFW2_OK)
			defw2_wire_status_set(&out.status, rc,
					      DEFW2_CAT_NOT_FOUND,
					      "the service is not serving");
		else if (answer.code != DEFW2_OK)
			defw2_wire_status_set(&out.status, answer.code,
					      answer.category, answer.message);
		else {
			out.payload.data = (char *)reply;
			out.payload.len = reply_len;
		}
	} else if (bound->ops.echo != NULL) {
		defw2_rc_t rc = bound->ops.echo(bound->ops.ctx,
						in.payload.data,
						in.payload.len, &reply,
						&reply_len);

		/* A reply too large to carry is the service's mistake, so
		 * it is reported as one rather than truncated. */
		if (rc == DEFW2_OK && reply_len > DEFW2_EAGER_MAX)
			rc = DEFW2_ERR_INVALID;
		if (rc != DEFW2_OK) {
			free(reply);
			reply = NULL;
			defw2_wire_status_set(&out.status, rc,
					      DEFW2_CAT_PROVIDER_FAILURE,
					      defw2_strerror(rc));
		} else {
			out.payload.data = (char *)reply;
			out.payload.len = reply_len;
		}
	} else {
		/*
		 * The built-in echo answers out of Mercury's own decode
		 * buffer, so the reference service copies nothing.
		 */
		out.payload.data = in.payload.data;
		out.payload.len = in.payload.len;
	}

	if (trace.recording) {
		trace.span.handler_ns = defw2_mono_ns() - mark;
		/* The wait for a consumer is not the service's own time, so
		 * the two are reported apart. */
		if (waited > 0 && waited < trace.span.handler_ns) {
			trace.span.queue_ns = waited;
			trace.span.handler_ns -= waited;
		}
		mark = defw2_mono_ns();
	}

	hret = margo_respond(handle, &out);
	if (hret != HG_SUCCESS && rt != NULL)
		defw2_log(rt, DEFW2_LOG_ERROR, "echo respond: %s",
			  HG_Error_to_string(hret));

	if (trace.recording) {
		trace.span.encode_ns = defw2_mono_ns() - mark;
		trace.span.response_bytes = out.payload.len;
		trace.span.api = DEFW2_API_ECHO;
		trace.span.method = "echo";
		trace.span.tier = DEFW2_TIER_TYPED;
		trace.span.category = out.status.category;
		trace.span.code = out.status.code;
		defw2_trace_end(rt, &trace);
	}

	free(reply);
	/* Freed after the respond, because the status message was borrowed
	 * for it. */
	defw2_status_free(&answer);
	margo_free_input(handle, &in);
	margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(defw2_echo_ult)

static void defw2_echo_bulk_ult(hg_handle_t handle)
{
	margo_instance_id mid = margo_hg_handle_get_instance(handle);
	struct defw2_echo_binding *bound = bound_for(handle, mid);
	const struct hg_info *info = margo_get_info(handle);
	struct defw2_rt *rt = bound ? bound->svc->rt : NULL;
	uint64_t arrived_wall = 0, arrived_mono = 0, mark = 0;
	struct defw2_trace trace = { 0 };
	hg_bulk_t local = HG_BULK_NULL;
	void *buffer = NULL;
	defw2_echo_bulk_in_t in;
	defw2_echo_bulk_out_t out;
	hg_return_t hret;
	hg_size_t size;

	if (defw2_profiling(rt)) {
		arrived_wall = defw2_wall_ns();
		arrived_mono = defw2_mono_ns();
	}
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	defw2_wire_status_ok(&out.status);

	hret = margo_get_input(handle, &in);
	if (hret != HG_SUCCESS) {
		/* A decode that fails part way still allocated the fields
		 * before the one it refused. defw2_free_partial says why
		 * this is not margo_free_input. */
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_echo_bulk_in_t, &in);
		defw2_wire_status_set(&out.status, DEFW2_ERR_INVALID,
				      DEFW2_CAT_INVALID_ARGUMENT,
				      "cannot decode request");
		margo_respond(handle, &out);
		margo_destroy(handle);
		return;
	}

	if (arrived_mono != 0) {
		defw2_trace_begin(rt, &trace, DEFW2_SPAN_SERVER,
				  in.hdr.traceparent);
		defw2_trace_backdate(&trace, arrived_wall, arrived_mono);
		mark = defw2_mono_ns();
		trace.span.decode_ns = mark - arrived_mono;
	}

	if (bound == NULL) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_NOT_FOUND,
				      DEFW2_CAT_NOT_FOUND,
				      "no echo service on this provider");
		goto respond;
	}
	if (!defw2_hdr_compatible(&in.hdr, DEFW2_API_VERSION)) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_VERSION,
				      DEFW2_CAT_VERSION_MISMATCH,
				      "unsupported api version");
		goto respond;
	}
	/*
	 * The caller names the size, so the handler is what stands between a
	 * wrong number and an allocation the size of the machine.
	 */
	if (in.nbytes == 0 || in.nbytes > DEFW2_BULK_MAX ||
	    in.source == HG_BULK_NULL || in.sink == HG_BULK_NULL) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_INVALID,
				      DEFW2_CAT_INVALID_ARGUMENT,
				      "bulk size or handle out of range");
		goto respond;
	}

	size = in.nbytes;
	buffer = malloc(size);
	if (buffer == NULL) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_NOMEM,
				      DEFW2_CAT_PROVIDER_FAILURE,
				      "no memory for the bulk buffer");
		goto respond;
	}
	hret = margo_bulk_create(mid, 1, &buffer, &size, HG_BULK_READWRITE,
				 &local);
	if (hret != HG_SUCCESS) {
		defw2_wire_status_set(&out.status, DEFW2_ERR_TRANSPORT,
				      DEFW2_CAT_TRANSPORT,
				      "cannot register the bulk buffer");
		goto respond;
	}

	hret = margo_bulk_transfer(mid, HG_BULK_PULL, info->addr, in.source, 0,
				   local, 0, size);
	if (hret != HG_SUCCESS) {
		defw2_wire_status_set(&out.status,
				      defw2_rc_from_hg(hret, NULL),
				      DEFW2_CAT_TRANSPORT, "bulk pull failed");
		goto respond;
	}
	out.pulled = size;

	if (bound->ops.transform != NULL) {
		defw2_rc_t rc = bound->ops.transform(bound->ops.ctx, buffer,
						     size);

		if (rc != DEFW2_OK) {
			defw2_wire_status_set(&out.status, rc,
					      DEFW2_CAT_PROVIDER_FAILURE,
					      defw2_strerror(rc));
			goto respond;
		}
	}

	hret = margo_bulk_transfer(mid, HG_BULK_PUSH, info->addr, in.sink, 0,
				   local, 0, size);
	if (hret != HG_SUCCESS) {
		defw2_wire_status_set(&out.status,
				      defw2_rc_from_hg(hret, NULL),
				      DEFW2_CAT_TRANSPORT, "bulk push failed");
		goto respond;
	}
	out.pushed = size;

respond:
	if (trace.recording) {
		/* Pull, transform and push are the handler's work here. */
		trace.span.handler_ns = defw2_mono_ns() - mark;
		mark = defw2_mono_ns();
	}

	hret = margo_respond(handle, &out);
	if (hret != HG_SUCCESS && rt != NULL)
		defw2_log(rt, DEFW2_LOG_ERROR,
			  "echo_bulk respond: %s", HG_Error_to_string(hret));

	if (trace.recording) {
		trace.span.encode_ns = defw2_mono_ns() - mark;
		trace.span.bulk_bytes = out.pulled + out.pushed;
		trace.span.api = DEFW2_API_ECHO;
		trace.span.method = "echo_bulk";
		trace.span.tier = DEFW2_TIER_TYPED;
		trace.span.category = out.status.category;
		trace.span.code = out.status.code;
		defw2_trace_end(rt, &trace);
	}

	if (local != HG_BULK_NULL)
		margo_bulk_free(local);
	free(buffer);
	margo_free_input(handle, &in);
	margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(defw2_echo_bulk_ult)

defw2_rc_t defw2_echo_bind(defw2_service_t *svc, const defw2_echo_ops_t *ops)
{
	struct defw2_echo_binding *bound;
	hg_id_t bulk_id;
	hg_id_t id;

	if (svc == NULL)
		return DEFW2_ERR_INVALID;

	bound = calloc(1, sizeof(*bound));
	if (bound == NULL)
		return DEFW2_ERR_NOMEM;
	bound->svc = svc;
	if (ops != NULL)
		bound->ops = *ops;

	id = MARGO_REGISTER_PROVIDER(svc->rt->mid, DEFW2_RPC_ECHO,
				     defw2_echo_in_t, defw2_echo_out_t,
				     defw2_echo_ult, svc->provider_id,
				     ABT_POOL_NULL);
	bulk_id = MARGO_REGISTER_PROVIDER(svc->rt->mid, DEFW2_RPC_ECHO_BULK,
					  defw2_echo_bulk_in_t,
					  defw2_echo_bulk_out_t,
					  defw2_echo_bulk_ult,
					  svc->provider_id, ABT_POOL_NULL);
	if (id == 0 || bulk_id == 0) {
		defw2_log(svc->rt, DEFW2_LOG_ERROR,
			  "cannot register %s on provider %u", DEFW2_API_ECHO,
			  svc->provider_id);
		free(bound);
		return DEFW2_ERR_INTERNAL;
	}

	/*
	 * One owner for the operations table. Margo releases it when the
	 * runtime finalizes, which is why the second registration attaches
	 * the same pointer with no free callback, and why destroying the
	 * service while a call is in flight is safe.
	 */
	margo_register_data(svc->rt->mid, id, bound, free);
	margo_register_data(svc->rt->mid, bulk_id, bound, NULL);

	defw2_log(svc->rt, DEFW2_LOG_MESSAGE, "%s bound on provider %u of %s",
		  DEFW2_API_ECHO, svc->provider_id, svc->service_id);
	return DEFW2_OK;
}
