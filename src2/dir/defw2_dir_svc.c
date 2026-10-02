/*
 * The qfw.directory service: six handlers over the store.
 *
 * Each handler does the same four things in the same order, which is the
 * point of the shared header: check the version, decode into the public
 * shapes, call the store, encode what came back. None of them makes a
 * decision the store does not, so the lifecycle rules live in exactly one
 * place and this file stays a translation layer.
 *
 * resolve and query are one handler behind two registrations. They differ
 * only in whether inactive records are included, and the design names them
 * separately because the callers are different: clients resolve, operators
 * query.
 */
#include <stdlib.h>
#include <string.h>

#include <margo-timer.h>

#include "defw2_dir_internal.h"
#include "defw2_dir_wire.h"

#include "../host/defw2_host.h"

struct defw2_dir_bound {
	defw2_service_t		*svc;
	defw2_dir_store_t	*store;
	/*
	 * The liveness scan's timer. Margo owns the timer once it is armed, so
	 * stopping is a flag the callback reads rather than a destroy racing
	 * a firing.
	 */
	margo_timer_t		scan_timer;
	bool			scanning;
};

static struct defw2_dir_bound *bound_for(hg_handle_t handle,
					 margo_instance_id mid)
{
	const struct hg_info *info = margo_get_info(handle);

	if (info == NULL)
		return NULL;
	return (struct defw2_dir_bound *)margo_registered_data(mid, info->id);
}

/*
 * Answer a request this provider will not process. Used for the version
 * check, which the design says must refuse before the payload is read, and
 * for a handler that cannot find its own store.
 */
#define DIR_REFUSE(handle, out, in, code, category, message)		\
	do {								\
		defw2_wire_status_set(&(out).status, (code), (category),	\
				      (message));			\
		margo_respond((handle), &(out));			\
		margo_free_input((handle), &(in));			\
		margo_destroy((handle));				\
		return;							\
	} while (0)

/* --- register_service ------------------------------------------------ */

/*
 * Rebuild the public record from the wire. Everything points into the
 * decoded input, which Mercury holds until margo_free_input, so the record is
 * only valid for the length of the handler. The store deep copies what it
 * keeps.
 */
static defw2_rc_t record_from_wire(const defw2_wire_record_t *wire,
				   defw2_dir_record_t *record,
				   defw2_dir_binding_t **bindings,
				   defw2_dir_property_t **properties)
{
	hg_uint32_t i;

	memset(record, 0, sizeof(*record));
	*bindings = NULL;
	*properties = NULL;

	record->service_id = dir_str_in(wire->service_id);
	record->service_type = dir_str_in(wire->service_type);
	record->runtime_id = dir_str_in(wire->runtime_id);
	record->address = dir_str_in(wire->address);
	record->endpoint.node_name = dir_str_in(wire->node_name);
	record->endpoint.hostname = dir_str_in(wire->hostname);
	record->endpoint.pid = wire->pid;
	record->selector.name = dir_str_in(wire->selector_name);
	/*
	 * The alias and resource arrays are const char *const * in the public
	 * record and defw2_str_t * on the wire, which is the same storage; the
	 * cast says so rather than copying an array to change one qualifier.
	 */
	record->selector.aliases = (const char *const *)wire->aliases.items;
	record->selector.alias_count = wire->aliases.count;
	record->selector.resources = (const char *const *)wire->resources.items;
	record->selector.resource_count = wire->resources.count;

	if (wire->bindings.count > 0) {
		*bindings = calloc(wire->bindings.count, sizeof(**bindings));
		if (*bindings == NULL)
			return DEFW2_ERR_NOMEM;
		for (i = 0; i < wire->bindings.count; i++) {
			(*bindings)[i].binding_name =
				dir_str_in(wire->bindings.items[i].binding_name);
			(*bindings)[i].api_id =
				dir_str_in(wire->bindings.items[i].api_id);
			(*bindings)[i].api_version =
				wire->bindings.items[i].api_version;
			(*bindings)[i].provider_id =
				wire->bindings.items[i].provider_id;
		}
		record->bindings = *bindings;
		record->binding_count = wire->bindings.count;
	}
	if (wire->properties.count > 0) {
		*properties = calloc(wire->properties.count,
				     sizeof(**properties));
		if (*properties == NULL)
			return DEFW2_ERR_NOMEM;
		for (i = 0; i < wire->properties.count; i++) {
			(*properties)[i].name =
				dir_str_in(wire->properties.items[i].name);
			(*properties)[i].value =
				dir_str_in(wire->properties.items[i].value);
		}
		record->properties = *properties;
		record->property_count = wire->properties.count;
	}
	return DEFW2_OK;
}

static void defw2_dir_register_ult(hg_handle_t handle)
{
	margo_instance_id mid = margo_hg_handle_get_instance(handle);
	struct defw2_dir_bound *bound = bound_for(handle, mid);
	defw2_dir_binding_t *bindings = NULL;
	defw2_dir_property_t *properties = NULL;
	defw2_dir_register_in_t in;
	defw2_dir_register_out_t out;
	defw2_dir_record_t record;
	defw2_status_t status;
	hg_return_t hret;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	memset(&status, 0, sizeof(status));
	defw2_wire_status_ok(&out.status);

	hret = margo_get_input(handle, &in);
	if (hret != HG_SUCCESS) {
		/* A decode that fails part way still allocated the fields
		 * before the one it refused. defw2_free_partial says why
		 * this is not margo_free_input. */
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_dir_register_in_t, &in);
		defw2_wire_status_set(&out.status, DEFW2_ERR_TRANSPORT,
				      DEFW2_CAT_TRANSPORT,
				      "could not decode the request");
		margo_respond(handle, &out);
		margo_destroy(handle);
		return;
	}
	if (!defw2_hdr_compatible(&in.hdr, DEFW2_API_VERSION))
		DIR_REFUSE(handle, out, in, DEFW2_ERR_VERSION,
			   DEFW2_CAT_VERSION_MISMATCH,
			   "the directory does not speak this wire version");
	if (bound == NULL)
		DIR_REFUSE(handle, out, in, DEFW2_ERR_INTERNAL,
			   DEFW2_CAT_PROVIDER_FAILURE,
			   "the directory has no store");

	if (record_from_wire(&in.record, &record, &bindings, &properties) !=
	    DEFW2_OK) {
		free(bindings);
		free(properties);
		DIR_REFUSE(handle, out, in, DEFW2_ERR_NOMEM,
			   DEFW2_CAT_PROVIDER_FAILURE,
			   "out of memory decoding the record");
	}

	if (defw2_dir_store_register(bound->store, &record, &out.generation,
				     &status) != DEFW2_OK)
		defw2_wire_status_set(&out.status, DEFW2_ERR_INTERNAL,
				      DEFW2_CAT_PROVIDER_FAILURE,
				      "the directory could not store the record");
	else
		defw2_wire_status_set(&out.status, status.code,
				      status.category, status.message);

	margo_respond(handle, &out);
	defw2_status_free(&status);
	free(bindings);
	free(properties);
	margo_free_input(handle, &in);
	margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_register_ult)

/* --- heartbeat and deregister ---------------------------------------- */

/*
 * Both take the same fields and both answer with a bare status, so one body
 * serves them and the caller says which store call to make.
 */
static void lease_ult(hg_handle_t handle, bool deregister)
{
	margo_instance_id mid = margo_hg_handle_get_instance(handle);
	struct defw2_dir_bound *bound = bound_for(handle, mid);
	defw2_dir_lease_in_t in;
	defw2_dir_lease_out_t out;
	defw2_status_t status;
	defw2_rc_t rc;
	hg_return_t hret;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	memset(&status, 0, sizeof(status));
	defw2_wire_status_ok(&out.status);

	hret = margo_get_input(handle, &in);
	if (hret != HG_SUCCESS) {
		/* A decode that fails part way still allocated the fields
		 * before the one it refused. defw2_free_partial says why
		 * this is not margo_free_input. */
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_dir_lease_in_t, &in);
		defw2_wire_status_set(&out.status, DEFW2_ERR_TRANSPORT,
				      DEFW2_CAT_TRANSPORT,
				      "could not decode the request");
		margo_respond(handle, &out);
		margo_destroy(handle);
		return;
	}
	if (!defw2_hdr_compatible(&in.hdr, DEFW2_API_VERSION))
		DIR_REFUSE(handle, out, in, DEFW2_ERR_VERSION,
			   DEFW2_CAT_VERSION_MISMATCH,
			   "the directory does not speak this wire version");
	if (bound == NULL)
		DIR_REFUSE(handle, out, in, DEFW2_ERR_INTERNAL,
			   DEFW2_CAT_PROVIDER_FAILURE,
			   "the directory has no store");

	if (deregister)
		rc = defw2_dir_store_deregister(bound->store,
						dir_str_in(in.service_id),
						dir_str_in(in.runtime_id),
						in.generation, &status);
	else
		rc = defw2_dir_store_heartbeat(bound->store,
					       dir_str_in(in.service_id),
					       dir_str_in(in.runtime_id),
					       in.generation, &status);
	if (rc != DEFW2_OK)
		defw2_wire_status_set(&out.status, DEFW2_ERR_INVALID,
				      DEFW2_CAT_INVALID_ARGUMENT,
				      "a service_id is required");
	else
		defw2_wire_status_set(&out.status, status.code,
				      status.category, status.message);

	margo_respond(handle, &out);
	defw2_status_free(&status);
	margo_free_input(handle, &in);
	margo_destroy(handle);
}

static void defw2_dir_heartbeat_ult(hg_handle_t handle)
{
	lease_ult(handle, false);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_heartbeat_ult)

static void defw2_dir_deregister_ult(hg_handle_t handle)
{
	lease_ult(handle, true);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_deregister_ult)

/* --- resolve and query ----------------------------------------------- */

/*
 * Encode one record for the wire. Every string is borrowed from the result
 * arena, which outlives the respond, so nothing here is copied and nothing
 * needs freeing. The lists point at arrays this function allocates, which
 * the caller releases with records_free.
 */
static bool record_to_wire(const defw2_dir_record_t *record,
			   defw2_wire_record_t *wire)
{
	size_t i;

	memset(wire, 0, sizeof(*wire));
	wire->service_id = (defw2_str_t)dir_str_out(record->service_id);
	wire->service_type = (defw2_str_t)dir_str_out(record->service_type);
	wire->runtime_id = (defw2_str_t)dir_str_out(record->runtime_id);
	wire->generation = record->generation;
	wire->state = (hg_uint32_t)record->state;
	wire->address = (defw2_str_t)dir_str_out(record->address);
	wire->node_name = (defw2_str_t)dir_str_out(record->endpoint.node_name);
	wire->hostname = (defw2_str_t)dir_str_out(record->endpoint.hostname);
	wire->pid = record->endpoint.pid;
	wire->selector_name = (defw2_str_t)dir_str_out(record->selector.name);
	wire->registered_at_ns = record->registered_at_ns;
	wire->last_heartbeat_ns = record->last_heartbeat_ns;
	wire->retention_deadline_ns = record->retention_deadline_ns;

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

/*
 * Release the arrays record_to_wire allocated. The strings inside them are
 * the arena's, so only the arrays go.
 */
static void records_free(defw2_wire_record_t *records, size_t count)
{
	size_t i;

	if (records == NULL)
		return;
	for (i = 0; i < count; i++) {
		free(records[i].aliases.items);
		free(records[i].resources.items);
		free(records[i].bindings.items);
		free(records[i].properties.items);
	}
	free(records);
}

static void resolve_ult(hg_handle_t handle, bool include_inactive)
{
	margo_instance_id mid = margo_hg_handle_get_instance(handle);
	struct defw2_dir_bound *bound = bound_for(handle, mid);
	defw2_dir_resolve_in_t in;
	defw2_dir_resolve_out_t out;
	defw2_dir_result_t result;
	defw2_dir_query_t query;
	defw2_dir_filter_t *filters = NULL;
	defw2_wire_record_t *records = NULL;
	uint32_t *selected = NULL;
	defw2_status_t status;
	hg_return_t hret;
	hg_uint32_t i;
	size_t published = 0;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	memset(&result, 0, sizeof(result));
	memset(&status, 0, sizeof(status));
	defw2_wire_status_ok(&out.status);

	hret = margo_get_input(handle, &in);
	if (hret != HG_SUCCESS) {
		/* A decode that fails part way still allocated the fields
		 * before the one it refused. defw2_free_partial says why
		 * this is not margo_free_input. */
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_dir_resolve_in_t, &in);
		defw2_wire_status_set(&out.status, DEFW2_ERR_TRANSPORT,
				      DEFW2_CAT_TRANSPORT,
				      "could not decode the request");
		margo_respond(handle, &out);
		margo_destroy(handle);
		return;
	}
	if (!defw2_hdr_compatible(&in.hdr, DEFW2_API_VERSION))
		DIR_REFUSE(handle, out, in, DEFW2_ERR_VERSION,
			   DEFW2_CAT_VERSION_MISMATCH,
			   "the directory does not speak this wire version");
	if (bound == NULL)
		DIR_REFUSE(handle, out, in, DEFW2_ERR_INTERNAL,
			   DEFW2_CAT_PROVIDER_FAILURE,
			   "the directory has no store");

	memset(&query, 0, sizeof(query));
	query.service_id = dir_str_in(in.query.service_id);
	query.service_type = dir_str_in(in.query.service_type);
	query.selector_name = dir_str_in(in.query.selector_name);
	query.resource = dir_str_in(in.query.resource);
	query.binding_name = dir_str_in(in.query.binding_name);
	query.api_version = in.query.api_version;
	query.limit = (size_t)in.query.limit;
	/*
	 * The registration decides this, not the caller: query_directory sees
	 * inactive records and resolve_services does not, whatever the
	 * request's own flag says. A client cannot talk its way into the
	 * operator view by setting a bit.
	 */
	query.include_inactive = include_inactive;

	if (in.query.filters.count > 0) {
		filters = calloc(in.query.filters.count, sizeof(*filters));
		if (filters == NULL)
			DIR_REFUSE(handle, out, in, DEFW2_ERR_NOMEM,
				   DEFW2_CAT_PROVIDER_FAILURE,
				   "out of memory decoding the query");
		for (i = 0; i < in.query.filters.count; i++) {
			filters[i].name =
				dir_str_in(in.query.filters.items[i].name);
			filters[i].value =
				dir_str_in(in.query.filters.items[i].value);
			filters[i].match = (defw2_dir_match_t)
				in.query.filters.items[i].match;
		}
		query.filters = filters;
		query.filter_count = in.query.filters.count;
	}

	if (defw2_dir_store_resolve_indexed(bound->store, &query, &result,
					    &selected, &status) != DEFW2_OK) {
		free(filters);
		DIR_REFUSE(handle, out, in, DEFW2_ERR_INTERNAL,
			   DEFW2_CAT_PROVIDER_FAILURE,
			   "the directory could not read its records");
	}

	if (result.entry_count > DEFW2_DIR_RESULT_MAX) {
		/*
		 * Truncate rather than refuse. A caller that asked an
		 * unbounded question still gets a usable answer, and the
		 * wire's own bound stays the thing that cannot be exceeded.
		 */
		defw2_log(defw2_service_runtime(bound->svc),
			  DEFW2_LOG_WARNING,
			  "directory: truncating a resolve of %zu records",
			  result.entry_count);
		result.entry_count = DEFW2_DIR_RESULT_MAX;
	}
	if (result.entry_count > 0) {
		records = calloc(result.entry_count, sizeof(*records));
		if (records == NULL) {
			free(filters);
			free(selected);
			defw2_dir_result_free(&result);
			defw2_status_free(&status);
			DIR_REFUSE(handle, out, in, DEFW2_ERR_NOMEM,
				   DEFW2_CAT_PROVIDER_FAILURE,
				   "out of memory encoding the answer");
		}
		for (published = 0; published < result.entry_count;
		     published++) {
			if (!record_to_wire(&result.entries[published].record,
					    &records[published]))
				break;
		}
		if (published != result.entry_count) {
			records_free(records, published + 1);
			free(filters);
			free(selected);
			defw2_dir_result_free(&result);
			defw2_status_free(&status);
			DIR_REFUSE(handle, out, in, DEFW2_ERR_NOMEM,
				   DEFW2_CAT_PROVIDER_FAILURE,
				   "out of memory encoding the answer");
		}
		out.records.items = records;
		out.records.count = (hg_uint32_t)result.entry_count;
		out.selected.items = selected;
		out.selected.count = (hg_uint32_t)result.entry_count;
	}
	defw2_wire_status_set(&out.status, status.code, status.category,
			      status.message);

	margo_respond(handle, &out);
	records_free(records, result.entry_count);
	free(selected);
	free(filters);
	defw2_dir_result_free(&result);
	defw2_status_free(&status);
	margo_free_input(handle, &in);
	margo_destroy(handle);
}

static void defw2_dir_resolve_ult(hg_handle_t handle)
{
	resolve_ult(handle, false);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_resolve_ult)

static void defw2_dir_query_ult(hg_handle_t handle)
{
	resolve_ult(handle, true);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_query_ult)

/* --- get_generation -------------------------------------------------- */

static void defw2_dir_generation_ult(hg_handle_t handle)
{
	margo_instance_id mid = margo_hg_handle_get_instance(handle);
	struct defw2_dir_bound *bound = bound_for(handle, mid);
	defw2_dir_generation_in_t in;
	defw2_dir_generation_out_t out;
	defw2_status_t status;
	hg_return_t hret;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	memset(&status, 0, sizeof(status));
	defw2_wire_status_ok(&out.status);

	hret = margo_get_input(handle, &in);
	if (hret != HG_SUCCESS) {
		/* A decode that fails part way still allocated the fields
		 * before the one it refused. defw2_free_partial says why
		 * this is not margo_free_input. */
		defw2_free_partial(margo_hg_handle_get_instance(handle),
				   hg_proc_defw2_dir_generation_in_t, &in);
		defw2_wire_status_set(&out.status, DEFW2_ERR_TRANSPORT,
				      DEFW2_CAT_TRANSPORT,
				      "could not decode the request");
		margo_respond(handle, &out);
		margo_destroy(handle);
		return;
	}
	if (!defw2_hdr_compatible(&in.hdr, DEFW2_API_VERSION))
		DIR_REFUSE(handle, out, in, DEFW2_ERR_VERSION,
			   DEFW2_CAT_VERSION_MISMATCH,
			   "the directory does not speak this wire version");
	if (bound == NULL)
		DIR_REFUSE(handle, out, in, DEFW2_ERR_INTERNAL,
			   DEFW2_CAT_PROVIDER_FAILURE,
			   "the directory has no store");

	if (defw2_dir_store_generation(bound->store, dir_str_in(in.service_id),
				       &out.generation, &status) != DEFW2_OK)
		defw2_wire_status_set(&out.status, DEFW2_ERR_INVALID,
				      DEFW2_CAT_INVALID_ARGUMENT,
				      "a service_id is required");
	else
		defw2_wire_status_set(&out.status, status.code,
				      status.category, status.message);

	margo_respond(handle, &out);
	defw2_status_free(&status);
	margo_free_input(handle, &in);
	margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(defw2_dir_generation_ult)

/* --- the liveness timer ---------------------------------------------- */

/*
 * The scan runs on a Margo timer, which is a one-shot, so the callback arms
 * the next one. It re-arms only while scanning is set, which is how
 * defw2_dir_bind's teardown stops it without racing a firing callback.
 */
static void scan_timer_cb(void *arg)
{
	struct defw2_dir_bound *bound = arg;
	size_t changed;

	if (bound == NULL || !bound->scanning)
		return;
	changed = defw2_dir_store_scan(bound->store);
	if (changed > 0)
		defw2_log(defw2_service_runtime(bound->svc), DEFW2_LOG_DEBUG,
			  "directory: liveness scan changed %zu records",
			  changed);
	if (bound->scanning)
		margo_timer_start(bound->scan_timer,
				  (double)defw2_dir_store_scan_interval_ms(
					  bound->store));
}

/* --- binding --------------------------------------------------------- */

/*
 * Released when the runtime finalizes, through the free callback on the first
 * registration. Stopping the timer here rather than in a destroy keeps the
 * order simple: Margo has already stopped serving by the time it frees
 * registered data, so nothing can be mid-scan.
 */
static void bound_free(void *arg)
{
	struct defw2_dir_bound *bound = arg;

	if (bound == NULL)
		return;
	bound->scanning = false;
	if (bound->scan_timer != MARGO_TIMER_NULL) {
		/*
		 * Cancel before destroy: clearing the flag stops the callback
		 * re-arming, but a timer already queued still has to be taken
		 * back before the memory it points at goes away.
		 */
		margo_timer_cancel(bound->scan_timer);
		margo_timer_destroy(bound->scan_timer);
	}
	defw2_dir_store_destroy(bound->store);
	free(bound);
}

defw2_rc_t defw2_dir_bind(defw2_service_t *svc,
			  const defw2_dir_store_opts_t *opts)
{
	struct defw2_dir_bound *bound;
	struct defw2_rt *rt;
	hg_id_t ids[6];
	size_t i;
	defw2_rc_t rc;

	if (svc == NULL)
		return DEFW2_ERR_INVALID;
	rt = defw2_service_runtime(svc);
	if (rt == NULL)
		return DEFW2_ERR_INVALID;

	bound = calloc(1, sizeof(*bound));
	if (bound == NULL)
		return DEFW2_ERR_NOMEM;
	bound->svc = svc;
	bound->scan_timer = MARGO_TIMER_NULL;
	rc = defw2_dir_store_create(rt, opts, &bound->store);
	if (rc != DEFW2_OK) {
		free(bound);
		return rc;
	}

	ids[0] = MARGO_REGISTER_PROVIDER(rt->mid, DEFW2_RPC_DIR_REGISTER,
					 defw2_dir_register_in_t,
					 defw2_dir_register_out_t,
					 defw2_dir_register_ult,
					 defw2_service_provider_id(svc),
					 ABT_POOL_NULL);
	ids[1] = MARGO_REGISTER_PROVIDER(rt->mid, DEFW2_RPC_DIR_HEARTBEAT,
					 defw2_dir_lease_in_t,
					 defw2_dir_lease_out_t,
					 defw2_dir_heartbeat_ult,
					 defw2_service_provider_id(svc),
					 ABT_POOL_NULL);
	ids[2] = MARGO_REGISTER_PROVIDER(rt->mid, DEFW2_RPC_DIR_DEREGISTER,
					 defw2_dir_lease_in_t,
					 defw2_dir_lease_out_t,
					 defw2_dir_deregister_ult,
					 defw2_service_provider_id(svc),
					 ABT_POOL_NULL);
	ids[3] = MARGO_REGISTER_PROVIDER(rt->mid, DEFW2_RPC_DIR_RESOLVE,
					 defw2_dir_resolve_in_t,
					 defw2_dir_resolve_out_t,
					 defw2_dir_resolve_ult,
					 defw2_service_provider_id(svc),
					 ABT_POOL_NULL);
	ids[4] = MARGO_REGISTER_PROVIDER(rt->mid, DEFW2_RPC_DIR_QUERY,
					 defw2_dir_resolve_in_t,
					 defw2_dir_resolve_out_t,
					 defw2_dir_query_ult,
					 defw2_service_provider_id(svc),
					 ABT_POOL_NULL);
	ids[5] = MARGO_REGISTER_PROVIDER(rt->mid, DEFW2_RPC_DIR_GENERATION,
					 defw2_dir_generation_in_t,
					 defw2_dir_generation_out_t,
					 defw2_dir_generation_ult,
					 defw2_service_provider_id(svc),
					 ABT_POOL_NULL);
	for (i = 0; i < 6; i++) {
		if (ids[i] == 0) {
			defw2_log(rt, DEFW2_LOG_ERROR,
				  "cannot register %s on provider %u",
				  DEFW2_API_DIR,
				  defw2_service_provider_id(svc));
			defw2_dir_store_destroy(bound->store);
			free(bound);
			return DEFW2_ERR_INTERNAL;
		}
	}

	/*
	 * One owner, as the echo service does it: the first registration frees
	 * the data when the runtime finalizes and the rest share the pointer.
	 */
	margo_register_data(rt->mid, ids[0], bound, bound_free);
	for (i = 1; i < 6; i++)
		margo_register_data(rt->mid, ids[i], bound, NULL);

	/* margo_timer_create reports zero for success, not an hg_return_t. */
	if (margo_timer_create(rt->mid, scan_timer_cb, bound,
			       &bound->scan_timer) == 0) {
		bound->scanning = true;
		margo_timer_start(bound->scan_timer,
				  (double)defw2_dir_store_scan_interval_ms(
					  bound->store));
	} else {
		/*
		 * Without the timer nothing ever times out, which is a
		 * directory that lies rather than one that is merely slow.
		 */
		defw2_log(rt, DEFW2_LOG_ERROR,
			  "directory: no liveness timer, records will not time out");
		bound->scan_timer = MARGO_TIMER_NULL;
	}

	defw2_log(rt, DEFW2_LOG_MESSAGE, "%s bound on provider %u of %s",
		  DEFW2_API_DIR, defw2_service_provider_id(svc),
		  defw2_service_id(svc));
	return DEFW2_OK;
}
