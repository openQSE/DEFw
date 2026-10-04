/*
 * The QPM's completion event: the task record read_cq answers with, sent
 * to a caller's sink when the task finishes.
 *
 * A statevector is described and never carried. An event is acknowledged
 * as soon as a sink has queued it, which is only cheap while events are
 * small, and a statevector can be gigabytes. The caller that wants it lends
 * read_cq or peek_cq a buffer of the size the description gives, which is
 * the result path the typed API already has.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_qpm_wire.h"
#include "../event/defw2_event_internal.h"

static defw2_rc_t completion_take(struct defw2_arena *arena, const void *wire,
				  const void **payload)
{
	defw2_qpm_task_t *task;
	defw2_rc_t rc;

	task = defw2_arena_alloc(arena, sizeof(*task));
	if (task == NULL)
		return DEFW2_ERR_NOMEM;
	/* The event's arena holds it all, so the task has none of its own,
	 * and defw2_qpm_task_free must never see it. */
	rc = defw2_qpm_task_from_wire(arena, wire, task);
	if (rc != DEFW2_OK)
		return rc;
	*payload = task;
	return DEFW2_OK;
}

static bool copy_str(struct defw2_arena *arena, const char *src,
		     const char **dst, uint64_t *bytes)
{
	*dst = NULL;
	if (src == NULL)
		return true;
	*dst = defw2_arena_strdup(arena, src);
	*bytes += strlen(src) + 1;
	return *dst != NULL;
}

static defw2_rc_t completion_make(struct defw2_arena *arena,
				  const void *payload, void **wire,
				  uint64_t *bytes)
{
	const defw2_qpm_task_t *task = payload;
	defw2_qpm_wire_task_t *w;
	uint64_t n = sizeof(*w);

	if (!defw2_qpm_task_fits(task) ||
	    task->statevector.rank > DEFW2_TENSOR_RANK_MAX)
		return DEFW2_ERR_INVALID;
	w = defw2_arena_alloc(arena, sizeof(*w));
	if (w == NULL)
		return DEFW2_ERR_NOMEM;
	/* The scalars and the description, then copies of the strings, since
	 * the task is the caller's and the event outlives the call. */
	defw2_qpm_task_to_wire(task, w);
	if (!copy_str(arena, task->outcome, &w->outcome, &n) ||
	    !copy_str(arena, task->lifecycle_state, &w->lifecycle_state, &n) ||
	    !copy_str(arena, task->cid, &w->cid, &n) ||
	    !copy_str(arena, task->reason, &w->reason, &n) ||
	    !copy_str(arena, task->message, &w->message, &n) ||
	    !copy_str(arena, task->extra, &w->extra, &n))
		return DEFW2_ERR_NOMEM;
	*wire = w;
	*bytes = n;
	return DEFW2_OK;
}

const struct defw2_event_kind defw2_qpm_completion_kind = {
	.method = {
		DEFW2_API_QPM_EXECUTION, DEFW2_QPM_EVENT_COMPLETION,
		DEFW2_RPC_EVENT_DELIVER, DEFW2_QPM_VERSION,
		hg_proc_defw2_event_in_t, hg_proc_defw2_event_out_t,
	},
	.proc = hg_proc_defw2_qpm_wire_task_t,
	.wire_size = sizeof(defw2_qpm_wire_task_t),
	.take = completion_take,
	.make = completion_make,
};

defw2_rc_t defw2_qpm_event_accept(defw2_event_sink_t *sink)
{
	return defw2_event_sink_accept(sink, &defw2_qpm_completion_kind);
}

defw2_rc_t defw2_qpm_publish_completion(defw2_event_publisher_t *pub,
					const defw2_event_target_t *target,
					const char *type,
					const defw2_qpm_task_t *task,
					const char *traceparent)
{
	return defw2_event_publish(pub, target, &defw2_qpm_completion_kind,
				   type, task, traceparent);
}

const defw2_qpm_task_t *defw2_qpm_event_task(const defw2_event_t *event)
{
	if (event == NULL || event->api == NULL || event->name == NULL ||
	    strcmp(event->api, DEFW2_API_QPM_EXECUTION) != 0 ||
	    strcmp(event->name, DEFW2_QPM_EVENT_COMPLETION) != 0)
		return NULL;
	return event->payload;
}
