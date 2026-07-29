#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <pthread.h>
#include "defw_transport.h"
#include "defw_agent.h"
#include "defw_common.h"
#include "defw_global.h"
#include "defw_print.h"

/*
 * libfabric / OFI transport.
 *
 * A single reliable-datagram (FI_EP_RDM) endpoint per process. Peers are
 * reached by fi_addr_t entries in the address vector, learned over the TCP
 * session handshake (phase 1b). The control channel (session info and
 * heartbeats) stays on TCP because it bootstraps the OFI addresses and
 * provides liveness; RPC traffic moves onto the fabric.
 *
 * Sends assemble the framed [header][body] message and block on a dedicated
 * transmit CQ until completion (serialized, so the temporary buffer is safe
 * to free). Receives are handled by a progress thread draining a separate
 * receive CQ into the shared message dispatch (defw_ofi_dispatch).
 *
 * The whole libfabric implementation is compiled only when DEFw is built with
 * libfabric (HAVE_LIBFABRIC, set by the SConstruct when libfabric is found).
 * Otherwise a stub reports that OFI is unavailable and the caller falls back
 * to TCP.
 */

#ifdef HAVE_LIBFABRIC

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>

/* Minimum libfabric API version DEFw targets. The container ships 2.3.1. */
#define DEFW_OFI_VERSION	FI_VERSION(1, 20)
/* DEFW_OFI_MAX_ADDRLEN is shared with the message layer (defw_message.h). */

/* Receive buffer pool. Each posted buffer holds one eager message (header
 * included). A message larger than this is a phase-3 (large payload / RMA)
 * concern and shows up here as a truncation error.
 */
#define DEFW_OFI_RECV_BUF_SIZE		(256 * 1024)
#define DEFW_OFI_NUM_RECV_BUFS		8
/* how long a blocking send waits for its completion before giving up (ms) */
#define DEFW_OFI_SEND_TIMEOUT_MS	20000
/* progress-thread CQ poll timeout (ms); short enough to notice shutdown */
#define DEFW_OFI_PROGRESS_TIMEOUT_MS	1000

/* A posted receive buffer. fi_context MUST be first so a completion's
 * op_context (which points at it) casts straight back to the buffer.
 */
struct ofi_recv_buf {
	struct fi_context context;
	unsigned char     data[DEFW_OFI_RECV_BUF_SIZE];
};

typedef struct defw_ofi_state_s {
	struct fi_info      *info;
	struct fid_fabric   *fabric;
	struct fid_domain   *domain;
	struct fid_ep       *ep;
	struct fid_av       *av;
	struct fid_cq       *tx_cq;  /* send completions (drained by the sender) */
	struct fid_cq       *rx_cq;  /* recv completions (drained by progress) */
	uint8_t              local_addr[DEFW_OFI_MAX_ADDRLEN];
	size_t               local_addrlen;
	struct ofi_recv_buf *recv_bufs;
	pthread_t            progress_thread;
	volatile bool        progress_run;
	pthread_mutex_t      send_lock;  /* one outstanding send at a time */
	struct fi_context    send_ctx;
	bool                 up;
} defw_ofi_state_t;

static defw_ofi_state_t g_ofi;

static ssize_t ofi_post_recv(struct ofi_recv_buf *buf)
{
	return fi_recv(g_ofi.ep, buf->data, DEFW_OFI_RECV_BUF_SIZE, NULL,
		       FI_ADDR_UNSPEC, &buf->context);
}

/*
 * Assemble the framed message ([header][body]) and send it to dst over the
 * fabric, blocking until the send completes so the temporary buffer is safe
 * to free. Sends are serialized, so exactly one completion is outstanding on
 * the transmit CQ and it is unambiguously ours.
 */
static defw_rc_t ofi_send_msg(fi_addr_t dst, char *body, size_t bodylen,
			      defw_msg_type_t type)
{
	size_t msglen = sizeof(defw_message_hdr_t) + bodylen;
	struct fi_cq_msg_entry entry;
	defw_message_hdr_t *hdr;
	defw_rc_t rc = EN_DEFW_RC_OK;
	char *msg;
	ssize_t ret;

	msg = malloc(msglen);
	if (!msg)
		return EN_DEFW_RC_OOM;

	hdr = (defw_message_hdr_t *)msg;
	hdr->type = htonl(type);
	hdr->len = htonl((unsigned int)bodylen);
	uuid_copy(hdr->sender_uuid, g_defw_cfg.uuid);
	hdr->version = htonl(DEFW_VERSION_NUMBER);
	if (bodylen)
		memcpy(msg + sizeof(*hdr), body, bodylen);

	PDEBUG("OFI RPC send: type=%d bodylen=%zu to fi_addr=%lu",
	       type, bodylen, (unsigned long)dst);

	pthread_mutex_lock(&g_ofi.send_lock);

	do {
		ret = fi_send(g_ofi.ep, msg, msglen, NULL, dst, &g_ofi.send_ctx);
		if (ret == -FI_EAGAIN)
			fi_cq_read(g_ofi.tx_cq, &entry, 1); /* drive progress */
	} while (ret == -FI_EAGAIN);

	if (ret) {
		PERROR("fi_send failed: %s", fi_strerror((int)-ret));
		rc = EN_DEFW_RC_SOCKET_FAIL;
		goto out;
	}

	ret = fi_cq_sread(g_ofi.tx_cq, &entry, 1, NULL, DEFW_OFI_SEND_TIMEOUT_MS);
	if (ret == 1) {
		/* send completed */
	} else if (ret == -FI_EAVAIL) {
		struct fi_cq_err_entry err = {0};

		fi_cq_readerr(g_ofi.tx_cq, &err, 0);
		PERROR("OFI send completion error: %s",
		       fi_cq_strerror(g_ofi.tx_cq, err.prov_errno,
				      err.err_data, NULL, 0));
		rc = EN_DEFW_RC_SOCKET_FAIL;
	} else if (ret == -FI_EAGAIN) {
		PERROR("OFI send completion timed out");
		rc = EN_DEFW_RC_TIMEOUT;
	} else {
		PERROR("fi_cq_sread(tx) failed: %s", fi_strerror((int)-ret));
		rc = EN_DEFW_RC_SOCKET_FAIL;
	}

out:
	pthread_mutex_unlock(&g_ofi.send_lock);
	free(msg);
	return rc;
}

/*
 * Active-transport send. The control channel (session info + heartbeats)
 * always stays on TCP: it bootstraps the OFI addresses and is the liveness
 * signal. RPC traffic goes over the fabric once we have learned the peer's
 * OFI address (DEFW_AGENT_OFI_ADDR_VALID); until then, or for a TCP-only
 * peer, it falls back to TCP.
 */
static defw_rc_t ofi_send(defw_agent_blk_t *agent, defw_channel_t ch,
			  char *buf, size_t len, defw_msg_type_t type)
{
	/* control channel always on TCP; RPC on the fabric only once the
	 * endpoint is up and we know the peer's OFI address, otherwise TCP
	 */
	if (ch == EN_DEFW_CHANNEL_CTRL || !g_ofi.up ||
	    !(agent->state & DEFW_AGENT_OFI_ADDR_VALID))
		return defw_transport_tcp_ops()->send(agent, ch, buf, len, type);

	return ofi_send_msg((fi_addr_t)agent->ofi_addr, buf, len, type);
}

static void *ofi_progress_thread(void *arg)
{
	struct fi_cq_msg_entry entry;
	ssize_t ret;

	(void)arg;

	while (g_ofi.progress_run) {
		ret = fi_cq_sread(g_ofi.rx_cq, &entry, 1, NULL,
				  DEFW_OFI_PROGRESS_TIMEOUT_MS);
		if (ret == 1) {
			struct ofi_recv_buf *buf = entry.op_context;
			size_t msglen = entry.len;
			char *copy = malloc(msglen);

			/* copy out and re-post the pinned buffer immediately so
			 * the dispatch (which may call into Python) does not
			 * hold up receive capacity
			 */
			if (copy)
				memcpy(copy, buf->data, msglen);
			ofi_post_recv(buf);
			PDEBUG("OFI RPC recv: %zu bytes", msglen);
			if (copy)
				defw_ofi_dispatch(copy, msglen);
		} else if (ret == -FI_EAGAIN) {
			continue; /* poll timeout; re-check progress_run */
		} else if (ret == -FI_EAVAIL) {
			struct fi_cq_err_entry err = {0};

			fi_cq_readerr(g_ofi.rx_cq, &err, 0);
			PERROR("OFI recv completion error: %s",
			       fi_cq_strerror(g_ofi.rx_cq, err.prov_errno,
					      err.err_data, NULL, 0));
			if (err.op_context)
				ofi_post_recv(err.op_context);
		} else {
			PERROR("fi_cq_sread(rx) failed: %s",
			       fi_strerror((int)-ret));
			break;
		}
	}

	return NULL;
}

static void ofi_fini(void)
{
	if (g_ofi.progress_run) {
		g_ofi.progress_run = false;
		pthread_join(g_ofi.progress_thread, NULL);
	}
	if (g_ofi.ep)
		fi_close(&g_ofi.ep->fid);
	if (g_ofi.tx_cq)
		fi_close(&g_ofi.tx_cq->fid);
	if (g_ofi.rx_cq)
		fi_close(&g_ofi.rx_cq->fid);
	if (g_ofi.av)
		fi_close(&g_ofi.av->fid);
	if (g_ofi.domain)
		fi_close(&g_ofi.domain->fid);
	if (g_ofi.fabric)
		fi_close(&g_ofi.fabric->fid);
	if (g_ofi.info)
		fi_freeinfo(g_ofi.info);
	free(g_ofi.recv_bufs);
	pthread_mutex_destroy(&g_ofi.send_lock);
	memset(&g_ofi, 0, sizeof(g_ofi));
}

static defw_transport_ops_t ofi_ops = {
	.name = "ofi",
	.send = ofi_send,
	.init = NULL,
	.connect = NULL,
	.progress = NULL,
	.disconnect = NULL,
	.fini = ofi_fini,
};

defw_rc_t defw_transport_ofi_init(const char *provider)
{
	struct fi_info *hints;
	struct fi_cq_attr cq_attr;
	struct fi_av_attr av_attr;
	int ret, i;

	memset(&g_ofi, 0, sizeof(g_ofi));
	memset(&av_attr, 0, sizeof(av_attr));
	pthread_mutex_init(&g_ofi.send_lock, NULL);

	hints = fi_allocinfo();
	if (!hints) {
		pthread_mutex_destroy(&g_ofi.send_lock);
		return EN_DEFW_RC_OOM;
	}

	/* Reliable datagram, message semantics. FI_CONTEXT is accepted because
	 * most providers require a per-operation context object.
	 */
	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG;
	hints->mode = FI_CONTEXT;
	hints->domain_attr->threading = FI_THREAD_SAFE;
	if (provider && strlen(provider)) {
		hints->fabric_attr->prov_name = strdup(provider);
		if (!hints->fabric_attr->prov_name) {
			fi_freeinfo(hints);
			pthread_mutex_destroy(&g_ofi.send_lock);
			return EN_DEFW_RC_OOM;
		}
	}

	ret = fi_getinfo(DEFW_OFI_VERSION, NULL, NULL, 0, hints, &g_ofi.info);
	fi_freeinfo(hints);
	if (ret) {
		PERROR("fi_getinfo failed: %s", fi_strerror(-ret));
		pthread_mutex_destroy(&g_ofi.send_lock);
		return EN_DEFW_RC_FAIL;
	}

	PMSG("OFI provider selected: %s", g_ofi.info->fabric_attr->prov_name);

	ret = fi_fabric(g_ofi.info->fabric_attr, &g_ofi.fabric, NULL);
	if (ret) {
		PERROR("fi_fabric failed: %s", fi_strerror(-ret));
		goto err;
	}

	ret = fi_domain(g_ofi.fabric, g_ofi.info, &g_ofi.domain, NULL);
	if (ret) {
		PERROR("fi_domain failed: %s", fi_strerror(-ret));
		goto err;
	}

	av_attr.type = FI_AV_MAP;
	ret = fi_av_open(g_ofi.domain, &av_attr, &g_ofi.av, NULL);
	if (ret) {
		PERROR("fi_av_open failed: %s", fi_strerror(-ret));
		goto err;
	}

	/* separate transmit and receive completion queues so a blocking send
	 * can wait on its own completion without racing the progress thread
	 */
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.format = FI_CQ_FORMAT_MSG;
	cq_attr.wait_obj = FI_WAIT_UNSPEC;
	cq_attr.size = g_ofi.info->tx_attr->size;
	ret = fi_cq_open(g_ofi.domain, &cq_attr, &g_ofi.tx_cq, NULL);
	if (ret) {
		PERROR("fi_cq_open(tx) failed: %s", fi_strerror(-ret));
		goto err;
	}

	cq_attr.size = g_ofi.info->rx_attr->size;
	ret = fi_cq_open(g_ofi.domain, &cq_attr, &g_ofi.rx_cq, NULL);
	if (ret) {
		PERROR("fi_cq_open(rx) failed: %s", fi_strerror(-ret));
		goto err;
	}

	ret = fi_endpoint(g_ofi.domain, g_ofi.info, &g_ofi.ep, NULL);
	if (ret) {
		PERROR("fi_endpoint failed: %s", fi_strerror(-ret));
		goto err;
	}

	ret = fi_ep_bind(g_ofi.ep, &g_ofi.av->fid, 0);
	if (ret) {
		PERROR("fi_ep_bind(av) failed: %s", fi_strerror(-ret));
		goto err;
	}

	ret = fi_ep_bind(g_ofi.ep, &g_ofi.tx_cq->fid, FI_TRANSMIT);
	if (ret) {
		PERROR("fi_ep_bind(tx_cq) failed: %s", fi_strerror(-ret));
		goto err;
	}

	ret = fi_ep_bind(g_ofi.ep, &g_ofi.rx_cq->fid, FI_RECV);
	if (ret) {
		PERROR("fi_ep_bind(rx_cq) failed: %s", fi_strerror(-ret));
		goto err;
	}

	ret = fi_enable(g_ofi.ep);
	if (ret) {
		PERROR("fi_enable failed: %s", fi_strerror(-ret));
		goto err;
	}

	/* Cache our endpoint name for the session-info exchange. Peers insert
	 * this into their address vector to reach us.
	 */
	g_ofi.local_addrlen = sizeof(g_ofi.local_addr);
	ret = fi_getname(&g_ofi.ep->fid, g_ofi.local_addr, &g_ofi.local_addrlen);
	if (ret) {
		PERROR("fi_getname failed: %s", fi_strerror(-ret));
		goto err;
	}

	/* pre-post the receive buffer pool before any peer can send to us */
	g_ofi.recv_bufs = calloc(DEFW_OFI_NUM_RECV_BUFS,
				 sizeof(*g_ofi.recv_bufs));
	if (!g_ofi.recv_bufs) {
		PERROR("OFI recv buffer allocation failed");
		goto err;
	}
	for (i = 0; i < DEFW_OFI_NUM_RECV_BUFS; i++) {
		if (ofi_post_recv(&g_ofi.recv_bufs[i])) {
			PERROR("fi_recv pre-post failed");
			goto err;
		}
	}

	g_ofi.up = true;

	/* start draining receive completions */
	g_ofi.progress_run = true;
	if (pthread_create(&g_ofi.progress_thread, NULL,
			   ofi_progress_thread, NULL)) {
		PERROR("failed to start OFI progress thread");
		g_ofi.progress_run = false;
		g_ofi.up = false;
		goto err;
	}

	PMSG("OFI endpoint up: provider=%s addrlen=%zu",
	     g_ofi.info->fabric_attr->prov_name, g_ofi.local_addrlen);

	/* make OFI the active transport; RPC traffic now uses the fabric */
	defw_transport_set_ops(&ofi_ops);

	return EN_DEFW_RC_OK;

err:
	ofi_fini();
	return EN_DEFW_RC_FAIL;
}

defw_rc_t defw_transport_ofi_local_addr(void *buf, size_t *len)
{
	if (!g_ofi.up || !buf || !len)
		return EN_DEFW_RC_FAIL;

	if (*len < g_ofi.local_addrlen)
		return EN_DEFW_RC_FAIL;

	memcpy(buf, g_ofi.local_addr, g_ofi.local_addrlen);
	*len = g_ofi.local_addrlen;

	return EN_DEFW_RC_OK;
}

defw_rc_t defw_transport_ofi_av_insert(const void *addr, size_t addrlen,
				       uint64_t *fi_addr_out)
{
	fi_addr_t fi_addr = FI_ADDR_UNSPEC;
	int ret;

	if (!g_ofi.up || !addr || !addrlen || !fi_addr_out)
		return EN_DEFW_RC_FAIL;

	/* fi_av_insert returns the number of addresses inserted (1), or a
	 * negative errno on failure.
	 */
	ret = fi_av_insert(g_ofi.av, addr, 1, &fi_addr, 0, NULL);
	if (ret != 1) {
		PERROR("fi_av_insert returned %d", ret);
		return EN_DEFW_RC_FAIL;
	}

	*fi_addr_out = (uint64_t)fi_addr;

	return EN_DEFW_RC_OK;
}

#else /* !HAVE_LIBFABRIC */

defw_rc_t defw_transport_ofi_init(const char *provider)
{
	(void)provider;
	PERROR("libfabric support was not built into DEFw");
	return EN_DEFW_RC_FAIL;
}

defw_rc_t defw_transport_ofi_local_addr(void *buf, size_t *len)
{
	(void)buf;
	(void)len;
	return EN_DEFW_RC_FAIL;
}

defw_rc_t defw_transport_ofi_av_insert(const void *addr, size_t addrlen,
				       uint64_t *fi_addr_out)
{
	(void)addr;
	(void)addrlen;
	(void)fi_addr_out;
	return EN_DEFW_RC_FAIL;
}

#endif /* HAVE_LIBFABRIC */
