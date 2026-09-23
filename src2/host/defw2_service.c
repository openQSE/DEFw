/*
 * The service host.
 *
 * A service is one Margo provider on the runtime's Margo instance. The host
 * owns the identity and the lifecycle; each API binds its own methods, which
 * is what keeps the host free of any knowledge of a particular API.
 *
 * Directory registration and heartbeats belong here too and arrive in phase
 * 1. Until then a caller reaches a service by the address it prints.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_host.h"

defw2_rc_t defw2_service_create(defw2_rt_t *rt, const char *service_id,
				const char *service_type, uint16_t provider_id,
				defw2_service_t **out)
{
	struct defw2_service *svc;

	if (rt == NULL || service_id == NULL || service_type == NULL ||
	    out == NULL)
		return DEFW2_ERR_INVALID;

	/*
	 * A client-mode Margo instance does not listen, so a service on one
	 * would register methods that nobody can reach. Say so here rather
	 * than let the first call time out.
	 */
	if (rt->role != DEFW2_ROLE_SERVER) {
		defw2_log(rt, DEFW2_LOG_ERROR,
			  "service %s needs a server runtime, set DEFW_AGENT_TYPE",
			  service_id);
		return DEFW2_ERR_CONFIG;
	}

	svc = calloc(1, sizeof(*svc));
	if (svc == NULL)
		return DEFW2_ERR_NOMEM;
	svc->rt = rt;
	svc->provider_id = provider_id;
	svc->service_id = strdup(service_id);
	svc->service_type = strdup(service_type);
	if (svc->service_id == NULL || svc->service_type == NULL) {
		defw2_service_destroy(svc);
		return DEFW2_ERR_NOMEM;
	}

	defw2_log(rt, DEFW2_LOG_MESSAGE, "service %s of type %s, provider %u at %s",
		  svc->service_id, svc->service_type, provider_id, rt->address);
	*out = svc;
	return DEFW2_OK;
}

void defw2_service_destroy(defw2_service_t *svc)
{
	if (svc == NULL)
		return;
	/*
	 * The registered methods and their operations tables belong to the
	 * Margo instance, which releases them when the runtime finalizes.
	 * That is what makes this safe while an RPC is still in flight.
	 */
	free(svc->service_id);
	free(svc->service_type);
	free(svc);
}

void defw2_service_run(defw2_service_t *svc)
{
	if (svc == NULL)
		return;
	/* Nothing to wait for if the runtime has already been stopped. */
	if (__atomic_load_n(&svc->rt->stopping, __ATOMIC_ACQUIRE) != 0)
		return;
	margo_wait_for_finalize(svc->rt->mid);
}

void defw2_service_shutdown(defw2_service_t *svc)
{
	if (svc == NULL)
		return;
	defw2_log(svc->rt, DEFW2_LOG_MESSAGE, "service %s stopping",
		  svc->service_id);
	defw2_runtime_stop(svc->rt);
}

const char *defw2_service_id(const defw2_service_t *svc)
{
	return svc ? svc->service_id : NULL;
}

const char *defw2_service_type(const defw2_service_t *svc)
{
	return svc ? svc->service_type : NULL;
}

const char *defw2_service_address(const defw2_service_t *svc)
{
	return svc ? svc->rt->address : NULL;
}

uint16_t defw2_service_provider_id(const defw2_service_t *svc)
{
	return svc ? svc->provider_id : 0;
}

defw2_rt_t *defw2_service_runtime(const defw2_service_t *svc)
{
	return svc ? svc->rt : NULL;
}
