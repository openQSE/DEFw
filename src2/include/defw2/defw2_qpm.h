/*
 * The QPM APIs: qfw.qpm.control, qfw.qpm.admission and qfw.qpm.execution,
 * whose hot path is typed here, and qfw.qpm.admission-policy,
 * qfw.qpm.scheduler and qfw.qpm.telemetry, which are documents alone, as
 * are the typed APIs' other methods. defw2_doc.h calls those.
 *
 * This is the QPM hot path, typed so that a C caller such as the Slurm plugin
 * can reserve, run and collect without an interpreter anywhere in its
 * process. Each API is its own Margo provider, so a service can answer
 * is_ready while its execution queue is full.
 *
 *	defw2_qpm_run_req_t run = {
 *		.ctx = { .reservation_id = rid },
 *		.circuit = { DEFW2_QPM_FORMAT_OPENQASM2, qasm, strlen(qasm) },
 *		.num_qubits = 5,
 *		.num_shots = 1024,
 *	};
 *	defw2_qpm_task_t task = { 0 };
 *
 *	defw2_qpm_async_run(execution, &run, &opts, &task, &status);
 *	// task.cid names the work, and read_cq collects it
 *	defw2_qpm_task_free(&task);
 *
 * Every method takes one request structure and fills one answer. Each answer
 * has two layers. The typed fields are what a C caller branches on. extra is
 * a JSON object carrying everything else the service said, which is how the
 * provider-shaped parts of a QPM's answer travel without a schema per
 * provider. A C caller can ignore it. The Python compatibility layer merges
 * it back, so a v1 caller sees the dictionary it always saw. A typed field
 * is zero or NULL when the service had no value for it, and extra never
 * repeats a field the typed layer carries.
 *
 * Outcomes are data and failures are status. A task whose reservation does
 * not match comes back with outcome INVALID_RESERVATION and a status of OK,
 * because the service answered. A service that could not answer at all,
 * such as one that has not finished starting, fails the status instead.
 *
 * Nothing in libdefw2's runtime, RPC, directory or host code refers to a
 * QPM. This header and src2/qpm/ are the whole of it, so the API can move to
 * QFw without touching the framework.
 */
#ifndef DEFW2_QPM_H
#define DEFW2_QPM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <defw2/defw2_bulk.h>
#include <defw2/defw2_event.h>
#include <defw2/defw2_rpc.h>
#include <defw2/defw2_service.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFW2_API_QPM_CONTROL		"qfw.qpm.control"
#define DEFW2_API_QPM_ADMISSION		"qfw.qpm.admission"
#define DEFW2_API_QPM_EXECUTION		"qfw.qpm.execution"

/*
 * Where a QPM serves each API unless it says otherwise. The directory record
 * is what a caller resolves, and it names the provider, so these are
 * defaults rather than reservations.
 */
#define DEFW2_PROVIDER_QPM_CONTROL	2
#define DEFW2_PROVIDER_QPM_ADMISSION	3
#define DEFW2_PROVIDER_QPM_EXECUTION	4

/*
 * The QPM's other APIs have no typed methods, so every method on them is a
 * document. Each still has a provider of its own, so a slow telemetry
 * query never holds up control.
 */
#define DEFW2_API_QPM_ADMISSION_POLICY		"qfw.qpm.admission-policy"
#define DEFW2_API_QPM_SCHEDULER			"qfw.qpm.scheduler"
#define DEFW2_API_QPM_TELEMETRY			"qfw.qpm.telemetry"
#define DEFW2_PROVIDER_QPM_ADMISSION_POLICY	6
#define DEFW2_PROVIDER_QPM_SCHEDULER		7
#define DEFW2_PROVIDER_QPM_TELEMETRY		8

/*
 * The version the three QPM APIs speak, major in the high sixteen bits. A
 * provider refuses a different major before it uses the request.
 *
 * 1.1 added register_event_notification. A 1.0 QPM never registered it, so
 * a caller meets one as a method that is not found.
 */
#define DEFW2_QPM_VERSION_MAJOR		1
#define DEFW2_QPM_VERSION_MINOR		1
#define DEFW2_QPM_VERSION	(((uint32_t)DEFW2_QPM_VERSION_MAJOR << 16) | \
				 (uint32_t)DEFW2_QPM_VERSION_MINOR)

/*
 * Task outcomes QFw reports today. They stay strings so that QFw can add one
 * without a wire change.
 */
#define DEFW2_QPM_ACCEPTED		"ACCEPTED"
#define DEFW2_QPM_DELAYED		"DELAYED"
#define DEFW2_QPM_IN_PROGRESS		"IN_PROGRESS"
#define DEFW2_QPM_COMPLETED		"COMPLETED"
#define DEFW2_QPM_FAILED		"FAILED"
#define DEFW2_QPM_CANCELLED		"CANCELLED"
#define DEFW2_QPM_CANCEL_PENDING	"CANCEL_PENDING"
#define DEFW2_QPM_TIMEOUT		"TIMEOUT"
#define DEFW2_QPM_INVALID_RESERVATION	"INVALID_RESERVATION"
#define DEFW2_QPM_MISSING_RESERVATION	"MISSING_RESERVATION"
#define DEFW2_QPM_NO_LONGER_RETAINED	"NO_LONGER_RETAINED"
#define DEFW2_QPM_UNKNOWN		"UNKNOWN"

/* Admission decisions. */
#define DEFW2_QPM_DECISION_ACCEPTED	"accepted"
#define DEFW2_QPM_DECISION_DELAYED	"delayed"
#define DEFW2_QPM_DECISION_REJECTED	"rejected"
#define DEFW2_QPM_DECISION_PENDING	"pending"

/* Circuit formats a QPM declares in its directory properties. */
#define DEFW2_QPM_FORMAT_OPENQASM2	"openqasm2"
#define DEFW2_QPM_FORMAT_QPY		"qpy"
#define DEFW2_QPM_FORMAT_QPY_GZIP	"qpy+gzip"

/* --- requests -------------------------------------------------------- */

/*
 * What every call carries: the reservation it acts under, and the caller's
 * token. For the admission methods that act on a reservation, renew,
 * release, cancel and get_reservation, reservation_id is that reservation.
 */
typedef struct {
	uint64_t	reservation_id;	/* 0 when the call has none */
	const char	*token;		/* opaque to DEFw, may be NULL */
} defw2_qpm_ctx_t;

/* The work a reservation is for. */
typedef struct {
	uint64_t	count;
	uint32_t	qubit_count;
	uint32_t	depth;
	uint64_t	one_q_gate_count;
	uint64_t	two_q_gate_count;
	uint64_t	shots;
	uint64_t	measurement_count;
} defw2_qpm_task_class_t;

/*
 * reserve. Every field is optional, and a zero or NULL leaves it to the
 * service. request_id is the caller's idempotency key, which is how a
 * retried reservation is recognised as the same one.
 */
typedef struct {
	defw2_qpm_ctx_t		ctx;
	uint64_t		request_id;
	const char		*user;
	const char		*job_id;
	const char		*allocation_id;
	const char		*target_device_id;
	const char		*scope_id;
	const char		*workload_kind;
	uint32_t		num_qubits;
	uint64_t		walltime_ns;
	uint64_t		ttl_ns;
	bool			has_task_class;
	defw2_qpm_task_class_t	task_class;
	const char		*extra;		/* JSON object, may be NULL */
} defw2_qpm_reserve_req_t;

/* renew. ttl_ns of 0 leaves the reservation's own. */
typedef struct {
	defw2_qpm_ctx_t		ctx;
	uint64_t		ttl_ns;
	const char		*extra;		/* JSON object, may be NULL */
} defw2_qpm_renew_req_t;

/* release and cancel. */
typedef struct {
	defw2_qpm_ctx_t		ctx;
	uint32_t		reason_code;
} defw2_qpm_close_req_t;

/*
 * A circuit, in a format the QPM declares. data is the circuit itself:
 * OpenQASM text or QPY bytes, never base64. It travels inside the message,
 * so it is at most DEFW2_EAGER_MAX bytes.
 */
typedef struct {
	const char	*format;	/* DEFW2_QPM_FORMAT_* */
	const void	*data;
	size_t		len;
} defw2_qpm_circuit_t;

/*
 * async_run and sync_run. timeout_ms counts only when has_timeout is set,
 * because a timeout of zero means "already expired", not "none".
 */
typedef struct {
	defw2_qpm_ctx_t		ctx;
	defw2_qpm_circuit_t	circuit;
	uint32_t		num_qubits;
	uint32_t		num_shots;
	const char		*compiler;	/* may be NULL */
	bool			return_statevector;
	bool			has_timeout;
	uint64_t		timeout_ms;
	bool			cancel_on_timeout;
	const char		*extra;		/* JSON object, may be NULL */
} defw2_qpm_run_req_t;

/*
 * Which task a call is about, for read_cq, peek_cq, task_status,
 * cancel_task and delete_circuit. cid names it, or qtask_id where the
 * service supports that. read_cq and peek_cq take the oldest completion when
 * neither is given.
 */
typedef struct {
	defw2_qpm_ctx_t		ctx;
	const char		*cid;		/* may be NULL */
	uint64_t		qtask_id;	/* 0 when cid names it */
	const char		*reason;	/* cancel_task only, may be NULL */
} defw2_qpm_task_req_t;

/*
 * register_event_notification. target is the sink the events go to, by the
 * address and provider defw2_event_sink_address and
 * defw2_event_sink_provider_id give, with a tag of the caller's own that
 * comes back on every event, as type does. A reservation limits the events
 * to that reservation's tasks. Without one, the service decides which tasks
 * a caller may hear about. extra carries the rest, such as QFw's filters.
 */
typedef struct {
	defw2_qpm_ctx_t		ctx;
	defw2_event_target_t	target;
	const char		*type;		/* may be NULL */
	const char		*extra;		/* JSON object, may be NULL */
} defw2_qpm_notify_req_t;

/* --- answers --------------------------------------------------------- */

/*
 * Every answer's strings, and extra, belong to the answer. A caller frees
 * the whole of it with the matching free function. A service fills the same
 * structures from storage its call owns, through defw2_call_strdup and its
 * relatives, and leaves arena alone.
 */

/* is_ready and get_service_status. */
typedef struct {
	const char	*state;			/* "running", "stopping", ... */
	bool		ready;
	bool		initialized;
	bool		accepting_requests;
	bool		provider_ready;
	uint32_t	active_task_count;
	uint32_t	active_reservation_count;
	const char	*extra;
	void		*arena;			/* internal */
} defw2_qpm_service_status_t;

/* reserve, renew, release, cancel and register_event_notification. */
typedef struct {
	const char	*decision;		/* DEFW2_QPM_DECISION_* */
	uint64_t	reservation_id;		/* 0 when none was granted */
	uint64_t	request_id;
	const char	*reason;
	uint32_t	reason_code;
	uint64_t	retry_after_ns;		/* 0 when the service gave none */
	const char	*message;
	const char	*extra;
	void		*arena;			/* internal */
} defw2_qpm_decision_t;

/* get_reservation. */
typedef struct {
	uint64_t	reservation_id;
	const char	*state;			/* "pending", "active", ... */
	uint64_t	created_at_ns;
	uint64_t	expires_at_ns;
	const char	*extra;
	void		*arena;			/* internal */
} defw2_qpm_reservation_t;

/* Every execution method. */
typedef struct {
	const char	*outcome;		/* DEFW2_QPM_COMPLETED, ... */
	const char	*lifecycle_state;
	const char	*cid;
	uint64_t	qtask_id;		/* 0 when unknown */
	uint64_t	reservation_id;
	const char	*reason;
	const char	*message;
	bool		completion_ready;	/* read_cq and peek_cq */
	/*
	 * The task's statevector, when it produced one and the call could
	 * carry it. The descriptor describes it whether or not it arrived.
	 * statevector_delivered says it is in the caller's result buffer.
	 * When it is false and statevector.nbytes is not, the buffer was
	 * missing or too small, nbytes is what a retry needs, and read_cq
	 * left the completion in the queue for that retry.
	 */
	defw2_tensor_t	statevector;
	bool		statevector_delivered;
	const char	*extra;
	void		*arena;			/* internal */
} defw2_qpm_task_t;

void defw2_qpm_service_status_free(defw2_qpm_service_status_t *status);
void defw2_qpm_decision_free(defw2_qpm_decision_t *decision);
void defw2_qpm_reservation_free(defw2_qpm_reservation_t *reservation);
void defw2_qpm_task_free(defw2_qpm_task_t *task);

/* --- calling --------------------------------------------------------- */

/*
 * The return code is the transport outcome and status the service's own, as
 * for every stub: DEFW2_OK means the call reached the service and came back.
 * An answer is filled only then, and is released with its free function
 * whatever the outcome.
 */

/* qfw.qpm.control */
defw2_rc_t defw2_qpm_is_ready(defw2_binding_t *qpm,
			      const defw2_qpm_ctx_t *req,
			      const defw2_call_opts_t *opts,
			      defw2_qpm_service_status_t *out,
			      defw2_status_t *status);
defw2_rc_t defw2_qpm_get_service_status(defw2_binding_t *qpm,
					const defw2_qpm_ctx_t *req,
					const defw2_call_opts_t *opts,
					defw2_qpm_service_status_t *out,
					defw2_status_t *status);

/* qfw.qpm.admission */
defw2_rc_t defw2_qpm_reserve(defw2_binding_t *qpm,
			     const defw2_qpm_reserve_req_t *req,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_decision_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_renew(defw2_binding_t *qpm,
			   const defw2_qpm_renew_req_t *req,
			   const defw2_call_opts_t *opts,
			   defw2_qpm_decision_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_release(defw2_binding_t *qpm,
			     const defw2_qpm_close_req_t *req,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_decision_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_cancel(defw2_binding_t *qpm,
			    const defw2_qpm_close_req_t *req,
			    const defw2_call_opts_t *opts,
			    defw2_qpm_decision_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_get_reservation(defw2_binding_t *qpm,
				     const defw2_qpm_ctx_t *req,
				     const defw2_call_opts_t *opts,
				     defw2_qpm_reservation_t *out,
				     defw2_status_t *status);

/*
 * qfw.qpm.execution
 *
 * sync_run, read_cq and peek_cq take a result buffer, which may be NULL. It
 * is registered for the length of the call and the statevector, if there is
 * one and it fits, is pushed into it. A caller that knows the size, as it
 * does for a statevector from num_qubits, lends a buffer of that size and
 * gets the result in one call.
 */
defw2_rc_t defw2_qpm_async_run(defw2_binding_t *qpm,
			       const defw2_qpm_run_req_t *req,
			       const defw2_call_opts_t *opts,
			       defw2_qpm_task_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_sync_run(defw2_binding_t *qpm,
			      const defw2_qpm_run_req_t *req,
			      defw2_result_buffer_t *result,
			      const defw2_call_opts_t *opts,
			      defw2_qpm_task_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_read_cq(defw2_binding_t *qpm,
			     const defw2_qpm_task_req_t *req,
			     defw2_result_buffer_t *result,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_task_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_peek_cq(defw2_binding_t *qpm,
			     const defw2_qpm_task_req_t *req,
			     defw2_result_buffer_t *result,
			     const defw2_call_opts_t *opts,
			     defw2_qpm_task_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_task_status(defw2_binding_t *qpm,
				 const defw2_qpm_task_req_t *req,
				 const defw2_call_opts_t *opts,
				 defw2_qpm_task_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_cancel_task(defw2_binding_t *qpm,
				 const defw2_qpm_task_req_t *req,
				 const defw2_call_opts_t *opts,
				 defw2_qpm_task_t *out, defw2_status_t *status);
defw2_rc_t defw2_qpm_delete_circuit(defw2_binding_t *qpm,
				    const defw2_qpm_task_req_t *req,
				    const defw2_call_opts_t *opts,
				    defw2_qpm_task_t *out,
				    defw2_status_t *status);

/*
 * Ask for completion events, which the events section below describes. An
 * accepted registration answers with DEFW2_QPM_DECISION_ACCEPTED. It lasts
 * until a delivery to its sink fails, as v1's did, so a caller ends it by
 * destroying the sink. Make the sink accept completions first, or the
 * service's first event to it ends the registration.
 */
defw2_rc_t defw2_qpm_register_event_notification(
	defw2_binding_t *qpm, const defw2_qpm_notify_req_t *req,
	const defw2_call_opts_t *opts, defw2_qpm_decision_t *out,
	defw2_status_t *status);

/* --- serving --------------------------------------------------------- */

/*
 * A C service supplies an operations table per API. Each operation answers
 * into out, with storage from call, and returns DEFW2_OK when it answered.
 * One that fails sets a status with defw2_call_set_status and returns the
 * code. A NULL operation is a method the service does not serve, which a
 * caller sees as DEFW2_CAT_NOT_FOUND.
 *
 * Operations run on Margo's handler pool and may block, because the progress
 * loop has its own execution stream.
 */
typedef struct {
	void		*ctx;
	defw2_rc_t	(*is_ready)(void *ctx, defw2_call_t *call,
				    const defw2_qpm_ctx_t *req,
				    defw2_qpm_service_status_t *out);
	defw2_rc_t	(*get_service_status)(void *ctx, defw2_call_t *call,
					      const defw2_qpm_ctx_t *req,
					      defw2_qpm_service_status_t *out);
} defw2_qpm_control_ops_t;

typedef struct {
	void		*ctx;
	defw2_rc_t	(*reserve)(void *ctx, defw2_call_t *call,
				   const defw2_qpm_reserve_req_t *req,
				   defw2_qpm_decision_t *out);
	defw2_rc_t	(*renew)(void *ctx, defw2_call_t *call,
				 const defw2_qpm_renew_req_t *req,
				 defw2_qpm_decision_t *out);
	defw2_rc_t	(*release)(void *ctx, defw2_call_t *call,
				   const defw2_qpm_close_req_t *req,
				   defw2_qpm_decision_t *out);
	defw2_rc_t	(*cancel)(void *ctx, defw2_call_t *call,
				  const defw2_qpm_close_req_t *req,
				  defw2_qpm_decision_t *out);
	defw2_rc_t	(*get_reservation)(void *ctx, defw2_call_t *call,
					   const defw2_qpm_ctx_t *req,
					   defw2_qpm_reservation_t *out);
} defw2_qpm_admission_ops_t;

/*
 * read_cq and peek_cq hand back a statevector through
 * defw2_call_bulk_reply, after checking defw2_call_result_capacity: a
 * read_cq that would not fit should leave the completion queued and answer
 * with the descriptor alone, so the caller can retry with a larger buffer.
 */
typedef struct {
	void		*ctx;
	defw2_rc_t	(*async_run)(void *ctx, defw2_call_t *call,
				     const defw2_qpm_run_req_t *req,
				     defw2_qpm_task_t *out);
	defw2_rc_t	(*sync_run)(void *ctx, defw2_call_t *call,
				    const defw2_qpm_run_req_t *req,
				    defw2_qpm_task_t *out);
	defw2_rc_t	(*read_cq)(void *ctx, defw2_call_t *call,
				   const defw2_qpm_task_req_t *req,
				   defw2_qpm_task_t *out);
	defw2_rc_t	(*peek_cq)(void *ctx, defw2_call_t *call,
				   const defw2_qpm_task_req_t *req,
				   defw2_qpm_task_t *out);
	defw2_rc_t	(*task_status)(void *ctx, defw2_call_t *call,
				       const defw2_qpm_task_req_t *req,
				       defw2_qpm_task_t *out);
	defw2_rc_t	(*cancel_task)(void *ctx, defw2_call_t *call,
				       const defw2_qpm_task_req_t *req,
				       defw2_qpm_task_t *out);
	defw2_rc_t	(*delete_circuit)(void *ctx, defw2_call_t *call,
					  const defw2_qpm_task_req_t *req,
					  defw2_qpm_task_t *out);
	defw2_rc_t	(*register_event_notification)(
				void *ctx, defw2_call_t *call,
				const defw2_qpm_notify_req_t *req,
				defw2_qpm_decision_t *out);
} defw2_qpm_execution_ops_t;

/*
 * Register an API on a service, whose provider identifier is where it is
 * served. ops is copied.
 */
defw2_rc_t defw2_qpm_control_bind(defw2_service_t *svc,
				  const defw2_qpm_control_ops_t *ops);
defw2_rc_t defw2_qpm_admission_bind(defw2_service_t *svc,
				    const defw2_qpm_admission_ops_t *ops);
defw2_rc_t defw2_qpm_execution_bind(defw2_service_t *svc,
				    const defw2_qpm_execution_ops_t *ops);

/* --- events ---------------------------------------------------------- */

/*
 * A completion event tells a caller that a task finished, so it need not
 * poll. Its payload is a defw2_qpm_task_t, the record read_cq would answer
 * with, except that a statevector is described and never carried:
 * statevector_delivered is false, and statevector.nbytes is the buffer to
 * lend read_cq or peek_cq for it. A large result never rides in an event,
 * which is what held v1's clients up behind one another.
 *
 * A caller readies its sink with defw2_qpm_event_accept and registers it
 * with defw2_qpm_register_event_notification. The service keeps its
 * registrations, decides which completions match each one, and sends each
 * with defw2_qpm_publish_completion, which returns what defw2_event.h says
 * publishing returns. When that says the target is gone, the service drops
 * the registration. The registration's type and tag come back on its
 * events.
 */
#define DEFW2_QPM_EVENT_COMPLETION	"completion"

defw2_rc_t defw2_qpm_event_accept(defw2_event_sink_t *sink);
defw2_rc_t defw2_qpm_publish_completion(defw2_event_publisher_t *pub,
					const defw2_event_target_t *target,
					const char *type,
					const defw2_qpm_task_t *task,
					const char *traceparent);

/*
 * The task a completion event carries, or NULL for any other event. It
 * belongs to the event: free the event, never the task.
 */
const defw2_qpm_task_t *defw2_qpm_event_task(const defw2_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_QPM_H */
