/*
 * The qfw.directory client stubs.
 *
 * Same shape as the echo stubs: look the RPC up, fill the header, forward
 * with a timeout, copy what came back out of Mercury's buffers, free them.
 * The one thing that differs is resolve, which has to rebuild a record graph
 * into the caller's arena rather than copy a flat payload.
 *
 * A defw2_dir_t owns its binding, so the address bootstrap happens once, at
 * open, and every later call reuses the resolved address.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_dir_internal.h"
#include "defw2_dir_wire.h"

#include "../telemetry/defw2_trace.h"

struct defw2_dir {
	struct defw2_rt		*rt;
	defw2_binding_t		*binding;
};

/*
 * Close a span the way the echo stubs do: describe it only when something is
 * recording, then end it. The status is the service's own outcome and is
 * what a trace should show, so it wins over the transport code when there is
 * one.
 */
/*
 * The span of one call. handle says what crossed the wire: Mercury counts
 * the request it encoded and the answer it received, which is zero for a
 * call that never got one.
 */
static void trace_finish(struct defw2_rt *rt, struct defw2_trace *trace,
			 hg_handle_t handle, const char *method,
			 defw2_rc_t rc, const defw2_status_t *status)
{
	if (trace->recording) {
		trace->span.api = DEFW2_API_DIR;
		trace->span.method = method;
		trace->span.tier = DEFW2_TIER_TYPED;
		trace->span.request_bytes = HG_Get_input_payload_size(handle);
		trace->span.response_bytes =
			HG_Get_output_payload_size(handle);
		if (rc == DEFW2_OK && status != NULL) {
			trace->span.code = status->code;
			trace->span.category = status->category;
		} else {
			trace->span.code = rc;
			trace->span.category = DEFW2_CAT_TRANSPORT;
		}
	}
	defw2_trace_end(rt, trace);
}

static double timeout_of(const defw2_call_opts_t *opts)
{
	return opts != NULL && opts->timeout_ms > 0 ?
		(double)opts->timeout_ms : 0.0;
}

static const char *trace_of(const defw2_call_opts_t *opts)
{
	return opts != NULL ? opts->traceparent : NULL;
}

defw2_rc_t defw2_dir_open(defw2_rt_t *rt, const char *address,
			  defw2_dir_t **out)
{
	defw2_dir_t *dir;
	defw2_rc_t rc;

	if (rt == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	/*
	 * A NULL address is a process that was never told where the directory
	 * is, which is a configuration answer rather than a transport one:
	 * DEFW2_DIRSVC was unset and the v1 parent variables composed nothing.
	 */
	if (address == NULL || address[0] == '\0')
		return DEFW2_ERR_CONFIG;

	dir = calloc(1, sizeof(*dir));
	if (dir == NULL)
		return DEFW2_ERR_NOMEM;
	dir->rt = rt;
	rc = defw2_binding_create(rt, address, DEFW2_PROVIDER_DIR,
				  &dir->binding);
	if (rc != DEFW2_OK) {
		free(dir);
		return rc;
	}
	*out = dir;
	return DEFW2_OK;
}

struct defw2_rt *defw2_dir_runtime(const defw2_dir_t *dir)
{
	return dir != NULL ? dir->rt : NULL;
}

void defw2_dir_close(defw2_dir_t *dir)
{
	if (dir == NULL)
		return;
	defw2_binding_free(dir->binding);
	free(dir);
}

/* --- register -------------------------------------------------------- */

/*
 * Fill the wire record from the public one. The lists point at arrays this
 * allocates; wire_record_free releases them, and every string is borrowed
 * from the caller's record.
 */
static bool wire_from_record(const defw2_dir_record_t *record,
			     defw2_wire_record_t *wire)
{
	size_t i;

	memset(wire, 0, sizeof(*wire));
	wire->service_id = (defw2_str_t)dir_str_out(record->service_id);
	wire->service_type = (defw2_str_t)dir_str_out(record->service_type);
	wire->runtime_id = (defw2_str_t)dir_str_out(record->runtime_id);
	wire->address = (defw2_str_t)dir_str_out(record->address);
	wire->node_name = (defw2_str_t)dir_str_out(record->endpoint.node_name);
	wire->hostname = (defw2_str_t)dir_str_out(record->endpoint.hostname);
	wire->pid = record->endpoint.pid;
	wire->selector_name = (defw2_str_t)dir_str_out(record->selector.name);
	/*
	 * The generation, the state and the timestamps are the directory's to
	 * decide, so they go out zeroed however the caller left them. Sending
	 * a value the server ignores would only invite someone to believe it
	 * matters.
	 */

	if (record->selector.alias_count > 0) {
		wire->aliases.items = calloc(record->selector.alias_count,
					     sizeof(*wire->aliases.items));
		if (wire->aliases.items == NULL)
			return false;
		wire->aliases.count = (hg_uint32_t)record->selector.alias_count;
		for (i = 0; i < record->selector.alias_count; i++)
			wire->aliases.items[i] = (defw2_str_t)dir_str_out(
				record->selector.aliases[i]);
	}
	if (record->selector.resource_count > 0) {
		wire->resources.items = calloc(
			record->selector.resource_count,
			sizeof(*wire->resources.items));
		if (wire->resources.items == NULL)
			return false;
		wire->resources.count =
			(hg_uint32_t)record->selector.resource_count;
		for (i = 0; i < record->selector.resource_count; i++)
			wire->resources.items[i] = (defw2_str_t)dir_str_out(
				record->selector.resources[i]);
	}
	if (record->binding_count > 0) {
		wire->bindings.items = calloc(record->binding_count,
					      sizeof(*wire->bindings.items));
		if (wire->bindings.items == NULL)
			return false;
		wire->bindings.count = (hg_uint32_t)record->binding_count;
		for (i = 0; i < record->binding_count; i++) {
			defw2_wire_binding_t *b = &wire->bindings.items[i];

			b->binding_name = (defw2_str_t)dir_str_out(
				record->bindings[i].binding_name);
			b->api_id = (defw2_str_t)dir_str_out(
				record->bindings[i].api_id);
			b->api_version = record->bindings[i].api_version;
			b->provider_id = record->bindings[i].provider_id;
		}
	}
	if (record->property_count > 0) {
		wire->properties.items = calloc(
			record->property_count,
			sizeof(*wire->properties.items));
		if (wire->properties.items == NULL)
			return false;
		wire->properties.count = (hg_uint32_t)record->property_count;
		for (i = 0; i < record->property_count; i++) {
			defw2_wire_property_t *p = &wire->properties.items[i];

			p->name = (defw2_str_t)dir_str_out(
				record->properties[i].name);
			p->value = (defw2_str_t)dir_str_out(
				record->properties[i].value);
		}
	}
	return true;
}

static void wire_record_free(defw2_wire_record_t *wire)
{
	free(wire->aliases.items);
	free(wire->resources.items);
	free(wire->bindings.items);
	free(wire->properties.items);
	memset(wire, 0, sizeof(*wire));
}

defw2_rc_t defw2_dir_register(defw2_dir_t *dir,
			      const defw2_dir_record_t *record,
			      const defw2_call_opts_t *opts,
			      uint64_t *generation, defw2_status_t *status)
{
	defw2_dir_register_in_t in;
	defw2_dir_register_out_t out;
	struct defw2_trace trace;
	hg_handle_t handle = HG_HANDLE_NULL;
	hg_return_t hret;
	defw2_rc_t rc = DEFW2_OK;
	hg_id_t id;

	if (dir == NULL || record == NULL)
		return DEFW2_ERR_INVALID;

	memset(&in, 0, sizeof(in));
	if (!wire_from_record(record, &in.record)) {
		wire_record_free(&in.record);
		return DEFW2_ERR_NOMEM;
	}

	id = defw2_rpc_lookup(dir->rt, DEFW2_RPC_DIR_REGISTER,
			      hg_proc_defw2_dir_register_in_t,
			      hg_proc_defw2_dir_register_out_t);
	hret = margo_create(dir->rt->mid, dir->binding->addr, id, &handle);
	if (hret != HG_SUCCESS) {
		wire_record_free(&in.record);
		return defw2_rc_from_hg(hret, NULL);
	}

	defw2_trace_begin(dir->rt, &trace, DEFW2_SPAN_CLIENT, trace_of(opts));
	defw2_hdr_fill(dir->rt, &in.hdr, DEFW2_API_VERSION,
		       trace.recording ? trace.traceparent : trace_of(opts));

	hret = margo_provider_forward_timed(dir->binding->provider_id, handle,
					    &in, timeout_of(opts));
	if (hret != HG_SUCCESS) {
		rc = defw2_rc_from_hg(hret, NULL);
		goto out;
	}
	/* Zeroed, so the free after a decode that fails part way only
	 * touches the fields it actually decoded. defw2_free_partial
	 * says why that free is not margo_free_output. */
	memset(&out, 0, sizeof(out));
	hret = margo_get_output(handle, &out);
	if (hret != HG_SUCCESS) {
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_dir_register_out_t, &out);
		rc = defw2_rc_from_hg(hret, NULL);
		goto out;
	}
	defw2_status_from_wire(status, &out.status);
	if (generation != NULL)
		*generation = out.generation;
	margo_free_output(handle, &out);

out:
	trace_finish(dir->rt, &trace, handle, "register_service", rc,
		     status);
	wire_record_free(&in.record);
	margo_destroy(handle);
	return rc;
}

/* --- heartbeat and deregister ---------------------------------------- */

static defw2_rc_t lease_call(defw2_dir_t *dir, const char *rpc_name,
			     const char *method, const char *service_id,
			     const char *runtime_id, uint64_t generation,
			     const defw2_call_opts_t *opts,
			     defw2_status_t *status)
{
	defw2_dir_lease_in_t in;
	defw2_dir_lease_out_t out;
	struct defw2_trace trace;
	hg_handle_t handle = HG_HANDLE_NULL;
	hg_return_t hret;
	defw2_rc_t rc = DEFW2_OK;
	hg_id_t id;

	if (dir == NULL || service_id == NULL)
		return DEFW2_ERR_INVALID;

	memset(&in, 0, sizeof(in));
	in.service_id = (defw2_str_t)dir_str_out(service_id);
	in.runtime_id = (defw2_str_t)dir_str_out(runtime_id);
	in.generation = generation;

	id = defw2_rpc_lookup(dir->rt, rpc_name,
			      hg_proc_defw2_dir_lease_in_t,
			      hg_proc_defw2_dir_lease_out_t);
	hret = margo_create(dir->rt->mid, dir->binding->addr, id, &handle);
	if (hret != HG_SUCCESS)
		return defw2_rc_from_hg(hret, NULL);

	defw2_trace_begin(dir->rt, &trace, DEFW2_SPAN_CLIENT, trace_of(opts));
	defw2_hdr_fill(dir->rt, &in.hdr, DEFW2_API_VERSION,
		       trace.recording ? trace.traceparent : trace_of(opts));

	hret = margo_provider_forward_timed(dir->binding->provider_id, handle,
					    &in, timeout_of(opts));
	if (hret != HG_SUCCESS) {
		rc = defw2_rc_from_hg(hret, NULL);
		goto out;
	}
	/* Zeroed, so the free after a decode that fails part way only
	 * touches the fields it actually decoded. defw2_free_partial
	 * says why that free is not margo_free_output. */
	memset(&out, 0, sizeof(out));
	hret = margo_get_output(handle, &out);
	if (hret != HG_SUCCESS) {
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_dir_lease_out_t, &out);
		rc = defw2_rc_from_hg(hret, NULL);
		goto out;
	}
	defw2_status_from_wire(status, &out.status);
	margo_free_output(handle, &out);

out:
	trace_finish(dir->rt, &trace, handle, method, rc, status);
	margo_destroy(handle);
	return rc;
}

defw2_rc_t defw2_dir_heartbeat(defw2_dir_t *dir, const char *service_id,
			       const char *runtime_id, uint64_t generation,
			       const defw2_call_opts_t *opts,
			       defw2_status_t *status)
{
	return lease_call(dir, DEFW2_RPC_DIR_HEARTBEAT, "heartbeat",
			  service_id, runtime_id, generation, opts, status);
}

defw2_rc_t defw2_dir_deregister(defw2_dir_t *dir, const char *service_id,
				const char *runtime_id, uint64_t generation,
				const defw2_call_opts_t *opts,
				defw2_status_t *status)
{
	return lease_call(dir, DEFW2_RPC_DIR_DEREGISTER, "deregister_service",
			  service_id, runtime_id, generation, opts, status);
}

/* --- resolve and query ----------------------------------------------- */

/*
 * Rebuild one record into the caller's arena. Everything is copied out of
 * Mercury's decoded buffers, because those go back at margo_free_output and
 * the result has to outlive the call. A sink's event is rebuilt the same way.
 */
bool defw2_dir_entry_from_wire(defw2_dir_arena_t *arena,
			       const defw2_wire_record_t *wire,
			       uint32_t selected, defw2_dir_entry_t *entry)
{
	defw2_dir_record_t *out = &entry->record;
	hg_uint32_t i;

	memset(entry, 0, sizeof(*entry));
	/*
	 * dir_str_in maps "" back to absent, and an absent field needs no
	 * copy, so a NULL here is the field genuinely being unset rather than
	 * an allocation that failed. The paired checks below are what tell
	 * those apart.
	 */
#define ARENA_STR(field, source)					\
	do {								\
		const char *src = dir_str_in(source);			\
									\
		(field) = defw2_dir_arena_str(arena, src);		\
		if (src != NULL && (field) == NULL)			\
			return false;					\
	} while (0)

	ARENA_STR(out->service_id, wire->service_id);
	ARENA_STR(out->service_type, wire->service_type);
	ARENA_STR(out->runtime_id, wire->runtime_id);
	ARENA_STR(out->address, wire->address);
	ARENA_STR(out->endpoint.node_name, wire->node_name);
	ARENA_STR(out->endpoint.hostname, wire->hostname);
	ARENA_STR(out->selector.name, wire->selector_name);
	out->generation = wire->generation;
	out->state = (defw2_dir_state_t)wire->state;
	out->endpoint.pid = wire->pid;
	out->registered_at_ns = wire->registered_at_ns;
	out->last_heartbeat_ns = wire->last_heartbeat_ns;
	out->retention_deadline_ns = wire->retention_deadline_ns;

	if (wire->aliases.count > 0) {
		const char **aliases = defw2_dir_arena_alloc(
			arena, wire->aliases.count * sizeof(*aliases));

		if (aliases == NULL)
			return false;
		for (i = 0; i < wire->aliases.count; i++)
			ARENA_STR(aliases[i], wire->aliases.items[i]);
		out->selector.aliases = aliases;
		out->selector.alias_count = wire->aliases.count;
	}
	if (wire->resources.count > 0) {
		const char **resources = defw2_dir_arena_alloc(
			arena, wire->resources.count * sizeof(*resources));

		if (resources == NULL)
			return false;
		for (i = 0; i < wire->resources.count; i++)
			ARENA_STR(resources[i], wire->resources.items[i]);
		out->selector.resources = resources;
		out->selector.resource_count = wire->resources.count;
	}
	if (wire->bindings.count > 0) {
		defw2_dir_binding_t *bindings = defw2_dir_arena_alloc(
			arena, wire->bindings.count * sizeof(*bindings));

		if (bindings == NULL)
			return false;
		for (i = 0; i < wire->bindings.count; i++) {
			ARENA_STR(bindings[i].binding_name,
				  wire->bindings.items[i].binding_name);
			ARENA_STR(bindings[i].api_id,
				  wire->bindings.items[i].api_id);
			bindings[i].api_version =
				wire->bindings.items[i].api_version;
			bindings[i].provider_id =
				wire->bindings.items[i].provider_id;
		}
		out->bindings = bindings;
		out->binding_count = wire->bindings.count;
		if (selected != DEFW2_DIR_NO_BINDING &&
		    selected < wire->bindings.count)
			entry->binding = bindings[selected];
	}
	if (wire->properties.count > 0) {
		defw2_dir_property_t *props = defw2_dir_arena_alloc(
			arena, wire->properties.count * sizeof(*props));

		if (props == NULL)
			return false;
		for (i = 0; i < wire->properties.count; i++) {
			ARENA_STR(props[i].name,
				  wire->properties.items[i].name);
			ARENA_STR(props[i].value,
				  wire->properties.items[i].value);
		}
		out->properties = props;
		out->property_count = wire->properties.count;
	}
#undef ARENA_STR
	return true;
}

static void query_to_wire(const defw2_dir_query_t *query,
			  defw2_wire_query_t *wire,
			  defw2_wire_filter_t *filters)
{
	size_t i;

	memset(wire, 0, sizeof(*wire));
	wire->service_id = (defw2_str_t)dir_str_out(query->service_id);
	wire->service_type = (defw2_str_t)dir_str_out(query->service_type);
	wire->selector_name = (defw2_str_t)dir_str_out(query->selector_name);
	wire->resource = (defw2_str_t)dir_str_out(query->resource);
	wire->binding_name = (defw2_str_t)dir_str_out(query->binding_name);
	wire->api_version = query->api_version;
	wire->include_inactive = query->include_inactive ? 1u : 0u;
	wire->limit = query->limit;
	if (query->filter_count > 0 && filters != NULL) {
		for (i = 0; i < query->filter_count; i++) {
			filters[i].name = (defw2_str_t)dir_str_out(
				query->filters[i].name);
			filters[i].value = (defw2_str_t)dir_str_out(
				query->filters[i].value);
			filters[i].match = (hg_uint32_t)query->filters[i].match;
		}
		wire->filters.items = filters;
		wire->filters.count = (hg_uint32_t)query->filter_count;
	}
}

static defw2_rc_t resolve_call(defw2_dir_t *dir, const char *rpc_name,
			       const char *method,
			       const defw2_dir_query_t *query,
			       const defw2_call_opts_t *opts,
			       defw2_dir_result_t *result,
			       defw2_status_t *status)
{
	static const defw2_dir_query_t match_all;
	defw2_dir_resolve_in_t in;
	defw2_dir_resolve_out_t out;
	struct defw2_trace trace;
	hg_handle_t handle = HG_HANDLE_NULL;
	defw2_wire_filter_t *filters = NULL;
	defw2_dir_arena_t *arena = NULL;
	defw2_dir_entry_t *entries = NULL;
	hg_return_t hret;
	defw2_rc_t rc = DEFW2_OK;
	hg_uint32_t i;
	hg_id_t id;

	if (dir == NULL || result == NULL)
		return DEFW2_ERR_INVALID;
	if (query == NULL)
		query = &match_all;
	if (query->filter_count > DEFW2_DIR_LIST_MAX)
		return DEFW2_ERR_INVALID;

	memset(result, 0, sizeof(*result));
	if (query->filter_count > 0) {
		filters = calloc(query->filter_count, sizeof(*filters));
		if (filters == NULL)
			return DEFW2_ERR_NOMEM;
	}
	memset(&in, 0, sizeof(in));
	query_to_wire(query, &in.query, filters);

	id = defw2_rpc_lookup(dir->rt, rpc_name,
			      hg_proc_defw2_dir_resolve_in_t,
			      hg_proc_defw2_dir_resolve_out_t);
	hret = margo_create(dir->rt->mid, dir->binding->addr, id, &handle);
	if (hret != HG_SUCCESS) {
		free(filters);
		return defw2_rc_from_hg(hret, NULL);
	}

	defw2_trace_begin(dir->rt, &trace, DEFW2_SPAN_CLIENT, trace_of(opts));
	defw2_hdr_fill(dir->rt, &in.hdr, DEFW2_API_VERSION,
		       trace.recording ? trace.traceparent : trace_of(opts));

	hret = margo_provider_forward_timed(dir->binding->provider_id, handle,
					    &in, timeout_of(opts));
	if (hret != HG_SUCCESS) {
		rc = defw2_rc_from_hg(hret, NULL);
		goto out;
	}
	/* Zeroed, so the free after a decode that fails part way only
	 * touches the fields it actually decoded. defw2_free_partial
	 * says why that free is not margo_free_output. */
	memset(&out, 0, sizeof(out));
	hret = margo_get_output(handle, &out);
	if (hret != HG_SUCCESS) {
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_dir_resolve_out_t, &out);
		rc = defw2_rc_from_hg(hret, NULL);
		goto out;
	}
	defw2_status_from_wire(status, &out.status);

	if (out.records.count > 0) {
		arena = calloc(1, sizeof(*arena));
		entries = calloc(out.records.count, sizeof(*entries));
		if (arena == NULL || entries == NULL) {
			rc = DEFW2_ERR_NOMEM;
			goto decoded;
		}
		for (i = 0; i < out.records.count; i++) {
			uint32_t selected = i < out.selected.count ?
				out.selected.items[i] : DEFW2_DIR_NO_BINDING;

			if (!defw2_dir_entry_from_wire(arena,
						       &out.records.items[i],
						       selected,
						       &entries[i])) {
				rc = DEFW2_ERR_NOMEM;
				goto decoded;
			}
		}
		result->entries = entries;
		result->entry_count = out.records.count;
		result->arena = arena;
		arena = NULL;
		entries = NULL;
	}
decoded:
	margo_free_output(handle, &out);
	if (rc != DEFW2_OK) {
		defw2_dir_arena_free(arena);
		free(arena);
		free(entries);
	}

out:
	trace_finish(dir->rt, &trace, handle, method, rc, status);
	free(filters);
	margo_destroy(handle);
	return rc;
}

defw2_rc_t defw2_dir_resolve(defw2_dir_t *dir, const defw2_dir_query_t *query,
			     const defw2_call_opts_t *opts,
			     defw2_dir_result_t *result,
			     defw2_status_t *status)
{
	return resolve_call(dir, DEFW2_RPC_DIR_RESOLVE, "resolve_services",
			    query, opts, result, status);
}

defw2_rc_t defw2_dir_query(defw2_dir_t *dir, const defw2_dir_query_t *query,
			   const defw2_call_opts_t *opts,
			   defw2_dir_result_t *result,
			   defw2_status_t *status)
{
	return resolve_call(dir, DEFW2_RPC_DIR_QUERY, "query_directory",
			    query, opts, result, status);
}

/* --- get_generation -------------------------------------------------- */

defw2_rc_t defw2_dir_generation(defw2_dir_t *dir, const char *service_id,
				const defw2_call_opts_t *opts,
				uint64_t *generation, defw2_status_t *status)
{
	defw2_dir_generation_in_t in;
	defw2_dir_generation_out_t out;
	struct defw2_trace trace;
	hg_handle_t handle = HG_HANDLE_NULL;
	hg_return_t hret;
	defw2_rc_t rc = DEFW2_OK;
	hg_id_t id;

	if (dir == NULL || service_id == NULL)
		return DEFW2_ERR_INVALID;

	memset(&in, 0, sizeof(in));
	in.service_id = (defw2_str_t)dir_str_out(service_id);

	id = defw2_rpc_lookup(dir->rt, DEFW2_RPC_DIR_GENERATION,
			      hg_proc_defw2_dir_generation_in_t,
			      hg_proc_defw2_dir_generation_out_t);
	hret = margo_create(dir->rt->mid, dir->binding->addr, id, &handle);
	if (hret != HG_SUCCESS)
		return defw2_rc_from_hg(hret, NULL);

	defw2_trace_begin(dir->rt, &trace, DEFW2_SPAN_CLIENT, trace_of(opts));
	defw2_hdr_fill(dir->rt, &in.hdr, DEFW2_API_VERSION,
		       trace.recording ? trace.traceparent : trace_of(opts));

	hret = margo_provider_forward_timed(dir->binding->provider_id, handle,
					    &in, timeout_of(opts));
	if (hret != HG_SUCCESS) {
		rc = defw2_rc_from_hg(hret, NULL);
		goto out;
	}
	/* Zeroed, so the free after a decode that fails part way only
	 * touches the fields it actually decoded. defw2_free_partial
	 * says why that free is not margo_free_output. */
	memset(&out, 0, sizeof(out));
	hret = margo_get_output(handle, &out);
	if (hret != HG_SUCCESS) {
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_dir_generation_out_t, &out);
		rc = defw2_rc_from_hg(hret, NULL);
		goto out;
	}
	defw2_status_from_wire(status, &out.status);
	if (generation != NULL)
		*generation = out.generation;
	margo_free_output(handle, &out);

out:
	trace_finish(dir->rt, &trace, handle, "get_generation", rc,
		     status);
	margo_destroy(handle);
	return rc;
}

/* --- subscribe and unsubscribe --------------------------------------- */

static defw2_rc_t take_subscription(struct defw2_typed_call *call)
{
	defw2_dir_subscribe_out_t *w = call->out;
	uint64_t *subscription_id = call->arg;

	if (subscription_id != NULL)
		*subscription_id = w->subscription_id;
	return DEFW2_OK;
}

defw2_rc_t defw2_dir_subscribe(defw2_dir_t *dir,
			       const defw2_dir_subscribe_req_t *req,
			       const defw2_call_opts_t *opts,
			       uint64_t *subscription_id,
			       defw2_status_t *status)
{
	struct defw2_typed_call call;
	defw2_dir_subscribe_out_t out;
	defw2_dir_subscribe_in_t in;

	if (dir == NULL || req == NULL)
		return DEFW2_ERR_INVALID;
	if (subscription_id != NULL)
		*subscription_id = 0;
	memset(&in, 0, sizeof(in));
	in.address = dir_str_out(req->target.address);
	in.provider_id = req->target.provider_id;
	in.tag = dir_str_out(req->target.tag);
	in.service_id = dir_str_out(req->service_id);
	in.service_type = dir_str_out(req->service_type);
	in.changes = req->changes;

	memset(&call, 0, sizeof(call));
	call.binding = dir->binding;
	call.method = &defw2_dir_m_subscribe;
	call.opts = opts;
	call.in = &in;
	call.out = &out;
	call.out_size = sizeof(out);
	call.take = take_subscription;
	call.arg = subscription_id;
	return defw2_typed_call(&call, status);
}

defw2_rc_t defw2_dir_unsubscribe(defw2_dir_t *dir, uint64_t subscription_id,
				 const defw2_call_opts_t *opts,
				 defw2_status_t *status)
{
	struct defw2_typed_call call;
	defw2_dir_unsubscribe_in_t in;
	defw2_dir_lease_out_t out;

	if (dir == NULL)
		return DEFW2_ERR_INVALID;
	memset(&in, 0, sizeof(in));
	in.subscription_id = subscription_id;

	memset(&call, 0, sizeof(call));
	call.binding = dir->binding;
	call.method = &defw2_dir_m_unsubscribe;
	call.opts = opts;
	call.in = &in;
	call.out = &out;
	call.out_size = sizeof(out);
	return defw2_typed_call(&call, status);
}

/* --- get_runtime_id -------------------------------------------------- */

struct runtime_id_take {
	char	*runtime_id;
	size_t	len;
};

static defw2_rc_t take_runtime_id(struct defw2_typed_call *call)
{
	defw2_dir_runtime_id_out_t *w = call->out;
	struct runtime_id_take *t = call->arg;

	/* A failure carries no id, and its status says why. */
	if (w->runtime_id == NULL)
		return DEFW2_OK;
	if (strlen(w->runtime_id) >= t->len)
		return DEFW2_ERR_INVALID;
	strcpy(t->runtime_id, w->runtime_id);
	return DEFW2_OK;
}

defw2_rc_t defw2_dir_runtime_id(defw2_dir_t *dir,
				const defw2_call_opts_t *opts,
				char *runtime_id, size_t len,
				defw2_status_t *status)
{
	struct runtime_id_take t = { runtime_id, len };
	struct defw2_typed_call call;
	defw2_dir_runtime_id_out_t out;
	defw2_dir_runtime_id_in_t in;

	if (dir == NULL || runtime_id == NULL || len == 0)
		return DEFW2_ERR_INVALID;
	runtime_id[0] = '\0';
	memset(&in, 0, sizeof(in));

	memset(&call, 0, sizeof(call));
	call.binding = dir->binding;
	call.method = &defw2_dir_m_get_runtime_id;
	call.opts = opts;
	call.in = &in;
	call.out = &out;
	call.out_size = sizeof(out);
	call.take = take_runtime_id;
	call.arg = &t;
	return defw2_typed_call(&call, status);
}
