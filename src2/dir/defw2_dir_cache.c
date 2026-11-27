/*
 * The client binding cache.
 *
 * Resolving before every call would put the directory on the hot path of
 * every RPC in the deployment, which is the opposite of what a directory is
 * for. So a query is resolved once and the binding kept.
 *
 * Keeping it is the easy half. Knowing when to stop is the other, and the
 * only thing that can tell is the caller: Mercury reports failures per call,
 * not per peer, so nothing inside here learns that a service went away. A
 * client whose forward fails reports it and the next lookup resolves again.
 *
 * Entries are a linked list under one mutex. A client holds a handful of
 * these, one per service it talks to, so a scan is cheaper than anything with
 * a hash in it, which is the same reasoning the store uses.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_dir_internal.h"

struct defw2_dir_cache_entry {
	/* The query, deep copied, because re-resolving needs it later. */
	defw2_dir_query_t		query;
	char				*service_id;
	char				*service_type;
	char				*selector_name;
	char				*resource;
	char				*binding_name;
	defw2_dir_filter_t		*filters;
	char				**filter_strings;
	size_t				filter_string_count;
	/* What it resolved to. */
	char				*resolved_service_id;
	uint64_t			generation;
	defw2_binding_t			*binding;
	struct defw2_dir_cache_entry	*next;
};

struct defw2_dir_cache {
	defw2_dir_t			*dir;
	struct defw2_rt			*rt;
	struct defw2_dir_cache_entry	*entries;
	size_t				count;
	pthread_mutex_t			lock;
};

static bool same_str(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a, b) == 0;
}

/* Every field, so passing the same query finds the same entry. */
static bool same_query(const defw2_dir_query_t *a,
		       const defw2_dir_query_t *b)
{
	size_t i;

	if (!same_str(a->service_id, b->service_id) ||
	    !same_str(a->service_type, b->service_type) ||
	    !same_str(a->selector_name, b->selector_name) ||
	    !same_str(a->resource, b->resource) ||
	    !same_str(a->binding_name, b->binding_name) ||
	    a->api_version != b->api_version ||
	    a->include_inactive != b->include_inactive ||
	    a->limit != b->limit ||
	    a->filter_count != b->filter_count)
		return false;
	for (i = 0; i < a->filter_count; i++) {
		if (!same_str(a->filters[i].name, b->filters[i].name) ||
		    !same_str(a->filters[i].value, b->filters[i].value) ||
		    a->filters[i].match != b->filters[i].match)
			return false;
	}
	return true;
}

static void entry_free(struct defw2_dir_cache_entry *entry)
{
	size_t i;

	if (entry == NULL)
		return;
	defw2_binding_free(entry->binding);
	free(entry->service_id);
	free(entry->service_type);
	free(entry->selector_name);
	free(entry->resource);
	free(entry->binding_name);
	for (i = 0; i < entry->filter_string_count; i++)
		free(entry->filter_strings[i]);
	free(entry->filter_strings);
	free(entry->filters);
	free(entry->resolved_service_id);
	free(entry);
}

/*
 * Copy a string into the entry and record it for the free. Returns false only
 * on allocation failure, so an absent field is a NULL and a success.
 */
static bool keep_str(const char *src, char **slot)
{
	if (src == NULL) {
		*slot = NULL;
		return true;
	}
	*slot = strdup(src);
	return *slot != NULL;
}

static struct defw2_dir_cache_entry *entry_new(const defw2_dir_query_t *query)
{
	struct defw2_dir_cache_entry *entry;
	size_t i;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;
	if (!keep_str(query->service_id, &entry->service_id) ||
	    !keep_str(query->service_type, &entry->service_type) ||
	    !keep_str(query->selector_name, &entry->selector_name) ||
	    !keep_str(query->resource, &entry->resource) ||
	    !keep_str(query->binding_name, &entry->binding_name))
		goto fail;

	if (query->filter_count > 0) {
		entry->filters = calloc(query->filter_count,
					sizeof(*entry->filters));
		/* Two strings per filter, freed through this one array. */
		entry->filter_strings = calloc(query->filter_count * 2,
					       sizeof(*entry->filter_strings));
		if (entry->filters == NULL || entry->filter_strings == NULL)
			goto fail;
		entry->filter_string_count = query->filter_count * 2;
		for (i = 0; i < query->filter_count; i++) {
			char **name = &entry->filter_strings[i * 2];
			char **value = &entry->filter_strings[i * 2 + 1];

			if (!keep_str(query->filters[i].name, name) ||
			    !keep_str(query->filters[i].value, value))
				goto fail;
			entry->filters[i].name = *name;
			entry->filters[i].value = *value;
			entry->filters[i].match = query->filters[i].match;
		}
	}

	entry->query = *query;
	entry->query.service_id = entry->service_id;
	entry->query.service_type = entry->service_type;
	entry->query.selector_name = entry->selector_name;
	entry->query.resource = entry->resource;
	entry->query.binding_name = entry->binding_name;
	entry->query.filters = entry->filters;
	return entry;

fail:
	entry_free(entry);
	return NULL;
}

defw2_rc_t defw2_dir_cache_create(defw2_dir_t *dir, defw2_dir_cache_t **out)
{
	defw2_dir_cache_t *cache;

	if (dir == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	cache = calloc(1, sizeof(*cache));
	if (cache == NULL)
		return DEFW2_ERR_NOMEM;
	if (pthread_mutex_init(&cache->lock, NULL) != 0) {
		free(cache);
		return DEFW2_ERR_INTERNAL;
	}
	cache->dir = dir;
	*out = cache;
	return DEFW2_OK;
}

void defw2_dir_cache_destroy(defw2_dir_cache_t *cache)
{
	struct defw2_dir_cache_entry *entry;

	if (cache == NULL)
		return;
	entry = cache->entries;
	while (entry != NULL) {
		struct defw2_dir_cache_entry *next = entry->next;

		entry_free(entry);
		entry = next;
	}
	pthread_mutex_destroy(&cache->lock);
	free(cache);
}

/* Caller holds the lock. */
static struct defw2_dir_cache_entry *find_locked(defw2_dir_cache_t *cache,
						 const defw2_dir_query_t *query)
{
	struct defw2_dir_cache_entry *entry;

	for (entry = cache->entries; entry != NULL; entry = entry->next) {
		if (same_query(&entry->query, query))
			return entry;
	}
	return NULL;
}

/* Caller holds the lock. Unlinks and frees. */
static void drop_locked(defw2_dir_cache_t *cache,
			struct defw2_dir_cache_entry *target)
{
	struct defw2_dir_cache_entry **link = &cache->entries;

	while (*link != NULL) {
		if (*link == target) {
			*link = target->next;
			target->next = NULL;
			cache->count--;
			entry_free(target);
			return;
		}
		link = &(*link)->next;
	}
}

defw2_rc_t defw2_dir_cache_binding(defw2_dir_cache_t *cache,
				   const defw2_dir_query_t *query,
				   const defw2_call_opts_t *opts,
				   defw2_binding_t **out,
				   defw2_status_t *status)
{
	static const defw2_dir_query_t match_all;
	struct defw2_dir_cache_entry *entry;
	defw2_dir_result_t result;
	defw2_binding_t *binding = NULL;
	char *resolved_id = NULL;
	uint64_t generation;
	defw2_rc_t rc;

	if (cache == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	if (query == NULL)
		query = &match_all;
	*out = NULL;

	pthread_mutex_lock(&cache->lock);
	entry = find_locked(cache, query);
	if (entry != NULL && entry->binding != NULL) {
		*out = entry->binding;
		pthread_mutex_unlock(&cache->lock);
		return DEFW2_OK;
	}
	pthread_mutex_unlock(&cache->lock);

	/*
	 * Resolve outside the lock. A resolve is a round trip, and holding the
	 * cache's lock across it would make every other thread's cache hit
	 * wait on the network.
	 */
	memset(&result, 0, sizeof(result));
	rc = defw2_dir_resolve(cache->dir, query, opts, &result, status);
	if (rc != DEFW2_OK) {
		defw2_dir_result_free(&result);
		return rc;
	}
	if (result.entry_count == 0) {
		/*
		 * Nothing serves this yet. Not cached: remembering an absence
		 * would mean never noticing the service arriving, and a caller
		 * retrying is the expected shape during startup.
		 */
		defw2_dir_result_free(&result);
		return DEFW2_ERR_NOT_FOUND;
	}

	/*
	 * The first record. A query that matches several and cares which
	 * should narrow itself; choosing here would be a scheduling policy,
	 * and the directory keeps none.
	 */
	if (result.entries[0].record.address == NULL) {
		defw2_dir_result_free(&result);
		return DEFW2_ERR_NOT_FOUND;
	}
	rc = defw2_binding_create(defw2_dir_runtime(cache->dir),
				  result.entries[0].record.address,
				  result.entries[0].binding.provider_id,
				  &binding);
	generation = result.entries[0].record.generation;
	if (rc == DEFW2_OK && result.entries[0].record.service_id != NULL) {
		resolved_id = strdup(result.entries[0].record.service_id);
		if (resolved_id == NULL) {
			defw2_binding_free(binding);
			defw2_dir_result_free(&result);
			return DEFW2_ERR_NOMEM;
		}
	}
	defw2_dir_result_free(&result);
	if (rc != DEFW2_OK) {
		free(resolved_id);
		return rc;
	}

	pthread_mutex_lock(&cache->lock);
	/*
	 * Another thread may have resolved the same query while this one was
	 * on the network. Keep theirs and drop this one, so every caller of a
	 * query shares one binding and the count means what it says.
	 */
	entry = find_locked(cache, query);
	if (entry != NULL && entry->binding != NULL) {
		*out = entry->binding;
		pthread_mutex_unlock(&cache->lock);
		defw2_binding_free(binding);
		free(resolved_id);
		return DEFW2_OK;
	}
	if (entry == NULL) {
		entry = entry_new(query);
		if (entry == NULL) {
			pthread_mutex_unlock(&cache->lock);
			defw2_binding_free(binding);
			free(resolved_id);
			return DEFW2_ERR_NOMEM;
		}
		entry->next = cache->entries;
		cache->entries = entry;
		cache->count++;
	}
	entry->binding = binding;
	free(entry->resolved_service_id);
	entry->resolved_service_id = resolved_id;
	entry->generation = generation;
	*out = binding;
	pthread_mutex_unlock(&cache->lock);
	return DEFW2_OK;
}

void defw2_dir_cache_failed(defw2_dir_cache_t *cache,
			    const defw2_binding_t *binding)
{
	struct defw2_dir_cache_entry *entry;

	if (cache == NULL || binding == NULL)
		return;
	pthread_mutex_lock(&cache->lock);
	for (entry = cache->entries; entry != NULL; entry = entry->next) {
		if (entry->binding != binding)
			continue;
		/*
		 * The whole entry goes, not just its binding. The address was
		 * what the entry knew, and a query whose answer is gone has
		 * nothing worth keeping.
		 */
		drop_locked(cache, entry);
		break;
	}
	pthread_mutex_unlock(&cache->lock);
}

void defw2_dir_cache_clear(defw2_dir_cache_t *cache)
{
	struct defw2_dir_cache_entry *entry;

	if (cache == NULL)
		return;
	pthread_mutex_lock(&cache->lock);
	entry = cache->entries;
	cache->entries = NULL;
	cache->count = 0;
	pthread_mutex_unlock(&cache->lock);
	while (entry != NULL) {
		struct defw2_dir_cache_entry *next = entry->next;

		entry_free(entry);
		entry = next;
	}
}

size_t defw2_dir_cache_revalidate(defw2_dir_cache_t *cache,
				  const defw2_call_opts_t *opts)
{
	struct defw2_dir_cache_entry *entry;
	char **ids = NULL;
	uint64_t *generations = NULL;
	size_t count = 0, i, dropped = 0;

	if (cache == NULL)
		return 0;

	/*
	 * Take a snapshot of what to ask about, then ask outside the lock, for
	 * the reason the resolve is outside it. The ids are copied because the
	 * entries they came from may be gone by the time the answers arrive.
	 */
	pthread_mutex_lock(&cache->lock);
	if (cache->count > 0) {
		ids = calloc(cache->count, sizeof(*ids));
		generations = calloc(cache->count, sizeof(*generations));
	}
	if (ids != NULL && generations != NULL) {
		for (entry = cache->entries; entry != NULL;
		     entry = entry->next) {
			if (entry->resolved_service_id == NULL)
				continue;
			ids[count] = strdup(entry->resolved_service_id);
			if (ids[count] == NULL)
				break;
			generations[count] = entry->generation;
			count++;
		}
	}
	pthread_mutex_unlock(&cache->lock);

	for (i = 0; i < count; i++) {
		defw2_status_t status;
		uint64_t now = 0;

		memset(&status, 0, sizeof(status));
		if (defw2_dir_generation(cache->dir, ids[i], opts, &now,
					 &status) == DEFW2_OK &&
		    status.code == DEFW2_OK && now == generations[i]) {
			defw2_status_free(&status);
			continue;
		}
		/*
		 * A changed generation means a restart, and an unreachable or
		 * unknown service means there is nothing to keep either, so
		 * both drop the entry. The cost of being wrong is one
		 * re-resolve.
		 */
		defw2_status_free(&status);
		pthread_mutex_lock(&cache->lock);
		for (entry = cache->entries; entry != NULL;
		     entry = entry->next) {
			if (entry->resolved_service_id != NULL &&
			    strcmp(entry->resolved_service_id, ids[i]) == 0) {
				drop_locked(cache, entry);
				dropped++;
				break;
			}
		}
		pthread_mutex_unlock(&cache->lock);
	}

	for (i = 0; i < count; i++)
		free(ids[i]);
	free(ids);
	free(generations);
	return dropped;
}

size_t defw2_dir_cache_size(const defw2_dir_cache_t *cache)
{
	size_t count;

	if (cache == NULL)
		return 0;
	pthread_mutex_lock((pthread_mutex_t *)&cache->lock);
	count = cache->count;
	pthread_mutex_unlock((pthread_mutex_t *)&cache->lock);
	return count;
}
