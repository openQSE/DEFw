/*
 * The service host's internals, shared with the services that bind to it.
 * Not installed.
 */
#ifndef DEFW2_HOST_H
#define DEFW2_HOST_H

#include <defw2/defw2_service.h>

#include "defw2_internal.h"

struct defw2_service {
	struct defw2_rt	*rt;
	char		*service_id;	/* owned */
	char		*service_type;	/* owned */
	uint16_t	provider_id;
};

#endif /* DEFW2_HOST_H */
