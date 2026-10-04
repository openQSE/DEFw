/*
 * The QPM client stubs.
 *
 * Each stub is its conversions and nothing else: fill the wire request from
 * the public one, and copy the wire answer into an arena the caller frees.
 * Forwarding, timeouts, the header and the span are defw2_typed_call's.
 *
 * A result buffer is registered for the length of one call and released
 * before the stub returns, so the caller's memory is never left registered
 * with Mercury.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_qpm_wire.h"

static void ctx_to_wire(const defw2_qpm_ctx_t *ctx, defw2_qpm_wire_ctx_t *w)
{
	w->reservation_id = ctx->reservation_id;
	w->token = ctx->token;
}

/*
 * Copy a decoded string into the answer's arena. An absent string stays
 * absent, and a copy that fails fails the whole answer.
 */
static bool copy_str(struct defw2_arena *arena, const char *src,
		     const char **dst)
{
	*dst = NULL;
	if (src == NULL)
		return true;
	*dst = defw2_arena_strdup(arena, src);
	return *dst != NULL;
}

static struct defw2_arena *answer_arena(void **slot)
{
	struct defw2_arena *arena = calloc(1, sizeof(*arena));

	*slot = arena;
	return arena;
}

static void arena_drop(void **slot)
{
	struct defw2_arena *arena = *slot;

	if (arena == NULL)
		return;
	defw2_arena_free(arena);
	free(arena);
	*slot = NULL;
}

/* --- the frees ------------------------------------------------------- */

void defw2_qpm_service_status_free(defw2_qpm_service_status_t *status)
{
	if (status == NULL)
		return;
	arena_drop(&status->arena);
	memset(status, 0, sizeof(*status));
}

void defw2_qpm_decision_free(defw2_qpm_decision_t *decision)
{
	if (decision == NULL)
		return;
	arena_drop(&decision->arena);
	memset(decision, 0, sizeof(*decision));
}

void defw2_qpm_reservation_free(defw2_qpm_reservation_t *reservation)
{
	if (reservation == NULL)
		return;
	arena_drop(&reservation->arena);
	memset(reservation, 0, sizeof(*reservation));
}

void defw2_qpm_task_free(defw2_qpm_task_t *task)
{
	if (task == NULL)
		return;
	arena_drop(&task->arena);
	memset(task, 0, sizeof(*task));
}

/* --- result buffers -------------------------------------------------- */

/*
 * Register what the caller lent, writable by the service. A buffer that
 * lends nothing leaves the wire's handle null, which is how a request says
 * so.
 */
static defw2_rc_t lend(struct defw2_rt *rt, defw2_result_buffer_t *result,
		       defw2_wire_result_t *w)
{
	hg_size_t size;
	hg_return_t hret;
	void *data;

	w->capacity = 0;
	w->handle = HG_BULK_NULL;
	if (result == NULL || result->data == NULL || result->capacity == 0)
		return DEFW2_OK;
	if (result->capacity > DEFW2_BULK_MAX)
		return DEFW2_ERR_INVALID;

	data = result->data;
	size = result->capacity;
	hret = margo_bulk_create(rt->mid, 1, &data, &size, HG_BULK_WRITE_ONLY,
				 &w->handle);
	if (hret != HG_SUCCESS)
		return defw2_rc_from_hg(hret, NULL);
	w->capacity = result->capacity;
	return DEFW2_OK;
}

static void unlend(defw2_wire_result_t *w)
{
	if (w->handle != HG_BULK_NULL)
		margo_bulk_free(w->handle);
	w->handle = HG_BULK_NULL;
}

/* --- control --------------------------------------------------------- */

static defw2_rc_t take_status(struct defw2_typed_call *call)
{
	defw2_qpm_status_out_t *w = call->out;
	defw2_qpm_service_status_t *out = call->arg;
	struct defw2_arena *arena = answer_arena(&out->arena);

	if (arena == NULL)
		return DEFW2_ERR_NOMEM;
	out->ready = w->ready != 0;
	out->initialized = w->initialized != 0;
	out->accepting_requests = w->accepting_requests != 0;
	out->provider_ready = w->provider_ready != 0;
	out->active_task_count = w->active_task_count;
	out->active_reservation_count = w->active_reservation_count;
	if (!copy_str(arena, w->state, &out->state) ||
	    !copy_str(arena, w->extra, &out->extra))
		return DEFW2_ERR_NOMEM;
	return DEFW2_OK;
}

static defw2_rc_t control_call(const struct defw2_method *method,
			       defw2_binding_t *qpm,
			       const defw2_qpm_ctx_t *req,
			       const defw2_call_opts_t *opts,
			       defw2_qpm_service_status_t *out,
			       defw2_status_t *status)
{
	struct defw2_typed_call call;
	defw2_qpm_status_out_t wout;
	defw2_qpm_ctx_in_t in;

	if (qpm == NULL || req == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	memset(out, 0, sizeof(*out));
	memset(&in, 0, sizeof(in));
	ctx_to_wire(req, &in.ctx);

	memset(&call, 0, sizeof(call));
	call.binding = qpm;
	call.method = method;
	call.opts = opts;
	call.in = &in;
	call.out = &wout;
	call.out_size = sizeof(wout);
	call.take = take_status;
	call.arg = out;
	return defw2_typed_call(&call, status);
}

defw2_rc_t defw2_qpm_is_ready(defw2_binding_t *qpm,
			      const defw2_qpm_ctx_t *req,
			      const defw2_call_opts_t *opts,
			      defw2_qpm_service_status_t *out,
			      defw2_status_t *status)
{
	return control_call(&defw2_qpm_m_is_ready, qpm, req, opts, out,
			    status);
}

defw2_rc_t defw2_qpm_get_service_status(defw2_binding_t *qpm,
					const defw2_qpm_ctx_t *req,
					const defw2_call_opts_t *opts,
					defw2_qpm_service_status_t *out,
					defw2_status_t *status)
{
	return control_call(&defw2_qpm_m_get_service_status, qpm, req, opts,
			    out, status);
}

/* --- admission ------------------------------------------------------- */

static defw2_rc_t take_decision(struct defw2_typed_call *call)
{
	defw2_qpm_decision_out_t *w = call->out;
	defw2_qpm_decision_t *out = call->arg;
	struct defw2_arena *arena = answer_arena(&out->arena);

	if (arena == NULL)
		return DEFW2_ERR_NOMEM;
	out->reservation_id = w->reservation_id;
	out->request_id = w->request_id;
	out->reason_code = w->reason_code;
	out->retry_after_ns = w->retry_after_ns;
	if (!copy_str(arena, w->decision, &out->decision) ||
	    !copy_str(arena, w->reason, &out->reason) ||
	    !copy_str(arena, w->message, &out->message) ||
	    !copy_str(arena, w->extra, &out->extra))
		return DEFW2_ERR_NOMEM;
	return DEFW2_OK;
}

static defw2_rc_t decision_call(const struct defw2_method *method,
				defw2_binding_t *qpm, void *in,
				const defw2_call_opts_t *opts,
				defw2_qpm_decision_t *out,
				defw2_status_t *status)
{
	struct defw2_typed_call call;
	defw2_qpm_decision_out_t wout;

	memset(&call, 0, sizeof(call));
	call.binding = qpm;
	call.method = method;
	call.opts = opts;
	call.in = in;
	call.out = &wout;
	call.out_size = sizeof(wout);
	call.take = take_decision;
	call.arg = out;
	return defw2_typed_call(&call, status);
}

defw2_rc_t defw2_qpm_reserve(defw2_binding_t *qpm,
			     const defw2_qpm_reserve_req_t *req,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_decision_t *out, defw2_status_t *status)
{
	defw2_qpm_reserve_in_t in;

	if (qpm == NULL || req == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	memset(out, 0, sizeof(*out));
	memset(&in, 0, sizeof(in));
	ctx_to_wire(&req->ctx, &in.ctx);
	in.request_id = req->request_id;
	in.user = req->user;
	in.job_id = req->job_id;
	in.allocation_id = req->allocation_id;
	in.target_device_id = req->target_device_id;
	in.scope_id = req->scope_id;
	in.workload_kind = req->workload_kind;
	in.num_qubits = req->num_qubits;
	in.walltime_ns = req->walltime_ns;
	in.ttl_ns = req->ttl_ns;
	in.has_task_class = req->has_task_class ? 1 : 0;
	if (req->has_task_class) {
		in.task_class.count = req->task_class.count;
		in.task_class.qubit_count = req->task_class.qubit_count;
		in.task_class.depth = req->task_class.depth;
		in.task_class.one_q_gate_count =
			req->task_class.one_q_gate_count;
		in.task_class.two_q_gate_count =
			req->task_class.two_q_gate_count;
		in.task_class.shots = req->task_class.shots;
		in.task_class.measurement_count =
			req->task_class.measurement_count;
	}
	in.extra = req->extra;
	return decision_call(&defw2_qpm_m_reserve, qpm, &in, opts, out,
			     status);
}

defw2_rc_t defw2_qpm_renew(defw2_binding_t *qpm,
			   const defw2_qpm_renew_req_t *req,
			   const defw2_call_opts_t *opts,
			   defw2_qpm_decision_t *out, defw2_status_t *status)
{
	defw2_qpm_renew_in_t in;

	if (qpm == NULL || req == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	memset(out, 0, sizeof(*out));
	memset(&in, 0, sizeof(in));
	ctx_to_wire(&req->ctx, &in.ctx);
	in.ttl_ns = req->ttl_ns;
	in.extra = req->extra;
	return decision_call(&defw2_qpm_m_renew, qpm, &in, opts, out, status);
}

static defw2_rc_t close_call(const struct defw2_method *method,
			     defw2_binding_t *qpm,
			     const defw2_qpm_close_req_t *req,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_decision_t *out, defw2_status_t *status)
{
	defw2_qpm_close_in_t in;

	if (qpm == NULL || req == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	memset(out, 0, sizeof(*out));
	memset(&in, 0, sizeof(in));
	ctx_to_wire(&req->ctx, &in.ctx);
	in.reason_code = req->reason_code;
	return decision_call(method, qpm, &in, opts, out, status);
}

defw2_rc_t defw2_qpm_release(defw2_binding_t *qpm,
			     const defw2_qpm_close_req_t *req,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_decision_t *out, defw2_status_t *status)
{
	return close_call(&defw2_qpm_m_release, qpm, req, opts, out, status);
}

defw2_rc_t defw2_qpm_cancel(defw2_binding_t *qpm,
			    const defw2_qpm_close_req_t *req,
			    const defw2_call_opts_t *opts,
			    defw2_qpm_decision_t *out, defw2_status_t *status)
{
	return close_call(&defw2_qpm_m_cancel, qpm, req, opts, out, status);
}

static defw2_rc_t take_reservation(struct defw2_typed_call *call)
{
	defw2_qpm_reservation_out_t *w = call->out;
	defw2_qpm_reservation_t *out = call->arg;
	struct defw2_arena *arena = answer_arena(&out->arena);

	if (arena == NULL)
		return DEFW2_ERR_NOMEM;
	out->reservation_id = w->reservation_id;
	out->created_at_ns = w->created_at_ns;
	out->expires_at_ns = w->expires_at_ns;
	if (!copy_str(arena, w->state, &out->state) ||
	    !copy_str(arena, w->extra, &out->extra))
		return DEFW2_ERR_NOMEM;
	return DEFW2_OK;
}

defw2_rc_t defw2_qpm_get_reservation(defw2_binding_t *qpm,
				     const defw2_qpm_ctx_t *req,
				     const defw2_call_opts_t *opts,
				     defw2_qpm_reservation_t *out,
				     defw2_status_t *status)
{
	struct defw2_typed_call call;
	defw2_qpm_reservation_out_t wout;
	defw2_qpm_ctx_in_t in;

	if (qpm == NULL || req == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	memset(out, 0, sizeof(*out));
	memset(&in, 0, sizeof(in));
	ctx_to_wire(req, &in.ctx);

	memset(&call, 0, sizeof(call));
	call.binding = qpm;
	call.method = &defw2_qpm_m_get_reservation;
	call.opts = opts;
	call.in = &in;
	call.out = &wout;
	call.out_size = sizeof(wout);
	call.take = take_reservation;
	call.arg = out;
	return defw2_typed_call(&call, status);
}

/* --- execution ------------------------------------------------------- */

struct task_take {
	defw2_qpm_task_t		*out;
	const defw2_result_buffer_t	*result;
};

static defw2_rc_t take_task(struct defw2_typed_call *call)
{
	defw2_qpm_task_out_t *w = call->out;
	struct task_take *t = call->arg;
	defw2_qpm_task_t *out = t->out;
	struct defw2_arena *arena = answer_arena(&out->arena);
	defw2_rc_t rc;

	if (arena == NULL)
		return DEFW2_ERR_NOMEM;
	rc = defw2_qpm_task_from_wire(arena, &w->task, out);
	if (rc != DEFW2_OK || !w->task.statevector.delivered)
		return rc;

	/*
	 * The service says the data is in the caller's buffer. Believe it
	 * only if the description is self-consistent and fits what was lent,
	 * because a caller about to index the buffer by shape needs both.
	 */
	if (t->result == NULL || t->result->data == NULL ||
	    out->statevector.nbytes > t->result->capacity ||
	    !defw2_tensor_valid(&out->statevector)) {
		defw2_log(call->binding->rt, DEFW2_LOG_ERROR,
			  "%s.%s: the service described a statevector that "
			  "does not fit what was lent", call->method->api,
			  call->method->name);
		return DEFW2_ERR_INVALID;
	}
	out->statevector_delivered = true;
	call->bulk_bytes = out->statevector.nbytes;
	return DEFW2_OK;
}

static defw2_rc_t task_call(const struct defw2_method *method,
			    defw2_binding_t *qpm, void *in,
			    defw2_wire_result_t *lent,
			    const defw2_result_buffer_t *result,
			    const defw2_call_opts_t *opts,
			    defw2_qpm_task_t *out, defw2_status_t *status)
{
	struct defw2_typed_call call;
	defw2_qpm_task_out_t wout;
	struct task_take t = { out, result };
	defw2_rc_t rc;

	memset(&call, 0, sizeof(call));
	call.binding = qpm;
	call.method = method;
	call.opts = opts;
	call.in = in;
	call.out = &wout;
	call.out_size = sizeof(wout);
	call.take = take_task;
	call.arg = &t;
	rc = defw2_typed_call(&call, status);
	if (lent != NULL)
		unlend(lent);
	return rc;
}

static defw2_rc_t run_call(const struct defw2_method *method,
			   defw2_binding_t *qpm,
			   const defw2_qpm_run_req_t *req,
			   defw2_result_buffer_t *result,
			   const defw2_call_opts_t *opts,
			   defw2_qpm_task_t *out, defw2_status_t *status)
{
	defw2_qpm_run_in_t in;
	defw2_rc_t rc;

	if (qpm == NULL || req == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	if (req->circuit.data == NULL && req->circuit.len > 0)
		return DEFW2_ERR_INVALID;
	/* Anything larger belongs in a bulk transfer, which runs do not
	 * take yet. Say so here rather than as a failed encode. */
	if (req->circuit.len > DEFW2_EAGER_MAX)
		return DEFW2_ERR_INVALID;
	memset(out, 0, sizeof(*out));
	memset(&in, 0, sizeof(in));
	ctx_to_wire(&req->ctx, &in.ctx);
	in.format = req->circuit.format;
	in.circuit.len = req->circuit.len;
	/* The encoder only reads the circuit, so a caller's const buffer is
	 * safe to hand to Mercury. */
	in.circuit.data = (char *)(uintptr_t)req->circuit.data;
	in.num_qubits = req->num_qubits;
	in.num_shots = req->num_shots;
	in.compiler = req->compiler;
	in.return_statevector = req->return_statevector ? 1 : 0;
	in.has_timeout = req->has_timeout ? 1 : 0;
	in.timeout_ms = req->has_timeout ? req->timeout_ms : 0;
	in.cancel_on_timeout = req->cancel_on_timeout ? 1 : 0;
	in.extra = req->extra;

	rc = lend(qpm->rt, result, &in.result);
	if (rc != DEFW2_OK)
		return rc;
	return task_call(method, qpm, &in, &in.result, result, opts, out,
			 status);
}

defw2_rc_t defw2_qpm_async_run(defw2_binding_t *qpm,
			       const defw2_qpm_run_req_t *req,
			       const defw2_call_opts_t *opts,
			       defw2_qpm_task_t *out, defw2_status_t *status)
{
	/* The result exists only once the task completes, so read_cq is
	 * where a buffer is lent, not here. */
	return run_call(&defw2_qpm_m_async_run, qpm, req, NULL, opts, out,
			status);
}

defw2_rc_t defw2_qpm_sync_run(defw2_binding_t *qpm,
			      const defw2_qpm_run_req_t *req,
			      defw2_result_buffer_t *result,
			      const defw2_call_opts_t *opts,
			      defw2_qpm_task_t *out, defw2_status_t *status)
{
	return run_call(&defw2_qpm_m_sync_run, qpm, req, result, opts, out,
			status);
}

static defw2_rc_t ref_call(const struct defw2_method *method,
			   defw2_binding_t *qpm,
			   const defw2_qpm_task_req_t *req,
			   defw2_result_buffer_t *result,
			   const defw2_call_opts_t *opts,
			   defw2_qpm_task_t *out, defw2_status_t *status)
{
	defw2_qpm_task_in_t in;
	defw2_rc_t rc;

	if (qpm == NULL || req == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	memset(out, 0, sizeof(*out));
	memset(&in, 0, sizeof(in));
	ctx_to_wire(&req->ctx, &in.ctx);
	in.cid = req->cid;
	in.qtask_id = req->qtask_id;
	in.reason = req->reason;

	rc = lend(qpm->rt, result, &in.result);
	if (rc != DEFW2_OK)
		return rc;
	return task_call(method, qpm, &in, &in.result, result, opts, out,
			 status);
}

defw2_rc_t defw2_qpm_read_cq(defw2_binding_t *qpm,
			     const defw2_qpm_task_req_t *req,
			     defw2_result_buffer_t *result,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_task_t *out, defw2_status_t *status)
{
	return ref_call(&defw2_qpm_m_read_cq, qpm, req, result, opts, out,
			status);
}

defw2_rc_t defw2_qpm_peek_cq(defw2_binding_t *qpm,
			     const defw2_qpm_task_req_t *req,
			     defw2_result_buffer_t *result,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_task_t *out, defw2_status_t *status)
{
	return ref_call(&defw2_qpm_m_peek_cq, qpm, req, result, opts, out,
			status);
}

defw2_rc_t defw2_qpm_task_status(defw2_binding_t *qpm,
				 const defw2_qpm_task_req_t *req,
				 const defw2_call_opts_t *opts,
				 defw2_qpm_task_t *out, defw2_status_t *status)
{
	return ref_call(&defw2_qpm_m_task_status, qpm, req, NULL, opts, out,
			status);
}

defw2_rc_t defw2_qpm_cancel_task(defw2_binding_t *qpm,
				 const defw2_qpm_task_req_t *req,
				 const defw2_call_opts_t *opts,
				 defw2_qpm_task_t *out, defw2_status_t *status)
{
	return ref_call(&defw2_qpm_m_cancel_task, qpm, req, NULL, opts, out,
			status);
}

defw2_rc_t defw2_qpm_delete_circuit(defw2_binding_t *qpm,
				    const defw2_qpm_task_req_t *req,
				    const defw2_call_opts_t *opts,
				    defw2_qpm_task_t *out,
				    defw2_status_t *status)
{
	return ref_call(&defw2_qpm_m_delete_circuit, qpm, req, NULL, opts,
			out, status);
}
