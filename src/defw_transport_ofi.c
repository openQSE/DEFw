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
#include "defw_list.h"
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
#include <rdma/fi_rma.h>
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

/* Set DEFW_OFI_RMA_SELFTEST=1 to run a loopback fi_read against our own
 * endpoint at startup. Diagnostic only, and off by default.
 */
#define DEFW_OFI_RMA_SELFTEST_ENV	"DEFW_OFI_RMA_SELFTEST"
#define DEFW_OFI_SELFTEST_LEN		(64 * 1024)

/* A posted receive buffer. fi_context MUST be first so a completion's
 * op_context (which points at it) casts straight back to the buffer.
 */
struct ofi_recv_buf {
	struct fi_context context;
	unsigned char     data[DEFW_OFI_RECV_BUF_SIZE];
};

/* A memory region currently exposed for a peer to fi_read. Registrations are
 * tracked here rather than handed out as pointers so that the rest of DEFw
 * (ultimately the Python layer, across a text message) can refer to one by an
 * opaque integer handle, and so a stale or duplicated release is rejected
 * instead of dereferencing freed memory.
 */
struct ofi_mr_entry {
	struct dlist_entry entry;
	uint64_t           handle;
	struct fid_mr     *mr;
	void              *buf;
	size_t             len;
	bool               owned; /* buf was copied by us, so free it */
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
	bool                 rma_capable; /* provider offers FI_RMA (fi_read) */
	int                  mr_mode;    /* registration mode the provider needs */
	struct dlist_entry   mr_list;    /* registered regions (ofi_mr_entry) */
	pthread_mutex_t      mr_lock;
	uint64_t             mr_next_handle;
	uint64_t             mr_next_key;
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

/*
 * Memory registration for the RMA rendezvous.
 *
 * A process that wants to hand a peer a large payload registers it here and
 * advertises the resulting {key, addr, len}; the peer issues an fi_read and
 * acknowledges, at which point the region is released.
 *
 * Two provider behaviors have to be absorbed here so that no caller (and in
 * particular no peer) has to reason about them:
 *
 *   - Key allocation. When the provider requires FI_MR_PROV_KEY it invents the
 *     key and ignores what we asked for; otherwise the key is ours to choose
 *     and must be unique among our live registrations. Passing a fresh key
 *     from a counter and then reading back fi_mr_key() is correct either way.
 *     A collision is not silent: fi_mr_reg fails with FI_ENOKEY.
 *
 *   - Region addressing. When the provider requires FI_MR_VIRT_ADDR the peer
 *     names the region by our virtual address; otherwise by an offset from the
 *     start of the region. We resolve that into desc->addr at registration.
 */
static defw_rc_t ofi_mr_reg_common(const void *buf, size_t len, bool copy,
				   defw_rma_desc_t *desc)
{
	struct ofi_mr_entry *e;
	void *region;
	uint64_t key;
	int ret;

	if (!buf || !len || !desc)
		return EN_DEFW_RC_BAD_PARAM;

	if (!g_ofi.up || !g_ofi.rma_capable)
		return EN_DEFW_RC_FAIL;

	e = calloc(1, sizeof(*e));
	if (!e)
		return EN_DEFW_RC_OOM;

	if (copy) {
		region = malloc(len);
		if (!region) {
			free(e);
			return EN_DEFW_RC_OOM;
		}
		memcpy(region, buf, len);
	} else {
		region = (void *)buf;
	}

	pthread_mutex_lock(&g_ofi.mr_lock);

	key = g_ofi.mr_next_key++;
	ret = fi_mr_reg(g_ofi.domain, region, len, FI_REMOTE_READ, 0, key, 0,
			&e->mr, NULL);
	if (ret) {
		pthread_mutex_unlock(&g_ofi.mr_lock);
		PERROR("fi_mr_reg failed for %zu bytes: %s", len,
		       fi_strerror(-ret));
		if (copy)
			free(region);
		free(e);
		return EN_DEFW_RC_FAIL;
	}

	e->handle = g_ofi.mr_next_handle++;
	e->buf = region;
	e->len = len;
	e->owned = copy;
	dlist_insert_tail(&e->entry, &g_ofi.mr_list);

	desc->handle = e->handle;
	desc->key = fi_mr_key(e->mr);
	desc->addr = (g_ofi.mr_mode & FI_MR_VIRT_ADDR) ?
		     (uint64_t)(uintptr_t)region : 0;
	desc->len = len;

	pthread_mutex_unlock(&g_ofi.mr_lock);

	PDEBUG("OFI MR registered: handle=%lu key=0x%lx addr=0x%lx len=%zu%s",
	       (unsigned long)desc->handle, (unsigned long)desc->key,
	       (unsigned long)desc->addr, len, copy ? " (copied)" : "");

	return EN_DEFW_RC_OK;
}

defw_rc_t defw_transport_ofi_mr_reg(const void *buf, size_t len,
				    defw_rma_desc_t *desc)
{
	return ofi_mr_reg_common(buf, len, false, desc);
}

defw_rc_t defw_transport_ofi_mr_reg_copy(const void *buf, size_t len,
					 defw_rma_desc_t *desc)
{
	return ofi_mr_reg_common(buf, len, true, desc);
}

defw_rc_t defw_transport_ofi_mr_release(uint64_t handle)
{
	struct ofi_mr_entry *e, *found = NULL;
	struct dlist_entry *tmp;

	if (!handle || !g_ofi.mr_list.next)
		return EN_DEFW_RC_BAD_PARAM;

	pthread_mutex_lock(&g_ofi.mr_lock);
	dlist_foreach_container_safe(&g_ofi.mr_list, struct ofi_mr_entry,
				     e, entry, tmp) {
		if (e->handle == handle) {
			dlist_remove(&e->entry);
			found = e;
			break;
		}
	}
	pthread_mutex_unlock(&g_ofi.mr_lock);

	if (!found) {
		PERROR("OFI MR release: unknown handle %lu",
		       (unsigned long)handle);
		return EN_DEFW_RC_FAIL;
	}

	fi_close(&found->mr->fid);
	if (found->owned)
		free(found->buf);
	PDEBUG("OFI MR released: handle=%lu", (unsigned long)handle);
	free(found);

	return EN_DEFW_RC_OK;
}

/* Drop every registration still outstanding. Called at teardown so a peer that
 * died before acknowledging a transfer cannot leave a region registered.
 */
static void ofi_mr_release_all(void)
{
	struct ofi_mr_entry *e;
	struct dlist_entry *tmp;
	int n = 0;

	if (!g_ofi.mr_list.next)
		return;

	pthread_mutex_lock(&g_ofi.mr_lock);
	dlist_foreach_container_safe(&g_ofi.mr_list, struct ofi_mr_entry,
				     e, entry, tmp) {
		dlist_remove(&e->entry);
		fi_close(&e->mr->fid);
		if (e->owned)
			free(e->buf);
		free(e);
		n++;
	}
	pthread_mutex_unlock(&g_ofi.mr_lock);

	if (n)
		PDEBUG("OFI released %d outstanding memory region(s)", n);
}

bool defw_transport_ofi_rma_capable(void)
{
	return g_ofi.up && g_ofi.rma_capable;
}

/*
 * Prove the RMA path works before anything depends on it: register a buffer,
 * fi_read it back out of ourselves through the fabric, and compare. This
 * exercises exactly what a peer will do, so a provider whose registration or
 * addressing rules we got wrong fails here with a clear message instead of
 * corrupting a payload later.
 *
 * Runs during init, before the progress thread starts and before any peer is
 * known, so nothing else is touching the endpoint or the transmit CQ.
 */
static void ofi_rma_selftest(void)
{
	struct fi_cq_msg_entry entry;
	struct fi_context ctx;
	defw_rma_desc_t desc;
	fi_addr_t self = FI_ADDR_UNSPEC;
	unsigned char *src, *dst;
	size_t len = DEFW_OFI_SELFTEST_LEN;
	size_t i;
	ssize_t ret;

	if (!g_ofi.rma_capable) {
		PMSG("OFI RMA selftest: skipped, provider %s is not RMA capable",
		     g_ofi.info->fabric_attr->prov_name);
		return;
	}

	src = malloc(len);
	dst = malloc(len);
	if (!src || !dst) {
		PERROR("OFI RMA selftest: out of memory");
		goto out;
	}

	for (i = 0; i < len; i++)
		src[i] = (unsigned char)(i & 0xff);
	memset(dst, 0, len);

	/* read from ourselves: our own endpoint name is a perfectly good peer */
	if (fi_av_insert(g_ofi.av, g_ofi.local_addr, 1, &self, 0, NULL) != 1) {
		PERROR("OFI RMA selftest: fi_av_insert(self) failed");
		goto out;
	}

	if (defw_transport_ofi_mr_reg(src, len, &desc) != EN_DEFW_RC_OK)
		goto out_av;

	/* the local destination needs no descriptor: DEFw only claims RMA
	 * capability for providers that do not require FI_MR_LOCAL
	 */
	do {
		ret = fi_read(g_ofi.ep, dst, len, NULL, self, desc.addr,
			      desc.key, &ctx);
		if (ret == -FI_EAGAIN)
			fi_cq_read(g_ofi.tx_cq, &entry, 1);
	} while (ret == -FI_EAGAIN);

	if (ret) {
		PERROR("OFI RMA selftest: fi_read failed: %s",
		       fi_strerror((int)-ret));
		goto out_mr;
	}

	ret = fi_cq_sread(g_ofi.tx_cq, &entry, 1, NULL,
			  DEFW_OFI_SEND_TIMEOUT_MS);
	if (ret == -FI_EAVAIL) {
		struct fi_cq_err_entry err = {0};

		fi_cq_readerr(g_ofi.tx_cq, &err, 0);
		PERROR("OFI RMA selftest: read completion error: %s",
		       fi_cq_strerror(g_ofi.tx_cq, err.prov_errno,
				      err.err_data, NULL, 0));
	} else if (ret != 1) {
		PERROR("OFI RMA selftest: no read completion: %s",
		       fi_strerror((int)-ret));
	} else if (memcmp(src, dst, len)) {
		PERROR("OFI RMA selftest: data mismatch after fi_read");
	} else {
		PMSG("OFI RMA selftest: passed, %zu bytes read over %s (key=0x%lx addr=0x%lx)",
		     len, g_ofi.info->fabric_attr->prov_name,
		     (unsigned long)desc.key, (unsigned long)desc.addr);
	}

out_mr:
	defw_transport_ofi_mr_release(desc.handle);
out_av:
	fi_av_remove(g_ofi.av, &self, 1, 0);
out:
	free(src);
	free(dst);
}

static void ofi_fini(void)
{
	ofi_mr_release_all();

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
	pthread_mutex_destroy(&g_ofi.mr_lock);
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
	pthread_mutex_init(&g_ofi.mr_lock, NULL);
	dlist_init(&g_ofi.mr_list);
	/* handle and key 0 are reserved as "no registration" */
	g_ofi.mr_next_handle = 1;
	g_ofi.mr_next_key = 1;

	hints = fi_allocinfo();
	if (!hints) {
		pthread_mutex_destroy(&g_ofi.send_lock);
		pthread_mutex_destroy(&g_ofi.mr_lock);
		return EN_DEFW_RC_OOM;
	}

	/* Reliable datagram, message semantics. FI_CONTEXT is accepted because
	 * most providers require a per-operation context object.
	 */
	hints->ep_attr->type = FI_EP_RDM;
	hints->mode = FI_CONTEXT;
	hints->domain_attr->threading = FI_THREAD_SAFE;
	if (provider && strlen(provider)) {
		hints->fabric_attr->prov_name = strdup(provider);
		if (!hints->fabric_attr->prov_name) {
			fi_freeinfo(hints);
			pthread_mutex_destroy(&g_ofi.send_lock);
			pthread_mutex_destroy(&g_ofi.mr_lock);
			return EN_DEFW_RC_OOM;
		}
	}

	/* Prefer an RMA-capable endpoint so large payloads can move by fi_read
	 * (phase 3). Some providers (e.g. sm2) do not offer FI_RMA, so fall back
	 * to message-only; attachments then stream over TCP instead.
	 */
	hints->caps = FI_MSG | FI_RMA | FI_READ | FI_REMOTE_READ;
	hints->domain_attr->mr_mode = FI_MR_VIRT_ADDR | FI_MR_ALLOCATED |
				      FI_MR_PROV_KEY;
	ret = fi_getinfo(DEFW_OFI_VERSION, NULL, NULL, 0, hints, &g_ofi.info);
	if (ret == 0) {
		g_ofi.rma_capable = true;
	} else {
		hints->caps = FI_MSG;
		hints->domain_attr->mr_mode = 0;
		ret = fi_getinfo(DEFW_OFI_VERSION, NULL, NULL, 0, hints,
				 &g_ofi.info);
		g_ofi.rma_capable = false;
	}
	fi_freeinfo(hints);
	if (ret) {
		PERROR("fi_getinfo failed: %s", fi_strerror(-ret));
		pthread_mutex_destroy(&g_ofi.send_lock);
		pthread_mutex_destroy(&g_ofi.mr_lock);
		return EN_DEFW_RC_FAIL;
	}

	/* The provider answers with the registration rules it actually needs,
	 * which are usually fewer than we offered (the tcp provider asks for
	 * none at all). Remember them: they decide how a region is named to a
	 * peer, and whether we are able to serve RMA at all.
	 */
	g_ofi.mr_mode = g_ofi.info->domain_attr->mr_mode;

	/* FI_MR_LOCAL means even the local buffer of an fi_read must be
	 * registered and passed as a descriptor. DEFw does not do that yet, so
	 * rather than issue reads the provider will reject, drop back to the
	 * inline payload path and say so.
	 */
	if (g_ofi.rma_capable && (g_ofi.mr_mode & FI_MR_LOCAL)) {
		PMSG("OFI provider %s requires FI_MR_LOCAL; RMA disabled, large payloads stay inline",
		     g_ofi.info->fabric_attr->prov_name);
		g_ofi.rma_capable = false;
	}

	PMSG("OFI provider selected: %s (rma=%s mr_mode=0x%x)",
	     g_ofi.info->fabric_attr->prov_name,
	     g_ofi.rma_capable ? "yes" : "no", g_ofi.mr_mode);

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

	if (getenv(DEFW_OFI_RMA_SELFTEST_ENV))
		ofi_rma_selftest();

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

bool defw_transport_ofi_rma_capable(void)
{
	return false;
}

defw_rc_t defw_transport_ofi_mr_reg(const void *buf, size_t len,
				    defw_rma_desc_t *desc)
{
	(void)buf;
	(void)len;
	(void)desc;
	return EN_DEFW_RC_FAIL;
}

defw_rc_t defw_transport_ofi_mr_reg_copy(const void *buf, size_t len,
					 defw_rma_desc_t *desc)
{
	(void)buf;
	(void)len;
	(void)desc;
	return EN_DEFW_RC_FAIL;
}

defw_rc_t defw_transport_ofi_mr_release(uint64_t handle)
{
	(void)handle;
	return EN_DEFW_RC_FAIL;
}

#endif /* HAVE_LIBFABRIC */
