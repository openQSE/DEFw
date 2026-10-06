/*
 * The qfw.echo client stubs.
 *
 * These are what every typed stub will look like: start the span, fill the
 * header, create a handle on the binding's address, forward with a timeout,
 * copy the output out of Mercury's buffers and free them. The span is one
 * boolean test when profiling is off.
 */
#include <stdint.h>

#include <defw2/defw2_echo.h>

#include "defw2_trace.h"
#include "defw2_wire.h"

static hg_id_t echo_id(const defw2_binding_t *binding)
{
	return defw2_rpc_lookup(binding->rt, DEFW2_RPC_ECHO,
				binding->provider_id, hg_proc_defw2_echo_in_t,
				hg_proc_defw2_echo_out_t);
}

static hg_id_t echo_bulk_id(const defw2_binding_t *binding)
{
	return defw2_rpc_lookup(binding->rt, DEFW2_RPC_ECHO_BULK,
				binding->provider_id,
				hg_proc_defw2_echo_bulk_in_t,
				hg_proc_defw2_echo_bulk_out_t);
}

static double timeout_of(const defw2_call_opts_t *opts)
{
	return opts ? (double)opts->timeout_ms : 0.0;
}

static const char *trace_of(const defw2_call_opts_t *opts)
{
	return opts ? opts->traceparent : NULL;
}

defw2_rc_t defw2_echo(defw2_binding_t *binding, const void *payload,
		      size_t len, const defw2_call_opts_t *opts,
		      defw2_buffer_t *reply, defw2_status_t *status)
{
	defw2_echo_in_t in;
	defw2_echo_out_t out;
	struct defw2_trace trace;
	hg_handle_t handle = HG_HANDLE_NULL;
	uint32_t category = DEFW2_CAT_OK;
	uint64_t request_bytes = 0;
	uint64_t response_bytes = 0;
	struct defw2_rt *rt;
	int32_t code = 0;
	hg_return_t hret;
	defw2_rc_t rc;

	if (binding == NULL || reply == NULL || (payload == NULL && len > 0))
		return DEFW2_ERR_INVALID;
	if (len > DEFW2_EAGER_MAX)
		return DEFW2_ERR_INVALID;

	rt = binding->rt;
	reply->data = NULL;
	reply->len = 0;

	hret = margo_create(rt->mid, binding->addr, echo_id(binding), &handle);
	if (hret != HG_SUCCESS)
		return defw2_rc_from_hg(hret, NULL);

	defw2_trace_begin(rt, &trace, DEFW2_SPAN_CLIENT, trace_of(opts));
	/* The service's span becomes a child of this one, which is what the
	 * traceparent this call carries is for. */
	defw2_hdr_fill(rt, &in.hdr, DEFW2_API_VERSION,
		       trace.recording ? trace.traceparent : trace_of(opts));
	in.payload.len = len;
	/* The encoder only reads the payload, so a caller's const buffer is
	 * safe to hand to Mercury. */
	in.payload.data = (char *)(uintptr_t)payload;

	hret = margo_provider_forward_timed(binding->provider_id, handle, &in,
					    timeout_of(opts));
	if (hret != HG_SUCCESS) {
		rc = defw2_rc_from_hg(hret, &category);
		code = rc;
		defw2_log(rt, DEFW2_LOG_ERROR, "echo to %s: %s",
			  binding->address, HG_Error_to_string(hret));
		goto out;
	}
	request_bytes = HG_Get_input_payload_size(handle);

	/* Zeroed, so the free after a decode that fails part way only
	 * touches the fields it actually decoded. defw2_free_partial
	 * says why that free is not margo_free_output. */
	memset(&out, 0, sizeof(out));
	hret = margo_get_output(handle, &out);
	if (hret != HG_SUCCESS) {
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_echo_out_t, &out);
		rc = defw2_rc_from_hg(hret, &category);
		code = rc;
		goto out;
	}
	response_bytes = HG_Get_output_payload_size(handle);

	category = out.status.category;
	code = out.status.code;
	defw2_status_from_wire(status, &out.status);
	if (out.payload.len > 0) {
		reply->data = malloc(out.payload.len);
		if (reply->data == NULL) {
			margo_free_output(handle, &out);
			rc = DEFW2_ERR_NOMEM;
			goto out;
		}
		memcpy(reply->data, out.payload.data, out.payload.len);
		reply->len = out.payload.len;
	}
	margo_free_output(handle, &out);
	rc = DEFW2_OK;
out:
	if (trace.recording) {
		trace.span.api = DEFW2_API_ECHO;
		trace.span.method = "echo";
		trace.span.tier = DEFW2_TIER_TYPED;
		trace.span.request_bytes = request_bytes;
		trace.span.response_bytes = response_bytes;
		trace.span.category = category;
		trace.span.code = code;
		defw2_trace_end(rt, &trace);
	}
	margo_destroy(handle);
	return rc;
}

defw2_rc_t defw2_echo_bulk(defw2_binding_t *binding, const void *source,
			   void *sink, size_t len,
			   const defw2_call_opts_t *opts,
			   uint64_t *bytes_moved, defw2_status_t *status)
{
	defw2_echo_bulk_in_t in;
	defw2_echo_bulk_out_t out;
	struct defw2_trace trace = { 0 };
	hg_handle_t handle = HG_HANDLE_NULL;
	hg_bulk_t source_bulk = HG_BULK_NULL;
	hg_bulk_t sink_bulk = HG_BULK_NULL;
	bool shared = (source == sink);
	uint32_t category = DEFW2_CAT_OK;
	uint64_t request_bytes = 0;
	uint64_t response_bytes = 0;
	uint64_t moved = 0;
	struct defw2_rt *rt;
	hg_size_t size = len;
	int32_t code = 0;
	hg_return_t hret;
	defw2_rc_t rc;
	void *buffer;

	if (binding == NULL || source == NULL || sink == NULL)
		return DEFW2_ERR_INVALID;
	if (len == 0 || len > DEFW2_BULK_MAX)
		return DEFW2_ERR_INVALID;

	rt = binding->rt;
	if (bytes_moved != NULL)
		*bytes_moved = 0;

	/*
	 * One registration serves both directions when the caller echoes a
	 * buffer into itself, which is what a bandwidth measurement does.
	 */
	buffer = (void *)(uintptr_t)source;
	hret = margo_bulk_create(rt->mid, 1, &buffer, &size,
				 shared ? HG_BULK_READWRITE : HG_BULK_READ_ONLY,
				 &source_bulk);
	if (hret != HG_SUCCESS)
		return defw2_rc_from_hg(hret, NULL);

	if (shared) {
		sink_bulk = source_bulk;
	} else {
		hret = margo_bulk_create(rt->mid, 1, &sink, &size,
					 HG_BULK_WRITE_ONLY, &sink_bulk);
		if (hret != HG_SUCCESS) {
			margo_bulk_free(source_bulk);
			return defw2_rc_from_hg(hret, NULL);
		}
	}

	hret = margo_create(rt->mid, binding->addr, echo_bulk_id(binding),
			    &handle);
	if (hret != HG_SUCCESS) {
		rc = defw2_rc_from_hg(hret, &category);
		code = rc;
		goto out;
	}

	defw2_trace_begin(rt, &trace, DEFW2_SPAN_CLIENT, trace_of(opts));
	defw2_hdr_fill(rt, &in.hdr, DEFW2_API_VERSION,
		       trace.recording ? trace.traceparent : trace_of(opts));
	in.nbytes = len;
	in.source = source_bulk;
	in.sink = sink_bulk;

	hret = margo_provider_forward_timed(binding->provider_id, handle, &in,
					    timeout_of(opts));
	if (hret != HG_SUCCESS) {
		rc = defw2_rc_from_hg(hret, &category);
		code = rc;
		defw2_log(rt, DEFW2_LOG_ERROR, "echo_bulk to %s: %s",
			  binding->address, HG_Error_to_string(hret));
		goto out;
	}
	request_bytes = HG_Get_input_payload_size(handle);

	/* Zeroed, so the free after a decode that fails part way only
	 * touches the fields it actually decoded. defw2_free_partial
	 * says why that free is not margo_free_output. */
	memset(&out, 0, sizeof(out));
	hret = margo_get_output(handle, &out);
	if (hret != HG_SUCCESS) {
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_echo_bulk_out_t, &out);
		rc = defw2_rc_from_hg(hret, &category);
		code = rc;
		goto out;
	}
	response_bytes = HG_Get_output_payload_size(handle);
	category = out.status.category;
	code = out.status.code;
	moved = out.pulled + out.pushed;
	defw2_status_from_wire(status, &out.status);
	if (bytes_moved != NULL)
		*bytes_moved = moved;
	margo_free_output(handle, &out);
	rc = DEFW2_OK;
out:
	if (trace.recording) {
		trace.span.api = DEFW2_API_ECHO;
		trace.span.method = "echo_bulk";
		trace.span.tier = DEFW2_TIER_TYPED;
		trace.span.request_bytes = request_bytes;
		trace.span.response_bytes = response_bytes;
		trace.span.bulk_bytes = moved;
		trace.span.category = category;
		trace.span.code = code;
		defw2_trace_end(rt, &trace);
	}
	if (handle != HG_HANDLE_NULL)
		margo_destroy(handle);
	if (!shared)
		margo_bulk_free(sink_bulk);
	margo_bulk_free(source_bulk);
	return rc;
}
