/*
 * The QPM wire. Shared inside libdefw2 and not installed.
 *
 * Six request shapes and four answers cover the fourteen methods, which is
 * the same grouping the public header uses: a method's request says what it
 * acts on and its answer says what kind of thing came back. Every string is
 * a checked one, every document is checked text, the circuit is counted
 * bytes, and a result buffer is a registration the caller made. Nothing here
 * can make the decoder allocate more than the message carries.
 *
 * Every request leads with the header and every answer with the status,
 * which is the layout rule the shared typed code depends on.
 */
#ifndef DEFW2_QPM_WIRE_H
#define DEFW2_QPM_WIRE_H

#include <defw2/defw2_qpm.h>

#include "../rpc/defw2_typed.h"

MERCURY_GEN_PROC(defw2_qpm_wire_ctx_t,
	((hg_uint64_t)(reservation_id))
	((defw2_str_t)(token)))

MERCURY_GEN_PROC(defw2_qpm_wire_task_class_t,
	((hg_uint64_t)(count))
	((hg_uint32_t)(qubit_count))
	((hg_uint32_t)(depth))
	((hg_uint64_t)(one_q_gate_count))
	((hg_uint64_t)(two_q_gate_count))
	((hg_uint64_t)(shots))
	((hg_uint64_t)(measurement_count)))

/* --- requests -------------------------------------------------------- */

/* is_ready, get_service_status and get_reservation. */
MERCURY_GEN_PROC(defw2_qpm_ctx_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_qpm_wire_ctx_t)(ctx)))

MERCURY_GEN_PROC(defw2_qpm_reserve_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_qpm_wire_ctx_t)(ctx))
	((hg_uint64_t)(request_id))
	((defw2_str_t)(user))
	((defw2_str_t)(job_id))
	((defw2_str_t)(allocation_id))
	((defw2_str_t)(target_device_id))
	((defw2_str_t)(scope_id))
	((defw2_str_t)(workload_kind))
	((hg_uint32_t)(num_qubits))
	((hg_uint64_t)(walltime_ns))
	((hg_uint64_t)(ttl_ns))
	((hg_uint8_t)(has_task_class))
	((defw2_qpm_wire_task_class_t)(task_class))
	((defw2_text_t)(extra)))

MERCURY_GEN_PROC(defw2_qpm_renew_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_qpm_wire_ctx_t)(ctx))
	((hg_uint64_t)(ttl_ns))
	((defw2_text_t)(extra)))

/* release and cancel. */
MERCURY_GEN_PROC(defw2_qpm_close_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_qpm_wire_ctx_t)(ctx))
	((hg_uint32_t)(reason_code)))

/* async_run and sync_run. Only sync_run lends a result buffer. */
MERCURY_GEN_PROC(defw2_qpm_run_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_qpm_wire_ctx_t)(ctx))
	((defw2_str_t)(format))
	((defw2_bytes_t)(circuit))
	((hg_uint32_t)(num_qubits))
	((hg_uint32_t)(num_shots))
	((defw2_str_t)(compiler))
	((hg_uint8_t)(return_statevector))
	((hg_uint8_t)(has_timeout))
	((hg_uint64_t)(timeout_ms))
	((hg_uint8_t)(cancel_on_timeout))
	((defw2_text_t)(extra))
	((defw2_wire_result_t)(result)))

/*
 * read_cq, peek_cq, task_status, cancel_task and delete_circuit. Only the
 * two completion reads lend a result buffer.
 */
MERCURY_GEN_PROC(defw2_qpm_task_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_qpm_wire_ctx_t)(ctx))
	((defw2_str_t)(cid))
	((hg_uint64_t)(qtask_id))
	((defw2_str_t)(reason))
	((defw2_wire_result_t)(result)))

/* --- answers --------------------------------------------------------- */

MERCURY_GEN_PROC(defw2_qpm_status_out_t,
	((defw2_wire_status_t)(status))
	((defw2_str_t)(state))
	((hg_uint8_t)(ready))
	((hg_uint8_t)(initialized))
	((hg_uint8_t)(accepting_requests))
	((hg_uint8_t)(provider_ready))
	((hg_uint32_t)(active_task_count))
	((hg_uint32_t)(active_reservation_count))
	((defw2_text_t)(extra)))

MERCURY_GEN_PROC(defw2_qpm_decision_out_t,
	((defw2_wire_status_t)(status))
	((defw2_str_t)(decision))
	((hg_uint64_t)(reservation_id))
	((hg_uint64_t)(request_id))
	((defw2_str_t)(reason))
	((hg_uint32_t)(reason_code))
	((hg_uint64_t)(retry_after_ns))
	((defw2_str_t)(message))
	((defw2_text_t)(extra)))

MERCURY_GEN_PROC(defw2_qpm_reservation_out_t,
	((defw2_wire_status_t)(status))
	((hg_uint64_t)(reservation_id))
	((defw2_str_t)(state))
	((hg_uint64_t)(created_at_ns))
	((hg_uint64_t)(expires_at_ns))
	((defw2_text_t)(extra)))

/*
 * A task, as every execution method answers it and as a completion event
 * carries it. Nesting it in the answer encodes the same bytes as listing
 * its fields there, so the answer's wire did not change when it moved.
 */
MERCURY_GEN_PROC(defw2_qpm_wire_task_t,
	((defw2_str_t)(outcome))
	((defw2_str_t)(lifecycle_state))
	((defw2_str_t)(cid))
	((hg_uint64_t)(qtask_id))
	((hg_uint64_t)(reservation_id))
	((defw2_str_t)(reason))
	((defw2_str_t)(message))
	((hg_uint8_t)(completion_ready))
	((defw2_wire_tensor_t)(statevector))
	((defw2_text_t)(extra)))

MERCURY_GEN_PROC(defw2_qpm_task_out_t,
	((defw2_wire_status_t)(status))
	((defw2_qpm_wire_task_t)(task)))

/*
 * Between a task and its wire form, for the answers and the event alike.
 *
 * defw2_qpm_task_fits says whether every string fits its field, so a
 * service's mistake is caught before an encode would fail on it.
 * defw2_qpm_task_to_wire points the wire form at the task's own strings, so
 * the task must outlive the encode. defw2_qpm_task_from_wire copies the
 * wire form into arena and describes the statevector without saying it was
 * delivered, which only the caller of a result buffer can decide.
 */
bool defw2_qpm_task_fits(const defw2_qpm_task_t *task);
void defw2_qpm_task_to_wire(const defw2_qpm_task_t *task,
			    defw2_qpm_wire_task_t *w);
defw2_rc_t defw2_qpm_task_from_wire(struct defw2_arena *arena,
				    const defw2_qpm_wire_task_t *w,
				    defw2_qpm_task_t *task);

/*
 * The completion event's kind, which carries a defw2_qpm_wire_task_t. Only
 * the event code and the tests need it by name.
 */
struct defw2_event_kind;
extern const struct defw2_event_kind defw2_qpm_completion_kind;

/* --- the fourteen methods -------------------------------------------- */

#define DEFW2_QPM_METHOD(var, api, name, in_t, out_t)			\
	static const struct defw2_method var = {			\
		api, #name, "defw2." api "." #name, DEFW2_QPM_VERSION,	\
		hg_proc_##in_t, hg_proc_##out_t,			\
	}

DEFW2_QPM_METHOD(defw2_qpm_m_is_ready, DEFW2_API_QPM_CONTROL, is_ready,
		 defw2_qpm_ctx_in_t, defw2_qpm_status_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_get_service_status, DEFW2_API_QPM_CONTROL,
		 get_service_status, defw2_qpm_ctx_in_t,
		 defw2_qpm_status_out_t);

DEFW2_QPM_METHOD(defw2_qpm_m_reserve, DEFW2_API_QPM_ADMISSION, reserve,
		 defw2_qpm_reserve_in_t, defw2_qpm_decision_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_renew, DEFW2_API_QPM_ADMISSION, renew,
		 defw2_qpm_renew_in_t, defw2_qpm_decision_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_release, DEFW2_API_QPM_ADMISSION, release,
		 defw2_qpm_close_in_t, defw2_qpm_decision_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_cancel, DEFW2_API_QPM_ADMISSION, cancel,
		 defw2_qpm_close_in_t, defw2_qpm_decision_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_get_reservation, DEFW2_API_QPM_ADMISSION,
		 get_reservation, defw2_qpm_ctx_in_t,
		 defw2_qpm_reservation_out_t);

DEFW2_QPM_METHOD(defw2_qpm_m_async_run, DEFW2_API_QPM_EXECUTION, async_run,
		 defw2_qpm_run_in_t, defw2_qpm_task_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_sync_run, DEFW2_API_QPM_EXECUTION, sync_run,
		 defw2_qpm_run_in_t, defw2_qpm_task_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_read_cq, DEFW2_API_QPM_EXECUTION, read_cq,
		 defw2_qpm_task_in_t, defw2_qpm_task_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_peek_cq, DEFW2_API_QPM_EXECUTION, peek_cq,
		 defw2_qpm_task_in_t, defw2_qpm_task_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_task_status, DEFW2_API_QPM_EXECUTION,
		 task_status, defw2_qpm_task_in_t, defw2_qpm_task_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_cancel_task, DEFW2_API_QPM_EXECUTION,
		 cancel_task, defw2_qpm_task_in_t, defw2_qpm_task_out_t);
DEFW2_QPM_METHOD(defw2_qpm_m_delete_circuit, DEFW2_API_QPM_EXECUTION,
		 delete_circuit, defw2_qpm_task_in_t, defw2_qpm_task_out_t);

#endif /* DEFW2_QPM_WIRE_H */
