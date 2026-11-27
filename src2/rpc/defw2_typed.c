/*
 * Typed methods: the steps every one of them takes. See defw2_typed.h.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_typed.h"

static double timeout_of(const defw2_call_opts_t *opts)
{
	return opts != NULL && opts->timeout_ms > 0 ?
		(double)opts->timeout_ms : 0.0;
}

static const char *trace_of(const defw2_call_opts_t *opts)
{
	return opts != NULL ? opts->traceparent : NULL;
}

/* --- calling --------------------------------------------------------- */

defw2_rc_t defw2_typed_call(struct defw2_typed_call *call,
			    defw2_status_t *status)
{
	const struct defw2_method *method;
	defw2_binding_t *binding;
	defw2_wire_status_t *wire;
	struct defw2_trace trace;
	hg_handle_t handle = HG_HANDLE_NULL;
	uint32_t category = DEFW2_CAT_OK;
	uint64_t request_bytes = 0;
	uint64_t response_bytes = 0;
	struct defw2_rt *rt;
	int32_t code = 0;
	hg_return_t hret;
	defw2_rc_t rc;
	hg_id_t id;

	if (call == NULL || call->binding == NULL || call->method == NULL ||
	    call->in == NULL || call->out == NULL)
		return DEFW2_ERR_INVALID;
	binding = call->binding;
	method = call->method;
	wire = call->out;
	rt = binding->rt;

	id = defw2_rpc_lookup(rt, method->rpc, binding->provider_id,
			      method->in_proc, method->out_proc);
	if (id == 0)
		return DEFW2_ERR_INTERNAL;
	hret = margo_create(rt->mid, binding->addr, id, &handle);
	if (hret != HG_SUCCESS)
		return defw2_rc_from_hg(hret, NULL);

	defw2_trace_begin(rt, &trace, DEFW2_SPAN_CLIENT, trace_of(call->opts));
	/* The service's span becomes a child of this one, which is what the
	 * traceparent the request carries is for. */
	defw2_hdr_fill(rt, call->in, method->version,
		       trace.recording ? trace.traceparent
				       : trace_of(call->opts));

	hret = margo_provider_forward_timed(binding->provider_id, handle,
					    call->in, timeout_of(call->opts));
	if (hret != HG_SUCCESS) {
		rc = defw2_rc_from_hg(hret, &category);
		code = rc;
		defw2_log(rt, DEFW2_LOG_ERROR, "%s.%s to %s: %s", method->api,
			  method->name, binding->address,
			  HG_Error_to_string(hret));
		goto out;
	}
	if (trace.recording)
		request_bytes = HG_Get_input_payload_size(handle);

	/* Zeroed, so the free after a decode that fails part way only
	 * touches the fields it actually decoded. defw2_free_partial says
	 * why that free is not margo_free_output. */
	memset(call->out, 0, call->out_size);
	hret = margo_get_output(handle, call->out);
	if (hret != HG_SUCCESS) {
		defw2_free_partial(rt->mid, method->out_proc, call->out);
		rc = defw2_rc_from_hg(hret, &category);
		code = rc;
		defw2_log(rt, DEFW2_LOG_ERROR, "%s.%s from %s: %s",
			  method->api, method->name, binding->address,
			  HG_Error_to_string(hret));
		goto out;
	}
	if (trace.recording)
		response_bytes = HG_Get_output_payload_size(handle);

	/* The service's own outcome is what the trace should show. */
	category = wire->category;
	code = wire->code;
	defw2_status_from_wire(status, wire);
	rc = call->take != NULL ? call->take(call) : DEFW2_OK;
	margo_free_output(handle, call->out);
out:
	if (trace.recording) {
		trace.span.api = method->api;
		trace.span.method = method->name;
		trace.span.tier = method->tier;
		trace.span.request_bytes = request_bytes;
		trace.span.response_bytes = response_bytes;
		trace.span.bulk_bytes = call->bulk_bytes;
		trace.span.category = category;
		trace.span.code = code;
	}
	defw2_trace_end(rt, &trace);
	margo_destroy(handle);
	return rc;
}

/* --- serving --------------------------------------------------------- */

void defw2_served_fail(struct defw2_served *served, defw2_rc_t code,
		       uint32_t category, const char *message)
{
	defw2_wire_status_set(served->status, code, category, message);
}

bool defw2_served_queued(const struct defw2_served *served)
{
	return served->bound != NULL &&
	       defw2_service_queued(served->bound->svc);
}

defw2_rc_t defw2_served_queue(struct defw2_served *served)
{
	defw2_rc_t rc;

	rc = defw2_service_dispatch_call(served->bound->svc, &served->call,
					 &served->queue_ns);
	if (rc == DEFW2_ERR_BUSY) {
		defw2_call_set_status(&served->call, rc,
				      DEFW2_CAT_PENDING_CAPACITY,
				      "the service is at capacity");
		return rc;
	}
	if (rc != DEFW2_OK) {
		defw2_call_set_status(&served->call, rc, DEFW2_CAT_NOT_FOUND,
				      "the service is not serving");
		return rc;
	}
	return served->call.status.code;
}

bool defw2_served_finish(struct defw2_served *served, defw2_rc_t rc)
{
	defw2_status_t *status = &served->call.status;

	/*
	 * A status the service set wins, even with DEFW2_OK returned, since
	 * saying why is the whole point of setting one. The message is
	 * borrowed until defw2_call_release, which comes after the respond.
	 */
	if (status->code != DEFW2_OK) {
		defw2_wire_status_set(served->status, status->code,
				      status->category, status->message);
		return false;
	}
	if (rc != DEFW2_OK) {
		defw2_wire_status_set(served->status, rc,
				      DEFW2_CAT_PROVIDER_FAILURE,
				      defw2_strerror(rc));
		return false;
	}
	return true;
}

bool defw2_served_push(struct defw2_served *served, hg_bulk_t sink)
{
	struct defw2_call *call = &served->call;
	hg_bulk_t local = HG_BULK_NULL;
	hg_size_t size;
	hg_return_t hret;
	void *buffer;

	if (call->bulk == NULL || call->bulk_len == 0 || sink == HG_BULK_NULL)
		return false;
	/*
	 * What the caller registered bounds the push, not what it said it
	 * lent. The two agree for any caller using the stubs.
	 */
	if (call->bulk_len > call->result_capacity ||
	    call->bulk_len > margo_bulk_get_size(sink))
		return false;

	buffer = call->bulk;
	size = call->bulk_len;
	hret = margo_bulk_create(served->mid, 1, &buffer, &size,
				 HG_BULK_READ_ONLY, &local);
	if (hret == HG_SUCCESS) {
		hret = margo_bulk_transfer(served->mid, HG_BULK_PUSH,
					   served->info->addr, sink, 0, local,
					   0, size);
		margo_bulk_free(local);
	}
	if (hret != HG_SUCCESS) {
		/*
		 * The service has already given the result up, so this is a
		 * failure the caller must hear about rather than a buffer that
		 * was too small, which it would answer by retrying.
		 */
		defw2_log(served->rt, DEFW2_LOG_ERROR, "%s.%s push of %zu: %s",
			  served->method->api, served->method->name,
			  (size_t)call->bulk_len, HG_Error_to_string(hret));
		defw2_served_fail(served, defw2_rc_from_hg(hret, NULL),
				  DEFW2_CAT_TRANSPORT,
				  "the result could not be pushed");
		return false;
	}
	served->bulk_bytes = size;
	return true;
}

void defw2_typed_serve(hg_handle_t handle, const struct defw2_method *method,
		       void *in, size_t in_size, void *out, size_t out_size,
		       defw2_serve_fn serve)
{
	uint64_t arrived_wall = 0, arrived_mono = 0, mark = 0;
	struct defw2_trace trace = { 0 };
	struct defw2_served served;
	hg_return_t hret;

	memset(&served, 0, sizeof(served));
	memset(in, 0, in_size);
	memset(out, 0, out_size);
	served.handle = handle;
	served.method = method;
	served.mid = margo_hg_handle_get_instance(handle);
	served.info = margo_get_info(handle);
	served.status = out;
	served.call.api = method->api;
	served.call.method = method->name;
	defw2_wire_status_ok(served.status);
	if (served.info != NULL)
		served.bound = margo_registered_data(served.mid,
						     served.info->id);
	if (served.bound != NULL)
		served.rt = served.bound->svc->rt;

	if (defw2_profiling(served.rt)) {
		arrived_wall = defw2_wall_ns();
		arrived_mono = defw2_mono_ns();
	}

	hret = margo_get_input(handle, in);
	if (hret != HG_SUCCESS) {
		/* A decode that fails part way still allocated the fields
		 * before the one it refused. defw2_free_partial says why
		 * this is not margo_free_input. */
		defw2_free_partial(served.mid, method->in_proc, in);
		defw2_served_fail(&served, DEFW2_ERR_INVALID,
				  DEFW2_CAT_INVALID_ARGUMENT,
				  "cannot decode request");
		margo_respond(handle, out);
		margo_destroy(handle);
		return;
	}

	/*
	 * The span joins the caller's trace and starts where the work did,
	 * rather than where the traceparent could first be read.
	 */
	if (arrived_mono != 0) {
		defw2_trace_begin(served.rt, &trace, DEFW2_SPAN_SERVER,
				  ((defw2_hdr_t *)in)->traceparent);
		defw2_trace_backdate(&trace, arrived_wall, arrived_mono);
		mark = defw2_mono_ns();
		trace.span.decode_ns = mark - arrived_mono;
	}
	/* What the handler's own work belongs under: this span when it is
	 * recorded, and the caller's otherwise, so a service that traces by
	 * itself still joins the caller's trace. */
	served.call.traceparent = trace.recording ? trace.traceparent :
		((defw2_hdr_t *)in)->traceparent;

	if (served.bound == NULL)
		defw2_served_fail(&served, DEFW2_ERR_NOT_FOUND,
				  DEFW2_CAT_NOT_FOUND,
				  "nothing serves this method on this provider");
	else if (!defw2_hdr_compatible(in, method->version))
		defw2_served_fail(&served, DEFW2_ERR_VERSION,
				  DEFW2_CAT_VERSION_MISMATCH,
				  "unsupported api version");
	else
		serve(&served, in, out);

	if (trace.recording) {
		trace.span.handler_ns = defw2_mono_ns() - mark;
		/* The wait for a consumer is not the service's own time, so
		 * the two are reported apart. */
		if (served.queue_ns > 0 &&
		    served.queue_ns < trace.span.handler_ns) {
			trace.span.queue_ns = served.queue_ns;
			trace.span.handler_ns -= served.queue_ns;
		}
		mark = defw2_mono_ns();
	}

	hret = margo_respond(handle, out);
	if (hret != HG_SUCCESS && served.rt != NULL)
		defw2_log(served.rt, DEFW2_LOG_ERROR, "%s.%s respond: %s",
			  method->api, method->name, HG_Error_to_string(hret));

	if (trace.recording) {
		trace.span.encode_ns = defw2_mono_ns() - mark;
		trace.span.request_bytes = HG_Get_input_payload_size(handle);
		trace.span.response_bytes = HG_Get_output_payload_size(handle);
		trace.span.bulk_bytes = served.bulk_bytes;
		trace.span.api = method->api;
		/* A document names its own method once it is decoded. */
		trace.span.method = served.call.method;
		trace.span.tier = method->tier;
		trace.span.category = served.status->category;
		trace.span.code = served.status->code;
		defw2_trace_end(served.rt, &trace);
	}

	/* Released after the respond, because the answer borrowed from all
	 * of it. */
	defw2_call_release(&served.call);
	margo_free_input(handle, in);
	margo_destroy(handle);
}

defw2_rc_t defw2_typed_bind(struct defw2_service *svc,
			    const struct defw2_typed_entry *entries,
			    size_t count, struct defw2_bound *bound)
{
	hg_id_t ids[DEFW2_RPC_CACHE_MAX];
	size_t i;

	if (svc == NULL || entries == NULL || bound == NULL || count == 0 ||
	    count > DEFW2_RPC_CACHE_MAX) {
		free(bound);
		return DEFW2_ERR_INVALID;
	}
	bound->svc = svc;

	/* Every method first, so a failure leaves nothing owning bound. */
	for (i = 0; i < count; i++) {
		ids[i] = margo_provider_register_name(svc->rt->mid,
						      entries[i].method->rpc,
						      entries[i].method->in_proc,
						      entries[i].method->out_proc,
						      entries[i].handler,
						      svc->provider_id,
						      ABT_POOL_NULL);
		if (ids[i] == 0) {
			defw2_log(svc->rt, DEFW2_LOG_ERROR,
				  "cannot register %s on provider %u",
				  entries[i].method->rpc, svc->provider_id);
			free(bound);
			return DEFW2_ERR_INTERNAL;
		}
	}

	/*
	 * One owner for bound. Margo frees it through the first method at
	 * finalize, which is why the rest attach the same pointer with no
	 * free callback.
	 */
	for (i = 0; i < count; i++)
		margo_register_data(svc->rt->mid, ids[i], bound,
				    i == 0 ? free : NULL);

	defw2_log(svc->rt, DEFW2_LOG_MESSAGE, "%s bound on provider %u of %s",
		  entries[0].method->api, svc->provider_id, svc->service_id);
	return DEFW2_OK;
}
