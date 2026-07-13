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
 * Phase 1a brings up a single reliable-datagram (FI_EP_RDM) endpoint with an
 * address vector and a completion queue, and makes OFI the active transport.
 * The data path is NOT wired yet: ofi_send() delegates to TCP so the
 * framework stays fully functional while the fabric path is built out. Later
 * phases add the session-info address exchange (fi_av_insert), the tagged
 * send/receive data path (fi_tsend/fi_trecv), and the CQ progress thread.
 *
 * The whole libfabric implementation is compiled only when DEFw is built with
 * libfabric (HAVE_LIBFABRIC, set by the SConstruct when libfabric is found).
 * Otherwise a stub reports that OFI is unavailable and the caller falls back
 * to TCP.
 */

#ifdef HAVE_LIBFABRIC

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_errno.h>

/* Minimum libfabric API version DEFw targets. The container ships 2.3.1. */
#define DEFW_OFI_VERSION	FI_VERSION(1, 20)
#define DEFW_OFI_MAX_ADDRLEN	256

/*
 * OFI transport state. One RDM endpoint per process. Peers are inserted into
 * the address vector and referenced by fi_addr_t (stored in the agent block
 * in a later phase); local_addr holds this process's endpoint name for the
 * session-info exchange that a later phase adds.
 */
typedef struct defw_ofi_state_s {
	struct fi_info    *info;
	struct fid_fabric *fabric;
	struct fid_domain *domain;
	struct fid_ep     *ep;
	struct fid_av     *av;
	struct fid_cq     *cq;
	uint8_t            local_addr[DEFW_OFI_MAX_ADDRLEN];
	size_t             local_addrlen;
	bool               up;
} defw_ofi_state_t;

static defw_ofi_state_t g_ofi;

/*
 * Phase 1a: the OFI data path is not wired yet. Keep sending over TCP so the
 * framework stays functional while the endpoint is validated. A later phase
 * replaces this body with fi_tsend() and a per-peer TCP fallback.
 */
static defw_rc_t ofi_send(defw_agent_blk_t *agent, defw_channel_t ch,
			  char *buf, size_t len, defw_msg_type_t type)
{
	return defw_transport_tcp_ops()->send(agent, ch, buf, len, type);
}

static void ofi_fini(void)
{
	if (g_ofi.ep)
		fi_close(&g_ofi.ep->fid);
	if (g_ofi.cq)
		fi_close(&g_ofi.cq->fid);
	if (g_ofi.av)
		fi_close(&g_ofi.av->fid);
	if (g_ofi.domain)
		fi_close(&g_ofi.domain->fid);
	if (g_ofi.fabric)
		fi_close(&g_ofi.fabric->fid);
	if (g_ofi.info)
		fi_freeinfo(g_ofi.info);
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
	int ret;

	memset(&g_ofi, 0, sizeof(g_ofi));
	memset(&cq_attr, 0, sizeof(cq_attr));
	memset(&av_attr, 0, sizeof(av_attr));

	hints = fi_allocinfo();
	if (!hints)
		return EN_DEFW_RC_OOM;

	/* Reliable datagram, message + tagged capabilities. FI_CONTEXT is
	 * accepted here because most providers require per-operation context;
	 * the tagged data path in a later phase supplies it.
	 */
	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG | FI_TAGGED;
	hints->mode = FI_CONTEXT;
	hints->domain_attr->threading = FI_THREAD_SAFE;
	if (provider && strlen(provider)) {
		hints->fabric_attr->prov_name = strdup(provider);
		if (!hints->fabric_attr->prov_name) {
			fi_freeinfo(hints);
			return EN_DEFW_RC_OOM;
		}
	}

	ret = fi_getinfo(DEFW_OFI_VERSION, NULL, NULL, 0, hints, &g_ofi.info);
	fi_freeinfo(hints);
	if (ret) {
		PERROR("fi_getinfo failed: %s", fi_strerror(-ret));
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

	cq_attr.format = FI_CQ_FORMAT_TAGGED;
	cq_attr.wait_obj = FI_WAIT_UNSPEC;
	cq_attr.size = g_ofi.info->rx_attr->size;
	ret = fi_cq_open(g_ofi.domain, &cq_attr, &g_ofi.cq, NULL);
	if (ret) {
		PERROR("fi_cq_open failed: %s", fi_strerror(-ret));
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

	ret = fi_ep_bind(g_ofi.ep, &g_ofi.cq->fid, FI_TRANSMIT | FI_RECV);
	if (ret) {
		PERROR("fi_ep_bind(cq) failed: %s", fi_strerror(-ret));
		goto err;
	}

	ret = fi_enable(g_ofi.ep);
	if (ret) {
		PERROR("fi_enable failed: %s", fi_strerror(-ret));
		goto err;
	}

	/* Cache our endpoint name for the session-info exchange a later phase
	 * adds. Peers insert this into their address vector to reach us.
	 */
	g_ofi.local_addrlen = sizeof(g_ofi.local_addr);
	ret = fi_getname(&g_ofi.ep->fid, g_ofi.local_addr, &g_ofi.local_addrlen);
	if (ret) {
		PERROR("fi_getname failed: %s", fi_strerror(-ret));
		goto err;
	}

	g_ofi.up = true;
	PMSG("OFI endpoint up: provider=%s addrlen=%zu",
	     g_ofi.info->fabric_attr->prov_name, g_ofi.local_addrlen);

	/* Make OFI the active transport. The data path still delegates to TCP
	 * in this phase (see ofi_send).
	 */
	defw_transport_set_ops(&ofi_ops);

	return EN_DEFW_RC_OK;

err:
	ofi_fini();
	return EN_DEFW_RC_FAIL;
}

#else /* !HAVE_LIBFABRIC */

defw_rc_t defw_transport_ofi_init(const char *provider)
{
	(void)provider;
	PERROR("libfabric support was not built into DEFw");
	return EN_DEFW_RC_FAIL;
}

#endif /* HAVE_LIBFABRIC */
