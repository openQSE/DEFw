#ifndef DEFW_AGENTS_H
#define DEFW_AGENTS_H

#ifndef SWIG
#include <stdint.h>
#endif
#include "defw_common.h"
#include "defw_message.h"

#define MAX_NUM_AGENTS		1024
#define HB_TO			2

/* "handle:key:addr:len" -- four unsigned 64-bit values in decimal, three
 * separators and a terminator, rounded up.
 */
#define DEFW_RMA_DESC_STR_LEN	96

#define DEFW_AGENT_STATE_ALIVE (1 << 0)
#define DEFW_AGENT_CNTRL_CHANNEL_CONNECTED (1 << 1)
#define DEFW_AGENT_RPC_CHANNEL_CONNECTED (1 << 2)
#define DEFW_AGENT_WORK_IN_PROGRESS (1 << 3)
#define DEFW_AGENT_STATE_DEAD (1 << 4)
#define DEFW_AGENT_STATE_NEW (1 << 5)
/* set once the peer's OFI address has been inserted into the address vector
 * and cached in ofi_addr below
 */
#define DEFW_AGENT_PEER_READY_REPORTED (1 << 6)
#define DEFW_AGENT_PEER_LOST_REPORTED (1 << 7)
#define DEFW_AGENT_PEER_REMOVED_REPORTED (1 << 8)
#define DEFW_AGENT_OFI_ADDR_VALID (1 << 9)

#define DEFW_PEER_UUID_STR_LEN 37

typedef enum defw_peer_event_type {
	DEFW_PEER_READY = 1,
	DEFW_PEER_DEGRADED,
	DEFW_PEER_LOST,
	DEFW_PEER_REMOVED,
} defw_peer_event_type_t;

typedef enum defw_connection_direction {
	DEFW_CONN_DIRECTION_UNKNOWN = 0,
	DEFW_CONN_DIRECTION_INBOUND,
	DEFW_CONN_DIRECTION_OUTBOUND,
} defw_connection_direction_t;

typedef enum defw_connection_lifecycle {
	DEFW_CONN_LIFECYCLE_NEW = 0,
	DEFW_CONN_LIFECYCLE_HANDSHAKE,
	DEFW_CONN_LIFECYCLE_READY,
	DEFW_CONN_LIFECYCLE_LOST,
	DEFW_CONN_LIFECYCLE_REMOVED,
} defw_connection_lifecycle_t;

typedef enum defw_heartbeat_mode {
	DEFW_HEARTBEAT_NONE = 0,
	DEFW_HEARTBEAT_REMOTE,
} defw_heartbeat_mode_t;

typedef struct defw_peer_event_s {
	defw_peer_event_type_t event_type;
	char peer_handle[DEFW_PEER_UUID_STR_LEN];
	char remote_runtime_id[DEFW_PEER_UUID_STR_LEN];
	int is_self;
	char transport_context[MAX_SHORT_STR_LEN];
	char connection_direction[MAX_SHORT_STR_LEN];
	char address[MAX_SHORT_STR_LEN];
	unsigned int listen_port;
	unsigned int node_type;
	char node_name[MAX_STR_LEN];
	char hostname[MAX_STR_LEN];
	unsigned int pid;
	char reason[MAX_STR_LEN];
	long timestamp_sec;
	long timestamp_usec;
} defw_peer_event_t;

#ifndef DLIST_ENTRY
#define DLIST_ENTRY
struct dlist_entry {
	struct dlist_entry	*next;
	struct dlist_entry	*prev;
};
#endif

typedef void (*defw_connect_status)(defw_rc_t status, uuid_t uuid);
typedef defw_rc_t (*defw_peer_event_cb)(const defw_peer_event_t *event);

typedef struct defw_agent_blk_s {
	struct dlist_entry entry;
	pthread_mutex_t state_mutex;
	pthread_mutex_t cond_mutex;
	pthread_cond_t rpc_wait_cond;
	pid_t pid;
	defw_agent_uuid_t id;
	unsigned int version;
	unsigned int listen_port;
	char name[MAX_STR_LEN];
	char hostname[MAX_STR_LEN];
	int iFileDesc;
	int iRpcFd;
	struct timeval time_stamp;
	struct sockaddr_in addr;
	unsigned int state;
	unsigned int ref_count;
	defw_type_t node_type;
	defw_connection_direction_t direction;
	defw_connection_lifecycle_t lifecycle;
	defw_heartbeat_mode_t heartbeat_mode;
	defw_connect_status connect_complete_cb;
	uuid_t connect_req_uuid;
	int connect_req_pending;
	int is_loopback;
	struct timeval last_heartbeat_tx;
	struct timeval last_heartbeat_rx;
	struct timeval last_control_activity;
	struct timeval handshake_deadline;
	char failure_reason[MAX_STR_LEN];
	char *rpc_response;
	/* peer's fi_addr_t (stored as uint64_t so this transport-agnostic
	 * header needs no libfabric include); valid only when the
	 * DEFW_AGENT_OFI_ADDR_VALID state bit is set
	 */
	uint64_t ofi_addr;
} defw_agent_blk_t;

/* agent_state2str
 *	print agent state
 */
char *defw_agent_state2str(defw_agent_blk_t *agent);

static inline void defw_free_state_str(char *str)
{
	free(str);
}

/* get_local_ip
 *   gets the local IP address being used to send messages to the master
 */
char *defw_get_local_ip();

/*
 * defw_agent_get_pid
 *	get pid of agent
 */
unsigned int defw_agent_get_pid(defw_agent_blk_t *agent);

/*
 * defw_agent_get_port
 *	get port of agent
 */
int defw_agent_get_port(defw_agent_blk_t *agent);

/*
 * defw_agent_get_listen_port
 *	get listen port of agent
 */
int defw_agent_get_listen_port(defw_agent_blk_t *agent);

/*
 * agent_ip2str
 *	Returns the ip string representation
 */
char *defw_agent_ip2str(defw_agent_blk_t *agent);

/*
 * agent_disable_hb
 *	Disables the HB
 */
void defw_agent_disable_hb(void);

/*
 * agent_enable_hb
 *	Enables the HB
 */
void defw_agent_enable_hb(void);

/*
 * defw_release_agent_blk
 *	Release the agent blk
 */
void defw_release_agent_blk(defw_agent_blk_t *agent, int dead);
void defw_release_agent_blk_unlocked(defw_agent_blk_t *agent, int dead);

/*
 * defw_connect_to_[service|client]
 *	Establish a connection with a new agent given connection
 *	information. All information indicated need to be given.
 *
 *	Parameters:
 *		ip_target: IP address of remote
 *		port: Listen port of the remote
 *		name: name of the remote
 *		hostname: hostname of the remote
 *		type: type of the remote agent
 */
defw_rc_t defw_connect_to_service(char *ip_addr, int port, char *name,
				char *hostname, defw_type_t type,
				char *uuid, defw_connect_status status_cb);

defw_rc_t defw_connect_to_client(char *ip_addr, int port, char *name,
				char *hostname, defw_type_t type,
				char *uuid, defw_connect_status status_cb);

/*
 * defw_get_agent_uuid
 *	Returns a string representation of the agent's uuid
 *	The character pointer is allocated by C, tracked and freed by
 *	python via SWIG's typemaps
 */
void defw_get_agent_uuid(defw_agent_blk_t *agent, char **remote_uuid,
			char **blk_uuid);

/*
 * defw_agent_uuid_cmp
 *	Compares the given agent ids
 *	return true if equal, false otherwise
 */
int defw_agent_uuid_compare(char *agent_id1, char *agent_id2);

const char *defw_peer_event_type2str(defw_peer_event_type_t event_type);

/*
 * defw_send_req/rsp
 *	Send a request/response to the specified agent.
 *	This is a non-blocking operation.
 *	Blocking semantics is built on top of this in the python layer.
 *   Parameters:
 *	dst_uuid: The UUID of the destination
 *	blk_uuid: The local agent UUID block
 *	yaml: NULL terminated string to send to the target
 *
 *  Return:
 *     Returns a string YAML block
 */
defw_rc_t defw_send_req(char *dst_uuid, char *blk_uuid, char *yaml);
defw_rc_t defw_send_rsp(char *dst_uuid, char *blk_uuid, char *yaml);

/*
 * Large binary payloads (RMA)
 *
 * These three carry an RPC's bulk data off the YAML message and onto the
 * fabric. The sender publishes a buffer, puts the returned descriptor in the
 * message in place of the data, and the receiver fetches it back. The
 * registration is released when the fetch acknowledges it, so a published
 * buffer must always be either fetched or abandoned at shutdown.
 *
 * Sizes are unsigned long long rather than uint64_t because SWIG wraps a
 * uint64_t argument as an opaque pointer object instead of a Python integer.
 *
 * defw_rma_available
 *	Whether bulk data can travel by RMA to the agent with this block uuid:
 *	the local endpoint must be RMA capable and the peer must be reachable
 *	over the fabric. Returns 0 when it cannot, and the caller should keep
 *	the payload inline.
 *
 * defw_rma_publish
 *	Register a copy of the buffer for the peer to read. Returns the
 *	descriptor as "handle:key:addr:len"; it is text because the message it
 *	is about to be written into is text, and the fields are split back
 *	apart by the caller. The copy means the source buffer needs no
 *	lifetime guarantee.
 *
 * defw_rma_fetch
 *	Read a published region from the agent with this block uuid and return
 *	it as bytes, acknowledging the transfer so the peer can deregister.
 *
 * defw_rma_discard
 *	Drop a published region that will never be fetched, which is how the
 *	sender unwinds when it fails partway through publishing a message's
 *	payloads and falls back to sending them inline.
 */
int defw_rma_available(char *blk_uuid);
defw_rc_t defw_rma_publish(const void *rma_src, size_t rma_srclen,
			   char **rma_desc);
defw_rc_t defw_rma_discard(unsigned long long handle);
defw_rc_t defw_rma_fetch(char *blk_uuid, unsigned long long handle,
			 unsigned long long key, unsigned long long addr,
			 unsigned long long len, char **rma_buf,
			 size_t *rma_len);

static inline defw_agent_uuid_t *defw_get_agent_uuid_raw(defw_agent_blk_t *agent)
{
	return &agent->id;
}

#endif /* DEFW_AGENTS_H */
