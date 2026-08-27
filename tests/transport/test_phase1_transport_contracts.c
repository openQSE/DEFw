#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>

#include "defw.h"
#include "defw_agent.h"
#include "defw_listener.h"
#include "defw_print.h"
#include "libdefw_agent.h"

static defw_peer_event_t peer_events[8];
static int peer_event_count;

static defw_rc_t record_peer_event(const defw_peer_event_t *event)
{
	if (peer_event_count <
	    (int)(sizeof(peer_events) / sizeof(peer_events[0])))
		peer_events[peer_event_count] = *event;
	peer_event_count++;
	return EN_DEFW_RC_OK;
}

static defw_agent_blk_t *make_agent(const char *addr_text, int port,
				    defw_connection_direction_t direction,
				    defw_type_t type, const char *name)
{
	struct sockaddr_in addr;
	defw_agent_blk_t *agent;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (!inet_aton(addr_text, &addr.sin_addr))
		return NULL;

	agent = defw_alloc_agent_blk(&addr, true);
	if (!agent)
		return NULL;

	agent->direction = direction;
	agent->node_type = type;
	agent->listen_port = port;
	agent->pid = port;
	agent->lifecycle = DEFW_CONN_LIFECYCLE_READY;
	strncpy(agent->name, name, sizeof(agent->name) - 1);
	strncpy(agent->hostname, "localhost", sizeof(agent->hostname) - 1);
	uuid_generate(agent->id.remote_uuid);
	set_agent_state(agent, DEFW_AGENT_STATE_ALIVE);
	unset_agent_state(agent, DEFW_AGENT_STATE_NEW);

	return agent;
}

static void release_agent(defw_agent_blk_t *agent)
{
	if (agent)
		defw_release_agent_blk(agent, true);
}

static int expect(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "%s\n", message);
		return 1;
	}
	return 0;
}

struct collected_agents {
	defw_agent_blk_t *agents[4];
	int count;
};

static int collect_agent(defw_agent_blk_t *agent, void *user_data)
{
	struct collected_agents *collected = user_data;

	if (collected->count <
	    (int)(sizeof(collected->agents) / sizeof(collected->agents[0])))
		collected->agents[collected->count] = agent;
	collected->count++;
	defw_release_agent_blk(agent, false);
	return 0;
}

static int test_connection_table_iteration_order(void)
{
	defw_agent_blk_t *dirsvc;
	defw_agent_blk_t *service;
	struct collected_agents collected = { 0 };
	int rc = 0;

	dirsvc = make_agent("127.0.0.1", 41001,
			    DEFW_CONN_DIRECTION_OUTBOUND, EN_DEFW_DIRSVC,
			    "dirsvc");
	service = make_agent("127.0.0.1", 41002,
			     DEFW_CONN_DIRECTION_OUTBOUND, EN_DEFW_SERVICE,
			     "service");
	if (!dirsvc || !service)
		return 1;

	defw_connection_agent_iter(collect_agent, &collected);
	rc |= expect(collected.count == 2,
		     "connection iterator returned wrong count");
	rc |= expect(collected.agents[0] == dirsvc,
		     "connection iterator did not preserve dirsvc order");
	rc |= expect(collected.agents[1] == service,
		     "connection iterator did not preserve service order");
	rc |= expect(defw_get_num_connection_agents() == 2,
		     "connection count did not include known records");

	release_agent(dirsvc);
	release_agent(service);
	return rc;
}

static int test_peer_ready_identity_update(void)
{
	defw_agent_blk_t *agent;
	char local_uuid[UUID_STR_LEN];
	int rc = 0;

	if (defw_register_peer_event_callback(record_peer_event))
		return 1;

	uuid_generate(g_defw_cfg.uuid);
	uuid_unparse_lower(g_defw_cfg.uuid, local_uuid);

	agent = make_agent("127.0.0.1", 41003,
			   DEFW_CONN_DIRECTION_OUTBOUND, EN_DEFW_SERVICE,
			   "loopback-service");
	if (!agent)
		return 1;
	uuid_clear(agent->id.remote_uuid);
	set_agent_state(agent, DEFW_AGENT_CNTRL_CHANNEL_CONNECTED);
	set_agent_state(agent, DEFW_AGENT_RPC_CHANNEL_CONNECTED);

	defw_agent_report_peer_ready(agent, "outbound-rpc-ready");
	rc |= expect(peer_event_count == 0,
		     "ready event was emitted before runtime identity");
	rc |= expect(agent->heartbeat_mode == DEFW_HEARTBEAT_NONE,
		     "unknown runtime identity should not enable heartbeat");

	uuid_copy(agent->id.remote_uuid, g_defw_cfg.uuid);
	defw_agent_report_peer_ready_update(agent, "remote-identity-ready");

	rc |= expect(peer_event_count == 1,
		     "identity-ready event was not emitted");
	rc |= expect(!strcmp(peer_events[0].remote_runtime_id, local_uuid),
		     "identity update did not report remote runtime");
	rc |= expect(peer_events[0].is_self,
		     "identity update did not classify loopback");
	rc |= expect(agent->is_loopback,
		     "agent loopback state was not updated");
	rc |= expect(agent->heartbeat_mode == DEFW_HEARTBEAT_NONE,
		     "loopback ready update should disable heartbeat");

	release_agent(agent);
	return rc;
}

static int test_send_rejects_invalid_arguments(void)
{
	char uuid[] = "00000000-0000-0000-0000-000000000000";
	char yaml[] = "{}";
	int rc = 0;

	rc |= expect(defw_send_req(NULL, uuid, yaml) == EN_DEFW_RC_BAD_PARAM,
		     "request send accepted a null destination UUID");
	rc |= expect(defw_send_req(uuid, NULL, yaml) == EN_DEFW_RC_BAD_PARAM,
		     "request send accepted a null block UUID");
	rc |= expect(defw_send_req(uuid, uuid, NULL) == EN_DEFW_RC_BAD_PARAM,
		     "request send accepted a null payload");
	rc |= expect(defw_send_rsp(NULL, uuid, yaml) == EN_DEFW_RC_BAD_PARAM,
		     "response send accepted a null destination UUID");
	rc |= expect(defw_send_rsp(uuid, NULL, yaml) == EN_DEFW_RC_BAD_PARAM,
		     "response send accepted a null block UUID");
	rc |= expect(defw_send_rsp(uuid, uuid, NULL) == EN_DEFW_RC_BAD_PARAM,
		     "response send accepted a null payload");
	return rc;
}

int main(void)
{
	defw_init_logging();
	defw_agent_init();

	if (test_connection_table_iteration_order())
		return 1;
	if (test_peer_ready_identity_update())
		return 1;
	if (test_send_rejects_invalid_arguments())
		return 1;
	return 0;
}
