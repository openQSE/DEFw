/*
 * Hosting DEFw v2 services.
 *
 * A service is a Margo provider: one identifier, one address and one set of
 * registered methods. There are no remote objects and no per-connection
 * instances, so a service is a singleton by construction. State that belongs
 * to one caller travels in the request.
 *
 *	defw2_service_t *svc = NULL;
 *
 *	defw2_service_create(rt, "echo-1", DEFW2_API_ECHO,
 *			     DEFW2_PROVIDER_ECHO, &svc);
 *	defw2_echo_bind(svc, NULL);
 *	defw2_service_run(svc);			 // until shutdown
 *	defw2_service_destroy(svc);
 *
 * Each API binds its own operations table, which is what keeps the host free
 * of any knowledge of a particular API. Directory registration, heartbeats
 * and the Python call queue join this in later phases.
 *
 * Handlers run on Margo's handler pool. A handler may block on a provider
 * call, because the progress loop has its own execution stream. A handler
 * must not call into a foreign runtime, which is the rule the Python binding
 * exists to enforce.
 */
#ifndef DEFW2_SERVICE_H
#define DEFW2_SERVICE_H

#include <defw2/defw2.h>
#include <defw2/defw2_rpc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct defw2_service defw2_service_t;

/*
 * The runtime must have been initialised with DEFW2_ROLE_SERVER, since a
 * client-mode Margo instance does not listen. provider_id separates services
 * that share one process and one address.
 */
defw2_rc_t defw2_service_create(defw2_rt_t *rt, const char *service_id,
				const char *service_type, uint16_t provider_id,
				defw2_service_t **svc);

/*
 * Destroy the service handle. Call this before defw2_finalize. The
 * registered methods and their operations tables belong to the Margo
 * instance and are released when the runtime finalizes, so an RPC still in
 * flight is safe.
 */
void defw2_service_destroy(defw2_service_t *svc);

/* Block until another thread calls defw2_service_shutdown. */
void defw2_service_run(defw2_service_t *svc);

/*
 * Stop serving. Safe from any thread, and safe to call more than once. It
 * waits for handlers that are already running. The runtime is spent
 * afterwards, so the next call on it is defw2_finalize.
 */
void defw2_service_shutdown(defw2_service_t *svc);

/*
 * Serving from a foreign runtime.
 *
 * A Python service cannot run on a Margo thread, so it does not. The C
 * handler decodes the request, puts it on this queue and parks on an
 * Argobots eventual. A thread the runtime knows nothing about blocks in
 * defw2_service_next_call, answers with defw2_service_respond, and that
 * wakes the handler to encode the reply. Python runs on Python threads,
 * Margo runs on Margo threads, and the queue is the only thing they share.
 *
 *	defw2_service_queue_open(svc, 0);
 *	while (defw2_service_next_call(svc, 1000, &call) == DEFW2_OK) {
 *		request = defw2_call_request(call, &len);
 *		defw2_service_respond(call, reply, reply_len);
 *	}
 *
 * A call handle belongs to the parked handler, not to the caller of
 * next_call, and it is spent the moment it is answered. Nothing may touch
 * it after that.
 */
typedef struct defw2_call defw2_call_t;

/*
 * Route this service's methods to the queue instead of to their operations
 * tables. depth is the most calls that may wait at once, or 0 for as many
 * as arrive. A caller that finds the queue full is told the service is at
 * capacity rather than made to wait.
 */
defw2_rc_t defw2_service_queue_open(defw2_service_t *svc, unsigned depth);

/*
 * Stop queueing. Calls that are waiting are failed, and consumers blocked
 * in next_call return. A call already taken by a consumer is that
 * consumer's to answer, and its handler stays parked until it does.
 */
void defw2_service_queue_close(defw2_service_t *svc);
bool defw2_service_queued(const defw2_service_t *svc);

/*
 * Wait for the next call. Returns DEFW2_ERR_TIMEOUT when nothing arrived in
 * time and DEFW2_ERR_NOT_FOUND once the queue is closed, which is how a
 * serving loop learns to stop. It holds no lock while it waits, so a
 * binding may release its interpreter lock around it.
 */
defw2_rc_t defw2_service_next_call(defw2_service_t *svc, uint32_t timeout_ms,
				   defw2_call_t **call);

/* What the call is. The request bytes last until the call is answered. */
const char *defw2_call_api(const defw2_call_t *call);
const char *defw2_call_method(const defw2_call_t *call);
const void *defw2_call_request(const defw2_call_t *call, size_t *len);

/* Answer it. reply is copied, so the caller keeps nothing. */
defw2_rc_t defw2_service_respond(defw2_call_t *call, const void *reply,
				 size_t len);
defw2_rc_t defw2_service_fail(defw2_call_t *call, defw2_rc_t code,
			      uint32_t category, const char *message);

const char *defw2_service_id(const defw2_service_t *svc);
const char *defw2_service_type(const defw2_service_t *svc);
const char *defw2_service_address(const defw2_service_t *svc);
uint16_t defw2_service_provider_id(const defw2_service_t *svc);
defw2_rt_t *defw2_service_runtime(const defw2_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_SERVICE_H */
