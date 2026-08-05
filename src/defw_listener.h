#ifndef DEFW_LISTENER_H
#define DEFW_LISTENER_H

#include "defw_common.h"
#include "defw_agent.h"

/* Message processing callbacks */
typedef defw_rc_t (*defw_msg_process_fn_t)(char *msg, defw_agent_blk_t *agent);

defw_rc_t defw_register_agent_update_notification_cb(defw_agent_update_cb cb);

defw_rc_t defw_register_msg_callback(defw_msg_type_t msg_type, defw_msg_process_fn_t cb);

defw_rc_t defw_register_connect_complete(defw_connect_status cb);

defw_rc_t defw_register_peer_event_callback(defw_peer_event_cb cb);

void defw_agent_updated_notify(void);

void defw_notify_connect_complete(defw_rc_t status, uuid_t uuid);

void defw_notify_peer_event(const defw_peer_event_t *event);

defw_rc_t defw_spawn_listener(pthread_t *id);

void defw_listener_shutdown(void);

int defw_get_highest_fd(void);

#endif /* DEFW_LISTENER_H */
