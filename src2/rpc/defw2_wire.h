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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <margo.h>
#include <mercury_macros.h>
#include <mercury_proc.h>

#include <defw2/defw2_rpc.h>

#include "defw2_internal.h"

/*
 * Strings.
 *
 * Mercury's own string procs trust the sender twice, which is verified in
 * mercury_proc_string.c and mercury_proc.h at 2.4.1. The decoder allocates
 * whatever length the message claims before checking that the bytes are
 * there, and when they are not, Mercury grows its buffer and copies
 * uninitialised memory into the string instead of failing. It also never
 * checks for the terminator, so a sender that leaves the NUL off hands the
 * receiver a string that strlen, strdup and strcmp read straight past.
 *
 * Every string v2 decodes therefore goes through this proc instead. It
 * refuses a length longer than the field allows or longer than the bytes
 * actually left in the message, and it refuses a string whose last byte is
 * not its only NUL. Both checks happen before the string is visible to
 * anything, which is what the design means by no unsafe deserialization.
 *
 * A length of zero is an absent string and decodes to NULL, so NULL and ""
 * are different values on this wire, which Mercury's procs could not say.
 */
static HG_INLINE hg_return_t defw2_proc_cstr(hg_proc_t proc, const char **str,
					     hg_uint64_t max)
{
	hg_uint64_t len = 0;
	hg_return_t ret;
	char *buf;

	switch (hg_proc_get_op(proc)) {
	case HG_ENCODE:
		len = *str != NULL ? (hg_uint64_t)strlen(*str) + 1 : 0;
		/* Refuse to send what the receiver would refuse. */
		if (len > max)
			return HG_OVERFLOW;
		ret = hg_proc_hg_uint64_t(proc, &len);
		if (ret != HG_SUCCESS || len == 0)
			return ret;
		return hg_proc_bytes(proc, (void *)(uintptr_t)*str, len);
	case HG_DECODE:
		*str = NULL;
		ret = hg_proc_hg_uint64_t(proc, &len);
		if (ret != HG_SUCCESS || len == 0)
			return ret;
		if (len > max || len > hg_proc_get_size_left(proc))
			return HG_OVERFLOW;
		buf = (char *)malloc(len);
		if (buf == NULL)
			return HG_NOMEM;
		ret = hg_proc_bytes(proc, buf, len);
		if (ret == HG_SUCCESS &&
		    (buf[len - 1] != '\0' || memchr(buf, '\0', len - 1) != NULL))
			ret = HG_PROTOCOL_ERROR;
		if (ret != HG_SUCCESS) {
			free(buf);
			return ret;
		}
		*str = buf;
		return HG_SUCCESS;
	case HG_FREE:
		free((void *)(uintptr_t)*str);
		*str = NULL;
		return HG_SUCCESS;
	}
	return HG_SUCCESS;
}

/*
 * An identifier, a name or a message, at most DEFW2_STR_MAX bytes with its
 * terminator. Const because a sender fills these from strings it does not
 * own. A decoded one belongs to Mercury until the input or output is freed.
 */
typedef const char *defw2_str_t;

static HG_INLINE hg_return_t hg_proc_defw2_str_t(hg_proc_t proc, void *arg)
{
	return defw2_proc_cstr(proc, (const char **)arg, DEFW2_STR_MAX);
}

/*
 * A document carried as text, such as the JSON a typed answer carries
 * beside its typed fields. It may be as long as anything else that travels
 * inside a message.
 */
typedef const char *defw2_text_t;

static HG_INLINE hg_return_t hg_proc_defw2_text_t(hg_proc_t proc, void *arg)
{
	return defw2_proc_cstr(proc, (const char **)arg, DEFW2_EAGER_MAX);
}

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

	/*
	 * A free walks whatever a decode left behind, which may be a length
	 * the decode refused, so it trusts nothing but the pointer.
	 */
	if (hg_proc_get_op(proc) == HG_FREE) {
		free(bytes->data);
		bytes->data = NULL;
		return HG_SUCCESS;
	}

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
		if (hg_proc_get_op(proc) == HG_DECODE)
			bytes->data = NULL;
		return HG_SUCCESS;
	}

	switch (hg_proc_get_op(proc)) {
	case HG_DECODE:
		/*
		 * The same check the string proc makes: a length the message
		 * does not actually carry would otherwise allocate it and
		 * fill it from memory Mercury never received.
		 */
		if (bytes->len > hg_proc_get_size_left(proc)) {
			bytes->data = NULL;
			return HG_OVERFLOW;
		}
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
	default:
		return HG_SUCCESS;
	}
}

/*
 * The header every request carries, always as the first member of the
 * request, which is what lets the shared typed code reach it without knowing
 * the rest of the structure.
 *
 * api_version is the version of the API the method belongs to. DEFw's own
 * APIs, echo and the directory, speak DEFW2_API_VERSION. An API defined on
 * top of DEFw, such as the QPM's, speaks its own.
 */
MERCURY_GEN_PROC(defw2_hdr_t,
	((hg_uint32_t)(api_version))
	((hg_uint64_t)(correlation_id))
	((defw2_str_t)(runtime_id))
	((defw2_str_t)(traceparent))
	((hg_uint64_t)(client_send_ns)))

/*
 * The status every response carries, always as the first member of the
 * response. It mirrors defw2_status_t, which is the public form, but stays a
 * separate type so the public header needs no Mercury.
 */
MERCURY_GEN_PROC(defw2_wire_status_t,
	((hg_int32_t)(code))
	((hg_uint32_t)(category))
	((defw2_str_t)(message)))

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

/*
 * Fill the header of an outgoing request for an API at api_version.
 * traceparent may be NULL.
 */
void defw2_hdr_fill(struct defw2_rt *rt, defw2_hdr_t *hdr,
		    uint32_t api_version, const char *traceparent);

/*
 * Is the caller speaking a version of the API this provider understands,
 * that is, the same major? Mercury has already decoded the whole request by
 * the time a handler runs, so the promise the design makes, that a mismatch
 * is refused before the payload is read, is kept by refusing before the
 * payload is used.
 */
bool defw2_hdr_compatible(const defw2_hdr_t *hdr, uint32_t api_version);

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

/*
 * Free what a decode that failed part way allocated, walking data with
 * proc_cb in HG_FREE mode.
 *
 * Not margo_free_input or margo_free_output: Mercury's free also drops a
 * reference on the handle, and only a decode that succeeded took one. After
 * a failed decode that reference is not there to drop, so Mercury recycles
 * the handle while the handler still holds it, the reply is lost and the
 * next request on that buffer fails ("Cannot re-use handle"). This walks the
 * fields with a proc of its own and leaves the handle alone. data must have
 * been zeroed before the decode, so the fields it never reached are empty.
 */
void defw2_free_partial(margo_instance_id mid, hg_proc_cb_t proc_cb,
			void *data);

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
