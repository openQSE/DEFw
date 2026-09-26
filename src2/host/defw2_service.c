/*
 * The service host.
 *
 * A service is one Margo provider on the runtime's Margo instance. The host
 * owns the identity and the lifecycle; each API binds its own methods, which
 * is what keeps the host free of any knowledge of a particular API.
 *
 * The host also owns the call queue, which is how a service written in a
 * language that must not run on a Margo thread is served. See the queue
 * section in defw2_service.h.
 *
 * Directory registration and heartbeats belong here too and arrive in phase
 * 1. Until then a caller reaches a service by the address it prints.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "defw2_host.h"
#include "defw2_trace.h"

static void queue_destroy(struct defw2_service *svc);

defw2_rc_t defw2_service_create(defw2_rt_t *rt, const char *service_id,
				const char *service_type, uint16_t provider_id,
				defw2_service_t **out)
{
	struct defw2_service *svc;

	if (rt == NULL || service_id == NULL || service_type == NULL ||
	    out == NULL)
		return DEFW2_ERR_INVALID;

	/*
	 * A client-mode Margo instance does not listen, so a service on one
	 * would register methods that nobody can reach. Say so here rather
	 * than let the first call time out.
	 */
	if (rt->role != DEFW2_ROLE_SERVER) {
		defw2_log(rt, DEFW2_LOG_ERROR,
			  "service %s needs a server runtime, set DEFW_AGENT_TYPE",
			  service_id);
		return DEFW2_ERR_CONFIG;
	}

	svc = calloc(1, sizeof(*svc));
	if (svc == NULL)
		return DEFW2_ERR_NOMEM;
	svc->rt = rt;
	svc->provider_id = provider_id;
	svc->service_id = strdup(service_id);
	svc->service_type = strdup(service_type);
	if (svc->service_id == NULL || svc->service_type == NULL) {
		defw2_service_destroy(svc);
		return DEFW2_ERR_NOMEM;
	}

	defw2_log(rt, DEFW2_LOG_MESSAGE, "service %s of type %s, provider %u at %s",
		  svc->service_id, svc->service_type, provider_id, rt->address);
	*out = svc;
	return DEFW2_OK;
}

void defw2_service_destroy(defw2_service_t *svc)
{
	if (svc == NULL)
		return;
	/*
	 * The registered methods and their operations tables belong to the
	 * Margo instance, which releases them when the runtime finalizes.
	 * That is what makes this safe while an RPC is still in flight.
	 */
	queue_destroy(svc);
	free(svc->service_id);
	free(svc->service_type);
	free(svc);
}

void defw2_service_run(defw2_service_t *svc)
{
	if (svc == NULL)
		return;
	/* Nothing to wait for if the runtime has already been stopped. */
	if (__atomic_load_n(&svc->rt->stopping, __ATOMIC_ACQUIRE) != 0)
		return;
	margo_wait_for_finalize(svc->rt->mid);
}

void defw2_service_shutdown(defw2_service_t *svc)
{
	if (svc == NULL)
		return;
	defw2_log(svc->rt, DEFW2_LOG_MESSAGE, "service %s stopping",
		  svc->service_id);
	/* Consumers and parked handlers unwind first, so margo_finalize is
	 * not left waiting on a handler waiting on a queue nobody is
	 * serving. */
	defw2_service_queue_close(svc);
	defw2_runtime_stop(svc->rt);
}

/*
 * The call queue.
 *
 * Two kinds of thread meet here and neither may assume anything about the
 * other. The producer is a Margo handler, an Argobots ULT, which must not
 * block its execution stream, so it parks on an eventual. The consumer is
 * an ordinary thread that Argobots knows nothing about, so it waits on a
 * condition variable. Setting an eventual from such a thread is allowed,
 * which is what makes the hand-off work.
 */

static void deadline_after(struct timespec *when, uint32_t timeout_ms)
{
	clock_gettime(CLOCK_REALTIME, when);
	when->tv_sec += timeout_ms / 1000;
	when->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (when->tv_nsec >= 1000000000L) {
		when->tv_sec++;
		when->tv_nsec -= 1000000000L;
	}
}

defw2_rc_t defw2_service_queue_open(defw2_service_t *svc, unsigned depth)
{
	struct defw2_queue *queue;

	if (svc == NULL)
		return DEFW2_ERR_INVALID;
	if (svc->queue != NULL)
		return DEFW2_ERR_INVALID;

	queue = calloc(1, sizeof(*queue));
	if (queue == NULL)
		return DEFW2_ERR_NOMEM;
	pthread_mutex_init(&queue->lock, NULL);
	pthread_cond_init(&queue->arrived, NULL);
	queue->depth = depth;
	queue->open = true;
	svc->queue = queue;

	defw2_log(svc->rt, DEFW2_LOG_MESSAGE,
		  "service %s serves from a queue, depth %u", svc->service_id,
		  depth);
	return DEFW2_OK;
}

bool defw2_service_queued(const defw2_service_t *svc)
{
	return svc != NULL && svc->queue != NULL && svc->queue->open;
}

void defw2_service_queue_close(defw2_service_t *svc)
{
	struct defw2_call *call;

	if (svc == NULL || svc->queue == NULL)
		return;

	pthread_mutex_lock(&svc->queue->lock);
	svc->queue->open = false;
	/*
	 * Everything still waiting is failed here, so its handler wakes and
	 * answers its caller. A call a consumer has already taken is that
	 * consumer's to answer.
	 */
	for (call = svc->queue->head; call != NULL; ) {
		struct defw2_call *next = call->next;

		call->status.code = DEFW2_ERR_CANCELLED;
		call->status.category = DEFW2_CAT_CANCELLED;
		ABT_eventual_set(call->done, NULL, 0);
		call = next;
	}
	svc->queue->head = NULL;
	svc->queue->tail = NULL;
	svc->queue->length = 0;
	pthread_cond_broadcast(&svc->queue->arrived);
	pthread_mutex_unlock(&svc->queue->lock);
}

static void queue_destroy(struct defw2_service *svc)
{
	if (svc->queue == NULL)
		return;
	defw2_service_queue_close(svc);
	pthread_cond_destroy(&svc->queue->arrived);
	pthread_mutex_destroy(&svc->queue->lock);
	free(svc->queue);
	svc->queue = NULL;
}

defw2_rc_t defw2_service_dispatch(struct defw2_service *svc, const char *api,
				  const char *method, const void *request,
				  size_t request_len, void **reply,
				  size_t *reply_len, defw2_status_t *status,
				  uint64_t *queue_ns)
{
	struct defw2_queue *queue = svc->queue;
	struct defw2_call call;

	memset(&call, 0, sizeof(call));
	call.api = api;
	call.method = method;
	call.request = request;
	call.request_len = request_len;
	call.queued_ns = defw2_mono_ns();
	/* Created before the call is visible, so a consumer that takes it
	 * at once still has something to set. */
	if (ABT_eventual_create(0, &call.done) != ABT_SUCCESS)
		return DEFW2_ERR_INTERNAL;

	pthread_mutex_lock(&queue->lock);
	if (!queue->open) {
		pthread_mutex_unlock(&queue->lock);
		ABT_eventual_free(&call.done);
		return DEFW2_ERR_NOT_FOUND;
	}
	if (queue->depth != 0 && queue->length >= queue->depth) {
		pthread_mutex_unlock(&queue->lock);
		ABT_eventual_free(&call.done);
		return DEFW2_ERR_TIMEOUT;
	}
	if (queue->tail == NULL)
		queue->head = &call;
	else
		queue->tail->next = &call;
	queue->tail = &call;
	queue->length++;
	pthread_cond_signal(&queue->arrived);
	pthread_mutex_unlock(&queue->lock);

	/* Parks this ULT and leaves the execution stream to other calls. */
	ABT_eventual_wait(call.done, NULL);
	ABT_eventual_free(&call.done);

	if (queue_ns != NULL)
		*queue_ns = call.taken_ns > call.queued_ns ?
				    call.taken_ns - call.queued_ns : 0;
	*reply = call.reply;
	*reply_len = call.reply_len;
	if (status != NULL)
		*status = call.status;
	else
		free(call.status.message);
	return DEFW2_OK;
}

defw2_rc_t defw2_service_next_call(defw2_service_t *svc, uint32_t timeout_ms,
				   defw2_call_t **out)
{
	struct defw2_queue *queue;
	struct defw2_call *call;
	struct timespec deadline;
	defw2_rc_t rc = DEFW2_OK;

	if (svc == NULL || svc->queue == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	queue = svc->queue;
	*out = NULL;
	deadline_after(&deadline, timeout_ms);

	pthread_mutex_lock(&queue->lock);
	while (queue->open && queue->head == NULL) {
		if (pthread_cond_timedwait(&queue->arrived, &queue->lock,
					   &deadline) == ETIMEDOUT)
			break;
	}
	call = queue->head;
	if (call != NULL) {
		queue->head = call->next;
		if (queue->head == NULL)
			queue->tail = NULL;
		queue->length--;
		call->next = NULL;
		call->taken_ns = defw2_mono_ns();
		*out = call;
	} else {
		/* A closed queue is how a serving loop learns to stop. */
		rc = queue->open ? DEFW2_ERR_TIMEOUT : DEFW2_ERR_NOT_FOUND;
	}
	pthread_mutex_unlock(&queue->lock);
	return rc;
}

const char *defw2_call_api(const defw2_call_t *call)
{
	return call ? call->api : NULL;
}

const char *defw2_call_method(const defw2_call_t *call)
{
	return call ? call->method : NULL;
}

const void *defw2_call_request(const defw2_call_t *call, size_t *len)
{
	if (call == NULL) {
		if (len != NULL)
			*len = 0;
		return NULL;
	}
	if (len != NULL)
		*len = call->request_len;
	return call->request;
}

/*
 * Answering wakes the parked handler, which then owns the call again, so
 * setting the eventual is the last thing either of these functions does.
 */
defw2_rc_t defw2_service_respond(defw2_call_t *call, const void *reply,
				 size_t len)
{
	if (call == NULL || (reply == NULL && len > 0))
		return DEFW2_ERR_INVALID;

	if (len > 0) {
		call->reply = malloc(len);
		if (call->reply == NULL) {
			defw2_service_fail(call, DEFW2_ERR_NOMEM,
					   DEFW2_CAT_PROVIDER_FAILURE,
					   "no memory for the reply");
			return DEFW2_ERR_NOMEM;
		}
		memcpy(call->reply, reply, len);
		call->reply_len = len;
	}
	call->status.code = DEFW2_OK;
	call->status.category = DEFW2_CAT_OK;
	ABT_eventual_set(call->done, NULL, 0);
	return DEFW2_OK;
}

defw2_rc_t defw2_service_fail(defw2_call_t *call, defw2_rc_t code,
			      uint32_t category, const char *message)
{
	if (call == NULL)
		return DEFW2_ERR_INVALID;
	call->status.code = code;
	call->status.category = category;
	call->status.message = message ? strdup(message) : NULL;
	ABT_eventual_set(call->done, NULL, 0);
	return DEFW2_OK;
}

const char *defw2_service_id(const defw2_service_t *svc)
{
	return svc ? svc->service_id : NULL;
}

const char *defw2_service_type(const defw2_service_t *svc)
{
	return svc ? svc->service_type : NULL;
}

const char *defw2_service_address(const defw2_service_t *svc)
{
	return svc ? svc->rt->address : NULL;
}

uint16_t defw2_service_provider_id(const defw2_service_t *svc)
{
	return svc ? svc->provider_id : 0;
}

defw2_rt_t *defw2_service_runtime(const defw2_service_t *svc)
{
	return svc ? svc->rt : NULL;
}
