#ifndef LIBDEFW_AGENT_H
#define LIBDEFW_AGENT_H

#include <stdbool.h>
#include "defw_agent.h"

typedef int (*process_agent)(defw_agent_blk_t *agent, void *user_data);

/*
 * agent_init
 *	Initialize the agent module
 */
void defw_agent_init(void);

/* defw_agent_get_highest_fd
 *	Find the highest connected FD in all connected agents.
 */
int defw_agent_get_highest_fd(void);

/*
 * defw_find_create_agent_blk_by_addr
 *	return an agent block with this address or create a new one
 */
defw_agent_blk_t *defw_find_create_agent_blk_by_addr(struct sockaddr_in *addr);

/*
 * defw_alloc_agent_blk
 *	allocate an agent block
 */
defw_agent_blk_t *defw_alloc_agent_blk(struct sockaddr_in *addr, bool add);

/*
 * acquire_agent_blk
 *	acquire the agent for work
 */
void acquire_agent_blk(defw_agent_blk_t *agent);

/*
 * agent_get_hb
 *	Get current HB state
 */
int agent_get_hb(void);

/*
 * get the number of known connection records
 */
int defw_get_num_connection_agents(void);

/*
 * set_agent_state
 *
 * convenience function to set the agent state
 */
void set_agent_state(defw_agent_blk_t *agent, unsigned int state);

/*
 * unset_agent_state
 *
 * unset the state and check if the agent is a zombie and
 * it has not pending work. If so then free it
 */
void unset_agent_state(defw_agent_blk_t *agent, unsigned int state);

/*
 * defw_agent_report_peer_ready
 *	report a callable transport peer to peer lifecycle listeners
 */
void defw_agent_report_peer_ready(defw_agent_blk_t *agent, const char *reason);

/*
 * defw_agent_report_peer_ready_update
 *	report corrected ready metadata after transport identity is known
 */
void defw_agent_report_peer_ready_update(defw_agent_blk_t *agent,
					 const char *reason);


/*
 * defw_release_agent_conn
 *	release an agent connection
 */
void defw_release_agent_conn(defw_agent_blk_t *agent);

/*
 * defw_get_next_new_agent_conn
 *	Iterate over the agent blocks on the new list
 */
defw_agent_blk_t *defw_get_next_new_agent_conn(defw_agent_blk_t *agent);

defw_rc_t defw_send_hb(defw_agent_blk_t *agent);
defw_rc_t defw_send_session_info(defw_agent_blk_t *agent, bool rpc_setup);
defw_agent_blk_t *defw_find_agent_by_uuid_global(defw_agent_uuid_t *id);
defw_agent_blk_t *defw_find_agent_by_uuid_passive(uuid_t uuid);
/* Find an agent by the block uuid, which is the identifier the Python layer
 * is handed for an incoming message and hands back when replying. Returns the
 * agent with a reference taken, or NULL.
 */
defw_agent_blk_t *defw_find_agent_by_blk_uuid(char *blk_uuid_str);
/* Tell a peer we have finished reading the region it registered under handle,
 * so it can deregister and release the memory.
 */
defw_rc_t defw_send_rma_ack(defw_agent_blk_t *agent, uint64_t handle);
void defw_move_to_client_list(defw_agent_blk_t *agent);
void defw_move_to_service_list(defw_agent_blk_t *agent);
void defw_release_dead_list_agents(void);
void defw_new_agent_iter(process_agent cb, void *user_data);
void defw_connection_agent_iter(process_agent cb, void *user_data);

#endif /* LIBDEFW_AGENT_H */
