/*
 * The service host's internals, shared with the services that bind to it.
 * Not installed.
 */
#ifndef DEFW2_HOST_H
#define DEFW2_HOST_H

#include <abt.h>

#include <defw2/defw2_service.h>

#include "defw2_arena.h"
#include "defw2_internal.h"

/*
 * One call, from the handler that decoded it to whoever answers it: an
 * operations table on the handler's own thread, or a consumer of the queue.
 * It lives on the handler's stack, so nothing is allocated per call and its
 * lifetime is exactly the handler's.
 *
 * A typed call adds an answer. request is then the method's public request
 * structure and response its public answer, which the service fills in.
 * Everything the answer points at comes from arena, and a bulk reply is
 * pushed into the buffer the caller lent, so the handler frees all of it
 * once the reply is on the wire.
 */
struct defw2_call {
	struct defw2_call	*next;
	const char		*api;		/* a literal */
	const char		*method;	/* a literal */
	const void		*request;	/* the decoded request */
	size_t			request_len;
	void			*reply;		/* the handler frees it */
	size_t			reply_len;
	defw2_status_t		status;
	ABT_eventual		done;
	uint64_t		queued_ns;	/* monotonic */
	uint64_t		taken_ns;
	void			*response;	/* a typed call's answer */
	struct defw2_arena	arena;
	void			*bulk;		/* a bulk reply, owned */
	uint64_t		bulk_len;
	uint64_t		result_capacity; /* lent by the caller, 0 for none */
};

/*
 * Free what a typed call accumulated: the answer's storage, any bulk reply
 * and the status message. The call itself is the handler's and is not
 * freed. Safe on a call that never went typed.
 */
void defw2_call_release(struct defw2_call *call);

struct defw2_queue {
	struct defw2_call	*head;
	struct defw2_call	*tail;
	pthread_mutex_t		lock;
	pthread_cond_t		arrived;
	unsigned		depth;		/* 0 means no limit */
	unsigned		length;
	bool			open;
};

struct defw2_service {
	struct defw2_rt		*rt;
	char			*service_id;	/* owned */
	char			*service_type;	/* owned */
	uint16_t		provider_id;
	struct defw2_queue	*queue;		/* NULL until opened */
};

/*
 * Hand a decoded request to the queue and wait for the answer. This is
 * what a typed handler calls in place of its operations table when the
 * service is queued. queue_ns, when it is not NULL, receives how long the
 * call waited before a consumer took it, which is the hand-off cost the
 * design says to measure.
 */
defw2_rc_t defw2_service_dispatch(struct defw2_service *svc, const char *api,
				  const char *method, const void *request,
				  size_t request_len, void **reply,
				  size_t *reply_len, defw2_status_t *status,
				  uint64_t *queue_ns);

#endif /* DEFW2_HOST_H */
