/*
 * Bindings: a caller's handle on one provider.
 *
 * Resolving an address is the expensive part of reaching a peer, so it
 * happens once here and every call reuses the result. In phase 1 the
 * directory produces the address and this is where its cache will sit.
 */
#include "defw2_wire.h"

defw2_rc_t defw2_binding_create(defw2_rt_t *rt, const char *address,
				uint16_t provider_id,
				defw2_binding_t **out)
{
	struct defw2_binding *binding;
	hg_return_t hret;

	if (rt == NULL || address == NULL || out == NULL)
		return DEFW2_ERR_INVALID;

	binding = calloc(1, sizeof(*binding));
	if (binding == NULL)
		return DEFW2_ERR_NOMEM;
	binding->rt = rt;
	binding->provider_id = provider_id;
	binding->addr = HG_ADDR_NULL;

	binding->address = strdup(address);
	if (binding->address == NULL) {
		free(binding);
		return DEFW2_ERR_NOMEM;
	}

	hret = margo_addr_lookup(rt->mid, address, &binding->addr);
	if (hret != HG_SUCCESS) {
		defw2_log(rt, DEFW2_LOG_ERROR, "cannot reach %s: %s", address,
			  HG_Error_to_string(hret));
		free(binding->address);
		free(binding);
		return DEFW2_ERR_TRANSPORT;
	}

	defw2_log(rt, DEFW2_LOG_DEBUG, "bound provider %u at %s", provider_id,
		  address);
	*out = binding;
	return DEFW2_OK;
}

void defw2_binding_free(defw2_binding_t *binding)
{
	if (binding == NULL)
		return;
	if (binding->addr != HG_ADDR_NULL)
		margo_addr_free(binding->rt->mid, binding->addr);
	free(binding->address);
	free(binding);
}

const char *defw2_binding_address(const defw2_binding_t *binding)
{
	return binding ? binding->address : NULL;
}

uint16_t defw2_binding_provider_id(const defw2_binding_t *binding)
{
	return binding ? binding->provider_id : 0;
}

void defw2_buffer_free(defw2_buffer_t *buffer)
{
	if (buffer == NULL)
		return;
	free(buffer->data);
	buffer->data = NULL;
	buffer->len = 0;
}
