/*
 * The directory store: in-memory records, generations and liveness.
 *
 * State is a single linked list under one mutex. A directory holds tens of
 * records, not thousands, and every operation but resolve touches exactly one
 * of them, so a list and a linear scan are the honest choice; anything
 * indexed would be more code for a cost nobody can measure here.
 *
 * The list also keeps records the clients can no longer see. A record that
 * timed out or deregistered stays until its retention deadline passes,
 * because "what happened to it" is the operator's first question and the
 * generation counter has to survive a restart to be able to increment.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defw2_dir_internal.h"

#include "../core/defw2_internal.h"

struct defw2_dir_store {
	struct defw2_rt		*rt;
	defw2_dir_record_own_t	*records;
	pthread_mutex_t		lock;
	uint32_t		heartbeat_timeout_ms;
	uint32_t		scan_interval_ms;
	uint64_t		retention_ms;
	char			*snapshot_path;	/* owned, may be NULL */
};

/* --- string arena ---------------------------------------------------- */

static bool arena_push(defw2_dir_arena_t *arena, void *block)
{
	if (arena->count == arena->cap) {
		size_t cap = arena->cap ? arena->cap * 2 : 16;
		void **blocks = realloc(arena->blocks, cap * sizeof(*blocks));

		if (blocks == NULL)
			return false;
		arena->blocks = blocks;
		arena->cap = cap;
	}
	arena->blocks[arena->count++] = block;
	return true;
}

void *defw2_dir_arena_alloc(defw2_dir_arena_t *arena, size_t size)
{
	void *block;

	if (arena == NULL)
		return NULL;
	/*
	 * A zero-size request still returns a distinct pointer the arena
	 * owns, so a caller can tell "an empty list" from "no list" without
	 * a separate flag.
	 */
	block = calloc(1, size ? size : 1);
	if (block == NULL)
		return NULL;
	if (!arena_push(arena, block)) {
		free(block);
		return NULL;
	}
	return block;
}

char *defw2_dir_arena_str(defw2_dir_arena_t *arena, const char *s)
{
	size_t len;
	char *copy;

	if (arena == NULL || s == NULL)
		return NULL;
	len = strlen(s);
	copy = defw2_dir_arena_alloc(arena, len + 1);
	if (copy == NULL)
		return NULL;
	memcpy(copy, s, len);
	return copy;
}

void defw2_dir_arena_free(defw2_dir_arena_t *arena)
{
	size_t i;

	if (arena == NULL)
		return;
	for (i = 0; i < arena->count; i++)
		free(arena->blocks[i]);
	free(arena->blocks);
	arena->blocks = NULL;
	arena->count = 0;
	arena->cap = 0;
}

void defw2_dir_result_free(defw2_dir_result_t *result)
{
	defw2_dir_arena_t *arena;

	if (result == NULL)
		return;
	arena = result->arena;
	if (arena != NULL) {
		defw2_dir_arena_free(arena);
		free(arena);
	}
	free(result->entries);
	result->entries = NULL;
	result->entry_count = 0;
	result->arena = NULL;
}

/* --- the owned record ------------------------------------------------ */

static char *dup_or_null(const char *s)
{
	return s ? strdup(s) : NULL;
}

/*
 * True when every string that had a source survived. strdup is the only thing
 * that can fail here, so one check after a run of them is enough and reads
 * better than a check per field.
 */
static bool copied(const char *src, const char *dst)
{
	return src == NULL || dst != NULL;
}

static void free_str_list(char **list, size_t count)
{
	size_t i;

	if (list == NULL)
		return;
	for (i = 0; i < count; i++)
		free(list[i]);
	free(list);
}

static defw2_rc_t copy_str_list(const char *const *src, size_t count,
				char ***out)
{
	char **list;
	size_t i;

	*out = NULL;
	if (src == NULL || count == 0)
		return DEFW2_OK;
	list = calloc(count, sizeof(*list));
	if (list == NULL)
		return DEFW2_ERR_NOMEM;
	for (i = 0; i < count; i++) {
		if (src[i] == NULL)
			continue;
		list[i] = strdup(src[i]);
		if (list[i] == NULL) {
			free_str_list(list, count);
			return DEFW2_ERR_NOMEM;
		}
	}
	*out = list;
	return DEFW2_OK;
}

void defw2_dir_record_own_free(defw2_dir_record_own_t *record)
{
	size_t i;

	if (record == NULL)
		return;
	free(record->service_id);
	free(record->service_type);
	free(record->runtime_id);
	free(record->address);
	free(record->node_name);
	free(record->hostname);
	free(record->selector_name);
	for (i = 0; i < record->binding_count; i++) {
		free(record->bindings[i].binding_name);
		free(record->bindings[i].api_id);
	}
	free(record->bindings);
	free_str_list(record->aliases, record->alias_count);
	free_str_list(record->resources, record->resource_count);
	for (i = 0; i < record->property_count; i++) {
		free(record->properties[i].name);
		free(record->properties[i].value);
	}
	free(record->properties);
	free(record);
}

defw2_rc_t defw2_dir_record_own_from(const defw2_dir_record_t *src,
				     defw2_dir_record_own_t **out)
{
	defw2_dir_record_own_t *record;
	defw2_rc_t rc;
	size_t i;

	if (src == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	record = calloc(1, sizeof(*record));
	if (record == NULL)
		return DEFW2_ERR_NOMEM;

	record->service_id = dup_or_null(src->service_id);
	record->service_type = dup_or_null(src->service_type);
	record->runtime_id = dup_or_null(src->runtime_id);
	record->address = dup_or_null(src->address);
	record->node_name = dup_or_null(src->endpoint.node_name);
	record->hostname = dup_or_null(src->endpoint.hostname);
	record->selector_name = dup_or_null(src->selector.name);
	record->pid = src->endpoint.pid;
	if (!copied(src->service_id, record->service_id) ||
	    !copied(src->service_type, record->service_type) ||
	    !copied(src->runtime_id, record->runtime_id) ||
	    !copied(src->address, record->address) ||
	    !copied(src->endpoint.node_name, record->node_name) ||
	    !copied(src->endpoint.hostname, record->hostname) ||
	    !copied(src->selector.name, record->selector_name))
		goto nomem;

	if (src->binding_count > 0 && src->bindings != NULL) {
		record->bindings = calloc(src->binding_count,
					  sizeof(*record->bindings));
		if (record->bindings == NULL)
			goto nomem;
		record->binding_count = src->binding_count;
		for (i = 0; i < src->binding_count; i++) {
			const defw2_dir_binding_t *b = &src->bindings[i];

			record->bindings[i].binding_name =
				dup_or_null(b->binding_name);
			record->bindings[i].api_id = dup_or_null(b->api_id);
			record->bindings[i].api_version = b->api_version;
			record->bindings[i].provider_id = b->provider_id;
			if (!copied(b->binding_name,
				    record->bindings[i].binding_name) ||
			    !copied(b->api_id, record->bindings[i].api_id))
				goto nomem;
		}
	}

	rc = copy_str_list(src->selector.aliases, src->selector.alias_count,
			   &record->aliases);
	if (rc != DEFW2_OK)
		goto fail;
	record->alias_count = record->aliases ? src->selector.alias_count : 0;
	rc = copy_str_list(src->selector.resources,
			   src->selector.resource_count, &record->resources);
	if (rc != DEFW2_OK)
		goto fail;
	record->resource_count = record->resources ?
		src->selector.resource_count : 0;

	if (src->property_count > 0 && src->properties != NULL) {
		record->properties = calloc(src->property_count,
					    sizeof(*record->properties));
		if (record->properties == NULL)
			goto nomem;
		record->property_count = src->property_count;
		for (i = 0; i < src->property_count; i++) {
			record->properties[i].name =
				dup_or_null(src->properties[i].name);
			record->properties[i].value =
				dup_or_null(src->properties[i].value);
			if (!copied(src->properties[i].name,
				    record->properties[i].name) ||
			    !copied(src->properties[i].value,
				    record->properties[i].value))
				goto nomem;
		}
	}

	*out = record;
	return DEFW2_OK;

nomem:
	rc = DEFW2_ERR_NOMEM;
fail:
	defw2_dir_record_own_free(record);
	return rc;
}

/* --- status helpers -------------------------------------------------- */

/*
 * Fill a status the caller will free. A failure to allocate the message is
 * not worth failing the call over, so the message is simply absent and the
 * code and category still say what happened.
 */
static void status_set(defw2_status_t *status, defw2_rc_t code,
		       uint32_t category, const char *message)
{
	if (status == NULL)
		return;
	status->code = code;
	status->category = category;
	free(status->message);
	status->message = dup_or_null(message);
}

static void status_ok(defw2_status_t *status)
{
	status_set(status, DEFW2_OK, DEFW2_CAT_OK, NULL);
}

/* --- the store ------------------------------------------------------- */

defw2_rc_t defw2_dir_store_create(struct defw2_rt *rt,
				  const defw2_dir_store_opts_t *opts,
				  defw2_dir_store_t **out)
{
	defw2_dir_store_t *store;

	if (out == NULL)
		return DEFW2_ERR_INVALID;
	store = calloc(1, sizeof(*store));
	if (store == NULL)
		return DEFW2_ERR_NOMEM;
	if (pthread_mutex_init(&store->lock, NULL) != 0) {
		free(store);
		return DEFW2_ERR_INTERNAL;
	}
	store->rt = rt;
	store->heartbeat_timeout_ms = DEFW2_DIR_DEFAULT_TIMEOUT_MS;
	store->scan_interval_ms = DEFW2_DIR_DEFAULT_SCAN_MS;
	store->retention_ms = DEFW2_DIR_DEFAULT_RETENTION_MS;
	if (opts != NULL) {
		if (opts->heartbeat_timeout_ms > 0)
			store->heartbeat_timeout_ms =
				opts->heartbeat_timeout_ms;
		if (opts->scan_interval_ms > 0)
			store->scan_interval_ms = opts->scan_interval_ms;
		if (opts->retention_ms > 0)
			store->retention_ms = opts->retention_ms;
		if (opts->snapshot_path != NULL) {
			store->snapshot_path = strdup(opts->snapshot_path);
			if (store->snapshot_path == NULL) {
				pthread_mutex_destroy(&store->lock);
				free(store);
				return DEFW2_ERR_NOMEM;
			}
		}
	}
	*out = store;
	return DEFW2_OK;
}

void defw2_dir_store_destroy(defw2_dir_store_t *store)
{
	defw2_dir_record_own_t *record;

	if (store == NULL)
		return;
	record = store->records;
	while (record != NULL) {
		defw2_dir_record_own_t *next = record->next;

		defw2_dir_record_own_free(record);
		record = next;
	}
	pthread_mutex_destroy(&store->lock);
	free(store->snapshot_path);
	free(store);
}

uint32_t defw2_dir_store_scan_interval_ms(const defw2_dir_store_t *store)
{
	return store ? store->scan_interval_ms : DEFW2_DIR_DEFAULT_SCAN_MS;
}

/* Caller holds the lock. */
static defw2_dir_record_own_t *find_locked(defw2_dir_store_t *store,
					   const char *service_id)
{
	defw2_dir_record_own_t *record;

	for (record = store->records; record != NULL; record = record->next) {
		if (record->service_id != NULL &&
		    strcmp(record->service_id, service_id) == 0)
			return record;
	}
	return NULL;
}

/* Caller holds the lock. Unlinks without freeing. */
static void unlink_locked(defw2_dir_store_t *store,
			  defw2_dir_record_own_t *target)
{
	defw2_dir_record_own_t **link = &store->records;

	while (*link != NULL) {
		if (*link == target) {
			*link = target->next;
			target->next = NULL;
			return;
		}
		link = &(*link)->next;
	}
}

static void snapshot_locked(defw2_dir_store_t *store);

defw2_rc_t defw2_dir_store_register(defw2_dir_store_t *store,
				    const defw2_dir_record_t *src,
				    uint64_t *generation,
				    defw2_status_t *status)
{
	defw2_dir_record_own_t *record, *existing;
	uint64_t next_generation = 1;
	defw2_rc_t rc;

	if (store == NULL || src == NULL)
		return DEFW2_ERR_INVALID;
	if (src->service_id == NULL || src->service_id[0] == '\0' ||
	    src->runtime_id == NULL || src->runtime_id[0] == '\0') {
		status_set(status, DEFW2_ERR_INVALID,
			   DEFW2_CAT_INVALID_ARGUMENT,
			   "a directory record needs a service_id and a runtime_id");
		return DEFW2_OK;
	}

	rc = defw2_dir_record_own_from(src, &record);
	if (rc != DEFW2_OK)
		return rc;

	pthread_mutex_lock(&store->lock);
	existing = find_locked(store, src->service_id);
	if (existing != NULL) {
		/*
		 * A live record held by a different process is a conflict, not
		 * a restart. Refusing it means the operator sees the mistake
		 * instead of the two services taking turns.
		 */
		if (existing->state == DEFW2_DIR_STATE_UP &&
		    existing->runtime_id != NULL &&
		    strcmp(existing->runtime_id, src->runtime_id) != 0) {
			pthread_mutex_unlock(&store->lock);
			defw2_dir_record_own_free(record);
			status_set(status, DEFW2_ERR_BUSY,
				   DEFW2_CAT_INVALID_ARGUMENT,
				   "service_id is already registered by a live runtime");
			return DEFW2_OK;
		}
		next_generation = existing->generation + 1;
		unlink_locked(store, existing);
		defw2_dir_record_own_free(existing);
	}

	record->generation = next_generation;
	record->state = DEFW2_DIR_STATE_UP;
	record->registered_at_ns = defw2_wall_ns();
	record->last_heartbeat_ns = record->registered_at_ns;
	record->retention_deadline_ns = 0;
	record->next = store->records;
	store->records = record;
	snapshot_locked(store);
	/*
	 * Logged while the lock is held on purpose: the moment it is dropped
	 * the record belongs to the store, and a scan may retire and free it
	 * before this line runs.
	 */
	defw2_log(store->rt, DEFW2_LOG_MESSAGE,
		  "directory: registered %s generation %" PRIu64 " at %s",
		  record->service_id, next_generation,
		  record->address ? record->address : "(no address)");
	pthread_mutex_unlock(&store->lock);

	if (generation != NULL)
		*generation = next_generation;
	status_ok(status);
	return DEFW2_OK;
}

/*
 * The shared half of heartbeat and deregister: find the record and prove the
 * sender still owns it. Caller holds the lock.
 */
static defw2_dir_record_own_t *owned_locked(defw2_dir_store_t *store,
					    const char *service_id,
					    const char *runtime_id,
					    uint64_t generation)
{
	defw2_dir_record_own_t *record = find_locked(store, service_id);

	if (record == NULL)
		return NULL;
	if (record->runtime_id == NULL || runtime_id == NULL ||
	    strcmp(record->runtime_id, runtime_id) != 0)
		return NULL;
	if (generation != 0 && record->generation != generation)
		return NULL;
	return record;
}

defw2_rc_t defw2_dir_store_heartbeat(defw2_dir_store_t *store,
				     const char *service_id,
				     const char *runtime_id,
				     uint64_t generation,
				     defw2_status_t *status)
{
	defw2_dir_record_own_t *record;

	if (store == NULL || service_id == NULL)
		return DEFW2_ERR_INVALID;
	pthread_mutex_lock(&store->lock);
	record = owned_locked(store, service_id, runtime_id, generation);
	if (record == NULL) {
		pthread_mutex_unlock(&store->lock);
		status_set(status, DEFW2_ERR_NOT_FOUND, DEFW2_CAT_NOT_FOUND,
			   "no such registration; register again");
		return DEFW2_OK;
	}
	record->last_heartbeat_ns = defw2_wall_ns();
	/*
	 * A heartbeat from a record the scan already timed out brings it back.
	 * The process never went away, only its heartbeats were late, and
	 * making it re-register would be churn for a transient stall.
	 */
	if (record->state == DEFW2_DIR_STATE_TIMED_OUT) {
		record->state = DEFW2_DIR_STATE_UP;
		record->retention_deadline_ns = 0;
		snapshot_locked(store);
		pthread_mutex_unlock(&store->lock);
		defw2_log(store->rt, DEFW2_LOG_WARNING,
			  "directory: %s heartbeat resumed, back to UP",
			  service_id);
		status_ok(status);
		return DEFW2_OK;
	}
	pthread_mutex_unlock(&store->lock);
	status_ok(status);
	return DEFW2_OK;
}

defw2_rc_t defw2_dir_store_deregister(defw2_dir_store_t *store,
				      const char *service_id,
				      const char *runtime_id,
				      uint64_t generation,
				      defw2_status_t *status)
{
	defw2_dir_record_own_t *record;

	if (store == NULL || service_id == NULL)
		return DEFW2_ERR_INVALID;
	pthread_mutex_lock(&store->lock);
	record = owned_locked(store, service_id, runtime_id, generation);
	if (record == NULL) {
		pthread_mutex_unlock(&store->lock);
		status_set(status, DEFW2_ERR_NOT_FOUND, DEFW2_CAT_NOT_FOUND,
			   "no such registration");
		return DEFW2_OK;
	}
	record->state = DEFW2_DIR_STATE_DEREGISTERED;
	/* The address is gone the moment the process is, so stop handing it out. */
	free(record->address);
	record->address = NULL;
	record->retention_deadline_ns = defw2_wall_ns() +
		store->retention_ms * 1000000ull;
	snapshot_locked(store);
	/* Under the lock, for the reason register logs under it. */
	defw2_log(store->rt, DEFW2_LOG_MESSAGE,
		  "directory: deregistered %s generation %" PRIu64,
		  service_id, record->generation);
	pthread_mutex_unlock(&store->lock);
	status_ok(status);
	return DEFW2_OK;
}

defw2_rc_t defw2_dir_store_generation(defw2_dir_store_t *store,
				      const char *service_id,
				      uint64_t *generation,
				      defw2_status_t *status)
{
	defw2_dir_record_own_t *record;

	if (store == NULL || service_id == NULL)
		return DEFW2_ERR_INVALID;
	pthread_mutex_lock(&store->lock);
	record = find_locked(store, service_id);
	if (record == NULL) {
		pthread_mutex_unlock(&store->lock);
		status_set(status, DEFW2_ERR_NOT_FOUND, DEFW2_CAT_NOT_FOUND,
			   "no such service_id");
		return DEFW2_OK;
	}
	if (generation != NULL)
		*generation = record->generation;
	pthread_mutex_unlock(&store->lock);
	status_ok(status);
	return DEFW2_OK;
}

size_t defw2_dir_store_scan(defw2_dir_store_t *store)
{
	defw2_dir_record_own_t *record, **link;
	uint64_t now, timeout_ns;
	size_t changed = 0;

	if (store == NULL)
		return 0;
	now = defw2_wall_ns();
	timeout_ns = (uint64_t)store->heartbeat_timeout_ms * 1000000ull;

	pthread_mutex_lock(&store->lock);
	for (record = store->records; record != NULL; record = record->next) {
		if (record->state != DEFW2_DIR_STATE_UP)
			continue;
		if (now < record->last_heartbeat_ns + timeout_ns)
			continue;
		record->state = DEFW2_DIR_STATE_TIMED_OUT;
		record->retention_deadline_ns = now +
			store->retention_ms * 1000000ull;
		changed++;
		defw2_log(store->rt, DEFW2_LOG_WARNING,
			  "directory: %s timed out, no heartbeat for %" PRIu64 " ms",
			  record->service_id,
			  (uint64_t)((now - record->last_heartbeat_ns) /
				     1000000u));
	}
	/*
	 * Then drop what retention has released. Done in the same pass so an
	 * operator never sees a record vanish and reappear, and kept separate
	 * from the timeout loop because a record timed out in this very scan
	 * must not also be dropped by it.
	 */
	link = &store->records;
	while (*link != NULL) {
		record = *link;
		if (record->retention_deadline_ns != 0 &&
		    now >= record->retention_deadline_ns) {
			*link = record->next;
			defw2_log(store->rt, DEFW2_LOG_DEBUG,
				  "directory: dropped %s after retention",
				  record->service_id);
			record->next = NULL;
			defw2_dir_record_own_free(record);
			changed++;
			continue;
		}
		link = &record->next;
	}
	if (changed > 0)
		snapshot_locked(store);
	pthread_mutex_unlock(&store->lock);
	return changed;
}

/* --- matching -------------------------------------------------------- */

static bool str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static bool in_list(char *const *list, size_t count, const char *want)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (str_eq(list[i], want))
			return true;
	}
	return false;
}

static const char *property_value(const defw2_dir_record_own_t *record,
				  const char *name)
{
	size_t i;

	for (i = 0; i < record->property_count; i++) {
		if (str_eq(record->properties[i].name, name))
			return record->properties[i].value;
	}
	return NULL;
}

/*
 * Compare one property. The directory attaches no meaning to the name: the
 * filter says how to compare, which is what keeps v1's hard-coded qpm_type
 * and qpm_capabilities bit tests out of here.
 *
 * A bitmask filter whose value or record value will not parse as an unsigned
 * integer fails the match rather than the request. A record carrying a
 * non-numeric value under a name someone bit-tests is a mismatch, not a
 * protocol error, and failing the whole resolve would let one bad record hide
 * every good one.
 */
static bool filter_matches(const defw2_dir_record_own_t *record,
			   const defw2_dir_filter_t *filter)
{
	const char *have = property_value(record, filter->name);
	unsigned long long want_bits, have_bits;
	char *end;

	if (have == NULL)
		return false;
	if (filter->match == DEFW2_DIR_MATCH_EQUAL)
		return str_eq(have, filter->value);
	if (filter->value == NULL)
		return false;

	want_bits = strtoull(filter->value, &end, 0);
	if (end == filter->value || *end != '\0')
		return false;
	have_bits = strtoull(have, &end, 0);
	if (end == have || *end != '\0')
		return false;
	if (filter->match == DEFW2_DIR_MATCH_BITS_ALL)
		return (have_bits & want_bits) == want_bits;
	/* BITS_ANY, and an empty mask matches nothing rather than everything. */
	return want_bits != 0 && (have_bits & want_bits) != 0;
}

/*
 * Which binding the query selected, or -1 when the record has none that fit.
 * A query naming no binding takes the first, which is the whole story for a
 * service that serves one API.
 */
static long select_binding(const defw2_dir_record_own_t *record,
			   const defw2_dir_query_t *query)
{
	size_t i;

	for (i = 0; i < record->binding_count; i++) {
		const defw2_dir_binding_own_t *b = &record->bindings[i];

		if (query->binding_name != NULL &&
		    !str_eq(b->binding_name, query->binding_name))
			continue;
		if (query->api_version != 0 &&
		    b->api_version != query->api_version)
			continue;
		return (long)i;
	}
	/*
	 * No bindings at all still matches a query that asked for nothing in
	 * particular, so a record can be found before its APIs are declared.
	 */
	if (record->binding_count == 0 && query->binding_name == NULL &&
	    query->api_version == 0)
		return -2;
	return -1;
}

static bool record_matches(const defw2_dir_record_own_t *record,
			   const defw2_dir_query_t *query)
{
	size_t i;

	if (!query->include_inactive && record->state != DEFW2_DIR_STATE_UP)
		return false;
	if (query->service_id != NULL &&
	    !str_eq(record->service_id, query->service_id))
		return false;
	if (query->service_type != NULL &&
	    !str_eq(record->service_type, query->service_type))
		return false;
	if (query->selector_name != NULL &&
	    !str_eq(record->selector_name, query->selector_name) &&
	    !in_list(record->aliases, record->alias_count,
		     query->selector_name))
		return false;
	if (query->resource != NULL &&
	    !in_list(record->resources, record->resource_count,
		     query->resource))
		return false;
	for (i = 0; i < query->filter_count; i++) {
		if (!filter_matches(record, &query->filters[i]))
			return false;
	}
	return true;
}

/* --- resolve --------------------------------------------------------- */

/* Copy an owned record into the arena as a public one. */
static bool publish(defw2_dir_arena_t *arena,
		    const defw2_dir_record_own_t *src, long binding_index,
		    defw2_dir_entry_t *entry)
{
	defw2_dir_record_t *out = &entry->record;
	size_t i;

	out->service_id = defw2_dir_arena_str(arena, src->service_id);
	out->service_type = defw2_dir_arena_str(arena, src->service_type);
	out->runtime_id = defw2_dir_arena_str(arena, src->runtime_id);
	out->address = defw2_dir_arena_str(arena, src->address);
	out->generation = src->generation;
	out->state = src->state;
	out->endpoint.node_name = defw2_dir_arena_str(arena, src->node_name);
	out->endpoint.hostname = defw2_dir_arena_str(arena, src->hostname);
	out->endpoint.pid = src->pid;
	out->registered_at_ns = src->registered_at_ns;
	out->last_heartbeat_ns = src->last_heartbeat_ns;
	out->retention_deadline_ns = src->retention_deadline_ns;
	if (!copied(src->service_id, out->service_id) ||
	    !copied(src->service_type, out->service_type) ||
	    !copied(src->runtime_id, out->runtime_id) ||
	    !copied(src->address, out->address) ||
	    !copied(src->node_name, out->endpoint.node_name) ||
	    !copied(src->hostname, out->endpoint.hostname))
		return false;

	if (src->binding_count > 0) {
		defw2_dir_binding_t *bindings =
			defw2_dir_arena_alloc(arena, src->binding_count *
					      sizeof(*bindings));

		if (bindings == NULL)
			return false;
		for (i = 0; i < src->binding_count; i++) {
			bindings[i].binding_name = defw2_dir_arena_str(
				arena, src->bindings[i].binding_name);
			bindings[i].api_id = defw2_dir_arena_str(
				arena, src->bindings[i].api_id);
			bindings[i].api_version = src->bindings[i].api_version;
			bindings[i].provider_id = src->bindings[i].provider_id;
			if (!copied(src->bindings[i].binding_name,
				    bindings[i].binding_name) ||
			    !copied(src->bindings[i].api_id,
				    bindings[i].api_id))
				return false;
		}
		out->bindings = bindings;
		out->binding_count = src->binding_count;
		if (binding_index >= 0)
			entry->binding = bindings[binding_index];
	}

	out->selector.name = defw2_dir_arena_str(arena, src->selector_name);
	if (!copied(src->selector_name, out->selector.name))
		return false;
	if (src->alias_count > 0) {
		const char **aliases = defw2_dir_arena_alloc(
			arena, src->alias_count * sizeof(*aliases));

		if (aliases == NULL)
			return false;
		for (i = 0; i < src->alias_count; i++) {
			aliases[i] = defw2_dir_arena_str(arena,
							 src->aliases[i]);
			if (!copied(src->aliases[i], aliases[i]))
				return false;
		}
		out->selector.aliases = aliases;
		out->selector.alias_count = src->alias_count;
	}
	if (src->resource_count > 0) {
		const char **resources = defw2_dir_arena_alloc(
			arena, src->resource_count * sizeof(*resources));

		if (resources == NULL)
			return false;
		for (i = 0; i < src->resource_count; i++) {
			resources[i] = defw2_dir_arena_str(
				arena, src->resources[i]);
			if (!copied(src->resources[i], resources[i]))
				return false;
		}
		out->selector.resources = resources;
		out->selector.resource_count = src->resource_count;
	}
	if (src->property_count > 0) {
		defw2_dir_property_t *props = defw2_dir_arena_alloc(
			arena, src->property_count * sizeof(*props));

		if (props == NULL)
			return false;
		for (i = 0; i < src->property_count; i++) {
			props[i].name = defw2_dir_arena_str(
				arena, src->properties[i].name);
			props[i].value = defw2_dir_arena_str(
				arena, src->properties[i].value);
			if (!copied(src->properties[i].name, props[i].name) ||
			    !copied(src->properties[i].value, props[i].value))
				return false;
		}
		out->properties = props;
		out->property_count = src->property_count;
	}
	return true;
}

defw2_rc_t defw2_dir_store_resolve(defw2_dir_store_t *store,
				   const defw2_dir_query_t *query,
				   defw2_dir_result_t *result,
				   defw2_status_t *status)
{
	return defw2_dir_store_resolve_indexed(store, query, result, NULL,
					       status);
}

defw2_rc_t defw2_dir_store_resolve_indexed(defw2_dir_store_t *store,
					   const defw2_dir_query_t *query,
					   defw2_dir_result_t *result,
					   uint32_t **selected,
					   defw2_status_t *status)
{
	static const defw2_dir_query_t match_all;
	defw2_dir_record_own_t *record;
	defw2_dir_arena_t *arena;
	defw2_dir_entry_t *entries = NULL;
	uint32_t *indices = NULL;
	size_t matched = 0, capacity = 0;
	defw2_rc_t rc = DEFW2_OK;

	if (store == NULL || result == NULL)
		return DEFW2_ERR_INVALID;
	if (query == NULL)
		query = &match_all;

	memset(result, 0, sizeof(*result));
	if (selected != NULL)
		*selected = NULL;
	arena = calloc(1, sizeof(*arena));
	if (arena == NULL)
		return DEFW2_ERR_NOMEM;

	pthread_mutex_lock(&store->lock);
	for (record = store->records; record != NULL; record = record->next) {
		long binding_index;

		if (!record_matches(record, query))
			continue;
		binding_index = select_binding(record, query);
		if (binding_index == -1)
			continue;
		if (query->limit > 0 && matched >= query->limit)
			break;
		if (matched == capacity) {
			size_t want = capacity ? capacity * 2 : 4;
			defw2_dir_entry_t *grown = realloc(
				entries, want * sizeof(*grown));
			uint32_t *grown_indices;

			if (grown == NULL) {
				rc = DEFW2_ERR_NOMEM;
				break;
			}
			memset(grown + capacity, 0,
			       (want - capacity) * sizeof(*grown));
			entries = grown;
			grown_indices = realloc(indices,
						want * sizeof(*grown_indices));
			if (grown_indices == NULL) {
				rc = DEFW2_ERR_NOMEM;
				break;
			}
			indices = grown_indices;
			capacity = want;
		}
		if (!publish(arena, record, binding_index, &entries[matched])) {
			rc = DEFW2_ERR_NOMEM;
			break;
		}
		indices[matched] = binding_index >= 0 ?
			(uint32_t)binding_index : DEFW2_DIR_NO_BINDING_INDEX;
		matched++;
	}
	pthread_mutex_unlock(&store->lock);

	if (rc != DEFW2_OK) {
		defw2_dir_arena_free(arena);
		free(arena);
		free(entries);
		free(indices);
		return rc;
	}
	result->entries = entries;
	result->entry_count = matched;
	result->arena = arena;
	if (selected != NULL)
		*selected = indices;
	else
		free(indices);
	status_ok(status);
	return DEFW2_OK;
}

/* --- snapshot -------------------------------------------------------- */

static void json_escaped(FILE *out, const char *s)
{
	fputc('"', out);
	for (; s != NULL && *s != '\0'; s++) {
		switch (*s) {
		case '"':
			fputs("\\\"", out);
			break;
		case '\\':
			fputs("\\\\", out);
			break;
		case '\n':
			fputs("\\n", out);
			break;
		case '\t':
			fputs("\\t", out);
			break;
		default:
			if ((unsigned char)*s < 0x20)
				fprintf(out, "\\u%04x", (unsigned char)*s);
			else
				fputc(*s, out);
		}
	}
	fputc('"', out);
}

/*
 * Write the whole store as JSON. Caller holds the lock.
 *
 * This is for operators and post-mortems and is never read back: a restarted
 * directory is a new generation of everything, which is the QFw design's
 * position. Written to a temporary and renamed so a reader never sees a
 * half-written file, and a failure is logged rather than propagated, because
 * losing the snapshot must not fail the registration that triggered it.
 */
static void snapshot_locked(defw2_dir_store_t *store)
{
	const defw2_dir_record_own_t *record;
	char temp[1024];
	FILE *out;
	bool first = true;
	size_t i;

	if (store->snapshot_path == NULL)
		return;
	if ((size_t)snprintf(temp, sizeof(temp), "%s.tmp",
			     store->snapshot_path) >= sizeof(temp)) {
		defw2_log(store->rt, DEFW2_LOG_WARNING,
			  "directory: snapshot path is too long to write");
		return;
	}
	out = fopen(temp, "w");
	if (out == NULL) {
		defw2_log(store->rt, DEFW2_LOG_WARNING,
			  "directory: cannot write snapshot %s", temp);
		return;
	}
	fprintf(out, "{\n  \"schema\": \"defw2-directory-v1\",\n");
	fprintf(out, "  \"written_at_ns\": %" PRIu64 ",\n", defw2_wall_ns());
	fprintf(out, "  \"records\": [");
	for (record = store->records; record != NULL; record = record->next) {
		fprintf(out, "%s\n    {", first ? "" : ",");
		first = false;
		fputs("\n      \"service_id\": ", out);
		json_escaped(out, record->service_id);
		fputs(",\n      \"service_type\": ", out);
		json_escaped(out, record->service_type);
		fputs(",\n      \"runtime_id\": ", out);
		json_escaped(out, record->runtime_id);
		fprintf(out, ",\n      \"generation\": %" PRIu64,
			record->generation);
		fputs(",\n      \"state\": ", out);
		json_escaped(out, defw2_dir_state_name(record->state));
		fputs(",\n      \"address\": ", out);
		json_escaped(out, record->address);
		fprintf(out, ",\n      \"pid\": %" PRId32, record->pid);
		fputs(",\n      \"hostname\": ", out);
		json_escaped(out, record->hostname);
		fprintf(out, ",\n      \"last_heartbeat_ns\": %" PRIu64,
			record->last_heartbeat_ns);
		fputs(",\n      \"bindings\": [", out);
		for (i = 0; i < record->binding_count; i++) {
			fprintf(out, "%s{\"binding_name\": ", i ? ", " : "");
			json_escaped(out, record->bindings[i].binding_name);
			fputs(", \"api_id\": ", out);
			json_escaped(out, record->bindings[i].api_id);
			fprintf(out,
				", \"api_version\": %" PRIu32
				", \"provider_id\": %" PRIu16 "}",
				record->bindings[i].api_version,
				record->bindings[i].provider_id);
		}
		fputs("],\n      \"properties\": {", out);
		for (i = 0; i < record->property_count; i++) {
			fputs(i ? ", " : "", out);
			json_escaped(out, record->properties[i].name);
			fputs(": ", out);
			json_escaped(out, record->properties[i].value);
		}
		fputs("}\n    }", out);
	}
	fprintf(out, "%s  ]\n}\n", first ? "" : "\n");
	if (fclose(out) != 0 || rename(temp, store->snapshot_path) != 0)
		defw2_log(store->rt, DEFW2_LOG_WARNING,
			  "directory: could not replace snapshot %s",
			  store->snapshot_path);
}

const char *defw2_dir_state_name(defw2_dir_state_t state)
{
	switch (state) {
	case DEFW2_DIR_STATE_UP:
		return "UP";
	case DEFW2_DIR_STATE_DOWN:
		return "DOWN";
	case DEFW2_DIR_STATE_TIMED_OUT:
		return "TIMED_OUT";
	case DEFW2_DIR_STATE_DEREGISTERED:
		return "DEREGISTERED";
	}
	return "UNKNOWN";
}
