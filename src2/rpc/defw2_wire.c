/*
 * The pieces of the wire that are not generated: header, status and the
 * mapping from Mercury's outcomes onto DEFw's.
 */
#include "defw2_wire.h"

void defw2_hdr_fill(struct defw2_rt *rt, defw2_hdr_t *hdr,
		    uint32_t api_version, const char *traceparent)
{
	hdr->api_version = api_version;
	hdr->correlation_id = __atomic_add_fetch(&rt->correlation, 1,
						 __ATOMIC_RELAXED);
	hdr->runtime_id = rt->runtime_id;
	hdr->traceparent = traceparent ? traceparent : "";
	hdr->client_send_ns = defw2_wall_ns();
}

bool defw2_hdr_compatible(const defw2_hdr_t *hdr, uint32_t api_version)
{
	return (hdr->api_version >> 16) == (api_version >> 16);
}

void defw2_status_from_wire(defw2_status_t *status,
			    const defw2_wire_status_t *wire)
{
	if (status == NULL)
		return;
	/*
	 * Release whatever a previous call left here. Reusing one status
	 * across several calls is the natural way to write a loop, and
	 * without this every message but the last would leak. Zero
	 * initialised is still the caller's obligation before the first call,
	 * since there is nothing to distinguish an uninitialised pointer
	 * from a live one.
	 */
	free(status->message);
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
	hg_bool_t flag;
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
	 * Ask Margo before registering anything.
	 *
	 * margo_register_name is margo_provider_register_name with provider 0
	 * and a NULL handler. So on a process that already serves this very
	 * API on provider 0, registering it again as a caller replaces the
	 * handler with NULL and the provider silently stops answering its own
	 * API. Mercury says "Overwriting RPC callback for a previously
	 * registered RPC ID" and nothing else notices.
	 *
	 * That is not hypothetical: the directory serves on provider 0, so a
	 * directory process that also called the directory API elsewhere, or
	 * a test that put both on one runtime, broke exactly this way. A
	 * service on any other provider never collided, which is why it took
	 * until the directory to find.
	 *
	 * Reusing the existing registration is safe because the target
	 * provider goes into the identifier when the call is forwarded, so one
	 * registration reaches every provider however it was made. The name
	 * determines the payload types here -- it is defw2.<api>.<method> --
	 * so an existing registration always carries the procs this caller
	 * would have supplied.
	 */
	flag = HG_FALSE;
	if (margo_provider_registered_name(rt->mid, name, 0, &id,
					   &flag) != HG_SUCCESS)
		flag = HG_FALSE;
	if (!flag) {
		id = margo_register_name(rt->mid, name, in_cb, out_cb, NULL);
	} else {
		defw2_log(rt, DEFW2_LOG_DEBUG,
			  "%s is already registered here, reusing it", name);
	}
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

void defw2_free_partial(margo_instance_id mid, hg_proc_cb_t proc_cb,
			void *data)
{
	hg_proc_t proc = HG_PROC_NULL;

	if (mid == MARGO_INSTANCE_NULL || proc_cb == NULL || data == NULL)
		return;
	if (hg_proc_create(margo_get_class(mid), HG_NOHASH, &proc) !=
	    HG_SUCCESS)
		return;
	if (hg_proc_reset(proc, NULL, 0, HG_FREE) == HG_SUCCESS)
		proc_cb(proc, data);
	hg_proc_free(proc);
}
