#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <pthread.h>
#include "defw_transport.h"
#include "defw_agent.h"
#include "libdefw_connect.h"
#include "defw_common.h"
#include "defw_print.h"

/*
 * Built-in TCP transport.
 *
 * Maps the logical CTRL and RPC channels onto the agent's two sockets and
 * forwards to the existing framed-message writer (defw_send_msg). This keeps
 * DEFw's historical wire behavior identical; the ops table simply names the
 * seam that a fabric transport will later plug into.
 */
static defw_rc_t tcp_send(defw_agent_blk_t *agent, defw_channel_t ch,
			  char *buf, size_t len, defw_msg_type_t type)
{
	int fd;

	if (!agent)
		return EN_DEFW_RC_BAD_PARAM;

	fd = (ch == EN_DEFW_CHANNEL_RPC) ? agent->iRpcFd : agent->iFileDesc;

	return defw_send_msg(fd, buf, len, type);
}

static defw_transport_ops_t tcp_ops = {
	.name = "tcp",
	.send = tcp_send,
	.init = NULL,
	.connect = NULL,
	.progress = NULL,
	.disconnect = NULL,
	.fini = NULL,
};

/* Statically initialized so the accessor is valid before any explicit
 * transport setup runs.
 */
static defw_transport_ops_t *g_transport = &tcp_ops;

defw_transport_ops_t *defw_transport_tcp_ops(void)
{
	return &tcp_ops;
}

defw_transport_ops_t *defw_transport_ops(void)
{
	return g_transport;
}

void defw_transport_set_ops(defw_transport_ops_t *ops)
{
	if (ops)
		g_transport = ops;
}
