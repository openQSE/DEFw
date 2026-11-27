/*
 * What sinks and publishers share: the request's proc, and an event's
 * release. See defw2_event_internal.h.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_event_internal.h"

static const struct defw2_event_kind *accepted_kind(const defw2_event_in_t *in)
{
	unsigned i;

	if (in->api == NULL || in->name == NULL)
		return NULL;
	for (i = 0; i < in->naccepted && i < DEFW2_EVENT_KINDS_MAX; i++) {
		const struct defw2_event_kind *kind = in->accepted[i];

		if (strcmp(kind->method.api, in->api) == 0 &&
		    strcmp(kind->method.name, in->name) == 0)
			return kind;
	}
	return NULL;
}

/*
 * The envelope, then the payload by its kind's own proc. A decode finds the
 * kind by the names the envelope carries, among those the sink accepts, and
 * refuses any other before it allocates anything for the payload. A free
 * walks whatever a decode left, which may stop part way through either.
 */
hg_return_t hg_proc_defw2_event_in_t(hg_proc_t proc, void *arg)
{
	defw2_event_in_t *in = arg;
	hg_return_t ret;

	ret = hg_proc_defw2_hdr_t(proc, &in->hdr);
	if (ret == HG_SUCCESS)
		ret = hg_proc_defw2_str_t(proc, &in->tag);
	if (ret == HG_SUCCESS)
		ret = hg_proc_defw2_str_t(proc, &in->type);
	if (ret == HG_SUCCESS)
		ret = hg_proc_hg_uint64_t(proc, &in->seq);
	if (ret == HG_SUCCESS)
		ret = hg_proc_defw2_str_t(proc, &in->api);
	if (ret == HG_SUCCESS)
		ret = hg_proc_defw2_str_t(proc, &in->name);
	if (ret != HG_SUCCESS)
		return ret;

	switch (hg_proc_get_op(proc)) {
	case HG_ENCODE:
		if (in->kind == NULL || in->payload == NULL)
			return HG_INVALID_ARG;
		return in->kind->proc(proc, in->payload);
	case HG_DECODE:
		in->payload = NULL;
		in->kind = accepted_kind(in);
		if (in->kind == NULL) {
			in->unknown = true;
			return HG_NOENTRY;
		}
		in->payload = calloc(1, in->kind->wire_size);
		if (in->payload == NULL)
			return HG_NOMEM;
		return in->kind->proc(proc, in->payload);
	case HG_FREE:
		if (in->kind != NULL && in->payload != NULL)
			in->kind->proc(proc, in->payload);
		free(in->payload);
		in->payload = NULL;
		return HG_SUCCESS;
	}
	return HG_SUCCESS;
}

void defw2_event_free(defw2_event_t *event)
{
	struct defw2_arena *arena;

	if (event == NULL)
		return;
	arena = event->arena;
	if (arena != NULL) {
		defw2_arena_free(arena);
		free(arena);
	}
	memset(event, 0, sizeof(*event));
}

bool defw2_event_target_gone(defw2_rc_t rc)
{
	switch (rc) {
	case DEFW2_ERR_TRANSPORT:
	case DEFW2_ERR_TIMEOUT:
	case DEFW2_ERR_NOT_FOUND:
	case DEFW2_ERR_VERSION:
		return true;
	default:
		return false;
	}
}
