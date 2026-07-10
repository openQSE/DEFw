#ifndef DEFW_TRANSPORT_H
#define DEFW_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "defw_common.h"
#include "defw_message.h"
#include "defw_agent.h"

/*
 * DEFw transport abstraction.
 *
 * DEFw historically spoke TCP directly: the agent and listener code called
 * the socket send/receive helpers by name. This ops table is the seam that
 * lets an alternate transport (e.g. libfabric/OFI) carry the same framed
 * messages without the agent, listener, or Python layers knowing which
 * transport is in use.
 *
 * Phase 0 introduces the seam and routes the outbound framed-message path
 * through it, with a single built-in TCP implementation that preserves the
 * historical behavior exactly. Later phases add an OFI implementation and
 * wire the remaining ops (endpoint init, peer connect, event-loop progress).
 */

/*
 * Logical channel a message travels on. TCP maps these onto the agent's two
 * sockets (control and RPC); a tagged transport such as OFI maps them onto
 * bits in the message tag (see defw_tag_make below).
 */
typedef enum {
	EN_DEFW_CHANNEL_CTRL = 0, /* session info + heartbeats */
	EN_DEFW_CHANNEL_RPC,      /* python request / response / event */
} defw_channel_t;

/*
 * Tag layout for tagged transports: [channel:8][msg_type:8][reserved:48].
 * TCP does not use tags (it demultiplexes by socket and reads the framed
 * header), but the encoding lives here so every transport shares one
 * definition. These helpers are unused until the OFI transport lands.
 */
static inline uint64_t defw_tag_make(defw_channel_t ch, defw_msg_type_t type)
{
	return (((uint64_t)ch & 0xff) << 56) |
	       (((uint64_t)type & 0xff) << 48);
}

static inline defw_channel_t defw_tag_channel(uint64_t tag)
{
	return (defw_channel_t)((tag >> 56) & 0xff);
}

static inline defw_msg_type_t defw_tag_msg_type(uint64_t tag)
{
	return (defw_msg_type_t)((tag >> 48) & 0xff);
}

/*
 * Transport operations.
 *
 * Phase 0 exercises send(). The remaining ops are declared now to document
 * the interface a transport must provide; they are left NULL by the TCP
 * transport because the existing listener/connect code still owns those
 * paths for TCP. The OFI transport will implement all of them, and the
 * listener/connect code will be routed through init/connect/progress in a
 * later phase.
 */
typedef struct defw_transport_ops_s {
	const char *name;

	/* Send a framed DEFw message to agent on the given channel. */
	defw_rc_t (*send)(defw_agent_blk_t *agent, defw_channel_t ch,
			  char *buf, size_t len, defw_msg_type_t type);

	/* Reserved for later phases; NULL in phase 0. */
	defw_rc_t (*init)(void *listener_info);
	defw_rc_t (*connect)(defw_agent_blk_t *agent);
	defw_rc_t (*progress)(void);
	void      (*disconnect)(defw_agent_blk_t *agent);
	void      (*fini)(void);
} defw_transport_ops_t;

/* Return the active transport ops. Defaults to the built-in TCP transport
 * and is never NULL.
 */
defw_transport_ops_t *defw_transport_ops(void);

/* Select the active transport. A NULL argument is ignored. Used by later
 * phases to switch to OFI based on configuration.
 */
void defw_transport_set_ops(defw_transport_ops_t *ops);

/* The built-in TCP transport. */
defw_transport_ops_t *defw_transport_tcp_ops(void);

#endif /* DEFW_TRANSPORT_H */
