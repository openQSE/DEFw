/*
 * The wire, shared inside libdefw2 and not installed.
 *
 * Every typed method carries the same header and answers with the same
 * status, so profiling, tracing, versioning and error handling do not depend
 * on which method was called. Mercury's generated proc functions do the
 * encoding; this header only declares the structures and the few helpers
 * that every stub and every handler needs.
 *
 * Registered RPC names are defw2.<api>.<method>, so two versions of an API
 * can be registered side by side during a transition.
 */
#ifndef DEFW2_WIRE_H
#define DEFW2_WIRE_H

#include <stdlib.h>
#include <string.h>

#include <margo.h>
#include <mercury_macros.h>
#include <mercury_proc.h>
#include <mercury_proc_string.h>

#include <defw2/defw2_rpc.h>

#include "defw2_internal.h"

/*
 * Counted bytes, for a payload that travels inside the message. On decode
 * the buffer is Mercury's to free, which happens in margo_free_input or
 * margo_free_output.
 */
typedef struct {
	hg_uint64_t	len;
	char		*data;
} defw2_bytes_t;

static HG_INLINE hg_return_t hg_proc_defw2_bytes_t(hg_proc_t proc, void *arg)
{
	defw2_bytes_t *bytes = (defw2_bytes_t *)arg;
	hg_return_t ret;

	ret = hg_proc_hg_uint64_t(proc, &bytes->len);
	if (ret != HG_SUCCESS)
		return ret;
	/*
	 * A length is the first thing a caller controls, so it is the first
	 * thing the decoder refuses. Anything larger belongs in a bulk
	 * transfer, where the memory is the caller's own.
	 */
	if (bytes->len > DEFW2_EAGER_MAX)
		return HG_OVERFLOW;
	if (bytes->len == 0) {
		if (hg_proc_get_op(proc) != HG_ENCODE)
			bytes->data = NULL;
		return HG_SUCCESS;
	}

	switch (hg_proc_get_op(proc)) {
	case HG_DECODE:
		bytes->data = (char *)malloc(bytes->len);
		if (bytes->data == NULL)
			return HG_NOMEM;
		ret = hg_proc_memcpy(proc, bytes->data, bytes->len);
		if (ret != HG_SUCCESS) {
			/*
			 * A decoded buffer is released through HG_FREE, which
			 * a decode that fails may never reach. Drop it here
			 * and leave NULL, so a later HG_FREE is a no-op
			 * whichever way Mercury unwinds.
			 */
			free(bytes->data);
			bytes->data = NULL;
		}
		return ret;
	case HG_ENCODE:
		return hg_proc_memcpy(proc, bytes->data, bytes->len);
	case HG_FREE:
		free(bytes->data);
		bytes->data = NULL;
		return HG_SUCCESS;
	}
	return HG_SUCCESS;
}

/* The header every request carries. Strings are never NULL on the wire. */
MERCURY_GEN_PROC(defw2_hdr_t,
	((hg_uint32_t)(api_version))
	((hg_uint64_t)(correlation_id))
	((hg_const_string_t)(runtime_id))
	((hg_const_string_t)(traceparent))
	((hg_uint64_t)(client_send_ns)))

/*
 * The status every response carries. It mirrors defw2_status_t, which is
 * the public form, but stays a separate type so the public header needs no
 * Mercury.
 */
MERCURY_GEN_PROC(defw2_wire_status_t,
	((hg_int32_t)(code))
	((hg_uint32_t)(category))
	((hg_const_string_t)(message)))

MERCURY_GEN_PROC(defw2_echo_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_bytes_t)(payload)))

MERCURY_GEN_PROC(defw2_echo_out_t,
	((defw2_wire_status_t)(status))
	((defw2_bytes_t)(payload)))

/*
 * Bulk echo. source is the caller's buffer to read and sink the caller's
 * buffer to write, both registered by the caller, so the service holds no
 * registration of the caller's memory beyond the call.
 */
MERCURY_GEN_PROC(defw2_echo_bulk_in_t,
	((defw2_hdr_t)(hdr))
	((hg_uint64_t)(nbytes))
	((hg_bulk_t)(source))
	((hg_bulk_t)(sink)))

MERCURY_GEN_PROC(defw2_echo_bulk_out_t,
	((defw2_wire_status_t)(status))
	((hg_uint64_t)(pulled))
	((hg_uint64_t)(pushed)))

#define DEFW2_RPC_ECHO		"defw2.qfw.echo.echo"
#define DEFW2_RPC_ECHO_BULK	"defw2.qfw.echo.echo_bulk"

/* A caller's handle on one provider. */
struct defw2_binding {
	struct defw2_rt	*rt;
	char		*address;	/* owned */
	hg_addr_t	addr;
	uint16_t	provider_id;
};

/* Fill the header of an outgoing request. traceparent may be NULL. */
void defw2_hdr_fill(struct defw2_rt *rt, defw2_hdr_t *hdr,
		    const char *traceparent);

/*
 * Is the caller speaking a version this provider understands? Mercury has
 * already decoded the whole request by the time a handler runs, so the
 * promise the design makes, that a mismatch is refused before the payload is
 * read, is kept by refusing before the payload is used.
 */
bool defw2_hdr_compatible(const defw2_hdr_t *hdr);

/* Move a decoded wire status into the public form, copying the message. */
void defw2_status_from_wire(defw2_status_t *status,
			    const defw2_wire_status_t *wire);

/*
 * Fill a response status. message is borrowed and must outlive the respond
 * call, so handlers pass a literal or a buffer on their own stack.
 */
void defw2_wire_status_ok(defw2_wire_status_t *wire);
void defw2_wire_status_set(defw2_wire_status_t *wire, defw2_rc_t code,
			   uint32_t category, const char *message);

/* Map a Mercury outcome onto a DEFw code, and onto a category when asked. */
defw2_rc_t defw2_rc_from_hg(hg_return_t hret, uint32_t *category);

/*
 * The identifier this runtime calls an RPC by, registering it the first time
 * it is asked for. Mercury cannot look a registration up by name once Margo
 * has put the provider into the identifier, so the runtime remembers what it
 * registered. name must outlive the runtime, which a literal does.
 */
hg_id_t defw2_rpc_lookup(struct defw2_rt *rt, const char *name,
			 hg_proc_cb_t in_cb, hg_proc_cb_t out_cb);

#endif /* DEFW2_WIRE_H */
