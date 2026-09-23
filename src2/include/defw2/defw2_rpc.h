/*
 * Calling a DEFw v2 service.
 *
 * A binding is a caller's handle on one provider: an address and a provider
 * identifier, resolved once and reused for every call. Typed stubs, such as
 * the echo API in defw2_echo.h, take a binding and the call options below.
 *
 *	defw2_binding_t *echo = NULL;
 *	defw2_call_opts_t opts = { .timeout_ms = 5000 };
 *	defw2_buffer_t reply = { 0 };
 *	defw2_status_t status = { 0 };
 *
 *	defw2_binding_create(rt, address, DEFW2_PROVIDER_ECHO, &echo);
 *	defw2_echo(echo, "hello", 5, &opts, &reply, &status);
 *	defw2_buffer_free(&reply);
 *	defw2_status_free(&status);
 *	defw2_binding_free(echo);
 *
 * Nothing here exposes Mercury. The wire structures the design shows live
 * inside libdefw2, so a binding generator and a C caller both see plain C.
 * That is a small departure from the design's stub signature, which passes
 * the wire input structure and a timeout: the header travels in the options
 * instead.
 */
#ifndef DEFW2_RPC_H
#define DEFW2_RPC_H

#include <stddef.h>

#include <defw2/defw2.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The wire contract version, major in the high sixteen bits. A provider
 * refuses a request whose major differs from its own with the
 * version-mismatch category, before it reads the payload.
 */
#define DEFW2_API_VERSION_MAJOR	0
#define DEFW2_API_VERSION_MINOR	1
#define DEFW2_API_VERSION	(((uint32_t)DEFW2_API_VERSION_MAJOR << 16) | \
				 (uint32_t)DEFW2_API_VERSION_MINOR)

/* The largest payload that may travel inside a request or a response. */
#define DEFW2_EAGER_MAX		(4u * 1024u * 1024u)

/* The largest single bulk transfer a handler will set up. */
#define DEFW2_BULK_MAX		(1024ull * 1024ull * 1024ull)

typedef struct defw2_binding defw2_binding_t;

/*
 * Per-call settings. Zero initialise for the defaults, which are no timeout
 * and no trace context. traceparent is the caller's W3C value and travels in
 * the RPC header, so v2 spans join QFw's traces the way v1's do.
 */
typedef struct {
	uint32_t	timeout_ms;
	const char	*traceparent;
} defw2_call_opts_t;

/*
 * Resolve a peer address once and keep the binding. The address is what the
 * peer publishes, such as "ofi+tcp://10.0.0.5:8090" or "na+sm://421-0". In
 * phase 1 the directory hands these out. A binding may be shared between
 * threads and outlives any single call.
 */
defw2_rc_t defw2_binding_create(defw2_rt_t *rt, const char *address,
				uint16_t provider_id,
				defw2_binding_t **binding);
void defw2_binding_free(defw2_binding_t *binding);

const char *defw2_binding_address(const defw2_binding_t *binding);
uint16_t defw2_binding_provider_id(const defw2_binding_t *binding);

/*
 * A buffer a stub hands back. data belongs to the caller and is released
 * with defw2_buffer_free, which is the whole ownership rule.
 */
typedef struct {
	void	*data;
	size_t	len;
} defw2_buffer_t;

void defw2_buffer_free(defw2_buffer_t *buffer);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_RPC_H */
