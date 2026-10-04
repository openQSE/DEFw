/*
 * A task record and its wire form, which the execution answers and the
 * completion event share. See defw2_qpm_wire.h.
 */
#include <string.h>

#include "defw2_qpm_wire.h"

static bool fits(const char *s, size_t max)
{
	return s == NULL || strnlen(s, max) < max;
}

bool defw2_qpm_task_fits(const defw2_qpm_task_t *task)
{
	return fits(task->outcome, DEFW2_STR_MAX) &&
	       fits(task->lifecycle_state, DEFW2_STR_MAX) &&
	       fits(task->cid, DEFW2_STR_MAX) &&
	       fits(task->reason, DEFW2_STR_MAX) &&
	       fits(task->message, DEFW2_STR_MAX) &&
	       fits(task->extra, DEFW2_EAGER_MAX);
}

void defw2_qpm_task_to_wire(const defw2_qpm_task_t *task,
			    defw2_qpm_wire_task_t *w)
{
	uint32_t i;

	w->outcome = task->outcome;
	w->lifecycle_state = task->lifecycle_state;
	w->cid = task->cid;
	w->qtask_id = task->qtask_id;
	w->reservation_id = task->reservation_id;
	w->reason = task->reason;
	w->message = task->message;
	w->completion_ready = task->completion_ready ? 1 : 0;
	w->statevector.dtype = task->statevector.dtype;
	w->statevector.rank = task->statevector.rank;
	for (i = 0; i < task->statevector.rank && i < DEFW2_TENSOR_RANK_MAX;
	     i++)
		w->statevector.shape[i] = task->statevector.shape[i];
	w->statevector.nbytes = task->statevector.nbytes;
	w->statevector.delivered = 0;
	w->extra = task->extra;
}

/*
 * Copy a decoded string into the arena. An absent string stays absent, and
 * a copy that fails fails the whole record.
 */
static bool copy_str(struct defw2_arena *arena, const char *src,
		     const char **dst)
{
	*dst = NULL;
	if (src == NULL)
		return true;
	*dst = defw2_arena_strdup(arena, src);
	return *dst != NULL;
}

defw2_rc_t defw2_qpm_task_from_wire(struct defw2_arena *arena,
				    const defw2_qpm_wire_task_t *w,
				    defw2_qpm_task_t *task)
{
	uint32_t i;

	task->qtask_id = w->qtask_id;
	task->reservation_id = w->reservation_id;
	task->completion_ready = w->completion_ready != 0;
	if (!copy_str(arena, w->outcome, &task->outcome) ||
	    !copy_str(arena, w->lifecycle_state, &task->lifecycle_state) ||
	    !copy_str(arena, w->cid, &task->cid) ||
	    !copy_str(arena, w->reason, &task->reason) ||
	    !copy_str(arena, w->message, &task->message) ||
	    !copy_str(arena, w->extra, &task->extra))
		return DEFW2_ERR_NOMEM;

	/* The decoder refused a rank past the limit, so this stays in
	 * bounds. */
	task->statevector.dtype = w->statevector.dtype;
	task->statevector.rank = w->statevector.rank;
	for (i = 0; i < w->statevector.rank; i++)
		task->statevector.shape[i] = w->statevector.shape[i];
	task->statevector.nbytes = w->statevector.nbytes;
	task->statevector_delivered = false;
	return DEFW2_OK;
}
