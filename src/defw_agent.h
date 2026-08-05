#ifndef DEFW_AGENTS_H
#define DEFW_AGENTS_H

#include "defw_common.h"
#include "defw_message.h"

#define MAX_NUM_AGENTS		1024
#define HB_TO			2

#define DEFW_AGENT_STATE_ALIVE (1 << 0)
#define DEFW_AGENT_CNTRL_CHANNEL_CONNECTED (1 << 1)
#define DEFW_AGENT_RPC_CHANNEL_CONNECTED (1 << 2)
#define DEFW_AGENT_WORK_IN_PROGRESS (1 << 3)
#define DEFW_AGENT_STATE_DEAD (1 << 4)
#define DEFW_AGENT_STATE_NEW (1 << 5)
#define DEFW_AGENT_PEER_READY_REPORTED (1 << 6)
#define DEFW_AGENT_PEER_LOST_REPORTED (1 << 7)
#define DEFW_AGENT_PEER_REMOVED_REPORTED (1 << 8)

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
	char address[MAX_SHORT_STR_LEN];
	unsigned int listen_port;
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

typedef defw_rc_t (*defw_agent_update_cb)(void);
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
	int is_loopback;
	struct timeval last_heartbeat_tx;
	struct timeval last_heartbeat_rx;
	struct timeval last_control_activity;
	struct timeval handshake_deadline;
	char failure_reason[MAX_STR_LEN];
	char *rpc_response;
} defw_agent_blk_t;

/* agent_state2str
 *	print agent state
 */
char *defw_agent_state2str(defw_agent_blk_t *agent);

static inline void defw_free_state_str(char *str)
{
	free(str);
}

void defw_lock_agent_lists(void);
void defw_release_agent_lists(void);

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
 * defw_get_next_service_agent
 *	Iterate over the agent blocks on the service list
 * defw_get_next_active_service_agent
 *	Iterate over the agent blocks on the service list I connected to
 */
defw_agent_blk_t *defw_get_next_service_agent(defw_agent_blk_t *agent);
defw_agent_blk_t *defw_get_next_active_service_agent(defw_agent_blk_t *agent);

/*
 * defw_get_next_client_agent
 *	Iterate over the agent blocks on the client list
 * defw_get_next_active_client_agent
 *	Iterate over the agent blocks on the client list I connected to
 */
defw_agent_blk_t *defw_get_next_client_agent(defw_agent_blk_t *agent);
defw_agent_blk_t *defw_get_next_active_client_agent(defw_agent_blk_t *agent);

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

static inline defw_agent_uuid_t *defw_get_agent_uuid_raw(defw_agent_blk_t *agent)
{
	return &agent->id;
}

#endif /* DEFW_AGENTS_H */
