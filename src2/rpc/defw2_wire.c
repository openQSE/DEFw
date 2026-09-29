/*
 * The pieces of the wire that are not generated: header, status and the
 * mapping from Mercury's outcomes onto DEFw's.
 */
#include "defw2_wire.h"

void defw2_hdr_fill(struct defw2_rt *rt, defw2_hdr_t *hdr,
		    const char *traceparent)
{
	hdr->api_version = DEFW2_API_VERSION;
	hdr->correlation_id = __atomic_add_fetch(&rt->correlation, 1,
						 __ATOMIC_RELAXED);
	hdr->runtime_id = rt->runtime_id;
	hdr->traceparent = traceparent ? traceparent : "";
	hdr->client_send_ns = defw2_wall_ns();
}

bool defw2_hdr_compatible(const defw2_hdr_t *hdr)
{
	return (hdr->api_version >> 16) == DEFW2_API_VERSION_MAJOR;
}

void defw2_status_from_wire(defw2_status_t *status,
			    const defw2_wire_status_t *wire)
{
	if (status == NULL)
		return;
	status->code = wire->code;
	status->category = wire->category;
	status->message = (wire->message != NULL && wire->message[0] != '\0')
				  ? strdup(wire->message)
				  : NULL;
}

void defw2_wire_status_ok(defw2_wire_status_t *wire)
{
	wire->code = DEFW2_OK;
	wire->category = DEFW2_CAT_OK;
	wire->message = "";
}

void defw2_wire_status_set(defw2_wire_status_t *wire, defw2_rc_t code,
			   uint32_t category, const char *message)
{
	wire->code = code;
	wire->category = category;
	wire->message = message ? message : "";
}

defw2_rc_t defw2_rc_from_hg(hg_return_t hret, uint32_t *category)
{
	defw2_rc_t rc;
	uint32_t cat;

	switch (hret) {
	case HG_SUCCESS:
		rc = DEFW2_OK;
		cat = DEFW2_CAT_OK;
		break;
	case HG_TIMEOUT:
		rc = DEFW2_ERR_TIMEOUT;
		cat = DEFW2_CAT_TIMEOUT;
		break;
	case HG_CANCELED:
		rc = DEFW2_ERR_CANCELLED;
		cat = DEFW2_CAT_CANCELLED;
		break;
	case HG_NOENTRY:
		rc = DEFW2_ERR_NOT_FOUND;
		cat = DEFW2_CAT_NOT_FOUND;
		break;
	case HG_INVALID_ARG:
	case HG_OVERFLOW:
	case HG_MSGSIZE:
		rc = DEFW2_ERR_INVALID;
		cat = DEFW2_CAT_INVALID_ARGUMENT;
		break;
	case HG_NOMEM:
		rc = DEFW2_ERR_NOMEM;
		cat = DEFW2_CAT_PROVIDER_FAILURE;
		break;
	default:
		rc = DEFW2_ERR_TRANSPORT;
		cat = DEFW2_CAT_TRANSPORT;
		break;
	}
	if (category != NULL)
		*category = cat;
	return rc;
}

hg_id_t defw2_rpc_lookup(struct defw2_rt *rt, const char *name,
			 hg_proc_cb_t in_cb, hg_proc_cb_t out_cb)
{
	hg_id_t id = 0;
	int i;

	pthread_mutex_lock(&rt->rpc_lock);
	for (i = 0; i < rt->rpc_cached; i++) {
		if (strcmp(rt->rpc_cache[i].name, name) == 0) {
			id = rt->rpc_cache[i].id;
			goto out;
		}
	}

	/*
	 * A caller registers with the default provider identifier. Margo puts
	 * the target provider into the identifier when the call is forwarded,
	 * so one registration reaches every provider.
	 */
	id = margo_register_name(rt->mid, name, in_cb, out_cb, NULL);
	if (id == 0) {
		defw2_log(rt, DEFW2_LOG_ERROR, "cannot register %s", name);
		goto out;
	}
	if (rt->rpc_cached == DEFW2_RPC_CACHE_MAX) {
		/* Registering again each call works, but Mercury says so every
		 * time and the scan is no longer bounded. Raise the limit. */
		defw2_log(rt, DEFW2_LOG_WARNING,
			  "the RPC cache is full at %d, %s will re-register",
			  DEFW2_RPC_CACHE_MAX, name);
		goto out;
	}
	/*
	 * The name is copied, not borrowed. Every caller today passes a
	 * literal, but the cache outlives the call and phase 1 looks names up
	 * from the directory, so a borrowed pointer would dangle in a table
	 * that is read on every lookup. A copy that fails costs only the
	 * cache entry, so the call itself still goes ahead.
	 */
	rt->rpc_cache[rt->rpc_cached].name = strdup(name);
	if (rt->rpc_cache[rt->rpc_cached].name == NULL) {
		defw2_log(rt, DEFW2_LOG_WARNING,
			  "cannot cache %s, it will re-register", name);
		goto out;
	}
	rt->rpc_cache[rt->rpc_cached].id = id;
	rt->rpc_cached++;
out:
	pthread_mutex_unlock(&rt->rpc_lock);
	return id;
}
