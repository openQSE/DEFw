#!/usr/bin/env python3
"""Build the defw2 Python extension.

cffi in API mode: the declarations below are compiled against the real
headers, so a mismatch is a compile error rather than a crash at run time.
Only what the binding uses is declared, which also documents exactly how
much of libdefw2 the Python side depends on.

	cffi_build.py --include <dir> --library <dir> --out <dir> [--rpath <dir>]

--library is where to link libdefw2 from, and where the extension looks
for it at run time unless --rpath says where else.

Called by CMake when cffi is present. The headers and the library come from
the build tree, or from an installed DEFw v2.
"""

import argparse
import os
import sys

from cffi import FFI

# Partial declarations. The trailing ... in an enum or a struct tells cffi
# to take the real values and layout from the header it compiles against.
CDEF = """
typedef enum {
	DEFW2_OK, DEFW2_ERR_INVALID, DEFW2_ERR_NOMEM, DEFW2_ERR_CONFIG,
	DEFW2_ERR_TRANSPORT, DEFW2_ERR_TIMEOUT, DEFW2_ERR_CANCELLED,
	DEFW2_ERR_NOT_FOUND, DEFW2_ERR_VERSION, DEFW2_ERR_INTERNAL,
	DEFW2_ERR_BUSY, ...
} defw2_rc_t;

typedef enum {
	DEFW2_CAT_OK, DEFW2_CAT_TRANSPORT, DEFW2_CAT_TIMEOUT,
	DEFW2_CAT_CANCELLED, DEFW2_CAT_NOT_FOUND,
	DEFW2_CAT_VERSION_MISMATCH, DEFW2_CAT_INVALID_ARGUMENT,
	DEFW2_CAT_INVALID_RESERVATION, DEFW2_CAT_INSUFFICIENT_ALLOWANCE,
	DEFW2_CAT_PENDING_CAPACITY, DEFW2_CAT_POLICY_DELAYED,
	DEFW2_CAT_EXPIRED_RESERVATION, DEFW2_CAT_SCHEDULER_FAILURE,
	DEFW2_CAT_PROVIDER_FAILURE, ...
} defw2_category_t;

typedef enum { DEFW2_ROLE_CLIENT, DEFW2_ROLE_SERVER, ... } defw2_role_t;

typedef struct {
	int32_t code; uint32_t category; char *message;
} defw2_status_t;
typedef struct { void *data; size_t len; } defw2_buffer_t;
typedef struct {
	uint32_t timeout_ms; const char *traceparent;
} defw2_call_opts_t;
typedef struct {
	uint64_t user_us; uint64_t system_us; uint64_t peak_rss_kib;
} defw2_process_stats_t;

typedef struct {
	const char *address;
	const char *dirsvc;
	const char *margo_config;
	const char *node_name;
	const char *log_dir;
	defw2_role_t role;
	int rpc_thread_count;
	bool profile;
	...;
} defw2_config_t;

typedef struct defw2_rt defw2_rt_t;
typedef struct defw2_binding defw2_binding_t;
typedef struct defw2_service defw2_service_t;
typedef struct defw2_call defw2_call_t;

/* runtime */
defw2_rc_t defw2_config_from_env(defw2_config_t *cfg);
defw2_rc_t defw2_init(const defw2_config_t *cfg, defw2_rt_t **rt);
void defw2_finalize(defw2_rt_t *rt);
const char *defw2_runtime_id(const defw2_rt_t *rt);
const char *defw2_address(const defw2_rt_t *rt);
const char *defw2_node_name(const defw2_rt_t *rt);
int defw2_pid(const defw2_rt_t *rt);
void defw2_status_free(defw2_status_t *status);
const char *defw2_category_name(uint32_t category);
const char *defw2_strerror(defw2_rc_t rc);
const char *defw2_version(void);

/* calling */
defw2_rc_t defw2_binding_create(defw2_rt_t *rt, const char *address,
				uint16_t provider_id, defw2_binding_t **binding);
void defw2_binding_free(defw2_binding_t *binding);
const char *defw2_binding_address(const defw2_binding_t *binding);
void defw2_buffer_free(defw2_buffer_t *buffer);

/* the echo api */
defw2_rc_t defw2_echo(defw2_binding_t *binding, const void *payload,
		      size_t len, const defw2_call_opts_t *opts,
		      defw2_buffer_t *reply, defw2_status_t *status);
defw2_rc_t defw2_echo_bulk(defw2_binding_t *binding, const void *source,
			   void *sink, size_t len,
			   const defw2_call_opts_t *opts,
			   uint64_t *bytes_moved, defw2_status_t *status);
defw2_rc_t defw2_echo_bind(defw2_service_t *svc, const void *ops);

/* serving */
defw2_rc_t defw2_service_create(defw2_rt_t *rt, const char *service_id,
				const char *service_type, uint16_t provider_id,
				defw2_service_t **svc);
void defw2_service_destroy(defw2_service_t *svc);
void defw2_service_shutdown(defw2_service_t *svc);
const char *defw2_service_address(const defw2_service_t *svc);
defw2_rc_t defw2_service_queue_open(defw2_service_t *svc, unsigned depth);
void defw2_service_queue_close(defw2_service_t *svc);
bool defw2_service_queued(const defw2_service_t *svc);
defw2_rc_t defw2_service_next_call(defw2_service_t *svc, uint32_t timeout_ms,
				   defw2_call_t **call);
const char *defw2_call_api(const defw2_call_t *call);
const char *defw2_call_method(const defw2_call_t *call);
const void *defw2_call_request(const defw2_call_t *call, size_t *len);
defw2_rc_t defw2_service_respond(defw2_call_t *call, const void *reply,
				 size_t len);
defw2_rc_t defw2_service_fail(defw2_call_t *call, defw2_rc_t code,
			      uint32_t category, const char *message);

/* documents */
#define DEFW2_DOC_VERSION ...
#define DEFW2_DOC_METHOD_MAX ...
defw2_rc_t defw2_doc_call(defw2_binding_t *binding, const char *api,
			  const char *method, const char *request,
			  const defw2_call_opts_t *opts, char **answer,
			  defw2_status_t *status);
bool defw2_doc_method_ok(const char *method);
typedef defw2_rc_t (*defw2_doc_fn)(void *arg, defw2_call_t *call,
				   const char *method, const char *request,
				   const char **answer);
typedef struct { defw2_doc_fn call; void *arg; } defw2_doc_ops_t;
defw2_rc_t defw2_doc_bind(defw2_service_t *svc, const char *api,
			  const defw2_doc_ops_t *ops);
const char *defw2_call_document(const defw2_call_t *call);
defw2_rc_t defw2_service_respond_document(defw2_call_t *call,
					  const char *answer);
/* A document's answer is the caller's, from malloc. */
void free(void *ptr);

/* bulk results */
typedef enum {
	DEFW2_DTYPE_NONE, DEFW2_DTYPE_U8, DEFW2_DTYPE_I32, DEFW2_DTYPE_I64,
	DEFW2_DTYPE_F32, DEFW2_DTYPE_F64, DEFW2_DTYPE_C64, DEFW2_DTYPE_C128,
	...
} defw2_dtype_t;
#define DEFW2_TENSOR_RANK_MAX ...
typedef struct {
	uint32_t dtype; uint32_t rank; uint64_t shape[...]; uint64_t nbytes;
} defw2_tensor_t;
typedef struct { void *data; size_t capacity; } defw2_result_buffer_t;
size_t defw2_dtype_size(uint32_t dtype);
bool defw2_tensor_valid(const defw2_tensor_t *tensor);
bool defw2_tensor_vector(defw2_tensor_t *tensor, uint32_t dtype,
			 uint64_t count);

/* answering a typed call */
void *defw2_call_response(defw2_call_t *call);
void *defw2_call_alloc(defw2_call_t *call, size_t size);
char *defw2_call_strdup(defw2_call_t *call, const char *s);
char *defw2_call_strndup(defw2_call_t *call, const char *s, size_t len);
void *defw2_call_bulk_reply(defw2_call_t *call, uint64_t nbytes);
uint64_t defw2_call_result_capacity(const defw2_call_t *call);
const char *defw2_call_traceparent(const defw2_call_t *call);

/*
 * Events, declared in full for the reason the QPM APIs below are. A Python
 * sink never has a callback: Python takes events with next.
 */
#define DEFW2_PROVIDER_EVENT ...
typedef struct {
	const char *address; uint16_t provider_id; const char *tag;
} defw2_event_target_t;
typedef struct {
	const char *api; const char *name; const char *type; const char *tag;
	const char *source; uint64_t seq; const char *traceparent;
	const void *payload; void *arena;
} defw2_event_t;
typedef struct defw2_event_sink defw2_event_sink_t;
typedef struct defw2_event_publisher defw2_event_publisher_t;
typedef void (*defw2_event_cb_t)(const defw2_event_t *event, void *arg);
typedef struct {
	defw2_event_cb_t callback; void *arg; unsigned depth;
	uint64_t max_bytes;
} defw2_event_sink_opts_t;
typedef struct {
	uint64_t received; uint64_t refused; uint64_t waiting;
} defw2_event_sink_stats_t;
typedef struct {
	uint32_t timeout_ms; unsigned depth; uint64_t max_bytes;
} defw2_event_publisher_opts_t;
typedef struct {
	uint64_t published; uint64_t delivered; uint64_t refused;
	uint64_t failed; uint64_t dropped; uint64_t waiting; uint32_t targets;
} defw2_event_publisher_stats_t;

void defw2_event_free(defw2_event_t *event);
defw2_rc_t defw2_event_sink_create(defw2_rt_t *rt, uint16_t provider_id,
				   const defw2_event_sink_opts_t *opts,
				   defw2_event_sink_t **sink);
void defw2_event_sink_destroy(defw2_event_sink_t *sink);
defw2_rc_t defw2_event_sink_next(defw2_event_sink_t *sink,
				 uint32_t timeout_ms, defw2_event_t *event);
const char *defw2_event_sink_address(const defw2_event_sink_t *sink);
uint16_t defw2_event_sink_provider_id(const defw2_event_sink_t *sink);
void defw2_event_sink_stats(defw2_event_sink_t *sink,
			    defw2_event_sink_stats_t *stats);
defw2_rc_t defw2_event_publisher_create(defw2_rt_t *rt,
					const defw2_event_publisher_opts_t *opts,
					defw2_event_publisher_t **pub);
void defw2_event_publisher_destroy(defw2_event_publisher_t *pub);
bool defw2_event_target_gone(defw2_rc_t rc);
void defw2_event_publisher_stats(defw2_event_publisher_t *pub,
				 defw2_event_publisher_stats_t *stats);

/*
 * The QPM APIs. Declared in full rather than with "...", so the compile
 * checks every field's type and offset against defw2_qpm.h and a drifted
 * header is a build error here rather than a corrupted call.
 */
#define DEFW2_PROVIDER_QPM_CONTROL ...
#define DEFW2_PROVIDER_QPM_ADMISSION ...
#define DEFW2_PROVIDER_QPM_EXECUTION ...
#define DEFW2_QPM_VERSION ...
#define DEFW2_STR_MAX ...
#define DEFW2_EAGER_MAX ...

typedef struct {
	uint64_t reservation_id; const char *token;
} defw2_qpm_ctx_t;
typedef struct {
	uint64_t count; uint32_t qubit_count; uint32_t depth;
	uint64_t one_q_gate_count; uint64_t two_q_gate_count;
	uint64_t shots; uint64_t measurement_count;
} defw2_qpm_task_class_t;
typedef struct {
	defw2_qpm_ctx_t ctx; uint64_t request_id; const char *user;
	const char *job_id; const char *allocation_id;
	const char *target_device_id; const char *scope_id;
	const char *workload_kind; uint32_t num_qubits; uint64_t walltime_ns;
	uint64_t ttl_ns; bool has_task_class;
	defw2_qpm_task_class_t task_class; const char *extra;
} defw2_qpm_reserve_req_t;
typedef struct {
	defw2_qpm_ctx_t ctx; uint64_t ttl_ns; const char *extra;
} defw2_qpm_renew_req_t;
typedef struct {
	defw2_qpm_ctx_t ctx; uint32_t reason_code;
} defw2_qpm_close_req_t;
typedef struct {
	const char *format; const void *data; size_t len;
} defw2_qpm_circuit_t;
typedef struct {
	defw2_qpm_ctx_t ctx; defw2_qpm_circuit_t circuit; uint32_t num_qubits;
	uint32_t num_shots; const char *compiler; bool return_statevector;
	bool has_timeout; uint64_t timeout_ms; bool cancel_on_timeout;
	const char *extra;
} defw2_qpm_run_req_t;
typedef struct {
	defw2_qpm_ctx_t ctx; const char *cid; uint64_t qtask_id;
	const char *reason;
} defw2_qpm_task_req_t;
typedef struct {
	defw2_qpm_ctx_t ctx; defw2_event_target_t target; const char *type;
	const char *extra;
} defw2_qpm_notify_req_t;
typedef struct {
	const char *state; bool ready; bool initialized;
	bool accepting_requests; bool provider_ready;
	uint32_t active_task_count; uint32_t active_reservation_count;
	const char *extra; void *arena;
} defw2_qpm_service_status_t;
typedef struct {
	const char *decision; uint64_t reservation_id; uint64_t request_id;
	const char *reason; uint32_t reason_code; uint64_t retry_after_ns;
	const char *message; const char *extra; void *arena;
} defw2_qpm_decision_t;
typedef struct {
	uint64_t reservation_id; const char *state; uint64_t created_at_ns;
	uint64_t expires_at_ns; const char *extra; void *arena;
} defw2_qpm_reservation_t;
typedef struct {
	const char *outcome; const char *lifecycle_state; const char *cid;
	uint64_t qtask_id; uint64_t reservation_id; const char *reason;
	const char *message; bool completion_ready;
	defw2_tensor_t statevector; bool statevector_delivered;
	const char *extra; void *arena;
} defw2_qpm_task_t;

void defw2_qpm_service_status_free(defw2_qpm_service_status_t *status);
void defw2_qpm_decision_free(defw2_qpm_decision_t *decision);
void defw2_qpm_reservation_free(defw2_qpm_reservation_t *reservation);
void defw2_qpm_task_free(defw2_qpm_task_t *task);

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
defw2_rc_t defw2_qpm_register_event_notification(
	defw2_binding_t *qpm, const defw2_qpm_notify_req_t *req,
	const defw2_call_opts_t *opts, defw2_qpm_decision_t *out,
	defw2_status_t *status);
defw2_rc_t defw2_qpm_event_accept(defw2_event_sink_t *sink);
defw2_rc_t defw2_qpm_publish_completion(defw2_event_publisher_t *pub,
					const defw2_event_target_t *target,
					const char *type,
					const defw2_qpm_task_t *task,
					const char *traceparent);
const defw2_qpm_task_t *defw2_qpm_event_task(const defw2_event_t *event);
defw2_rc_t defw2_qpm_control_bind(defw2_service_t *svc, const void *ops);
defw2_rc_t defw2_qpm_admission_bind(defw2_service_t *svc, const void *ops);
defw2_rc_t defw2_qpm_execution_bind(defw2_service_t *svc, const void *ops);

/* the directory */
typedef enum {
	DEFW2_DIR_STATE_UP, DEFW2_DIR_STATE_DOWN, DEFW2_DIR_STATE_TIMED_OUT,
	DEFW2_DIR_STATE_DEREGISTERED, ...
} defw2_dir_state_t;
typedef enum {
	DEFW2_DIR_MATCH_EQUAL, DEFW2_DIR_MATCH_BITS_ALL,
	DEFW2_DIR_MATCH_BITS_ANY, ...
} defw2_dir_match_t;
typedef struct {
	const char *binding_name; const char *api_id; uint32_t api_version;
	uint16_t provider_id;
} defw2_dir_binding_t;
typedef struct {
	const char *node_name; const char *hostname; int32_t pid;
} defw2_dir_endpoint_t;
typedef struct {
	const char *name; const char *const *aliases; size_t alias_count;
	const char *const *resources; size_t resource_count;
} defw2_dir_selector_t;
typedef struct { const char *name; const char *value; } defw2_dir_property_t;
typedef struct {
	const char *service_id; const char *service_type;
	const char *runtime_id; uint64_t generation; defw2_dir_state_t state;
	const char *address; defw2_dir_endpoint_t endpoint;
	const defw2_dir_binding_t *bindings; size_t binding_count;
	defw2_dir_selector_t selector; const defw2_dir_property_t *properties;
	size_t property_count; uint64_t registered_at_ns;
	uint64_t last_heartbeat_ns; uint64_t retention_deadline_ns;
} defw2_dir_record_t;
typedef struct {
	const char *name; const char *value; defw2_dir_match_t match;
} defw2_dir_filter_t;
typedef struct {
	const char *service_id; const char *service_type;
	const char *selector_name; const char *resource;
	const char *binding_name; uint32_t api_version;
	const defw2_dir_filter_t *filters; size_t filter_count;
	bool include_inactive; size_t limit;
} defw2_dir_query_t;
typedef struct {
	defw2_dir_record_t record; defw2_dir_binding_t binding;
} defw2_dir_entry_t;
typedef struct {
	defw2_dir_entry_t *entries; size_t entry_count; void *arena;
} defw2_dir_result_t;
typedef struct defw2_dir defw2_dir_t;
typedef struct defw2_dir_agent defw2_dir_agent_t;

#define DEFW2_DIR_CONNECTED ...
#define DEFW2_DIR_DISCONNECTED ...
#define DEFW2_DIR_RUNTIME_ID_LEN ...
typedef struct {
	defw2_event_target_t target; const char *service_id;
	const char *service_type; uint32_t changes;
} defw2_dir_subscribe_req_t;
typedef struct {
	bool connected; const char *reason; defw2_dir_record_t record;
} defw2_dir_change_t;

const char *defw2_dir_state_name(defw2_dir_state_t state);
defw2_rc_t defw2_dir_open(defw2_rt_t *rt, const char *address,
			  defw2_dir_t **dir);
void defw2_dir_close(defw2_dir_t *dir);
defw2_rc_t defw2_dir_resolve(defw2_dir_t *dir, const defw2_dir_query_t *query,
			     const defw2_call_opts_t *opts,
			     defw2_dir_result_t *result,
			     defw2_status_t *status);
defw2_rc_t defw2_dir_query(defw2_dir_t *dir, const defw2_dir_query_t *query,
			   const defw2_call_opts_t *opts,
			   defw2_dir_result_t *result,
			   defw2_status_t *status);
defw2_rc_t defw2_dir_generation(defw2_dir_t *dir, const char *service_id,
				const defw2_call_opts_t *opts,
				uint64_t *generation, defw2_status_t *status);
void defw2_dir_result_free(defw2_dir_result_t *result);
defw2_rc_t defw2_dir_agent_start(defw2_service_t *svc, const char *dir_address,
				 const defw2_dir_record_t *record,
				 uint32_t interval_ms,
				 defw2_dir_agent_t **agent);
void defw2_dir_agent_stop(defw2_dir_agent_t *agent);
uint64_t defw2_dir_agent_generation(const defw2_dir_agent_t *agent);
const char *defw2_dir_agent_runtime_id(const defw2_dir_agent_t *agent);
defw2_rc_t defw2_dir_subscribe(defw2_dir_t *dir,
			       const defw2_dir_subscribe_req_t *req,
			       const defw2_call_opts_t *opts,
			       uint64_t *subscription_id,
			       defw2_status_t *status);
defw2_rc_t defw2_dir_unsubscribe(defw2_dir_t *dir, uint64_t subscription_id,
				 const defw2_call_opts_t *opts,
				 defw2_status_t *status);
defw2_rc_t defw2_dir_runtime_id(defw2_dir_t *dir,
				const defw2_call_opts_t *opts,
				char *runtime_id, size_t len,
				defw2_status_t *status);
defw2_rc_t defw2_dir_event_accept(defw2_event_sink_t *sink);
const defw2_dir_change_t *defw2_dir_event_change(const defw2_event_t *event);
const char *defw2_dirsvc(const defw2_rt_t *rt);
const char *defw2_hostname(const defw2_rt_t *rt);
/* telemetry */
bool defw2_profiling(const defw2_rt_t *rt);
const char *defw2_trace_id(const defw2_rt_t *rt);
defw2_rc_t defw2_telemetry_resource(defw2_rt_t *rt, const char *key,
				    const char *value);
defw2_rc_t defw2_telemetry_run_begin(defw2_rt_t *rt, const char *name);
void defw2_telemetry_run_attr(defw2_rt_t *rt, const char *key,
			      const char *value);
void defw2_telemetry_run_attr_int(defw2_rt_t *rt, const char *key,
				  int64_t value);
void defw2_telemetry_run_end(defw2_rt_t *rt, const char *error);
defw2_rc_t defw2_telemetry_flush(defw2_rt_t *rt);
void defw2_process_stats(defw2_process_stats_t *stats);
"""

SOURCE = """
#include <stdlib.h>

#include <defw2/defw2.h>
#include <defw2/defw2_rpc.h>
#include <defw2/defw2_service.h>
#include <defw2/defw2_bulk.h>
#include <defw2/defw2_dir.h>
#include <defw2/defw2_doc.h>
#include <defw2/defw2_echo.h>
#include <defw2/defw2_event.h>
#include <defw2/defw2_qpm.h>
#include <defw2/defw2_telemetry.h>
"""


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument('--include', required=True,
			    help="where defw2/defw2.h is")
	parser.add_argument('--library', required=True,
			    help='where libdefw2 is')
	parser.add_argument('--out', required=True,
			    help='where to put the built package')
	parser.add_argument('--rpath', action='append', default=[],
			    help='where the extension looks for libdefw2, '
				 'such as $ORIGIN relative to an install, in '
				 'place of --library; repeatable')
	args = parser.parse_args()

	builder = FFI()
	builder.cdef(CDEF)
	builder.set_source(
		'defw2._defw2', SOURCE,
		include_dirs=[os.path.abspath(args.include)],
		library_dirs=[os.path.abspath(args.library)],
		libraries=['defw2'],
		# So the extension finds libdefw2 without LD_LIBRARY_PATH: in
		# the build tree, or for an install only where the install
		# put it, so it never reaches back into a build tree.
		extra_link_args=['-Wl,-rpath,' + path for path in
				 args.rpath or [os.path.abspath(args.library)]])

	os.makedirs(args.out, exist_ok=True)
	built = builder.compile(tmpdir=args.out, verbose=False)
	print('built ' + built)
	return 0


if __name__ == '__main__':
	sys.exit(main())
