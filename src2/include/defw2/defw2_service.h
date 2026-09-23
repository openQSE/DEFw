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

const char *defw2_service_id(const defw2_service_t *svc);
const char *defw2_service_type(const defw2_service_t *svc);
const char *defw2_service_address(const defw2_service_t *svc);
uint16_t defw2_service_provider_id(const defw2_service_t *svc);
defw2_rt_t *defw2_service_runtime(const defw2_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_SERVICE_H */
