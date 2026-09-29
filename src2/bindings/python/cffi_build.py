#!/usr/bin/env python3
"""Build the defw2 Python extension.

cffi in API mode: the declarations below are compiled against the real
headers, so a mismatch is a compile error rather than a crash at run time.
Only what the binding uses is declared, which also documents exactly how
much of libdefw2 the Python side depends on.

	cffi_build.py --include <dir> --library <dir> --out <dir>

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
#include <defw2/defw2.h>
#include <defw2/defw2_rpc.h>
#include <defw2/defw2_service.h>
#include <defw2/defw2_echo.h>
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
	args = parser.parse_args()

	builder = FFI()
	builder.cdef(CDEF)
	builder.set_source(
		'defw2._defw2', SOURCE,
		include_dirs=[os.path.abspath(args.include)],
		library_dirs=[os.path.abspath(args.library)],
		libraries=['defw2'],
		# So the extension finds libdefw2 without LD_LIBRARY_PATH.
		extra_link_args=['-Wl,-rpath,' + os.path.abspath(args.library)])

	os.makedirs(args.out, exist_ok=True)
	built = builder.compile(tmpdir=args.out, verbose=False)
	print('built ' + built)
	return 0


if __name__ == '__main__':
	sys.exit(main())
