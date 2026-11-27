/*
 * The echo API, qfw.echo.
 *
 * Echo is the reference service. It exists so that the benchmarks have a
 * method whose own cost is nothing, which makes what they measure the
 * framework and not the work. echo carries its payload inside the message
 * and answers workloads W1 and W2. echo_bulk moves the payload through
 * registered memory in both directions and answers W3.
 *
 * A service supplies operations or takes the built-in ones, which copy the
 * request back unchanged.
 */
#ifndef DEFW2_ECHO_H
#define DEFW2_ECHO_H

#include <defw2/defw2_rpc.h>
#include <defw2/defw2_service.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFW2_API_ECHO		"qfw.echo"
#define DEFW2_PROVIDER_ECHO	1

/*
 * Send len bytes and receive what the service returns. reply is filled on
 * success and released with defw2_buffer_free. status carries the service's
 * own outcome and is released with defw2_status_free. A payload larger than
 * DEFW2_EAGER_MAX belongs in defw2_echo_bulk and is refused here.
 *
 * The return code is the transport outcome: DEFW2_OK means the call reached
 * the service and came back, whatever the service made of it, so callers
 * check the return code and then the status.
 */
defw2_rc_t defw2_echo(defw2_binding_t *binding, const void *payload,
		      size_t len, const defw2_call_opts_t *opts,
		      defw2_buffer_t *reply, defw2_status_t *status);

/*
 * Bulk echo. The service pulls len bytes out of source, applies the
 * service's transform if it has one, and pushes the result into sink. Both
 * buffers belong to the caller and neither travels inside the message, so
 * this is the path a statevector or a large parameter array takes.
 *
 * source and sink may be the same buffer. bytes_moved, when it is not NULL,
 * receives the total pulled plus pushed, which is what a bandwidth number is
 * computed from.
 */
defw2_rc_t defw2_echo_bulk(defw2_binding_t *binding, const void *source,
			   void *sink, size_t len,
			   const defw2_call_opts_t *opts,
			   uint64_t *bytes_moved, defw2_status_t *status);

/*
 * What a service does with an echo. Leave a member NULL for the built-in
 * behaviour, and pass NULL to defw2_echo_bind for all of it.
 *
 * echo owns nothing it is given and allocates the reply with malloc. The
 * host frees the reply once it is on the wire. transform edits the pulled
 * bulk buffer in place, before it is pushed back.
 */
typedef struct {
	void		*ctx;
	defw2_rc_t	(*echo)(void *ctx, const void *request, size_t len,
				void **reply, size_t *reply_len);
	defw2_rc_t	(*transform)(void *ctx, void *buffer, size_t len);
} defw2_echo_ops_t;

/* Register qfw.echo on a service. ops may be NULL for the built-in echo. */
defw2_rc_t defw2_echo_bind(defw2_service_t *svc, const defw2_echo_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_ECHO_H */
