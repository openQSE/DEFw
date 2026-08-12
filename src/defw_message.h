#ifndef DEFW_MESSAGE_H
#define DEFW_MESSAGE_H

#include "defw_common.h"

/* Maximum serialized OFI endpoint address (fi_getname output) carried in the
 * session handshake. libfabric endpoint names are small (e.g. 16 bytes for the
 * tcp provider); 256 is a generous upper bound shared by the message layer and
 * the OFI transport.
 */
#define DEFW_OFI_MAX_ADDRLEN 256

typedef struct defw_agent_uuid_s {
	uuid_t remote_uuid;  /* uuid of the remote process/agent */
	uuid_t blk_uuid; /* assigned locally. unique to agent */
} defw_agent_uuid_t;

typedef enum {
	EN_MSG_TYPE_HB = 0,
	EN_MSG_TYPE_SESSION_INFO,
	EN_MSG_TYPE_GET_NUM_AGENTS,
	EN_MSG_TYPE_PY_REQUEST,
	EN_MSG_TYPE_PY_RESPONSE,
	EN_MSG_TYPE_PY_EVENT,
	EN_MSG_TYPE_RMA_ACK,
	EN_MSG_TYPE_MAX
} defw_msg_type_t;

/* The sender identity is the sender's remote uuid rather than its source
 * IP address. A uuid is meaningful on any transport (TCP sockets, but also
 * connectionless fabrics where there is no per-peer socket to read an
 * address from), so this is the identity check shared by all transports.
 */
typedef struct defw_message_hdr_s {
	defw_msg_type_t type;
	unsigned int len;
	uuid_t sender_uuid;
	unsigned int version;
} defw_message_hdr_t;

/* add a uuid in the session message.
 * Active sends to passive as part of session creation
 * Passive sends to active in the heart beat
 *
 * ofi_addr carries the sender's serialized OFI endpoint name so a peer can
 * reach it over the fabric. ofi_addrlen is 0 when the sender has no OFI
 * endpoint (TCP-only build, or transport is tcp), which tells the receiver to
 * leave that peer on the TCP path.
 */
typedef struct defw_msg_session_s {
	defw_agent_uuid_t agent_id;
	defw_type_t node_type;
	pid_t pid;
	int rpc_setup;
	int listen_port;
	char node_name[MAX_STR_LEN];
	char node_hostname[MAX_STR_LEN];
	unsigned int ofi_addrlen;
	unsigned char ofi_addr[DEFW_OFI_MAX_ADDRLEN];
} defw_msg_session_t;

typedef struct defw_msg_num_agents_query_s {
	int num_agents;
} defw_msg_num_agents_query_t;

/* Acknowledgement that a peer has finished reading a memory region we exposed
 * for RMA, so the registration can be dropped. The handle is the registration
 * id the reader was given in the message that advertised the region; it is
 * split into two 32-bit halves because DEFw stamps wire integers with htonl
 * and there is no portable 64-bit equivalent.
 */
typedef struct defw_msg_rma_ack_s {
	unsigned int handle_hi;
	unsigned int handle_lo;
} defw_msg_rma_ack_t;

#endif /* DEFW_MESSAGE_H */
