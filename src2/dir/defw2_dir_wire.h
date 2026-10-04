/*
 * The directory wire. Shared inside libdefw2 and not installed.
 *
 * A directory record is the first thing v2 sends that is not a flat
 * structure: it carries three variable-length lists, and one of them is a
 * list of structures. Mercury's generated procs handle the flat parts, and
 * the list procs below are written out because a generated one cannot bound
 * what it allocates.
 *
 * That bound is the point. Every count is checked against a maximum before
 * anything is allocated from it, the way hg_proc_defw2_bytes_t checks a
 * length, so a malicious or corrupt count cannot ask the decoder for an
 * arbitrary allocation. Every string goes through hg_proc_defw2_str_t, which
 * checks its length against what the message carries and its terminator
 * before anything reads it. This is what the design means by no unsafe
 * deserialization on any path.
 *
 * Absent and empty are the same thing in a directory record. An absent field
 * travels as "" and comes back NULL through dir_str_in. The wire could carry
 * the difference now, but a directory field that is legitimately an empty
 * string does not exist -- the store already refuses an empty service_id --
 * so nothing is lost and every caller is spared a NULL check that would only
 * ever fire on a field it did not set.
 */
#ifndef DEFW2_DIR_WIRE_H
#define DEFW2_DIR_WIRE_H

#include <defw2/defw2_dir.h>

#include "defw2_dir_internal.h"
#include "../rpc/defw2_typed.h"
#include "../rpc/defw2_wire.h"

/*
 * How long any one list may be. A record with more than this many aliases,
 * resources, bindings or properties is a mistake rather than a deployment,
 * and refusing it costs nothing a real caller will notice.
 */
#define DEFW2_DIR_LIST_MAX	256

/* How many records one resolve may answer with. */
#define DEFW2_DIR_RESULT_MAX	4096

/* Absent becomes "" going out, and "" becomes absent coming in. */
static HG_INLINE const char *dir_str_out(const char *s)
{
	return s != NULL ? s : "";
}

static HG_INLINE const char *dir_str_in(const char *s)
{
	return (s != NULL && s[0] != '\0') ? s : NULL;
}

/* --- string lists ---------------------------------------------------- */

typedef struct {
	hg_uint32_t	count;
	defw2_str_t	*items;
} defw2_wire_strs_t;

static HG_INLINE hg_return_t hg_proc_defw2_wire_strs_t(hg_proc_t proc,
						       void *arg)
{
	defw2_wire_strs_t *list = (defw2_wire_strs_t *)arg;
	hg_proc_op_t op = hg_proc_get_op(proc);
	hg_return_t ret;
	hg_uint32_t i;

	ret = hg_proc_hg_uint32_t(proc, &list->count);
	if (ret != HG_SUCCESS)
		return ret;
	if (list->count > DEFW2_DIR_LIST_MAX)
		return HG_OVERFLOW;
	if (list->count == 0) {
		if (op != HG_ENCODE)
			list->items = NULL;
		return HG_SUCCESS;
	}
	if (op == HG_DECODE) {
		list->items = (defw2_str_t *)calloc(list->count,
						    sizeof(*list->items));
		if (list->items == NULL)
			return HG_NOMEM;
	}
	/*
	 * A decode that failed part way leaves no array to walk, so HG_FREE
	 * has to tolerate that rather than trust the count.
	 */
	if (list->items == NULL)
		return HG_SUCCESS;
	for (i = 0; i < list->count; i++) {
		ret = hg_proc_defw2_str_t(proc, &list->items[i]);
		if (ret != HG_SUCCESS)
			return ret;
	}
	if (op == HG_FREE) {
		free(list->items);
		list->items = NULL;
	}
	return HG_SUCCESS;
}

/* --- integer lists --------------------------------------------------- */

typedef struct {
	hg_uint32_t	count;
	hg_uint32_t	*items;
} defw2_wire_u32s_t;

static HG_INLINE hg_return_t hg_proc_defw2_wire_u32s_t(hg_proc_t proc,
						       void *arg)
{
	defw2_wire_u32s_t *list = (defw2_wire_u32s_t *)arg;
	hg_proc_op_t op = hg_proc_get_op(proc);
	hg_return_t ret;
	hg_uint32_t i;

	ret = hg_proc_hg_uint32_t(proc, &list->count);
	if (ret != HG_SUCCESS)
		return ret;
	if (list->count > DEFW2_DIR_RESULT_MAX)
		return HG_OVERFLOW;
	if (list->count == 0) {
		if (op != HG_ENCODE)
			list->items = NULL;
		return HG_SUCCESS;
	}
	if (op == HG_DECODE) {
		list->items = (hg_uint32_t *)calloc(list->count,
						    sizeof(*list->items));
		if (list->items == NULL)
			return HG_NOMEM;
	}
	if (list->items == NULL)
		return HG_SUCCESS;
	for (i = 0; i < list->count; i++) {
		ret = hg_proc_hg_uint32_t(proc, &list->items[i]);
		if (ret != HG_SUCCESS)
			return ret;
	}
	if (op == HG_FREE) {
		free(list->items);
		list->items = NULL;
	}
	return HG_SUCCESS;
}

/* --- bindings -------------------------------------------------------- */

MERCURY_GEN_PROC(defw2_wire_binding_t,
	((defw2_str_t)(binding_name))
	((defw2_str_t)(api_id))
	((hg_uint32_t)(api_version))
	((hg_uint16_t)(provider_id)))

typedef struct {
	hg_uint32_t		count;
	defw2_wire_binding_t	*items;
} defw2_wire_bindings_t;

static HG_INLINE hg_return_t hg_proc_defw2_wire_bindings_t(hg_proc_t proc,
							   void *arg)
{
	defw2_wire_bindings_t *list = (defw2_wire_bindings_t *)arg;
	hg_proc_op_t op = hg_proc_get_op(proc);
	hg_return_t ret;
	hg_uint32_t i;

	ret = hg_proc_hg_uint32_t(proc, &list->count);
	if (ret != HG_SUCCESS)
		return ret;
	if (list->count > DEFW2_DIR_LIST_MAX)
		return HG_OVERFLOW;
	if (list->count == 0) {
		if (op != HG_ENCODE)
			list->items = NULL;
		return HG_SUCCESS;
	}
	if (op == HG_DECODE) {
		list->items = (defw2_wire_binding_t *)calloc(
			list->count, sizeof(*list->items));
		if (list->items == NULL)
			return HG_NOMEM;
	}
	if (list->items == NULL)
		return HG_SUCCESS;
	for (i = 0; i < list->count; i++) {
		ret = hg_proc_defw2_wire_binding_t(proc, &list->items[i]);
		if (ret != HG_SUCCESS)
			return ret;
	}
	if (op == HG_FREE) {
		free(list->items);
		list->items = NULL;
	}
	return HG_SUCCESS;
}

/* --- properties ------------------------------------------------------ */

MERCURY_GEN_PROC(defw2_wire_property_t,
	((defw2_str_t)(name))
	((defw2_str_t)(value)))

typedef struct {
	hg_uint32_t		count;
	defw2_wire_property_t	*items;
} defw2_wire_properties_t;

static HG_INLINE hg_return_t hg_proc_defw2_wire_properties_t(hg_proc_t proc,
							     void *arg)
{
	defw2_wire_properties_t *list = (defw2_wire_properties_t *)arg;
	hg_proc_op_t op = hg_proc_get_op(proc);
	hg_return_t ret;
	hg_uint32_t i;

	ret = hg_proc_hg_uint32_t(proc, &list->count);
	if (ret != HG_SUCCESS)
		return ret;
	if (list->count > DEFW2_DIR_LIST_MAX)
		return HG_OVERFLOW;
	if (list->count == 0) {
		if (op != HG_ENCODE)
			list->items = NULL;
		return HG_SUCCESS;
	}
	if (op == HG_DECODE) {
		list->items = (defw2_wire_property_t *)calloc(
			list->count, sizeof(*list->items));
		if (list->items == NULL)
			return HG_NOMEM;
	}
	if (list->items == NULL)
		return HG_SUCCESS;
	for (i = 0; i < list->count; i++) {
		ret = hg_proc_defw2_wire_property_t(proc, &list->items[i]);
		if (ret != HG_SUCCESS)
			return ret;
	}
	if (op == HG_FREE) {
		free(list->items);
		list->items = NULL;
	}
	return HG_SUCCESS;
}

/* --- the record ------------------------------------------------------ */

MERCURY_GEN_PROC(defw2_wire_record_t,
	((defw2_str_t)(service_id))
	((defw2_str_t)(service_type))
	((defw2_str_t)(runtime_id))
	((hg_uint64_t)(generation))
	((hg_uint32_t)(state))
	((defw2_str_t)(address))
	((defw2_str_t)(node_name))
	((defw2_str_t)(hostname))
	((hg_int32_t)(pid))
	((defw2_str_t)(selector_name))
	((defw2_wire_strs_t)(aliases))
	((defw2_wire_strs_t)(resources))
	((defw2_wire_bindings_t)(bindings))
	((defw2_wire_properties_t)(properties))
	((hg_uint64_t)(registered_at_ns))
	((hg_uint64_t)(last_heartbeat_ns))
	((hg_uint64_t)(retention_deadline_ns)))

typedef struct {
	hg_uint32_t		count;
	defw2_wire_record_t	*items;
} defw2_wire_records_t;

static HG_INLINE hg_return_t hg_proc_defw2_wire_records_t(hg_proc_t proc,
							  void *arg)
{
	defw2_wire_records_t *list = (defw2_wire_records_t *)arg;
	hg_proc_op_t op = hg_proc_get_op(proc);
	hg_return_t ret;
	hg_uint32_t i;

	ret = hg_proc_hg_uint32_t(proc, &list->count);
	if (ret != HG_SUCCESS)
		return ret;
	if (list->count > DEFW2_DIR_RESULT_MAX)
		return HG_OVERFLOW;
	if (list->count == 0) {
		if (op != HG_ENCODE)
			list->items = NULL;
		return HG_SUCCESS;
	}
	if (op == HG_DECODE) {
		list->items = (defw2_wire_record_t *)calloc(
			list->count, sizeof(*list->items));
		if (list->items == NULL)
			return HG_NOMEM;
	}
	if (list->items == NULL)
		return HG_SUCCESS;
	for (i = 0; i < list->count; i++) {
		ret = hg_proc_defw2_wire_record_t(proc, &list->items[i]);
		if (ret != HG_SUCCESS)
			return ret;
	}
	if (op == HG_FREE) {
		free(list->items);
		list->items = NULL;
	}
	return HG_SUCCESS;
}

/* --- the query ------------------------------------------------------- */

MERCURY_GEN_PROC(defw2_wire_filter_t,
	((defw2_str_t)(name))
	((defw2_str_t)(value))
	((hg_uint32_t)(match)))

typedef struct {
	hg_uint32_t		count;
	defw2_wire_filter_t	*items;
} defw2_wire_filters_t;

static HG_INLINE hg_return_t hg_proc_defw2_wire_filters_t(hg_proc_t proc,
							  void *arg)
{
	defw2_wire_filters_t *list = (defw2_wire_filters_t *)arg;
	hg_proc_op_t op = hg_proc_get_op(proc);
	hg_return_t ret;
	hg_uint32_t i;

	ret = hg_proc_hg_uint32_t(proc, &list->count);
	if (ret != HG_SUCCESS)
		return ret;
	if (list->count > DEFW2_DIR_LIST_MAX)
		return HG_OVERFLOW;
	if (list->count == 0) {
		if (op != HG_ENCODE)
			list->items = NULL;
		return HG_SUCCESS;
	}
	if (op == HG_DECODE) {
		list->items = (defw2_wire_filter_t *)calloc(
			list->count, sizeof(*list->items));
		if (list->items == NULL)
			return HG_NOMEM;
	}
	if (list->items == NULL)
		return HG_SUCCESS;
	for (i = 0; i < list->count; i++) {
		ret = hg_proc_defw2_wire_filter_t(proc, &list->items[i]);
		if (ret != HG_SUCCESS)
			return ret;
	}
	if (op == HG_FREE) {
		free(list->items);
		list->items = NULL;
	}
	return HG_SUCCESS;
}

MERCURY_GEN_PROC(defw2_wire_query_t,
	((defw2_str_t)(service_id))
	((defw2_str_t)(service_type))
	((defw2_str_t)(selector_name))
	((defw2_str_t)(resource))
	((defw2_str_t)(binding_name))
	((hg_uint32_t)(api_version))
	((defw2_wire_filters_t)(filters))
	((hg_uint32_t)(include_inactive))
	((hg_uint64_t)(limit)))

/* --- the six methods ------------------------------------------------- */

MERCURY_GEN_PROC(defw2_dir_register_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_wire_record_t)(record)))

MERCURY_GEN_PROC(defw2_dir_register_out_t,
	((defw2_wire_status_t)(status))
	((hg_uint64_t)(generation)))

/*
 * heartbeat and deregister take the same three fields, so they share one
 * input type. Keeping the methods separate still matters: they are different
 * events in a log and different spans in a trace.
 */
MERCURY_GEN_PROC(defw2_dir_lease_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_str_t)(service_id))
	((defw2_str_t)(runtime_id))
	((hg_uint64_t)(generation)))

MERCURY_GEN_PROC(defw2_dir_lease_out_t,
	((defw2_wire_status_t)(status)))

MERCURY_GEN_PROC(defw2_dir_resolve_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_wire_query_t)(query)))

/*
 * The selected binding travels beside the records rather than inside them,
 * one index into each record's bindings, because which binding a query chose
 * is a property of the answer and not of the record. A record the query
 * matched without selecting a binding, which is a record that declares none,
 * carries DEFW2_DIR_NO_BINDING.
 *
 * Sending the index rather than letting the client re-derive it keeps the
 * selection rules in one place, on the side that already has them.
 */
#define DEFW2_DIR_NO_BINDING	0xffffffffu

MERCURY_GEN_PROC(defw2_dir_resolve_out_t,
	((defw2_wire_status_t)(status))
	((defw2_wire_records_t)(records))
	((defw2_wire_u32s_t)(selected)))

MERCURY_GEN_PROC(defw2_dir_generation_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_str_t)(service_id)))

MERCURY_GEN_PROC(defw2_dir_generation_out_t,
	((defw2_wire_status_t)(status))
	((hg_uint64_t)(generation)))

#define DEFW2_RPC_DIR_REGISTER		"defw2.qfw.directory.register_service"
#define DEFW2_RPC_DIR_HEARTBEAT		"defw2.qfw.directory.heartbeat"
#define DEFW2_RPC_DIR_DEREGISTER	"defw2.qfw.directory.deregister_service"
#define DEFW2_RPC_DIR_RESOLVE		"defw2.qfw.directory.resolve_services"
#define DEFW2_RPC_DIR_QUERY		"defw2.qfw.directory.query_directory"
#define DEFW2_RPC_DIR_GENERATION	"defw2.qfw.directory.get_generation"

/*
 * Rebuild a record and its selected binding into an arena, for a resolve's
 * answer and for an event alike. Everything is copied, so the entry outlives
 * the decoded buffers it came from. False when an allocation failed, and the
 * arena then holds a partial entry that goes when it does.
 */
bool defw2_dir_entry_from_wire(defw2_dir_arena_t *arena,
			       const defw2_wire_record_t *wire,
			       uint32_t selected, defw2_dir_entry_t *entry);

/* --- events ---------------------------------------------------------- */

/*
 * subscribe and unsubscribe postdate the six methods above and take the
 * typed path every API since has taken, through defw2_typed.h. They are the
 * directory's own, so they speak the directory's version.
 */

/* subscribe: the target's three fields, flattened, and what to hear of. */
MERCURY_GEN_PROC(defw2_dir_subscribe_in_t,
	((defw2_hdr_t)(hdr))
	((defw2_str_t)(address))
	((hg_uint16_t)(provider_id))
	((defw2_str_t)(tag))
	((defw2_str_t)(service_id))
	((defw2_str_t)(service_type))
	((hg_uint32_t)(changes)))

MERCURY_GEN_PROC(defw2_dir_subscribe_out_t,
	((defw2_wire_status_t)(status))
	((hg_uint64_t)(subscription_id)))

MERCURY_GEN_PROC(defw2_dir_unsubscribe_in_t,
	((defw2_hdr_t)(hdr))
	((hg_uint64_t)(subscription_id)))

/* A directory event's payload: which way the record went, why, and it. */
MERCURY_GEN_PROC(defw2_dir_wire_change_t,
	((hg_uint8_t)(connected))
	((defw2_str_t)(reason))
	((defw2_wire_record_t)(record)))

#define DEFW2_DIR_METHOD(var, name, in_t, out_t)			\
	static const struct defw2_method var = {			\
		DEFW2_API_DIR, #name, "defw2." DEFW2_API_DIR "." #name,	\
		DEFW2_API_VERSION, hg_proc_##in_t, hg_proc_##out_t,	\
	}

DEFW2_DIR_METHOD(defw2_dir_m_subscribe, subscribe,
		 defw2_dir_subscribe_in_t, defw2_dir_subscribe_out_t);
DEFW2_DIR_METHOD(defw2_dir_m_unsubscribe, unsubscribe,
		 defw2_dir_unsubscribe_in_t, defw2_dir_lease_out_t);

/* The event's kind, which carries a defw2_dir_wire_change_t. */
struct defw2_event_kind;
extern const struct defw2_event_kind defw2_dir_change_kind;

#endif /* DEFW2_DIR_WIRE_H */
