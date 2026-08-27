#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <sys/types.h>
#include <netdb.h>
#include "defw_global.h"
#include "defw_agent.h"
#include "libdefw_agent.h"
#include "defw.h"
#include "defw_list.h"
#include "defw_python.h"
#include "defw_listener.h"
#include "defw_transport.h"
#include "defw_print.h"

extern fd_set g_tAllSet;
extern int g_iMaxSelectFd;
extern pthread_mutex_t global_var_mutex;
static bool initialized;
static pthread_mutex_t agent_array_mutex;
static struct dlist_entry agent_connection_table;

static bool g_agent_enable_hb = true;
static struct in_addr g_local_ip;

typedef struct defw_connect_req_s {
	char ip_addr[MAX_SHORT_STR_LEN];
	char name[MAX_SHORT_STR_LEN];
	char hostname[MAX_SHORT_STR_LEN];
	int port;
	defw_type_t type;
	uuid_t uuid;
	struct dlist_entry *list;
	defw_connect_status status_cb;
} defw_connect_req_t;

#define DEFAULT_RPC_RSP "rpc:\n   src: %s\n   dst: %s\n   type: internal-failure\n"

#define MUTEX_LOCK(x) \
  pthread_mutex_lock(x)

#define MUTEX_UNLOCK(x) \
  pthread_mutex_unlock(x)

const char *defw_peer_event_type2str(defw_peer_event_type_t event_type)
{
	switch (event_type) {
	case DEFW_PEER_READY:
		return "PEER_READY";
	case DEFW_PEER_DEGRADED:
		return "PEER_DEGRADED";
	case DEFW_PEER_LOST:
		return "PEER_LOST";
	case DEFW_PEER_REMOVED:
		return "PEER_REMOVED";
	default:
		return "UNKNOWN_PEER_EVENT";
	}
}

static void defw_agent_report_peer_lost(defw_agent_blk_t *agent,
					const char *reason);
static void defw_agent_report_peer_removed(defw_agent_blk_t *agent,
					   const char *reason);

static void count_lists(void)
{
	struct dlist_entry *tmp;
	int count = 0;

	dlist_foreach(&agent_connection_table, tmp) {
		count++;
	}

	PDEBUG("agent_connection_table len is: %d", count);
}

void defw_agent_init(void)
{
	if (initialized)
		return;

	dlist_init(&agent_connection_table);
	pthread_mutex_init(&agent_array_mutex, NULL);
	initialized = true;
}

char *defw_get_local_ip()
{
	return inet_ntoa(g_local_ip);
}

unsigned int defw_agent_get_pid(defw_agent_blk_t *agent)
{
	return (unsigned int) agent->pid;
}

int defw_agent_get_port(defw_agent_blk_t *agent)
{
	return agent->addr.sin_port;
}

int defw_agent_get_listen_port(defw_agent_blk_t *agent)
{
	return agent->listen_port;
}

void defw_get_agent_uuid(defw_agent_blk_t *agent, char **remote_uuid,
			char **blk_uuid)
{
	*remote_uuid = calloc(1, UUID_STR_LEN);
	uuid_unparse_lower(agent->id.remote_uuid, *remote_uuid);

	*blk_uuid = calloc(1, UUID_STR_LEN);
	uuid_unparse_lower(agent->id.blk_uuid, *blk_uuid);
}

static inline
defw_rc_t defw_uuids_to_agent_id(char *remote_uuid_str, char *blk_uuid_str,
			      defw_agent_uuid_t *out)
{
	if (remote_uuid_str && uuid_parse(remote_uuid_str, out->remote_uuid))
		return EN_DEFW_RC_BAD_UUID;
	if (blk_uuid_str && uuid_parse(blk_uuid_str, out->blk_uuid))
		return EN_DEFW_RC_BAD_UUID;

	return EN_DEFW_RC_OK;
}

int defw_agent_uuid_compare(char *agent_id1, char *agent_id2)
{
	uuid_t uuid1, uuid2;

	if (!agent_id1 && !agent_id2)
		return true;

	if (!agent_id1 || (agent_id1 && uuid_parse(agent_id1, uuid1)))
		return false;

	if (!agent_id2 || (agent_id2 && uuid_parse(agent_id2, uuid2)))
		return false;

	return (uuid_compare(uuid1, uuid2) == 0);
}

static void free_dead_agent_if_unreferenced(defw_agent_blk_t *agent)
{
	assert(agent && agent->state & DEFW_AGENT_STATE_DEAD);

	if (agent->ref_count == 0) {
		defw_agent_report_peer_removed(agent, "transport-cleanup");
		agent->lifecycle = DEFW_CONN_LIFECYCLE_REMOVED;
		dlist_remove(&agent->entry);
		memset(agent, 0xdeadbeef, sizeof(*agent));
		free(agent);
	}
}

static void del_dead_agent_locked(defw_agent_blk_t *agent)
{
	assert(agent && agent->state & DEFW_AGENT_STATE_DEAD);

	assert(agent->ref_count > 0);
	agent->ref_count--;
	free_dead_agent_if_unreferenced(agent);
}

void defw_release_dead_list_agents(void)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent;

	MUTEX_LOCK(&agent_array_mutex);
	dlist_foreach_container_safe(&agent_connection_table, defw_agent_blk_t, agent,
				     entry, tmp)
		if (agent->state & DEFW_AGENT_STATE_DEAD)
			del_dead_agent_locked(agent);
	MUTEX_UNLOCK(&agent_array_mutex);
}

static inline bool defw_agent_alive(defw_agent_blk_t *agent)
{
	bool viable = false;

	MUTEX_LOCK(&agent->state_mutex);
	if (agent->state & DEFW_AGENT_STATE_ALIVE)
		viable = true;
	MUTEX_UNLOCK(&agent->state_mutex);

	return viable;
}

static bool defw_agent_mark_once(defw_agent_blk_t *agent, unsigned int state)
{
	bool marked = false;

	MUTEX_LOCK(&agent->state_mutex);
	if (!(agent->state & state)) {
		agent->state |= state;
		marked = true;
	}
	MUTEX_UNLOCK(&agent->state_mutex);

	return marked;
}

static bool defw_agent_state_is_set(defw_agent_blk_t *agent, unsigned int state)
{
	bool set;

	MUTEX_LOCK(&agent->state_mutex);
	set = agent->state & state;
	MUTEX_UNLOCK(&agent->state_mutex);

	return set;
}

static bool defw_agent_ready_to_report(defw_agent_blk_t *agent)
{
	if (!agent)
		return false;
	if (uuid_is_null(agent->id.remote_uuid))
		return false;
	if (!defw_agent_state_is_set(agent, DEFW_AGENT_CNTRL_CHANNEL_CONNECTED))
		return false;
	if (!defw_agent_state_is_set(agent, DEFW_AGENT_RPC_CHANNEL_CONNECTED))
		return false;

	return true;
}

static void defw_agent_complete_pending_connect(defw_agent_blk_t *agent,
						defw_rc_t status)
{
	defw_connect_status cb = NULL;
	uuid_t req_uuid;
	bool pending = false;

	if (!agent)
		return;

	MUTEX_LOCK(&agent->state_mutex);
	if (agent->connect_req_pending && agent->connect_complete_cb) {
		cb = agent->connect_complete_cb;
		uuid_copy(req_uuid, agent->connect_req_uuid);
		agent->connect_req_pending = 0;
		pending = true;
	}
	MUTEX_UNLOCK(&agent->state_mutex);

	if (pending)
		cb(status, req_uuid);
}

static const char *defw_connection_direction2str(defw_connection_direction_t dir)
{
	switch (dir) {
	case DEFW_CONN_DIRECTION_INBOUND:
		return "INBOUND";
	case DEFW_CONN_DIRECTION_OUTBOUND:
		return "OUTBOUND";
	default:
		return "";
	}
}

static void defw_agent_fill_peer_event(defw_agent_blk_t *agent,
				       defw_peer_event_type_t event_type,
				       const char *reason,
				       defw_peer_event_t *event)
{
	struct timeval now;
	const char *addr;

	memset(event, 0, sizeof(*event));
	event->event_type = event_type;
	uuid_unparse_lower(agent->id.blk_uuid, event->peer_handle);
	if (!uuid_is_null(agent->id.remote_uuid)) {
		uuid_unparse_lower(agent->id.remote_uuid,
				   event->remote_runtime_id);
		if (!uuid_is_null(g_defw_cfg.uuid) &&
		    !uuid_compare(agent->id.remote_uuid, g_defw_cfg.uuid))
			event->is_self = true;
	}

	agent->is_loopback = event->is_self;
	strncpy(event->transport_context, "defw-tcp",
		sizeof(event->transport_context) - 1);
	strncpy(event->connection_direction,
		defw_connection_direction2str(agent->direction),
		sizeof(event->connection_direction) - 1);
	addr = inet_ntoa(agent->addr.sin_addr);
	if (addr)
		strncpy(event->address, addr, sizeof(event->address) - 1);
	event->listen_port = agent->listen_port;
	event->node_type = agent->node_type;
	strncpy(event->node_name, agent->name, sizeof(event->node_name) - 1);
	strncpy(event->hostname, agent->hostname, sizeof(event->hostname) - 1);
	event->pid = (unsigned int) agent->pid;
	if (reason)
		strncpy(event->reason, reason, sizeof(event->reason) - 1);
	gettimeofday(&now, NULL);
	event->timestamp_sec = now.tv_sec;
	event->timestamp_usec = now.tv_usec;
}

void defw_agent_report_peer_ready(defw_agent_blk_t *agent, const char *reason)
{
	defw_peer_event_t event;

	if (!agent)
		return;
	if (!defw_agent_ready_to_report(agent))
		return;
	if (!defw_agent_mark_once(agent, DEFW_AGENT_PEER_READY_REPORTED))
		return;

	defw_agent_fill_peer_event(agent, DEFW_PEER_READY, reason, &event);
	agent->lifecycle = DEFW_CONN_LIFECYCLE_READY;
	agent->heartbeat_mode = event.is_self ? DEFW_HEARTBEAT_NONE :
				 DEFW_HEARTBEAT_REMOTE;
	defw_notify_peer_event(&event);
	defw_agent_complete_pending_connect(agent, EN_DEFW_RC_OK);
}

void defw_agent_report_peer_ready_update(defw_agent_blk_t *agent,
					 const char *reason)
{
	defw_peer_event_t event;

	if (!agent)
		return;
	if (!defw_agent_state_is_set(agent, DEFW_AGENT_PEER_READY_REPORTED)) {
		defw_agent_report_peer_ready(agent, reason);
		return;
	}
	if (uuid_is_null(agent->id.remote_uuid))
		return;

	defw_agent_fill_peer_event(agent, DEFW_PEER_READY, reason, &event);
	agent->lifecycle = DEFW_CONN_LIFECYCLE_READY;
	agent->heartbeat_mode = event.is_self ? DEFW_HEARTBEAT_NONE :
				 DEFW_HEARTBEAT_REMOTE;
	defw_notify_peer_event(&event);
}

static void defw_agent_report_peer_lost(defw_agent_blk_t *agent,
					const char *reason)
{
	defw_peer_event_t event;

	if (!agent)
		return;
	if (!defw_agent_state_is_set(agent, DEFW_AGENT_PEER_READY_REPORTED))
		return;
	if (!defw_agent_mark_once(agent, DEFW_AGENT_PEER_LOST_REPORTED))
		return;

	agent->lifecycle = DEFW_CONN_LIFECYCLE_LOST;
	agent->heartbeat_mode = DEFW_HEARTBEAT_NONE;
	if (reason)
		strncpy(agent->failure_reason, reason,
			sizeof(agent->failure_reason) - 1);
	defw_agent_fill_peer_event(agent, DEFW_PEER_LOST, reason, &event);
	defw_notify_peer_event(&event);
}

static void defw_agent_report_peer_removed(defw_agent_blk_t *agent,
					   const char *reason)
{
	defw_peer_event_t event;

	if (!agent)
		return;
	if (!defw_agent_state_is_set(agent, DEFW_AGENT_PEER_READY_REPORTED))
		return;
	if (!defw_agent_mark_once(agent, DEFW_AGENT_PEER_REMOVED_REPORTED))
		return;

	agent->lifecycle = DEFW_CONN_LIFECYCLE_REMOVED;
	defw_agent_fill_peer_event(agent, DEFW_PEER_REMOVED, reason, &event);
	defw_notify_peer_event(&event);
}

static void close_agent_connection_unlocked(defw_agent_blk_t *agent)
{
	if (agent->iFileDesc != INVALID_TCP_SOCKET) {
		pthread_mutex_lock(&global_var_mutex);
		FD_CLR(agent->iFileDesc, &g_tAllSet);
		g_iMaxSelectFd = defw_agent_get_highest_fd();
		pthread_mutex_unlock(&global_var_mutex);
		closeTcpConnection(agent->iFileDesc);
		agent->iFileDesc = -1;
	}
	if (agent->iRpcFd != INVALID_TCP_SOCKET) {
		pthread_mutex_lock(&global_var_mutex);
		FD_CLR(agent->iRpcFd, &g_tAllSet);
		g_iMaxSelectFd = defw_agent_get_highest_fd();
		pthread_mutex_unlock(&global_var_mutex);
		closeTcpConnection(agent->iRpcFd);
		agent->iRpcFd = -1;
	}

}

static void close_agent_connection(defw_agent_blk_t *agent)
{
	MUTEX_LOCK(&agent_array_mutex);
	close_agent_connection_unlocked(agent);
	MUTEX_UNLOCK(&agent_array_mutex);
}

void defw_release_agent_blk_unlocked(defw_agent_blk_t *agent, int dead)
{
	assert(agent);

	assert(agent->ref_count > 0);
	agent->ref_count--;

	/* if the agent isn't alive and isn't new then it must be dead */
	if (agent->state & DEFW_AGENT_STATE_DEAD) {
		free_dead_agent_if_unreferenced(agent);
		return;
	}

	if (agent->ref_count == 0) {
		dlist_remove(&agent->entry);
		assert(!(agent->state & DEFW_AGENT_WORK_IN_PROGRESS));
		/* a new agent represents a connection which we don't
		 * exactly know if it's from an agent we have previous
		 * connections from. If it is a new connection, then we
		 * don't want to close that connection after we've
		 * transferred it to the agent we already have.
		 */
		if (!(agent->state & DEFW_AGENT_STATE_NEW) || dead) {
			if (dead)
				defw_agent_report_peer_lost(agent,
					agent->failure_reason[0] ?
					agent->failure_reason :
					"transport-failure");
			close_agent_connection_unlocked(agent);
			defw_agent_report_peer_removed(agent,
						       "transport-cleanup");
		}
		memset(agent, 0xdeadbeef, sizeof(*agent));
		free(agent);
	} else if (dead) {
		/* remove from the live list and put on the dead list */
		set_agent_state(agent, DEFW_AGENT_STATE_DEAD);
		unset_agent_state(agent, DEFW_AGENT_STATE_ALIVE);
		unset_agent_state(agent, DEFW_AGENT_RPC_CHANNEL_CONNECTED);
		unset_agent_state(agent, DEFW_AGENT_CNTRL_CHANNEL_CONNECTED);
		defw_agent_report_peer_lost(agent,
					    agent->failure_reason[0] ?
					    agent->failure_reason :
					    "transport-failure");
		close_agent_connection_unlocked(agent);
	}
}

void defw_release_agent_blk(defw_agent_blk_t *agent, int dead)
{
	MUTEX_LOCK(&agent_array_mutex);
	defw_release_agent_blk_unlocked(agent, dead);
	MUTEX_UNLOCK(&agent_array_mutex);
}

void defw_release_agent_conn(defw_agent_blk_t *agent)
{
	MUTEX_LOCK(&agent_array_mutex);
	MUTEX_LOCK(&agent->state_mutex);

	assert(agent->state & DEFW_AGENT_STATE_NEW);
	assert(agent->ref_count > 0);
	agent->ref_count--;

	if (agent->ref_count == 0) {
		dlist_remove(&agent->entry);
		free(agent);
	}

	MUTEX_UNLOCK(&agent->state_mutex);
	MUTEX_UNLOCK(&agent_array_mutex);
}

void acquire_agent_blk(defw_agent_blk_t *agent)
{
	/* acquire the agent blk mutex */
	MUTEX_LOCK(&agent->state_mutex);
	if (agent)
		agent->ref_count++;
	MUTEX_UNLOCK(&agent->state_mutex);
}

char *defw_agent_state2str(defw_agent_blk_t *agent)
{
	char *agent_state_str = calloc(1, 128);

	if (!agent || !agent_state_str)
		return "SOMETHING WRONG";

	sprintf(agent_state_str, "%s%s%s%s",
		(agent->state & DEFW_AGENT_STATE_ALIVE) ? "alive " : "dead ",
		(agent->state & DEFW_AGENT_CNTRL_CHANNEL_CONNECTED) ? " CTRL" : "",
		(agent->state & DEFW_AGENT_RPC_CHANNEL_CONNECTED) ? " RPC" : "",
		(agent->state & DEFW_AGENT_WORK_IN_PROGRESS) ? " WIP" : "");

	return agent_state_str;
}

static bool defw_agent_matches_filter(defw_agent_blk_t *agent,
				      defw_connection_direction_t direction,
				      defw_type_t role,
				      bool new_only)
{
	if (!agent)
		return false;
	if (new_only)
		return agent->state & DEFW_AGENT_STATE_NEW;
	if (agent->state & DEFW_AGENT_STATE_NEW)
		return false;
	if (direction != DEFW_CONN_DIRECTION_UNKNOWN &&
	    agent->direction != direction)
		return false;
	if (role != EN_DEFW_INVALID && agent->node_type != role)
		return false;
	return true;
}

static defw_agent_blk_t *find_agent_blk_by_addr(struct sockaddr_in *addr)
{
	defw_agent_blk_t *agent;
	struct dlist_entry *tmp;

	if (!addr)
		return NULL;

	MUTEX_LOCK(&agent_array_mutex);
	dlist_foreach_container_safe(&agent_connection_table, defw_agent_blk_t, agent,
				     entry, tmp) {
		if (agent && defw_agent_alive(agent) &&
		    agent->addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
		    agent->addr.sin_port == addr->sin_port) {
			acquire_agent_blk(agent);
			MUTEX_UNLOCK(&agent_array_mutex);
			return agent;
		}
	}
	MUTEX_UNLOCK(&agent_array_mutex);

	return NULL;
}

static void defw_agent_iter_filtered(process_agent cb, void *user_data,
				     defw_connection_direction_t direction,
				     defw_type_t role, bool new_only)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent;
	int rc;

	dlist_foreach_container_safe(&agent_connection_table, defw_agent_blk_t, agent,
				     entry, tmp) {
		if (!defw_agent_matches_filter(agent, direction, role, new_only))
			continue;
		acquire_agent_blk(agent);
		rc = cb(agent, user_data);
		if (rc)
			break;
	}
}

void defw_new_agent_iter(process_agent cb, void *user_data)
{
	defw_agent_iter_filtered(cb, user_data, DEFW_CONN_DIRECTION_UNKNOWN,
				 EN_DEFW_INVALID, true);
}

void defw_connection_agent_iter(process_agent cb, void *user_data)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent;
	int rc;

	dlist_foreach_container_safe(&agent_connection_table, defw_agent_blk_t,
				     agent, entry, tmp) {
		acquire_agent_blk(agent);
		rc = cb(agent, user_data);
		if (rc)
			break;
	}
}

static defw_agent_blk_t *
defw_get_next_agent_filtered(defw_agent_blk_t *previous,
			     defw_connection_direction_t direction,
			     defw_type_t role, bool new_only)
{
	struct dlist_entry *entry;
	defw_agent_blk_t *agent = NULL;

	defw_agent_init();

	if (previous)
		entry = previous->entry.next;
	else
		entry = agent_connection_table.next;
	if (!entry)
		goto out;

	while (entry != &agent_connection_table) {
		agent = container_of(entry, defw_agent_blk_t, entry);
		if (defw_agent_matches_filter(agent, direction, role, new_only)) {
			acquire_agent_blk(agent);
			goto out;
		}
		entry = entry->next;
	}

	agent = NULL;
out:
	return agent;
}

defw_agent_blk_t *defw_get_next_new_agent_conn(defw_agent_blk_t *agent)
{
	return defw_get_next_agent_filtered(agent, DEFW_CONN_DIRECTION_UNKNOWN,
					    EN_DEFW_INVALID, true);
}

defw_agent_blk_t *defw_find_create_agent_blk_by_addr(struct sockaddr_in *addr)
{
	defw_agent_blk_t *agent;

	agent = find_agent_blk_by_addr(addr);
	if (!agent)
		return defw_alloc_agent_blk(addr, true);
	defw_release_agent_blk(agent, false);

	return agent;
}

void calculate_highest_fd(struct dlist_entry *list, int *iMaxFd)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent;

	dlist_foreach_container_safe(list, defw_agent_blk_t, agent,
				     entry, tmp) {
		if (agent) {
			if (agent->iFileDesc > *iMaxFd)
				*iMaxFd = agent->iFileDesc;
			if (agent->iRpcFd > *iMaxFd)
				*iMaxFd = agent->iRpcFd;
		}
	}
}

int defw_agent_get_highest_fd(void)
{
	int iMaxFd = INVALID_TCP_SOCKET;

	calculate_highest_fd(&agent_connection_table, &iMaxFd);

	return iMaxFd;
}

void defw_agent_disable_hb(void)
{
	g_agent_enable_hb = false;
}

void defw_agent_enable_hb(void)
{
	g_agent_enable_hb = true;
}

int agent_get_hb(void)
{
	return g_agent_enable_hb;
}

defw_agent_blk_t *defw_alloc_agent_blk(struct sockaddr_in *addr, bool add)
{
	int i = 0;
	defw_agent_blk_t *agent;

	/* grab the lock for the array */
	MUTEX_LOCK(&agent_array_mutex);

	/* allocate a new agent blk and assign it to that entry */
	agent = calloc(sizeof(char), sizeof(defw_agent_blk_t));
	if (!agent) {
		MUTEX_UNLOCK(&agent_array_mutex);
		return NULL;
	}

	dlist_init(&agent->entry);
	pthread_mutex_init(&agent->state_mutex, NULL);
	pthread_mutex_init(&agent->cond_mutex, NULL);
	pthread_cond_init(&agent->rpc_wait_cond, NULL);
	gettimeofday(&agent->time_stamp, NULL);
	agent->last_heartbeat_rx = agent->time_stamp;
	agent->last_heartbeat_tx = agent->time_stamp;
	agent->last_control_activity = agent->time_stamp;
	agent->handshake_deadline = agent->time_stamp;
	agent->handshake_deadline.tv_sec += TCP_READ_TIMEOUT_SEC;
	agent->iFileDesc = INVALID_TCP_SOCKET;
	agent->iRpcFd = INVALID_TCP_SOCKET;
	agent->addr = *addr;
	agent->direction = DEFW_CONN_DIRECTION_INBOUND;
	agent->lifecycle = DEFW_CONN_LIFECYCLE_NEW;
	agent->heartbeat_mode = DEFW_HEARTBEAT_NONE;
	agent->node_type = EN_DEFW_INVALID;
	set_agent_state(agent, DEFW_AGENT_STATE_NEW);
	uuid_generate(agent->id.blk_uuid);
	acquire_agent_blk(agent);

	/* this is a new connection. It could be another connection on an
	 * agent we're already tracking. We will consolidate it once the
	 * agent verifies their identity
	 */
	if (add) {
		dlist_insert_tail(&agent->entry, &agent_connection_table);
		count_lists();
	}

	PDEBUG("Adding agent %d:%s:%d:%d", i, inet_ntoa(addr->sin_addr),
	       addr->sin_port, agent->node_type);

	/* release the array mutex */
	MUTEX_UNLOCK(&agent_array_mutex);

	/* return the agent blk */
	return agent;
}


void set_agent_state(defw_agent_blk_t *agent, unsigned int state)
{
	MUTEX_LOCK(&agent->state_mutex);
	agent->state |= state;
	MUTEX_UNLOCK(&agent->state_mutex);
}

void unset_agent_state(defw_agent_blk_t *agent, unsigned int state)
{
	MUTEX_LOCK(&agent->state_mutex);
	agent->state &= ~state;
	MUTEX_UNLOCK(&agent->state_mutex);
}

char *defw_agent_ip2str(defw_agent_blk_t *agent)
{
	if (!agent)
		return NULL;

	return inet_ntoa(agent->addr.sin_addr);
}

static int get_num_agents(defw_connection_direction_t direction, defw_type_t role)
{
	int num = 0;
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent;

	dlist_foreach_container_safe(&agent_connection_table, defw_agent_blk_t, agent,
				     entry, tmp) {
		if (defw_agent_matches_filter(agent, direction, role, false))
			num++;
	}

	return num;
}

int defw_get_num_connection_agents(void)
{
	return get_num_agents(DEFW_CONN_DIRECTION_INBOUND, EN_DEFW_SERVICE) +
	       get_num_agents(DEFW_CONN_DIRECTION_INBOUND, EN_DEFW_DIRSVC) +
	       get_num_agents(DEFW_CONN_DIRECTION_INBOUND, EN_DEFW_AGENT) +
	       get_num_agents(DEFW_CONN_DIRECTION_OUTBOUND, EN_DEFW_SERVICE) +
	       get_num_agents(DEFW_CONN_DIRECTION_OUTBOUND, EN_DEFW_DIRSVC) +
	       get_num_agents(DEFW_CONN_DIRECTION_OUTBOUND, EN_DEFW_AGENT);
}

defw_agent_blk_t *find_agent_blk_by_pid(pid_t pid)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent, *found = NULL;

	MUTEX_LOCK(&agent_array_mutex);

	dlist_foreach_container_safe(&agent_connection_table, defw_agent_blk_t, agent,
				     entry, tmp) {
		if (agent->pid == pid) {
			found = agent;
			acquire_agent_blk(agent);
			break;
		}
	}

	MUTEX_UNLOCK(&agent_array_mutex);

	/* return the agent blk */
	return found;
}

defw_agent_blk_t *find_agent_blk_by_name(char *hostname, char *name)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent, *found = NULL;

	if (!name || !hostname)
		return NULL;

	MUTEX_LOCK(&agent_array_mutex);

	dlist_foreach_container_safe(&agent_connection_table, defw_agent_blk_t, agent,
				     entry, tmp) {
		if (!strcmp(agent->name, name) &&
		    !strcmp(agent->hostname, hostname)) {
			found = agent;
			break;
		}
	}

	MUTEX_UNLOCK(&agent_array_mutex);

	/* return the agent blk */
	return found;
}

defw_agent_blk_t *find_agent_by_name_global(char *hostname, char *name)
{
	return find_agent_blk_by_name(hostname, name);
}

defw_rc_t defw_send_session_info(defw_agent_blk_t *agent, bool rpc_setup)
{
	defw_msg_session_t msg;
	size_t ofi_len = sizeof(msg.ofi_addr);
	int rc;

//	PDEBUG("Sending session info to agent %p on fd %d\n",
//		agent, (rpc_setup) ? agent->iRpcFd : agent->iFileDesc);

	memset(&msg, 0, sizeof(msg));

	uuid_copy(msg.agent_id.remote_uuid, g_defw_cfg.uuid);

	msg.pid = htonl(getpid());
	msg.rpc_setup = htonl(rpc_setup);
	msg.listen_port = htonl(g_defw_cfg.l_info.listen_address.sin_port);
	msg.node_type = htonl(g_defw_cfg.l_info.type);
	strncpy(msg.node_name, g_defw_cfg.l_info.hb_info.node_name, MAX_STR_LEN);
	msg.node_name[MAX_STR_LEN-1] = '\0';
	gethostname(msg.node_hostname, MAX_STR_LEN);

	/* advertise our OFI endpoint address when OFI is active; otherwise
	 * ofi_addrlen stays 0 (from the memset) and the peer keeps us on TCP
	 */
	if (defw_transport_ofi_local_addr(msg.ofi_addr, &ofi_len) == EN_DEFW_RC_OK)
		msg.ofi_addrlen = htonl((unsigned int)ofi_len);

	rc = defw_transport_ops()->send(agent,
			(rpc_setup) ? EN_DEFW_CHANNEL_RPC : EN_DEFW_CHANNEL_CTRL,
			(char *)&msg, sizeof(msg), EN_MSG_TYPE_SESSION_INFO);
	if (rc != EN_DEFW_RC_OK) {
		PERROR("Failed to send heart beat %s\n",
			defw_rc2str(rc));
	}

	return rc;
}

defw_rc_t defw_send_hb(defw_agent_blk_t *agent)
{
	defw_msg_session_t msg;
	size_t ofi_len = sizeof(msg.ofi_addr);
	int rc;

	memset(&msg, 0, sizeof(msg));

	uuid_copy(msg.agent_id.remote_uuid, g_defw_cfg.uuid);

	msg.pid = htonl(getpid());
	msg.node_type = htonl(g_defw_cfg.l_info.type);
	strncpy(msg.node_name, g_defw_cfg.l_info.hb_info.node_name, MAX_STR_LEN);
	msg.node_name[MAX_STR_LEN-1] = '\0';
	gethostname(msg.node_hostname, MAX_STR_LEN);

	/* carry our OFI address in the heartbeat too, so a peer that connected
	 * to us (and only receives our heartbeats) can also learn it
	 */
	if (defw_transport_ofi_local_addr(msg.ofi_addr, &ofi_len) == EN_DEFW_RC_OK)
		msg.ofi_addrlen = htonl((unsigned int)ofi_len);

	//PDEBUG("agent %s: fd %d rpc %d\n", agent->name, agent->iFileDesc,
	//       agent->iRpcFd);

	/* send the heart beat */
	rc = defw_transport_ops()->send(agent, EN_DEFW_CHANNEL_CTRL,
			(char *)&msg, sizeof(msg), EN_MSG_TYPE_HB);
	if (rc != EN_DEFW_RC_OK) {
		PERROR("Failed to send heart beat %s\n",
			defw_rc2str(rc));
	}

	return rc;
}

static
defw_rc_t hostname_to_ip(char *hostname, char *ip, int len)
{
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_in *h;
	int rv;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	rv = getaddrinfo(hostname, NULL, &hints, &servinfo);
	if (rv != 0) {
		PERROR("getaddrinfo: %s", gai_strerror(rv));
		return EN_DEFW_RC_BAD_ADDR;
	}

	memset(ip, 0, len);
	for (p = servinfo; p != NULL; p = p->ai_next) {
		h = (struct sockaddr_in *) p->ai_addr;
		strncpy(ip, inet_ntoa(h->sin_addr), len-1);
		PDEBUG("hostname %s has ip %s", hostname, ip);
		break;
	}

	freeaddrinfo(servinfo);
	return EN_DEFW_RC_OK;
}

static void *defw_connect_to_agent_thread(void *user_data)
{
	struct sockaddr_in sockaddr;
	defw_agent_blk_t *agent;
	defw_rc_t rc = EN_DEFW_RC_SOCKET_FAIL;
	defw_connect_req_t *req = user_data;
	char *ip_addr = req->ip_addr;
	int port = req->port;
	char *name = req->name;
	char *hostname = req->hostname;
	defw_type_t type = req->type;
	uuid_t req_uuid;
	defw_connect_status status_cb = req->status_cb;
	socklen_t  tCliLen;
	char ip[MAX_STR_LEN];
	struct sockaddr_in tmp_addr;

	uuid_copy(req_uuid, req->uuid);

	if (strlen(hostname) != 0) {
		/* check if a valid parent_hostname is provided. If it is,
		 * let's use it instead of the ip address
		 */
		if (!hostname_to_ip(hostname, ip, MAX_STR_LEN))
			ip_addr = ip;
	}

	if (!inet_aton(ip_addr, &sockaddr.sin_addr)) {
		rc = EN_DEFW_RC_BAD_ADDR;
		goto fail;
	}
	sockaddr.sin_port = port;

	agent = defw_alloc_agent_blk(&sockaddr, false);
	if (!agent) {
		rc = EN_DEFW_RC_OOM;
		goto fail;
	}

	agent->listen_port = port;
	agent->direction = DEFW_CONN_DIRECTION_OUTBOUND;
	agent->node_type = type;
	agent->lifecycle = DEFW_CONN_LIFECYCLE_HANDSHAKE;
	agent->connect_complete_cb = status_cb;
	uuid_copy(agent->connect_req_uuid, req_uuid);
	agent->connect_req_pending = 1;

	/* establish two connection: CTRL and RPC */
	agent->iFileDesc = establishTCPConnection(
				agent->addr.sin_addr.s_addr,
				htons(agent->listen_port),
				false, false);
	if (agent->iFileDesc < 0)
		goto free_agent;
	rc = defw_send_session_info(agent, false);
	if (rc)
		goto close;

	PDEBUG("Establishing CTRL channel on FD: %p:%d", agent, agent->iFileDesc);

	set_agent_state(agent, DEFW_AGENT_CNTRL_CHANNEL_CONNECTED);
	set_agent_state(agent, DEFW_AGENT_STATE_ALIVE);
	unset_agent_state(agent, DEFW_AGENT_STATE_NEW);

	agent->iRpcFd = establishTCPConnection(
				agent->addr.sin_addr.s_addr,
				htons(agent->listen_port),
				false, false);
	if (agent->iRpcFd < 0)
		goto close;
	rc = defw_send_session_info(agent, true);
	if (rc)
		goto close;
	PDEBUG("Establishing RPC channel on FD: %p:%d", agent, agent->iRpcFd);
	set_agent_state(agent, DEFW_AGENT_RPC_CHANNEL_CONNECTED);

	strncpy(agent->name, name, MAX_STR_LEN);
	agent->name[MAX_STR_LEN-1] = '\0';

	if (strlen(hostname) != 0) {
		strncpy(agent->hostname, hostname, MAX_STR_LEN);
		agent->hostname[MAX_STR_LEN-1] = '\0';
	} else {
		gethostname(agent->hostname, MAX_STR_LEN);
		agent->hostname[MAX_STR_LEN-1] = '\0';
	}

	/* get socket information for the iFileDesc */
	tCliLen = sizeof(agent->addr);
	getsockname(agent->iFileDesc, (struct sockaddr *)&tmp_addr,
		    &tCliLen);
	agent->addr.sin_port = tmp_addr.sin_port;
	PDEBUG("Active port = %d\n", agent->addr.sin_port);

	MUTEX_LOCK(&agent_array_mutex);
	dlist_insert_tail(&agent->entry, &agent_connection_table);
	MUTEX_UNLOCK(&agent_array_mutex);

	pthread_mutex_lock(&global_var_mutex);
	FD_SET(agent->iFileDesc, &g_tAllSet);
	FD_SET(agent->iRpcFd, &g_tAllSet);
	g_iMaxSelectFd = defw_get_highest_fd();
	pthread_mutex_unlock(&global_var_mutex);

	defw_agent_report_peer_ready(agent, "outbound-rpc-ready");

	free(user_data);

	return NULL;

close:
	close_agent_connection(agent);
free_agent:
	free(agent);
fail:
	free(user_data);
	status_cb(rc, req_uuid);

	return NULL;
}

/* TODO: if ip address is not provided but hostname is, then resolve
 * hostname to an ip and try to connect that way
 */
defw_rc_t defw_connect_to_agent(char *ip_addr, int port, char *name,
			      char *hostname, defw_type_t type,
			      char *uuid, struct dlist_entry *list,
			      defw_connect_status status_cb)
{
	int trc;
	pthread_t tid;
	defw_connect_req_t *req = calloc(1, sizeof(*req));

	if (!req)
		return EN_DEFW_RC_OOM;

	strncpy(req->ip_addr, ip_addr, MAX_SHORT_STR_LEN);
	strncpy(req->name, name, MAX_SHORT_STR_LEN);
	strncpy(req->hostname, hostname, MAX_SHORT_STR_LEN);
	req->port = port;
	req->type = type;
	req->list = list;
	req->status_cb = status_cb;
	if (uuid) {
		if (uuid_parse(uuid, req->uuid)) {
			free(req);
			return EN_DEFW_RC_BAD_PARAM;
		}
	} else {
		memset(req->uuid, 0, sizeof(req->uuid));
	}

	trc = pthread_create(&tid, NULL, defw_connect_to_agent_thread, req);
	if (trc) {
		PERROR("Failed to start connection thread");
		return EN_DEFW_RC_ERR_THREAD_STARTUP;
	}

	return EN_DEFW_RC_IN_PROGRESS;
}

defw_rc_t defw_connect_to_service(char *ip_addr, int port, char *name,
				char *hostname, defw_type_t type,
				char *uuid, defw_connect_status status_cb)
{
	/* TODO we need a better way of doing this. For now I don't know
	 * how to handle function pointer passing in swig */
	defw_connect_status cb = (status_cb) ? status_cb : defw_notify_connect_complete;
	return defw_connect_to_agent(ip_addr, port, name, hostname,
				    type, uuid, NULL, cb);
}

defw_rc_t defw_connect_to_client(char *ip_addr, int port, char *name,
				char *hostname, defw_type_t type,
				char *uuid, defw_connect_status status_cb)
{
	/* TODO we need a better way of doing this. For now I don't know
	 * how to handle function pointer passing in swig */
	defw_connect_status cb = (status_cb) ? status_cb : defw_notify_connect_complete;
	return defw_connect_to_agent(ip_addr, port, name, hostname,
				   type, uuid, NULL, cb);
}

static defw_rc_t
defw_send(char *dst_uuid, char *blk_uuid, char *yaml, defw_msg_type_t type)
{
	defw_rc_t rc = EN_DEFW_RC_RPC_FAIL;
	defw_agent_uuid_t agent_id;
	defw_agent_blk_t *agent_blk;
	size_t msg_size;

	if (!dst_uuid || !blk_uuid || !yaml)
		return EN_DEFW_RC_BAD_PARAM;

	msg_size = strlen(yaml) + 1;

	if (defw_uuids_to_agent_id(dst_uuid, blk_uuid, &agent_id))
		goto fail_rpc_no_agent;
	agent_blk = defw_find_agent_by_uuid_global(&agent_id);
	if (!agent_blk) {
		PERROR("Can't find agent with address: %s", dst_uuid);
		goto fail_rpc_no_agent;
	}

	PMSG("Sending to %s:%d\n%s", agent_blk->name,
	     agent_blk->iRpcFd, yaml);

	MUTEX_LOCK(&agent_blk->state_mutex);
	if (!(agent_blk->state & DEFW_AGENT_RPC_CHANNEL_CONNECTED)) {
		MUTEX_UNLOCK(&agent_blk->state_mutex);
		PDEBUG("Establishing an RPC channel to agent %s:%s:%d",
		       agent_blk->name,
		       inet_ntoa(agent_blk->addr.sin_addr),
		       agent_blk->listen_port);
		/* in network byte order, convert so we can have a
		 * uniform API
		 */
		agent_blk->iRpcFd = establishTCPConnection(
				agent_blk->addr.sin_addr.s_addr,
				htons(agent_blk->listen_port),
				false, false);
		if (agent_blk->iRpcFd < 0)
			goto fail_rpc;
		rc = defw_send_session_info(agent_blk, true);
		if (rc) {
			PERROR("Failed send session info: %s",
				defw_rc2str(rc));
			goto fail_rpc;
		}
		set_agent_state(agent_blk,
				DEFW_AGENT_RPC_CHANNEL_CONNECTED);
	} else {
		MUTEX_UNLOCK(&agent_blk->state_mutex);
	}

	set_agent_state(agent_blk, DEFW_AGENT_WORK_IN_PROGRESS);

	rc = defw_transport_ops()->send(agent_blk, EN_DEFW_CHANNEL_RPC,
			yaml, msg_size, type);
	if (rc != EN_DEFW_RC_OK) {
		PERROR("Failed to send rpc message: %s", yaml);
		goto fail_rpc;
	}

	unset_agent_state(agent_blk, DEFW_AGENT_WORK_IN_PROGRESS);
	defw_release_agent_blk(agent_blk, false);

	return EN_DEFW_RC_OK;

fail_rpc:
	unset_agent_state(agent_blk, DEFW_AGENT_WORK_IN_PROGRESS);
	if (rc == EN_DEFW_RC_SOCKET_FAIL) {
		set_agent_state(agent_blk, DEFW_AGENT_STATE_DEAD);
		defw_release_agent_blk(agent_blk, true);
	} else {
		defw_release_agent_blk(agent_blk, false);
	}

fail_rpc_no_agent:
	return rc;
}

defw_rc_t defw_send_req(char *dst_uuid, char *blk_uuid, char *yaml)
{
	return defw_send(dst_uuid, blk_uuid, yaml, EN_MSG_TYPE_PY_REQUEST);
}

defw_rc_t defw_send_rsp(char *dst_uuid, char *blk_uuid, char *yaml)
{
	return defw_send(dst_uuid, blk_uuid, yaml, EN_MSG_TYPE_PY_RESPONSE);
}

defw_rc_t defw_send_rma_ack(defw_agent_blk_t *agent, uint64_t handle)
{
	defw_msg_rma_ack_t msg;

	if (!agent)
		return EN_DEFW_RC_BAD_PARAM;

	msg.handle_hi = htonl((unsigned int)(handle >> 32));
	msg.handle_lo = htonl((unsigned int)(handle & 0xffffffff));

	return defw_transport_ops()->send(agent, EN_DEFW_CHANNEL_RPC,
					  (char *)&msg, sizeof(msg),
					  EN_MSG_TYPE_RMA_ACK);
}

int defw_rma_available(char *blk_uuid)
{
	defw_agent_blk_t *agent;
	int available;

	if (!defw_transport_ofi_rma_capable())
		return 0;

	agent = defw_find_agent_by_blk_uuid(blk_uuid);
	if (!agent)
		return 0;

	/* the peer has to be reachable on the fabric: it advertised an OFI
	 * address in the handshake and we inserted it (phase 1b)
	 */
	available = (agent->state & DEFW_AGENT_OFI_ADDR_VALID) ? 1 : 0;
	defw_release_agent_blk(agent, false);

	return available;
}

defw_rc_t defw_rma_publish(const void *rma_src, size_t rma_srclen,
			   char **rma_desc)
{
	defw_rma_desc_t desc;
	defw_rc_t rc;
	char *out;

	if (!rma_src || !rma_srclen || !rma_desc)
		return EN_DEFW_RC_BAD_PARAM;

	rc = defw_transport_ofi_mr_reg_copy(rma_src, rma_srclen, &desc);
	if (rc != EN_DEFW_RC_OK)
		return rc;

	out = calloc(1, DEFW_RMA_DESC_STR_LEN);
	if (!out) {
		defw_transport_ofi_mr_release(desc.handle);
		return EN_DEFW_RC_OOM;
	}

	snprintf(out, DEFW_RMA_DESC_STR_LEN, "%llu:%llu:%llu:%llu",
		 (unsigned long long)desc.handle, (unsigned long long)desc.key,
		 (unsigned long long)desc.addr, (unsigned long long)desc.len);
	*rma_desc = out;

	return EN_DEFW_RC_OK;
}

defw_rc_t defw_rma_discard(unsigned long long handle)
{
	return defw_transport_ofi_mr_release((uint64_t)handle);
}

defw_rc_t defw_rma_fetch(char *blk_uuid, unsigned long long handle,
			 unsigned long long key, unsigned long long addr,
			 unsigned long long len, char **rma_buf,
			 size_t *rma_len)
{
	defw_agent_blk_t *agent;
	defw_rc_t rc;
	char *buf;

	if (!blk_uuid || !len || !rma_buf || !rma_len)
		return EN_DEFW_RC_BAD_PARAM;

	agent = defw_find_agent_by_blk_uuid(blk_uuid);
	if (!agent) {
		PERROR("RMA fetch: no agent with block uuid %s", blk_uuid);
		return EN_DEFW_RC_AGENT_NOT_FOUND;
	}

	if (!(agent->state & DEFW_AGENT_OFI_ADDR_VALID)) {
		PERROR("RMA fetch: agent %s is not reachable over the fabric",
		       agent->name);
		defw_release_agent_blk(agent, false);
		return EN_DEFW_RC_FAIL;
	}

	buf = malloc((size_t)len);
	if (!buf) {
		defw_release_agent_blk(agent, false);
		return EN_DEFW_RC_OOM;
	}

	rc = defw_transport_ofi_rma_read(agent->ofi_addr, key, addr, buf,
					 (size_t)len);
	if (rc != EN_DEFW_RC_OK) {
		free(buf);
		defw_release_agent_blk(agent, false);
		return rc;
	}

	/* The peer keeps the region registered until it hears the read is
	 * done. A failed acknowledgement is not fatal for this transfer -- we
	 * have the data -- but it does strand the registration until the peer
	 * tears its endpoint down, so it is worth saying out loud.
	 */
	rc = defw_send_rma_ack(agent, (uint64_t)handle);
	if (rc != EN_DEFW_RC_OK)
		PERROR("RMA fetch: could not acknowledge handle %llu: %s",
		       handle, defw_rc2str(rc));

	defw_release_agent_blk(agent, false);

	*rma_buf = buf;
	*rma_len = (size_t)len;

	return EN_DEFW_RC_OK;
}

static
defw_agent_blk_t *find_agent_blk_by_uuid(defw_agent_uuid_t *id, bool full,
					defw_connection_direction_t direction)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent, *found = NULL;

	MUTEX_LOCK(&agent_array_mutex);

	dlist_foreach_container_safe(&agent_connection_table, defw_agent_blk_t, agent,
				     entry, tmp) {
		bool cmp;

		if (direction != DEFW_CONN_DIRECTION_UNKNOWN &&
		    agent->direction != direction)
			continue;

		if (full) {
			cmp = uuid_compare(agent->id.remote_uuid, id->remote_uuid) == 0 &&
			      (uuid_compare(agent->id.blk_uuid, id->blk_uuid) == 0 ||
			       uuid_is_null(id->blk_uuid));
		} else {
			cmp = uuid_compare(agent->id.remote_uuid, id->remote_uuid) == 0;
		}

		if (cmp) {
			found = agent;
			acquire_agent_blk(agent);
			break;
		}
	}

	MUTEX_UNLOCK(&agent_array_mutex);

	/* return the agent blk */
	return found;
}

static defw_agent_blk_t *
defw_find_client_agent_by_uuid(defw_agent_uuid_t *id, bool full)
{
	return find_agent_blk_by_uuid(id, full, DEFW_CONN_DIRECTION_INBOUND);
}

static defw_agent_blk_t *
defw_find_service_agent_by_uuid(defw_agent_uuid_t *id, bool full)
{
	return find_agent_blk_by_uuid(id, full, DEFW_CONN_DIRECTION_INBOUND);
}

static defw_agent_blk_t *
defw_find_active_client_agent_by_uuid(defw_agent_uuid_t *id, bool full)
{
	return find_agent_blk_by_uuid(id, full, DEFW_CONN_DIRECTION_OUTBOUND);
}

static defw_agent_blk_t *
defw_find_active_service_agent_by_uuid(defw_agent_uuid_t *id, bool full)
{
	return find_agent_blk_by_uuid(id, full, DEFW_CONN_DIRECTION_OUTBOUND);
}

defw_agent_blk_t *
defw_find_agent_by_uuid_global(defw_agent_uuid_t *id)
{
	defw_agent_blk_t *agent;

	agent = defw_find_active_service_agent_by_uuid(id, true);
	if (!agent)
		agent = defw_find_service_agent_by_uuid(id, true);
	if (!agent)
		agent = defw_find_client_agent_by_uuid(id, true);
	if (!agent)
		agent = defw_find_active_client_agent_by_uuid(id, true);

	return agent;
}

static defw_agent_blk_t *
find_blk_uuid_in_list(uuid_t blk_uuid, struct dlist_entry *list)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent, *found = NULL;

	dlist_foreach_container_safe(list, defw_agent_blk_t, agent,
				     entry, tmp) {
		if (uuid_compare(agent->id.blk_uuid, blk_uuid) == 0) {
			found = agent;
			acquire_agent_blk(agent);
			break;
		}
	}

	return found;
}

/*
 * The block uuid is assigned locally and is unique to an agent block, so it
 * identifies a peer on its own. The existing lookups all key on the remote
 * uuid, which the Python layer is never given; it only ever sees the block
 * uuid, which is why the RMA fetch needs this one.
 */
defw_agent_blk_t *defw_find_agent_by_blk_uuid(char *blk_uuid_str)
{
	defw_agent_blk_t *agent;
	uuid_t blk_uuid;

	if (!blk_uuid_str || uuid_parse(blk_uuid_str, blk_uuid))
		return NULL;

	MUTEX_LOCK(&agent_array_mutex);
	agent = find_blk_uuid_in_list(blk_uuid, &agent_connection_table);
	MUTEX_UNLOCK(&agent_array_mutex);

	return agent;
}

defw_agent_blk_t *
defw_find_agent_by_uuid_passive(uuid_t uuid)
{
	defw_agent_blk_t *agent;
	defw_agent_uuid_t id;

	uuid_copy(id.remote_uuid, uuid);

	agent = defw_find_service_agent_by_uuid(&id, false);
	if (!agent)
		agent = defw_find_client_agent_by_uuid(&id, false);

	return agent;
}

void defw_move_to_client_list(defw_agent_blk_t *agent)
{
	agent->direction = DEFW_CONN_DIRECTION_INBOUND;
	agent->node_type = EN_DEFW_AGENT;
	agent->lifecycle = DEFW_CONN_LIFECYCLE_HANDSHAKE;
}

void defw_move_to_service_list(defw_agent_blk_t *agent)
{
	agent->direction = DEFW_CONN_DIRECTION_INBOUND;
	agent->lifecycle = DEFW_CONN_LIFECYCLE_HANDSHAKE;
}
