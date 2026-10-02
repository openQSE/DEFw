/*
 * Bindings: a caller's handle on one provider.
 *
 * Resolving an address is the expensive part of reaching a peer, so it
 * happens once here and every call reuses the result. In phase 1 the
 * directory produces the address and this is where its cache will sit.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "defw2_wire.h"

/*
 * Mercury's OFI plugins look up numeric addresses only, and a v1
 * deployment names its hosts: DEFW_PARENT_HOSTNAME is a host name in every
 * QFw site configuration, and the directory address composed from it
 * inherits the name. So a host name in an ofi+tcp address is resolved
 * here, to the IPv4 address it names, and anything else is left alone.
 * Returns the address to look up, which is buf when one was resolved.
 */
static const char *numeric_address(const char *address, char *buf,
				   size_t len)
{
	struct addrinfo hints, *found = NULL;
	char name[DEFW2_NAME_MAX];
	char ip[INET_ADDRSTRLEN];
	const char *host, *port;
	size_t n;

	if (strncmp(address, "ofi+tcp", strlen("ofi+tcp")) != 0)
		return address;
	host = strstr(address, "://");
	if (host == NULL)
		return address;
	host += strlen("://");
	port = strrchr(host, ':');
	if (port == NULL || port == host)
		return address;
	n = (size_t)(port - host);
	if (n >= sizeof(name) || strspn(host, "0123456789.") >= n)
		return address;	/* already numeric, or too long to be a name */
	memcpy(name, host, n);
	name[n] = '\0';

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(name, NULL, &hints, &found) != 0 || found == NULL)
		return address;	/* the lookup below says it cannot reach it */
	inet_ntop(AF_INET, &((struct sockaddr_in *)found->ai_addr)->sin_addr,
		  ip, sizeof(ip));
	freeaddrinfo(found);
	snprintf(buf, len, "%.*s%s%s", (int)(host - address), address, ip,
		 port);
	return buf;
}

defw2_rc_t defw2_binding_create(defw2_rt_t *rt, const char *address,
				uint16_t provider_id,
				defw2_binding_t **out)
{
	char resolved[DEFW2_NAME_MAX];
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

	hret = margo_addr_lookup(rt->mid,
				 numeric_address(address, resolved,
						 sizeof(resolved)),
				 &binding->addr);
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
