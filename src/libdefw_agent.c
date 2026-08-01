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
#include "defw_print.h"

extern fd_set g_tAllSet;
extern int g_iMaxSelectFd;
extern pthread_mutex_t global_var_mutex;
static bool initialized;
static pthread_mutex_t agent_array_mutex;
/* new connections which haven't verified themselves are added to this
 * list
 */
static struct dlist_entry agent_new_list;
/* verified services connected to me */
static struct dlist_entry agent_service_list;
/* clients connected to me */
static struct dlist_entry agent_client_list;
/* services I connect to */
static struct dlist_entry agent_active_service_list;
/* clients I connect to */
static struct dlist_entry agent_active_client_list;
/* dead agents that still have ref count */
static struct dlist_entry agent_dead_list;

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

void defw_lock_agent_lists(void)
{
	defw_agent_init();
	MUTEX_LOCK(&agent_array_mutex);
}

void defw_release_agent_lists(void)
{
	defw_agent_init();
	MUTEX_UNLOCK(&agent_array_mutex);
}

static void count_lists(void)
{
	struct dlist_entry *tmp;
	int count = 0;

	dlist_foreach(&agent_new_list, tmp) {
		count++;
	}

	PDEBUG("agent_new_list len is: %d", count);

	count = 0;
	dlist_foreach(&agent_service_list, tmp) {
		count++;
	}

	PDEBUG("agent_service_list len is: %d", count);

	count = 0;
	dlist_foreach(&agent_client_list, tmp) {
		count++;
	}

	PDEBUG("agent_client_list len is: %d", count);
}

void defw_agent_init(void)
{
	if (initialized)
		return;

	dlist_init(&agent_new_list);
	dlist_init(&agent_service_list);
	dlist_init(&agent_client_list);
	dlist_init(&agent_active_service_list);
	dlist_init(&agent_active_client_list);
	dlist_init(&agent_dead_list);
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

static void del_dead_agent_locked(defw_agent_blk_t *agent)
{
	assert(agent && agent->state & DEFW_AGENT_STATE_DEAD);

	assert(agent->ref_count > 0);
	agent->ref_count--;

	if (agent->ref_count == 0) {
		dlist_remove(&agent->entry);
		memset(agent, 0xdeadbeef, sizeof(*agent));
		free(agent);
	}
}

void defw_release_dead_list_agents(void)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent;

	MUTEX_LOCK(&agent_array_mutex);
	dlist_foreach_container_safe(&agent_dead_list, defw_agent_blk_t, agent,
				     entry, tmp)
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

	defw_agent_updated_notify();
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
		del_dead_agent_locked(agent);
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
		if (!(agent->state & DEFW_AGENT_STATE_NEW) || dead)
			close_agent_connection_unlocked(agent);
		memset(agent, 0xdeadbeef, sizeof(*agent));
		free(agent);
	} else if (dead) {
		/* remove from the live list and put on the dead list */
		set_agent_state(agent, DEFW_AGENT_STATE_DEAD);
		unset_agent_state(agent, DEFW_AGENT_STATE_ALIVE);
		unset_agent_state(agent, DEFW_AGENT_RPC_CHANNEL_CONNECTED);
		unset_agent_state(agent, DEFW_AGENT_CNTRL_CHANNEL_CONNECTED);
		dlist_remove(&agent->entry);
		dlist_insert_tail(&agent->entry, &agent_dead_list);
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
	assert(agent->state == DEFW_AGENT_STATE_NEW);
	MUTEX_LOCK(&agent_array_mutex);
	MUTEX_LOCK(&agent->state_mutex);

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

static defw_agent_blk_t *
find_agent_blk_by_addr(struct sockaddr_in *addr, struct dlist_entry *list)
{
	defw_agent_blk_t *agent;
	struct dlist_entry *tmp;

	if (!addr)
		return NULL;

	MUTEX_LOCK(&agent_array_mutex);
	dlist_foreach_container_safe(list, defw_agent_blk_t, agent,
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

void defw_agent_iter(struct dlist_entry *list, process_agent cb, void *user_data)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent;
	int rc;

	dlist_foreach_container_safe(list, defw_agent_blk_t, agent,
				     entry, tmp) {
		acquire_agent_blk(agent);
		rc = cb(agent, user_data);
		if (rc)
			break;
	}
}

void defw_service_agent_iter(process_agent cb, void *user_data)
{
	defw_agent_iter(&agent_service_list, cb, user_data);
}

void defw_client_agent_iter(process_agent cb, void *user_data)
{
	defw_agent_iter(&agent_client_list, cb, user_data);
}

void defw_active_service_agent_iter(process_agent cb, void *user_data)
{
	defw_agent_iter(&agent_active_service_list, cb, user_data);
}

void defw_active_client_agent_iter(process_agent cb, void *user_data)
{
	defw_agent_iter(&agent_active_client_list, cb, user_data);
}

void defw_new_agent_iter(process_agent cb, void *user_data)
{
	defw_agent_iter(&agent_new_list, cb, user_data);
}

defw_agent_blk_t *defw_get_next_agent(struct dlist_entry *head, struct dlist_entry *list)
{
	defw_agent_blk_t *agent = NULL;

	defw_agent_init();

	if (!head || !list || !head->next)
		goto out;

	/* reached the end of the list */
	if (head->next == list)
		goto out;

	agent = container_of(head->next, defw_agent_blk_t, entry);
	acquire_agent_blk(agent);
out:
	return agent;
}

defw_agent_blk_t *defw_get_next_active_service_agent(defw_agent_blk_t *agent)
{
	struct dlist_entry *start = (agent) ? &agent->entry
	  : &agent_active_service_list;

	return defw_get_next_agent(start, &agent_active_service_list);
}

defw_agent_blk_t *defw_get_next_active_client_agent(defw_agent_blk_t *agent)
{
	struct dlist_entry *start = (agent) ? &agent->entry
	  : &agent_active_client_list;

	return defw_get_next_agent(start, &agent_active_client_list);
}

defw_agent_blk_t *defw_get_next_service_agent(defw_agent_blk_t *agent)
{
	struct dlist_entry *start = (agent) ? &agent->entry : &agent_service_list;

	return defw_get_next_agent(start, &agent_service_list);
}

defw_agent_blk_t *defw_get_next_client_agent(defw_agent_blk_t *agent)
{
	struct dlist_entry *start = (agent) ? &agent->entry : &agent_client_list;

	return defw_get_next_agent(start, &agent_client_list);
}

defw_agent_blk_t *defw_get_next_new_agent_conn(defw_agent_blk_t *agent)
{
	struct dlist_entry *start = (agent) ? &agent->entry : &agent_new_list;

	return defw_get_next_agent(start, &agent_new_list);
}

defw_agent_blk_t *defw_find_create_agent_blk_by_addr(struct sockaddr_in *addr)
{
	defw_agent_blk_t *agent;

	agent = find_agent_blk_by_addr(addr, &agent_service_list);
	if (!agent)
		agent = find_agent_blk_by_addr(addr, &agent_client_list);
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

	calculate_highest_fd(&agent_service_list, &iMaxFd);
	calculate_highest_fd(&agent_active_service_list, &iMaxFd);
	calculate_highest_fd(&agent_client_list, &iMaxFd);
	calculate_highest_fd(&agent_active_client_list, &iMaxFd);
	calculate_highest_fd(&agent_new_list, &iMaxFd);

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
	agent->iFileDesc = INVALID_TCP_SOCKET;
	agent->iRpcFd = INVALID_TCP_SOCKET;
	agent->addr = *addr;
	set_agent_state(agent, DEFW_AGENT_STATE_NEW);
	uuid_generate(agent->id.blk_uuid);
	acquire_agent_blk(agent);

	/* this is a new connection. It could be another connection on an
	 * agent we're already tracking. We will consolidate it once the
	 * agent verifies their identity
	 */
	if (add) {
		dlist_insert_tail(&agent->entry, &agent_new_list);
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

int get_num_agents(struct dlist_entry *list)
{
	int num = 0;
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent;

	dlist_foreach_container_safe(list, defw_agent_blk_t, agent,
				     entry, tmp) {
		num++;
	}

	return num;
}

int get_num_service_agents(void)
{
	return get_num_agents(&agent_service_list);
}

int get_num_client_agents(void)
{
	return get_num_agents(&agent_client_list);
}

defw_agent_blk_t *find_agent_blk_by_pid(pid_t pid, struct dlist_entry *list)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent, *found = NULL;

	MUTEX_LOCK(&agent_array_mutex);

	dlist_foreach_container_safe(list, defw_agent_blk_t, agent,
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

defw_agent_blk_t *find_agent_blk_by_name(char *hostname, char *name,
					struct dlist_entry *list)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent, *found = NULL;

	if (!name || !hostname)
		return NULL;

	MUTEX_LOCK(&agent_array_mutex);

	dlist_foreach_container_safe(list, defw_agent_blk_t, agent,
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
	defw_agent_blk_t *agent;

	agent = find_agent_blk_by_name(hostname, name, &agent_service_list);
	if (!agent)
		agent = find_agent_blk_by_name(hostname, name, &agent_client_list);

	return agent;
}

defw_rc_t defw_send_session_info(defw_agent_blk_t *agent, bool rpc_setup)
{
	defw_msg_session_t msg;
	int rc;

//	PDEBUG("Sending session info to agent %p on fd %d\n",
//		agent, (rpc_setup) ? agent->iRpcFd : agent->iFileDesc);

	uuid_copy(msg.agent_id.remote_uuid, g_defw_cfg.uuid);

	msg.pid = htonl(getpid());
	msg.rpc_setup = htonl(rpc_setup);
	msg.listen_port = htonl(g_defw_cfg.l_info.listen_address.sin_port);
	msg.node_type = htonl(g_defw_cfg.l_info.type);
	strncpy(msg.node_name, g_defw_cfg.l_info.hb_info.node_name, MAX_STR_LEN);
	msg.node_name[MAX_STR_LEN-1] = '\0';
	gethostname(msg.node_hostname, MAX_STR_LEN);

	rc = defw_send_msg((rpc_setup) ? agent->iRpcFd : agent->iFileDesc,
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
	int rc;

	uuid_copy(msg.agent_id.remote_uuid, g_defw_cfg.uuid);

	msg.pid = htonl(getpid());
	msg.node_type = htonl(g_defw_cfg.l_info.type);
	strncpy(msg.node_name, g_defw_cfg.l_info.hb_info.node_name, MAX_STR_LEN);
	msg.node_name[MAX_STR_LEN-1] = '\0';
	gethostname(msg.node_hostname, MAX_STR_LEN);

	//PDEBUG("agent %s: fd %d rpc %d\n", agent->name, agent->iFileDesc,
	//       agent->iRpcFd);

	/* send the heart beat */
	rc = defw_send_msg(agent->iFileDesc, (char *)&msg,
			   sizeof(msg), EN_MSG_TYPE_HB);
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
	struct dlist_entry *list = req->list;
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

	agent->node_type = type;

	/* get socket information for the iFileDesc */
	tCliLen = sizeof(agent->addr);
	getsockname(agent->iFileDesc, (struct sockaddr *)&tmp_addr,
		    &tCliLen);
	agent->addr.sin_port = tmp_addr.sin_port;
	PDEBUG("Active port = %d\n", agent->addr.sin_port);

	MUTEX_LOCK(&agent_array_mutex);
	dlist_insert_tail(&agent->entry, list);
	MUTEX_UNLOCK(&agent_array_mutex);

	pthread_mutex_lock(&global_var_mutex);
	FD_SET(agent->iFileDesc, &g_tAllSet);
	FD_SET(agent->iRpcFd, &g_tAllSet);
	g_iMaxSelectFd = defw_get_highest_fd();
	pthread_mutex_unlock(&global_var_mutex);

	status_cb(EN_DEFW_RC_OK, req_uuid);

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
				    type, uuid, &agent_active_service_list, cb);
}

defw_rc_t defw_connect_to_client(char *ip_addr, int port, char *name,
				char *hostname, defw_type_t type,
				char *uuid, defw_connect_status status_cb)
{
	/* TODO we need a better way of doing this. For now I don't know
	 * how to handle function pointer passing in swig */
	defw_connect_status cb = (status_cb) ? status_cb : defw_notify_connect_complete;
	return defw_connect_to_agent(ip_addr, port, name, hostname,
				   type, uuid, &agent_active_client_list, cb);
}

static defw_rc_t
defw_send(char *dst_uuid, char *blk_uuid, char *yaml, defw_msg_type_t type)
{
	defw_rc_t rc = EN_DEFW_RC_RPC_FAIL;
	defw_agent_uuid_t agent_id;
	defw_agent_blk_t *agent_blk;
	size_t msg_size;

	if (!dst_uuid || !blk_uuid || !yaml)
		goto fail_rpc;

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

	rc = defw_send_msg(agent_blk->iRpcFd, yaml, msg_size, type);
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

static
defw_agent_blk_t *find_agent_blk_by_uuid(defw_agent_uuid_t *id, bool full,
					struct dlist_entry *list)
{
	struct dlist_entry *tmp;
	defw_agent_blk_t *agent, *found = NULL;

	MUTEX_LOCK(&agent_array_mutex);

	dlist_foreach_container_safe(list, defw_agent_blk_t, agent,
				     entry, tmp) {
		bool cmp;

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
	return find_agent_blk_by_uuid(id, full, &agent_client_list);
}

static defw_agent_blk_t *
defw_find_service_agent_by_uuid(defw_agent_uuid_t *id, bool full)
{
	return find_agent_blk_by_uuid(id, full, &agent_service_list);
}

static defw_agent_blk_t *
defw_find_active_client_agent_by_uuid(defw_agent_uuid_t *id, bool full)
{
	return find_agent_blk_by_uuid(id, full, &agent_active_client_list);
}

static defw_agent_blk_t *
defw_find_active_service_agent_by_uuid(defw_agent_uuid_t *id, bool full)
{
	return find_agent_blk_by_uuid(id, full, &agent_active_service_list);
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
	MUTEX_LOCK(&agent_array_mutex);
	dlist_remove(&agent->entry);
	dlist_insert_tail(&agent->entry, &agent_client_list);
	MUTEX_UNLOCK(&agent_array_mutex);
}

void defw_move_to_service_list(defw_agent_blk_t *agent)
{
	MUTEX_LOCK(&agent_array_mutex);
	dlist_remove(&agent->entry);
	dlist_insert_tail(&agent->entry, &agent_service_list);
	MUTEX_UNLOCK(&agent_array_mutex);
}
