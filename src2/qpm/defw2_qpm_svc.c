/*
 * The QPM provider: fourteen handlers over three operations tables.
 *
 * Decoding, the version check, the span and the respond are
 * defw2_typed_serve's. What is left here is each method's conversions: build
 * the public request from the decoded one, call the operation, and turn its
 * answer back into the wire's. The public request points into Mercury's
 * decoded input, which lives until the handler returns, so nothing is copied
 * on the way in.
 *
 * An answer is checked before it is encoded. A string too long for the wire
 * would make the respond fail, and a caller with no timeout would then wait
 * for ever, so an answer that will not fit becomes a provider failure the
 * caller can see. A statevector must match its own description before it is
 * pushed.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_qpm_wire.h"

struct qpm_control_bound {
	struct defw2_bound		base;
	defw2_qpm_control_ops_t		ops;
};

struct qpm_admission_bound {
	struct defw2_bound		base;
	defw2_qpm_admission_ops_t	ops;
};

struct qpm_execution_bound {
	struct defw2_bound		base;
	defw2_qpm_execution_ops_t	ops;
};

static void ctx_from_wire(const defw2_qpm_wire_ctx_t *w, defw2_qpm_ctx_t *ctx)
{
	ctx->reservation_id = w->reservation_id;
	ctx->token = w->token;
}

/* Hand the call the request and the answer, for whoever answers it. */
static void call_frame(struct defw2_served *served, const void *req,
		       size_t req_len, void *answer)
{
	served->call.request = req;
	served->call.request_len = req_len;
	served->call.response = answer;
}

static bool unserved(struct defw2_served *served, const void *op)
{
	if (op != NULL)
		return false;
	defw2_served_fail(served, DEFW2_ERR_NOT_FOUND, DEFW2_CAT_NOT_FOUND,
			  "this QPM does not serve that method");
	return true;
}

/* --- answers onto the wire ------------------------------------------- */

static bool fits(const char *s, size_t max)
{
	return s == NULL || strnlen(s, max) < max;
}

static bool too_long(struct defw2_served *served)
{
	defw2_served_fail(served, DEFW2_ERR_INVALID,
			  DEFW2_CAT_PROVIDER_FAILURE,
			  "the service's answer has a field too long to send");
	return false;
}

static bool status_to_wire(struct defw2_served *served,
			   const defw2_qpm_service_status_t *a,
			   defw2_qpm_status_out_t *w)
{
	if (!fits(a->state, DEFW2_STR_MAX) || !fits(a->extra, DEFW2_EAGER_MAX))
		return too_long(served);
	w->state = a->state;
	w->ready = a->ready ? 1 : 0;
	w->initialized = a->initialized ? 1 : 0;
	w->accepting_requests = a->accepting_requests ? 1 : 0;
	w->provider_ready = a->provider_ready ? 1 : 0;
	w->active_task_count = a->active_task_count;
	w->active_reservation_count = a->active_reservation_count;
	w->extra = a->extra;
	return true;
}

static bool decision_to_wire(struct defw2_served *served,
			     const defw2_qpm_decision_t *a,
			     defw2_qpm_decision_out_t *w)
{
	if (!fits(a->decision, DEFW2_STR_MAX) ||
	    !fits(a->reason, DEFW2_STR_MAX) ||
	    !fits(a->message, DEFW2_STR_MAX) ||
	    !fits(a->extra, DEFW2_EAGER_MAX))
		return too_long(served);
	w->decision = a->decision;
	w->reservation_id = a->reservation_id;
	w->request_id = a->request_id;
	w->reason = a->reason;
	w->reason_code = a->reason_code;
	w->retry_after_ns = a->retry_after_ns;
	w->message = a->message;
	w->extra = a->extra;
	return true;
}

static bool reservation_to_wire(struct defw2_served *served,
				const defw2_qpm_reservation_t *a,
				defw2_qpm_reservation_out_t *w)
{
	if (!fits(a->state, DEFW2_STR_MAX) || !fits(a->extra, DEFW2_EAGER_MAX))
		return too_long(served);
	w->reservation_id = a->reservation_id;
	w->state = a->state;
	w->created_at_ns = a->created_at_ns;
	w->expires_at_ns = a->expires_at_ns;
	w->extra = a->extra;
	return true;
}

static bool task_to_wire(struct defw2_served *served,
			 const defw2_qpm_task_t *a, defw2_qpm_task_out_t *w)
{
	uint32_t i;

	if (!fits(a->outcome, DEFW2_STR_MAX) ||
	    !fits(a->lifecycle_state, DEFW2_STR_MAX) ||
	    !fits(a->cid, DEFW2_STR_MAX) ||
	    !fits(a->reason, DEFW2_STR_MAX) ||
	    !fits(a->message, DEFW2_STR_MAX) ||
	    !fits(a->extra, DEFW2_EAGER_MAX))
		return too_long(served);
	/* Checked again on the far side, but a service's own mistake is
	 * better reported as one here. */
	if (a->statevector.rank > DEFW2_TENSOR_RANK_MAX) {
		defw2_served_fail(served, DEFW2_ERR_INVALID,
				  DEFW2_CAT_PROVIDER_FAILURE,
				  "the service's statevector has too many "
				  "dimensions");
		return false;
	}
	w->outcome = a->outcome;
	w->lifecycle_state = a->lifecycle_state;
	w->cid = a->cid;
	w->qtask_id = a->qtask_id;
	w->reservation_id = a->reservation_id;
	w->reason = a->reason;
	w->message = a->message;
	w->completion_ready = a->completion_ready ? 1 : 0;
	w->statevector.dtype = a->statevector.dtype;
	w->statevector.rank = a->statevector.rank;
	for (i = 0; i < a->statevector.rank; i++)
		w->statevector.shape[i] = a->statevector.shape[i];
	w->statevector.nbytes = a->statevector.nbytes;
	w->extra = a->extra;
	return true;
}

/*
 * The capacity the service is told about is the smaller of what the caller
 * said it lent and what it actually registered, so a service never decides
 * on a size the push would refuse.
 */
static void lent(struct defw2_served *served, const defw2_wire_result_t *r)
{
	uint64_t registered;

	served->call.result_capacity = 0;
	if (r->handle == HG_BULK_NULL || r->capacity == 0)
		return;
	registered = margo_bulk_get_size(r->handle);
	served->call.result_capacity = r->capacity < registered ?
					       r->capacity : registered;
}

/*
 * After a task answer is on its way: push the statevector when the service
 * handed one back. The service's description has to match what it handed
 * over, since the caller will index its buffer by that description.
 */
static void deliver(struct defw2_served *served, const defw2_qpm_task_t *a,
		    const defw2_wire_result_t *r, defw2_qpm_task_out_t *w)
{
	struct defw2_call *call = &served->call;

	if (call->bulk == NULL)
		return;
	if (!defw2_tensor_valid(&a->statevector) ||
	    a->statevector.nbytes != call->bulk_len) {
		defw2_served_fail(served, DEFW2_ERR_INVALID,
				  DEFW2_CAT_PROVIDER_FAILURE,
				  "the service's statevector does not match "
				  "its description");
		return;
	}
	w->statevector.delivered = defw2_served_push(served, r->handle) ? 1
									  : 0;
}

/* --- control --------------------------------------------------------- */

typedef defw2_rc_t (*control_op_t)(void *, defw2_call_t *,
				   const defw2_qpm_ctx_t *,
				   defw2_qpm_service_status_t *);

static void serve_control(struct defw2_served *served,
			  defw2_qpm_ctx_in_t *in, defw2_qpm_status_out_t *out,
			  control_op_t op)
{
	struct qpm_control_bound *b = (struct qpm_control_bound *)served->bound;
	defw2_qpm_service_status_t answer;
	defw2_qpm_ctx_t req;

	if (unserved(served, (const void *)op))
		return;
	memset(&answer, 0, sizeof(answer));
	ctx_from_wire(&in->ctx, &req);
	call_frame(served, &req, sizeof(req), &answer);
	if (defw2_served_finish(served, op(b->ops.ctx, &served->call, &req,
					   &answer)))
		status_to_wire(served, &answer, out);
}

static void serve_is_ready(struct defw2_served *served, void *in, void *out)
{
	struct qpm_control_bound *b = (struct qpm_control_bound *)served->bound;

	serve_control(served, in, out, b->ops.is_ready);
}

static void serve_get_service_status(struct defw2_served *served, void *in,
				     void *out)
{
	struct qpm_control_bound *b = (struct qpm_control_bound *)served->bound;

	serve_control(served, in, out, b->ops.get_service_status);
}

/* --- admission ------------------------------------------------------- */

static void serve_reserve(struct defw2_served *served, void *vin, void *vout)
{
	struct qpm_admission_bound *b =
		(struct qpm_admission_bound *)served->bound;
	defw2_qpm_reserve_in_t *in = vin;
	defw2_qpm_decision_t answer;
	defw2_qpm_reserve_req_t req;

	if (unserved(served, (const void *)b->ops.reserve))
		return;
	memset(&answer, 0, sizeof(answer));
	memset(&req, 0, sizeof(req));
	ctx_from_wire(&in->ctx, &req.ctx);
	req.request_id = in->request_id;
	req.user = in->user;
	req.job_id = in->job_id;
	req.allocation_id = in->allocation_id;
	req.target_device_id = in->target_device_id;
	req.scope_id = in->scope_id;
	req.workload_kind = in->workload_kind;
	req.num_qubits = in->num_qubits;
	req.walltime_ns = in->walltime_ns;
	req.ttl_ns = in->ttl_ns;
	req.has_task_class = in->has_task_class != 0;
	if (req.has_task_class) {
		req.task_class.count = in->task_class.count;
		req.task_class.qubit_count = in->task_class.qubit_count;
		req.task_class.depth = in->task_class.depth;
		req.task_class.one_q_gate_count =
			in->task_class.one_q_gate_count;
		req.task_class.two_q_gate_count =
			in->task_class.two_q_gate_count;
		req.task_class.shots = in->task_class.shots;
		req.task_class.measurement_count =
			in->task_class.measurement_count;
	}
	req.extra = in->extra;
	call_frame(served, &req, sizeof(req), &answer);
	if (defw2_served_finish(served, b->ops.reserve(b->ops.ctx,
						       &served->call, &req,
						       &answer)))
		decision_to_wire(served, &answer, vout);
}

static void serve_renew(struct defw2_served *served, void *vin, void *vout)
{
	struct qpm_admission_bound *b =
		(struct qpm_admission_bound *)served->bound;
	defw2_qpm_renew_in_t *in = vin;
	defw2_qpm_decision_t answer;
	defw2_qpm_renew_req_t req;

	if (unserved(served, (const void *)b->ops.renew))
		return;
	memset(&answer, 0, sizeof(answer));
	memset(&req, 0, sizeof(req));
	ctx_from_wire(&in->ctx, &req.ctx);
	req.ttl_ns = in->ttl_ns;
	req.extra = in->extra;
	call_frame(served, &req, sizeof(req), &answer);
	if (defw2_served_finish(served, b->ops.renew(b->ops.ctx,
						     &served->call, &req,
						     &answer)))
		decision_to_wire(served, &answer, vout);
}

typedef defw2_rc_t (*close_op_t)(void *, defw2_call_t *,
				 const defw2_qpm_close_req_t *,
				 defw2_qpm_decision_t *);

static void serve_close(struct defw2_served *served,
			defw2_qpm_close_in_t *in,
			defw2_qpm_decision_out_t *out, close_op_t op)
{
	struct qpm_admission_bound *b =
		(struct qpm_admission_bound *)served->bound;
	defw2_qpm_decision_t answer;
	defw2_qpm_close_req_t req;

	if (unserved(served, (const void *)op))
		return;
	memset(&answer, 0, sizeof(answer));
	memset(&req, 0, sizeof(req));
	ctx_from_wire(&in->ctx, &req.ctx);
	req.reason_code = in->reason_code;
	call_frame(served, &req, sizeof(req), &answer);
	if (defw2_served_finish(served, op(b->ops.ctx, &served->call, &req,
					   &answer)))
		decision_to_wire(served, &answer, out);
}

static void serve_release(struct defw2_served *served, void *in, void *out)
{
	struct qpm_admission_bound *b =
		(struct qpm_admission_bound *)served->bound;

	serve_close(served, in, out, b->ops.release);
}

static void serve_cancel(struct defw2_served *served, void *in, void *out)
{
	struct qpm_admission_bound *b =
		(struct qpm_admission_bound *)served->bound;

	serve_close(served, in, out, b->ops.cancel);
}

static void serve_get_reservation(struct defw2_served *served, void *vin,
				  void *vout)
{
	struct qpm_admission_bound *b =
		(struct qpm_admission_bound *)served->bound;
	defw2_qpm_ctx_in_t *in = vin;
	defw2_qpm_reservation_t answer;
	defw2_qpm_ctx_t req;

	if (unserved(served, (const void *)b->ops.get_reservation))
		return;
	memset(&answer, 0, sizeof(answer));
	ctx_from_wire(&in->ctx, &req);
	call_frame(served, &req, sizeof(req), &answer);
	if (defw2_served_finish(served,
				b->ops.get_reservation(b->ops.ctx,
						       &served->call, &req,
						       &answer)))
		reservation_to_wire(served, &answer, vout);
}

/* --- execution ------------------------------------------------------- */

typedef defw2_rc_t (*run_op_t)(void *, defw2_call_t *,
			       const defw2_qpm_run_req_t *,
			       defw2_qpm_task_t *);

static void serve_run(struct defw2_served *served, defw2_qpm_run_in_t *in,
		      defw2_qpm_task_out_t *out, run_op_t op, bool lends)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;
	defw2_qpm_task_t answer;
	defw2_qpm_run_req_t req;

	if (unserved(served, (const void *)op))
		return;
	/* A run is a circuit. Without one there is nothing to ask for. */
	if (in->format == NULL || in->circuit.len == 0) {
		defw2_served_fail(served, DEFW2_ERR_INVALID,
				  DEFW2_CAT_INVALID_ARGUMENT,
				  "a run needs a circuit and its format");
		return;
	}
	memset(&answer, 0, sizeof(answer));
	memset(&req, 0, sizeof(req));
	ctx_from_wire(&in->ctx, &req.ctx);
	req.circuit.format = in->format;
	req.circuit.data = in->circuit.data;
	req.circuit.len = in->circuit.len;
	req.num_qubits = in->num_qubits;
	req.num_shots = in->num_shots;
	req.compiler = in->compiler;
	req.return_statevector = in->return_statevector != 0;
	req.has_timeout = in->has_timeout != 0;
	req.timeout_ms = in->timeout_ms;
	req.cancel_on_timeout = in->cancel_on_timeout != 0;
	req.extra = in->extra;
	if (lends)
		lent(served, &in->result);
	call_frame(served, &req, sizeof(req), &answer);
	if (!defw2_served_finish(served, op(b->ops.ctx, &served->call, &req,
					    &answer)))
		return;
	if (task_to_wire(served, &answer, out) && lends)
		deliver(served, &answer, &in->result, out);
}

static void serve_async_run(struct defw2_served *served, void *in, void *out)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;

	serve_run(served, in, out, b->ops.async_run, false);
}

static void serve_sync_run(struct defw2_served *served, void *in, void *out)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;

	serve_run(served, in, out, b->ops.sync_run, true);
}

typedef defw2_rc_t (*task_op_t)(void *, defw2_call_t *,
				const defw2_qpm_task_req_t *,
				defw2_qpm_task_t *);

static void serve_task(struct defw2_served *served, defw2_qpm_task_in_t *in,
		       defw2_qpm_task_out_t *out, task_op_t op, bool lends)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;
	defw2_qpm_task_t answer;
	defw2_qpm_task_req_t req;

	if (unserved(served, (const void *)op))
		return;
	memset(&answer, 0, sizeof(answer));
	memset(&req, 0, sizeof(req));
	ctx_from_wire(&in->ctx, &req.ctx);
	req.cid = in->cid;
	req.qtask_id = in->qtask_id;
	req.reason = in->reason;
	if (lends)
		lent(served, &in->result);
	call_frame(served, &req, sizeof(req), &answer);
	if (!defw2_served_finish(served, op(b->ops.ctx, &served->call, &req,
					    &answer)))
		return;
	if (task_to_wire(served, &answer, out) && lends)
		deliver(served, &answer, &in->result, out);
}

static void serve_read_cq(struct defw2_served *served, void *in, void *out)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;

	serve_task(served, in, out, b->ops.read_cq, true);
}

static void serve_peek_cq(struct defw2_served *served, void *in, void *out)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;

	serve_task(served, in, out, b->ops.peek_cq, true);
}

static void serve_task_status(struct defw2_served *served, void *in,
			      void *out)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;

	serve_task(served, in, out, b->ops.task_status, false);
}

static void serve_cancel_task(struct defw2_served *served, void *in,
			      void *out)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;

	serve_task(served, in, out, b->ops.cancel_task, false);
}

static void serve_delete_circuit(struct defw2_served *served, void *in,
				 void *out)
{
	struct qpm_execution_bound *b =
		(struct qpm_execution_bound *)served->bound;

	serve_task(served, in, out, b->ops.delete_circuit, false);
}

/* --- the handlers ---------------------------------------------------- */

/*
 * One Margo handler per method. Each owns its wire structures on its own
 * stack and hands them to the shared serve with the method's own part.
 */
#define DEFW2_QPM_HANDLER(name, in_t, out_t)				\
	static void defw2_qpm_##name##_ult(hg_handle_t handle)		\
	{								\
		in_t in;						\
		out_t out;						\
									\
		defw2_typed_serve(handle, &defw2_qpm_m_##name, &in,	\
				  sizeof(in), &out, sizeof(out),	\
				  serve_##name);			\
	}								\
	DEFINE_MARGO_RPC_HANDLER(defw2_qpm_##name##_ult)

DEFW2_QPM_HANDLER(is_ready, defw2_qpm_ctx_in_t, defw2_qpm_status_out_t)
DEFW2_QPM_HANDLER(get_service_status, defw2_qpm_ctx_in_t,
		  defw2_qpm_status_out_t)
DEFW2_QPM_HANDLER(reserve, defw2_qpm_reserve_in_t, defw2_qpm_decision_out_t)
DEFW2_QPM_HANDLER(renew, defw2_qpm_renew_in_t, defw2_qpm_decision_out_t)
DEFW2_QPM_HANDLER(release, defw2_qpm_close_in_t, defw2_qpm_decision_out_t)
DEFW2_QPM_HANDLER(cancel, defw2_qpm_close_in_t, defw2_qpm_decision_out_t)
DEFW2_QPM_HANDLER(get_reservation, defw2_qpm_ctx_in_t,
		  defw2_qpm_reservation_out_t)
DEFW2_QPM_HANDLER(async_run, defw2_qpm_run_in_t, defw2_qpm_task_out_t)
DEFW2_QPM_HANDLER(sync_run, defw2_qpm_run_in_t, defw2_qpm_task_out_t)
DEFW2_QPM_HANDLER(read_cq, defw2_qpm_task_in_t, defw2_qpm_task_out_t)
DEFW2_QPM_HANDLER(peek_cq, defw2_qpm_task_in_t, defw2_qpm_task_out_t)
DEFW2_QPM_HANDLER(task_status, defw2_qpm_task_in_t, defw2_qpm_task_out_t)
DEFW2_QPM_HANDLER(cancel_task, defw2_qpm_task_in_t, defw2_qpm_task_out_t)
DEFW2_QPM_HANDLER(delete_circuit, defw2_qpm_task_in_t, defw2_qpm_task_out_t)

#define DEFW2_QPM_ENTRY(name)						\
	{ &defw2_qpm_m_##name, _handler_for_defw2_qpm_##name##_ult }

/* --- binding --------------------------------------------------------- */

defw2_rc_t defw2_qpm_control_bind(defw2_service_t *svc,
				  const defw2_qpm_control_ops_t *ops)
{
	static const struct defw2_typed_entry entries[] = {
		DEFW2_QPM_ENTRY(is_ready),
		DEFW2_QPM_ENTRY(get_service_status),
	};
	struct qpm_control_bound *bound;

	if (svc == NULL)
		return DEFW2_ERR_INVALID;
	bound = calloc(1, sizeof(*bound));
	if (bound == NULL)
		return DEFW2_ERR_NOMEM;
	if (ops != NULL)
		bound->ops = *ops;
	return defw2_typed_bind(svc, entries,
				sizeof(entries) / sizeof(entries[0]),
				&bound->base);
}

defw2_rc_t defw2_qpm_admission_bind(defw2_service_t *svc,
				    const defw2_qpm_admission_ops_t *ops)
{
	static const struct defw2_typed_entry entries[] = {
		DEFW2_QPM_ENTRY(reserve),
		DEFW2_QPM_ENTRY(renew),
		DEFW2_QPM_ENTRY(release),
		DEFW2_QPM_ENTRY(cancel),
		DEFW2_QPM_ENTRY(get_reservation),
	};
	struct qpm_admission_bound *bound;

	if (svc == NULL)
		return DEFW2_ERR_INVALID;
	bound = calloc(1, sizeof(*bound));
	if (bound == NULL)
		return DEFW2_ERR_NOMEM;
	if (ops != NULL)
		bound->ops = *ops;
	return defw2_typed_bind(svc, entries,
				sizeof(entries) / sizeof(entries[0]),
				&bound->base);
}

defw2_rc_t defw2_qpm_execution_bind(defw2_service_t *svc,
				    const defw2_qpm_execution_ops_t *ops)
{
	static const struct defw2_typed_entry entries[] = {
		DEFW2_QPM_ENTRY(async_run),
		DEFW2_QPM_ENTRY(sync_run),
		DEFW2_QPM_ENTRY(read_cq),
		DEFW2_QPM_ENTRY(peek_cq),
		DEFW2_QPM_ENTRY(task_status),
		DEFW2_QPM_ENTRY(cancel_task),
		DEFW2_QPM_ENTRY(delete_circuit),
	};
	struct qpm_execution_bound *bound;

	if (svc == NULL)
		return DEFW2_ERR_INVALID;
	bound = calloc(1, sizeof(*bound));
	if (bound == NULL)
		return DEFW2_ERR_NOMEM;
	if (ops != NULL)
		bound->ops = *ops;
	return defw2_typed_bind(svc, entries,
				sizeof(entries) / sizeof(entries[0]),
				&bound->base);
}
