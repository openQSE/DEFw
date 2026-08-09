#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <uuid/uuid.h>
#include "defw.h"
#include "defw_agent.h"
#include "defw_global.h"
#include "libdefw_agent.h"
#include "defw_message.h"
#include "defw_listener.h"
#include "defw_print.h"

#define MAX_AGENT_NOTIFICATION 1024

fd_set g_tAllSet;
int g_iMaxSelectFd = INVALID_TCP_SOCKET;
static int g_iListenFd = INVALID_TCP_SOCKET;
static bool g_bShutdown;
bool dirsvc_connected;
bool dirsvc_connect_in_progress;
pthread_mutex_t global_var_mutex;
static int agent_notification_idx;
static defw_agent_update_cb agent_notifications[MAX_AGENT_NOTIFICATION];
static int connect_complete_idx;
static defw_connect_status connect_notifications[MAX_AGENT_NOTIFICATION];
static int peer_event_idx;
static defw_peer_event_cb peer_event_notifications[MAX_AGENT_NOTIFICATION];

typedef struct connection_info_s {
	int *iNReady;
	fd_set *tReadSet;
} connection_info_t;

// TODO: Add a callback registration for python module to use to register
// for incoming messages
// TODO: Consider making the message types expandable. Modules can
// register their message type and the associated callback. If a message
// comes with that message type then the callback is called. This will
// make adding extra messages to be received easy. And python can have the
// freedom to register any calls. Trick is: can python generate
// C functions which can be called on the fly? IE in python code?

static defw_rc_t process_msg_unknown(char *msg, defw_agent_blk_t *agent);
static defw_rc_t process_msg_hb(char *msg, defw_agent_blk_t *agent);
static defw_rc_t process_msg_get_num_agents(char *msg, defw_agent_blk_t *agent);
static defw_rc_t process_msg_session_info(char *msg, defw_agent_blk_t *agent);

static defw_msg_process_fn_t msg_process_tbl[EN_MSG_TYPE_MAX] = {
	[EN_MSG_TYPE_HB] = process_msg_hb,
	[EN_MSG_TYPE_GET_NUM_AGENTS] = process_msg_get_num_agents,
	[EN_MSG_TYPE_PY_REQUEST] = process_msg_unknown,
	[EN_MSG_TYPE_PY_RESPONSE] = process_msg_unknown,
	[EN_MSG_TYPE_PY_EVENT] = process_msg_unknown,
	[EN_MSG_TYPE_SESSION_INFO] = process_msg_session_info,
};

defw_rc_t defw_register_agent_update_notification_cb(defw_agent_update_cb cb)
{
	if (agent_notification_idx >= MAX_AGENT_NOTIFICATION)
		return EN_DEFW_RC_FAIL;
	agent_notifications[agent_notification_idx] = cb;
	agent_notification_idx++;
	return EN_DEFW_RC_OK;
}

defw_rc_t defw_register_msg_callback(defw_msg_type_t msg_type, defw_msg_process_fn_t cb)
{
	if (msg_type >= EN_MSG_TYPE_MAX || msg_type < EN_MSG_TYPE_HB)
		return EN_DEFW_RC_FAIL;

	msg_process_tbl[msg_type] = cb;

	return EN_DEFW_RC_OK;
}

defw_rc_t defw_register_connect_complete(defw_connect_status cb)
{
	if (connect_complete_idx >= MAX_AGENT_NOTIFICATION)
		return EN_DEFW_RC_FAIL;
	connect_notifications[connect_complete_idx] = cb;
	connect_complete_idx++;
	return EN_DEFW_RC_OK;
}

defw_rc_t defw_register_peer_event_callback(defw_peer_event_cb cb)
{
	if (!cb)
		return EN_DEFW_RC_BAD_PARAM;
	if (peer_event_idx >= MAX_AGENT_NOTIFICATION)
		return EN_DEFW_RC_FAIL;
	peer_event_notifications[peer_event_idx] = cb;
	peer_event_idx++;
	return EN_DEFW_RC_OK;
}

void defw_agent_updated_notify(void)
{
	int i;
	for (i = 0; i < agent_notification_idx; i++)
		agent_notifications[i]();
}

void defw_notify_connect_complete(defw_rc_t status, uuid_t uuid)
{
	int i;
	for (i = 0; i < connect_complete_idx; i++)
		connect_notifications[i](status, uuid);
}

void defw_notify_peer_event(const defw_peer_event_t *event)
{
	int i;
	defw_rc_t rc;

	if (!event)
		return;

	for (i = 0; i < peer_event_idx; i++) {
		rc = peer_event_notifications[i](event);
		if (rc)
			PERROR("Peer event callback failed: %s", defw_rc2str(rc));
	}
}

static void set_dirsvc_connected(defw_rc_t status, uuid_t uuid)
{
	pthread_mutex_lock(&global_var_mutex);
	if (!status)
		dirsvc_connected = true;
	else
		dirsvc_connected = false;
	dirsvc_connect_in_progress = false;
	pthread_mutex_unlock(&global_var_mutex);
}

void defw_listener_shutdown(void)
{
	g_bShutdown = true;
}

int defw_get_highest_fd(void)
{
	int iAgentFd = defw_agent_get_highest_fd();
	int iMaxFd;

	if (iAgentFd > g_iListenFd)
		iMaxFd = iAgentFd;
	else
		iMaxFd = g_iListenFd;
	PDEBUG("Current highest FD = %d", iMaxFd);

	return iMaxFd;
}

static defw_rc_t process_msg_session_info(char *msg, defw_agent_blk_t *agent)
{
	defw_msg_session_t *ses = (defw_msg_session_t *)msg;
	defw_agent_blk_t *existing;
	defw_type_t agent_type = ntohl(ses->node_type);
	defw_rc_t rc;

	if (agent_type != EN_DEFW_AGENT &&
	    agent_type != EN_DEFW_SERVICE &&
	    agent_type != EN_DEFW_DIRSVC)
		return EN_DEFW_RC_PROTO_ERROR;

	/* This is an agent on the new list. Let's see if there exists an
	 * agent that has the session information */
	existing = defw_find_agent_by_uuid_passive(ses->agent_id.remote_uuid);
	if (existing) {
		existing->iRpcFd = agent->iFileDesc;
		PDEBUG("existing = %p, agent = %p", existing, agent);
		PDEBUG("Second connection on an existing agent (%s) is the RPC connection: %d",
		       existing->name, existing->iRpcFd);
		if (ses->rpc_setup)
			set_agent_state(existing, DEFW_AGENT_RPC_CHANNEL_CONNECTED);
		if (ses->rpc_setup) {
			gettimeofday(&existing->last_control_activity, NULL);
			defw_agent_report_peer_ready(existing, "inbound-rpc-ready");
			rc = defw_send_hb(existing);
			if (rc != EN_DEFW_RC_OK)
				PERROR("Failed to send peer identity: %s",
				       defw_rc2str(rc));
		}
		/* release ref count acquired when you found the agent */
		defw_release_agent_blk(existing, false);
		/* agent should never be the same as existing.
		 * existing looks at the client and service lists while
		 * the agent passed in here should always be from the new
		 * list
		 */
		assert(agent != existing);
		defw_release_agent_blk(agent, false);
		return EN_DEFW_RC_OK;
	}

	if (ses->rpc_setup) {
		PERROR("Protocol Error. Setup of RPC before CNTRL");
		//defw_release_agent_blk(agent, true);
		return EN_DEFW_RC_PROTO_ERROR;
	}

	if (agent_type == EN_DEFW_AGENT)
		defw_move_to_client_list(agent);
	else
		defw_move_to_service_list(agent);

	/* update the agent with the information */
	uuid_copy(agent->id.remote_uuid, ses->agent_id.remote_uuid);
	agent->node_type = agent_type;
	agent->pid = ntohl(ses->pid);
	agent->listen_port = ntohl(ses->listen_port);
	strncpy(agent->hostname, ses->node_hostname, MAX_STR_LEN);
	agent->hostname[MAX_STR_LEN-1] = '\0';
	strncpy(agent->name, ses->node_name, MAX_STR_LEN);
	agent->name[MAX_STR_LEN-1] = '\0';
	set_agent_state(agent, DEFW_AGENT_CNTRL_CHANNEL_CONNECTED);
	set_agent_state(agent, DEFW_AGENT_STATE_ALIVE);
	unset_agent_state(agent, DEFW_AGENT_STATE_NEW);
	gettimeofday(&agent->time_stamp, NULL);
	agent->last_control_activity = agent->time_stamp;
	agent->last_heartbeat_rx = agent->time_stamp;
	PDEBUG("First connection on a new agent (%s) is the Cntrl connection: %d",
		agent->name, agent->iFileDesc);

	return EN_DEFW_RC_OK;
}

static defw_rc_t process_msg_unknown(char *msg, defw_agent_blk_t *agent)
{
	PERROR("Received an unsupported message");
	return EN_DEFW_RC_UNKNOWN_MESSAGE;
}

static defw_rc_t process_msg_hb(char *msg, defw_agent_blk_t *agent)
{
	defw_msg_session_t *hb = (defw_msg_session_t *)msg;
	bool learned_runtime_id = false;
	/*
	char uuid[UUID_STR_LEN];
	char uuid2[UUID_STR_LEN];
	*/
	/* update the agent with the information */
	/* TODO: Can check if the uuid has changed for security. If the
	 * uuid has been set before and the connection has not been dropped,
	 * but the uuid changed then this could be from an impostor.
	 */
	if (uuid_is_null(agent->id.remote_uuid)) {
		uuid_copy(agent->id.remote_uuid, hb->agent_id.remote_uuid);
		learned_runtime_id = true;
	} else if (uuid_compare(agent->id.remote_uuid,
				hb->agent_id.remote_uuid)) {
		PERROR("Agent %s has changed it's uuid. Has it restarted?",
		       agent->name);
	}
/*
	uuid_unparse_lower(agent->id.remote_uuid, uuid);
	uuid_unparse_lower(agent->id.blk_uuid, uuid2);
	PDEBUG("Received a heartbeat from %s-%s", uuid, uuid2);
*/
	agent->node_type = ntohl(hb->node_type);
	agent->pid = ntohl(hb->pid);
	strncpy(agent->hostname, hb->node_hostname, MAX_STR_LEN);
	agent->hostname[MAX_STR_LEN-1] = '\0';
	strncpy(agent->name, hb->node_name, MAX_STR_LEN);
	agent->name[MAX_STR_LEN-1] = '\0';
	gettimeofday(&agent->time_stamp, NULL);
	agent->last_heartbeat_rx = agent->time_stamp;
	agent->last_control_activity = agent->time_stamp;
	if (learned_runtime_id)
		defw_agent_report_peer_ready(agent, "remote-identity-ready");

	return EN_DEFW_RC_OK;
}

static defw_rc_t process_msg_get_num_agents(char *msg, defw_agent_blk_t *agent)
{
	defw_rc_t rc;
	defw_msg_num_agents_query_t query;

	query.num_agents = defw_get_num_connection_agents();
	rc = sendTcpMessage(agent->iFileDesc, (char *)&query, sizeof(query));
	if (rc) {
		PERROR("failed to send tcp message to get num agents query");
		return rc;
	}

	return EN_DEFW_RC_OK;
}

static defw_rc_t process_agent_message(defw_agent_blk_t *agent, int fd)
{
	defw_rc_t rc = EN_DEFW_RC_OK;
	defw_message_hdr_t hdr = {0};
	char *buffer;
	defw_msg_process_fn_t proc_fn;
	int cmp;

	/* get the header first */
	rc = readTcpMessage(fd, (char *)&hdr, sizeof(hdr),
			    TCP_READ_TIMEOUT_SEC, false);

	if (rc)
		return rc;

	hdr.version = ntohl(hdr.version);
	if (hdr.version != DEFW_VERSION_NUMBER) {
		PERROR("version %d != %d", hdr.version,
		       DEFW_VERSION_NUMBER);
		return EN_DEFW_RC_BAD_VERSION;
	}

	/* if the ips don't match ignore the message */
	hdr.ip.s_addr = ntohl(hdr.ip.s_addr);
	if ((cmp = memcmp(&agent->addr.sin_addr, &hdr.ip, sizeof(hdr.ip)))) {
		PERROR("IP addresses don't match");
		PERROR("agent IP = %s", inet_ntoa(agent->addr.sin_addr));
		PERROR("hdr IP = %s", inet_ntoa(hdr.ip));
		return EN_DEFW_RC_BAD_ADDR;
	}

	hdr.type = ntohl(hdr.type);
	hdr.len = ntohl(hdr.len);

	if (hdr.type >= EN_MSG_TYPE_MAX) {
		PERROR("Received an unknown message: %d", hdr.type);
		return EN_DEFW_RC_UNKNOWN_MESSAGE;
	}

	buffer = calloc(hdr.len, 1);
	if (!buffer)
		return EN_DEFW_RC_OOM;

	/* get the rest of the message */
	rc = readTcpMessage(fd, buffer, hdr.len,
			    TCP_READ_TIMEOUT_SEC, false);

	if (rc) {
		free(buffer);
		return rc;
	}

	/* call the appropriate processing function */
	proc_fn = msg_process_tbl[hdr.type];
	if (proc_fn) {
		rc = proc_fn(buffer, agent);
	} else {
		free(buffer);
		return EN_DEFW_RC_UNKNOWN_MESSAGE;
	}

	if (rc != EN_DEFW_RC_KEEP_DATA)
		free(buffer);

	if (rc == EN_DEFW_RC_KEEP_DATA)
		return EN_DEFW_RC_OK;

	return rc;
}

int process_new_agents_helper(defw_agent_blk_t *agent, void *user_data)
{
	connection_info_t *info = user_data;
	int *iNReady = info->iNReady;
	fd_set *tReadSet = info->tReadSet;
	int hb_fd = INVALID_TCP_SOCKET, rpc_fd = INVALID_TCP_SOCKET, rc;

	if (*iNReady) {
		if (FD_ISSET(agent->iFileDesc, tReadSet))
			hb_fd = agent->iFileDesc;
		if (FD_ISSET(agent->iRpcFd, tReadSet))
			rpc_fd = agent->iRpcFd;

		/* need to release reference on the connection here,
		 * because by the time the message gets processed the
		 * agent might've moved over to another list
		 */
		defw_release_agent_conn(agent);

		if (rpc_fd != INVALID_TCP_SOCKET &&
		    hb_fd == INVALID_TCP_SOCKET) {
			PERROR("agent connection is unexpected (%p) hb_fd = %d, rpc_fd = %d",
			       agent, hb_fd, rpc_fd);
			strncpy(agent->failure_reason, "unexpected-new-agent-rpc",
				sizeof(agent->failure_reason) - 1);
			defw_release_agent_blk(agent, true);
			FD_CLR(rpc_fd, tReadSet);
			(*iNReady)--;
			goto out;
		}

		if (hb_fd != INVALID_TCP_SOCKET) {
			rc = process_agent_message(agent, hb_fd);
			FD_CLR(hb_fd, tReadSet);
			if (rc) {
				PERROR("Error processing new agent: %s", defw_rc2str(rc));
				strncpy(agent->failure_reason,
					"new-agent-message-failure",
					sizeof(agent->failure_reason) - 1);
				defw_release_agent_blk(agent, true);
			}
			(*iNReady)--;
		}
	}

out:
	if (*iNReady)
		return 0;
	return 1;
}

static defw_rc_t process_new_agents(fd_set *tReadSet, int *iNReady)
{
	connection_info_t info;
	info.iNReady = iNReady;
	info.tReadSet = tReadSet;

	defw_new_agent_iter(process_new_agents_helper, &info);

	return EN_DEFW_RC_OK;
}

static int process_connection_agents_helper(defw_agent_blk_t *agent,
					    void *user_data)
{
	connection_info_t *info = user_data;
	int *iNReady = info->iNReady;
	fd_set *tReadSet = info->tReadSet;
	int hb_fd = INVALID_TCP_SOCKET, rpc_fd = INVALID_TCP_SOCKET, rc;
	bool dead = false;

	if (agent->state & DEFW_AGENT_STATE_NEW)
		goto out;

	if (*iNReady) {
		if (FD_ISSET(agent->iFileDesc, tReadSet))
			hb_fd = agent->iFileDesc;
		if (FD_ISSET(agent->iRpcFd, tReadSet))
			rpc_fd = agent->iRpcFd;

		if (hb_fd == INVALID_TCP_SOCKET &&
		    rpc_fd == INVALID_TCP_SOCKET)
			goto out;

		/* process heart beat */
		if (hb_fd != INVALID_TCP_SOCKET) {
			/* process the message */
			rc = process_agent_message(agent, hb_fd);
			FD_CLR(hb_fd, tReadSet);
			(*iNReady)--;
			if (rc && rc != EN_DEFW_RC_NO_DATA_ON_SOCKET) {
				if (agent->node_type == EN_DEFW_DIRSVC)
					set_dirsvc_connected(rc, NULL);
				PERROR("CTRL msg failure: %s: %d", defw_rc2str(rc),
				       agent->node_type);
				dead = true;
				goto out;
			}
		}

		if (*iNReady <= 0)
			goto out;

		/* process rpc */
		if (rpc_fd != INVALID_TCP_SOCKET) {
			/* process the message */
			PDEBUG("Received a message on %p:%d\n", agent, rpc_fd);
			rc = process_agent_message(agent, rpc_fd);
			FD_CLR(rpc_fd, tReadSet);
			(*iNReady)--;
			if (rc && rc != EN_DEFW_RC_NO_DATA_ON_SOCKET) {
				if (agent->node_type == EN_DEFW_DIRSVC)
					set_dirsvc_connected(rc, NULL);
				dead = true;
				PERROR("RPC msg failure: %s: %d", defw_rc2str(rc),
				       agent->node_type);
				goto out;
			}
		}
	}

out:
	defw_release_agent_blk(agent, dead);

	if (*iNReady)
		return 0;
	return 1;
}

static defw_rc_t process_connection_agents(fd_set *tReadSet, int *iNReady)
{
	connection_info_t info;
	info.iNReady = iNReady;
	info.tReadSet = tReadSet;

	defw_connection_agent_iter(process_connection_agents_helper, &info);

	return EN_DEFW_RC_OK;
}

static defw_rc_t init_comm(struct sockaddr_in *listen_addr)
{
	int iFlags;
	struct sockaddr_in sServAddr;

	signal(SIGPIPE, SIG_IGN);

	/*  Create a socket to listen to.  */
	g_iListenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_iListenFd < 0) {
		/*  Cannot create a listening socket.  */
		return EN_DEFW_RC_SOCKET_FAIL;
	}

	/* Set a socket option which will allow us to be quickly restarted
	 * if necessary.
	 */
	iFlags = 1;
	if (setsockopt(g_iListenFd, SOL_SOCKET, SO_REUSEADDR, (void *) &iFlags,
		       sizeof(iFlags)) < 0) {
		/*  Cannot change the socket options.  */
		closeTcpConnection(g_iListenFd);
		return EN_DEFW_RC_FAIL;
	}

	/*  Bind to our listening socket.  */
	bzero((char *) &sServAddr, sizeof(sServAddr));
	sServAddr.sin_family = AF_INET;
	sServAddr.sin_addr.s_addr = htonl(listen_addr->sin_addr.s_addr);
	sServAddr.sin_port = htons(listen_addr->sin_port);

	if (bind(g_iListenFd, (struct sockaddr *) &sServAddr,
		 sizeof(sServAddr)) < 0) {
		/*  Cannot bind our listening socket.  */
		closeTcpConnection(g_iListenFd);
		return EN_DEFW_RC_BIND_FAILED;
	}

	/* Let the system know we wish to listen to this port for
	 * connections.
	 */
	if (listen(g_iListenFd, 2) < 0) {
		/*  Cannot listen to socket, close and fail  */
		closeTcpConnection(g_iListenFd);
		return EN_DEFW_RC_LISTEN_FAILED;
	}

	/* We want this socket to be non-blocking even though it will be used
	 * in a blocking select call. This is to avoid a problem identified by
	 * Richard Stevens.
	 */
	iFlags = fcntl(g_iListenFd, F_GETFL, 0);
	fcntl(g_iListenFd, F_SETFL, iFlags | O_NONBLOCK);

	/*  Add the listening socket to our select() mask.  */
	FD_ZERO(&g_tAllSet);
	FD_SET(g_iListenFd, &g_tAllSet);

	return EN_DEFW_RC_OK;
}

static int agent_liveness_check_helper(defw_agent_blk_t *agent,
				       void *user_data)
{
	struct timeval *t = user_data;
	bool dead = false;

	if (agent->state & DEFW_AGENT_STATE_NEW) {
		if (t->tv_sec >= agent->handshake_deadline.tv_sec) {
			strncpy(agent->failure_reason, "handshake-timeout",
				sizeof(agent->failure_reason) - 1);
			PERROR("agent %s handshake timed out", agent->name);
			dead = true;
		}
	} else if (agent->heartbeat_mode == DEFW_HEARTBEAT_REMOTE &&
		   t->tv_sec - agent->last_heartbeat_rx.tv_sec >= HB_TO * 100) {
		strncpy(agent->failure_reason, "heartbeat-timeout",
			sizeof(agent->failure_reason) - 1);
		PERROR("agent %s presumed dead", agent->name);
		dead = true;
	}

	defw_release_agent_blk(agent, dead);
	return 0;
}

void agent_hb_check(struct timeval *t, defw_type_t me)
{
	defw_connection_agent_iter(agent_liveness_check_helper, t);
}

static int send_hb_to_agents(defw_agent_blk_t *agent, void *user_data)
{
	bool dead = false;
	defw_rc_t rc;

	if (agent->heartbeat_mode != DEFW_HEARTBEAT_REMOTE) {
		defw_release_agent_blk(agent, false);
		return 0;
	}

	rc = defw_send_hb(agent);
	if (rc != EN_DEFW_RC_OK) {
		strncpy(agent->failure_reason, "heartbeat-send-failure",
			sizeof(agent->failure_reason) - 1);
		dead = true;
	} else {
		gettimeofday(&agent->last_heartbeat_tx, NULL);
	}
	defw_release_agent_blk(agent, dead);

	return 0;
}

/*
 * defw_listener_main
 *   main loop.  Listens for incoming agent connections, and for agent
 *   messages.  Every period of time it triggers a walk through the agent
 *   list to see if any of the HBs stopped
 *
 *   If I am an Agent, then attempt to connect to the dirsvc and add an
 *   agent block on the list of agents. After successful connection send
 *   a regular heart beat.
 *
 *   Since the dirsvc's agent block is on the list of agents and its FD is
 *   on the select FD set, then if the dirsvc sends the agent a message
 *   the agent should be able to process it.
 */
static void *defw_listener_main(void *usr_data)
{
	int iConnFd;
	struct sockaddr_in sCliAddr;
	socklen_t  tCliLen;
	fd_set tReadSet;
	int iNReady;
	defw_rc_t rc;
	defw_agent_blk_t *agent = NULL;
	struct timeval time_1, time_2, select_to;
	defw_listener_info_t *info;
	bool send_hb_now = false;

	dirsvc_connect_in_progress = false;
	dirsvc_connected = false;

	info = (defw_listener_info_t *)usr_data;
	if ((!info) ||
	    ((info) && (info->listen_address.sin_port == 0))) {
		PERROR("No liston port provided");
		return NULL;
	}

	rc = init_comm(&info->listen_address);
	if (rc) {
		PERROR("init_comm failed: %s", defw_rc2str(rc));
		return NULL;
	}

	defw_agent_init();

	g_iMaxSelectFd = g_iListenFd;

	gettimeofday(&time_2, NULL);

	/*  Main Processing Loop: Keep going until we have reason
	 * to shutdown.
	 */
	while (!g_bShutdown) {
		/*  Wait on our select mask for an event to occur.  */
		select_to.tv_sec = HB_TO;
		select_to.tv_usec = 0;

		FD_ZERO(&tReadSet);
		pthread_mutex_lock(&global_var_mutex);
		tReadSet = g_tAllSet;
		pthread_mutex_unlock(&global_var_mutex);
		iNReady = select(g_iMaxSelectFd + 1, &tReadSet, NULL, NULL,
				 &select_to);

		//PDEBUG("iNReady == %d, g_iMaxSelectFd %d\n", iNReady, g_iMaxSelectFd);

		defw_release_dead_list_agents();

		/* Everyone registers with the dirsvc, even the dirsvc
		 * registers with itself
		 */
		if (!dirsvc_connected && strlen(get_parent_name()) != 0 &&
		    !dirsvc_connect_in_progress && !dirsvc_disabled()) {
			char *dirsvc_name = get_parent_name();
			char *ip_addr = get_parent_address();
			int port = get_parent_port();

			PDEBUG("Attempting a connection on dirsvc %s:%s:%d",
			       dirsvc_name, ip_addr, port);
			rc = defw_connect_to_service(ip_addr, get_parent_port(),
						    get_parent_name(), get_parent_hostname(),
						    EN_DEFW_DIRSVC, NULL, set_dirsvc_connected);
			if (rc == EN_DEFW_RC_IN_PROGRESS) {
				pthread_mutex_lock(&global_var_mutex);
				dirsvc_connect_in_progress = true;
				pthread_mutex_unlock(&global_var_mutex);
			}
		}

		/*  Determine if we failed the select call */
		if (iNReady < 0) {
			/*  Check to see if we were interrupted by a signal.  */
			if ((errno == EINTR) || (errno == EAGAIN)) {
				PERROR("Select failure: errno = %d", errno);
			} else if (errno != ECONNABORTED) {
				/* If this is an ECONNABORTED error, just
				 * ignore it. Raise a fatal alarm and shut
				 * down.
				 */
				PERROR("Shutting down Listener thread. errno: %d",
				       errno);
				defw_listener_shutdown();
			}

			/* store the current time */
			time_1 = time_2;

			/* Zero out the g_tAllSet */
			FD_ZERO(&g_tAllSet);

			continue;
		}

		if (FD_ISSET(g_iListenFd, &tReadSet)) {
			/* A new incoming connection */
			tCliLen = sizeof(sCliAddr);
			iConnFd = accept(g_iListenFd,
					 (struct sockaddr *) &sCliAddr,
					 &tCliLen);
			if (iConnFd < 0) {
				/*  Cannot accept new connection... just ignore.
				 */
				if (errno != EWOULDBLOCK)
					PERROR("Error on accept(), errno = %d", errno);
			} else {
				PDEBUG("Accepted a connection on %d from client port %d",
				       iConnFd, sCliAddr.sin_port);
				send_hb_now = true;
				/* For new connections we create an agent
				 * block, which goes on the new list. It
				 * stays there until the client sends
				 * session information we're able to
				 * figure out if it's a new agent or a new
				 * connection on an existing agent.
				 */
				agent = defw_find_create_agent_blk_by_addr(&sCliAddr);
				if (!agent) {
					/*  Cannot support more clients...just ignore.  */
					PERROR("Cannot accept more clients");
					closeTcpConnection(iConnFd);
				} else {
					int iOption, iFlags;

					PDEBUG("Received a connection (%p) from %s on FD %d",
					       agent, inet_ntoa(agent->addr.sin_addr), iConnFd);

					agent->iFileDesc = iConnFd;

					/*  Add new client to our select mask.  */
					FD_SET(iConnFd, &g_tAllSet);
					pthread_mutex_lock(&global_var_mutex);
					g_iMaxSelectFd = defw_get_highest_fd();
					pthread_mutex_unlock(&global_var_mutex);

					/* Ok, it seems that the connected socket gains
					 * the same flags as the listen socket.  We want
					 * to make it blocking here.
					 */
					iFlags = fcntl(iConnFd, F_GETFL, 0);
					fcntl(iConnFd, F_SETFL, iFlags & (~O_NONBLOCK));

					/*  And, we want to turn off Nagle's algorithm to
					 *  reduce latency
					 */
					iOption = 1;
					setsockopt(iConnFd, IPPROTO_TCP, TCP_NODELAY,
						   (void *)&iOption,
						   sizeof(iOption));
				}
			}

			/*  See if there are other messages waiting.  */
			iNReady--;
		}

		/* Let's go over the agent connection table and see if any
		 * known connection received a message. New connections are
		 * processed first so they can complete identity setup before
		 * normal message handling.
		 */
		if (iNReady)
			process_new_agents(&tReadSet, &iNReady);
		if (iNReady)
			process_connection_agents(&tReadSet, &iNReady);

		/*
		 * Each node can have a list of clients connected to it
		 * and a list of services connected to it. It can also be
		 * connected to other services.
		 *
		 * For all services we're connected to, we need to send
		 * a heart beat to tell them we're still around.
		 *
		 * For all clients connected to us, we expect a heart beat
		 * to tell us they are still alive. Otherwise we clean
		 * them up.
		 */
		gettimeofday(&time_2, NULL);
		if (agent_get_hb()) {
			/* check if HB_TO seconds has passed since the last
			 * time we collected the time
			 */
			if (time_2.tv_sec - time_1.tv_sec >= HB_TO * 100) {
				/* do the heartbeat check */
				agent_hb_check(&time_1, info->type);
			}
		}

		if (time_2.tv_sec - time_1.tv_sec >= HB_TO || send_hb_now) {
			defw_connection_agent_iter(send_hb_to_agents, NULL);
			send_hb_now = false;
		}

		/* store the current time */
		memcpy(&time_1, &time_2, sizeof(time_1));
	}

	/* Zero out the g_tAllSet */
	FD_ZERO(&g_tAllSet);

	return NULL;
}

defw_rc_t defw_spawn_listener(pthread_t *id)
{
	pthread_t tid;
	pthread_t *ptid;
	int trc;

	if (id)
		ptid = id;
	else
		ptid = &tid;

	/* initialize global mutex used for protecting global variables */
	pthread_mutex_init(&global_var_mutex, NULL);

	/*
	 * Spawn the listener thread if we are in dirsvc Mode.
	 * The listener thread listens for Heart beats and deals
	 * with maintaining the health of the agents. If an agent
	 * dies and comes back again, then we know how to deal
	 * with it.
	 */
	trc = pthread_create(ptid, NULL,
			     defw_listener_main,
			     &g_defw_cfg.l_info);
	if (trc) {
		PERROR("Failed to start listener thread");
		return EN_DEFW_RC_ERR_THREAD_STARTUP;
	}

	return EN_DEFW_RC_OK;
}
