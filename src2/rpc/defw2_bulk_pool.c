/*
 * The bulk buffer pool. See defw2_bulk_pool.h.
 *
 * One lock guards the lists and the counts, and nothing is allocated,
 * registered or freed while it is held, so a handler on a Margo stream
 * and a thread of the caller's can both take it without stalling either.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "defw2_internal.h"
#include "defw2_bulk_pool.h"

/* A buffer is a power of two from 64 KiB: class 0 is 2^16 bytes. */
#define DEFW2_BULK_MIN_SHIFT	16
#define DEFW2_BULK_CLASSES	48

struct defw2_bulk_entry {
	struct defw2_bulk_entry	*next;
	void			*data;
	hg_size_t		capacity;
	hg_bulk_t		bulk;
	int			cls;
};

struct defw2_bulk_pool {
	pthread_mutex_t		lock;
	struct defw2_bulk_entry	*idle[DEFW2_BULK_CLASSES];
	defw2_bulk_pool_stats_t	stats;
	bool			drained;
};

/* The class whose buffers hold size bytes, or -1 if none does. */
static int class_of(hg_size_t size)
{
	int cls = 0;

	while (cls < DEFW2_BULK_CLASSES &&
	       ((hg_size_t)1 << (cls + DEFW2_BULK_MIN_SHIFT)) < size)
		cls++;
	return cls < DEFW2_BULK_CLASSES ? cls : -1;
}

static hg_size_t class_size(int cls)
{
	return (hg_size_t)1 << (cls + DEFW2_BULK_MIN_SHIFT);
}

/*
 * A registered buffer of size bytes. One the pool keeps is aligned to a
 * page. One made for a single call comes from malloc, as every buffer did
 * before the pool: W3 measured glibc giving a 16 MiB aligned block fresh
 * pages on every call, where it reuses a plain one, and a runtime that
 * keeps no buffers should do as well as it did before.
 */
static defw2_rc_t make_buffer(struct defw2_rt *rt, hg_size_t size,
			      bool keep, void **data, hg_bulk_t *bulk)
{
	void *buffer = NULL;
	hg_size_t len = size;

	if (!keep)
		buffer = malloc(size);
	else if (posix_memalign(&buffer, 4096, size) != 0)
		buffer = NULL;
	if (buffer == NULL)
		return DEFW2_ERR_NOMEM;
	if (margo_bulk_create(rt->mid, 1, &buffer, &len, HG_BULK_READWRITE,
			      bulk) != HG_SUCCESS) {
		free(buffer);
		return DEFW2_ERR_TRANSPORT;
	}
	*data = buffer;
	return DEFW2_OK;
}

static void free_entries(struct defw2_bulk_entry *entry)
{
	struct defw2_bulk_entry *next;

	for (; entry != NULL; entry = next) {
		next = entry->next;
		margo_bulk_free(entry->bulk);
		free(entry->data);
		free(entry);
	}
}

/*
 * Takes idle buffers off the lists, the largest first, until capacity more
 * fits the budget, and returns them to be freed once the lock is dropped.
 * The caller has checked that freeing every idle buffer would be enough.
 */
static struct defw2_bulk_entry *evict(struct defw2_bulk_pool *pool,
				      hg_size_t capacity)
{
	struct defw2_bulk_entry *victims = NULL, *entry;
	int cls = DEFW2_BULK_CLASSES - 1;

	while (pool->stats.held + capacity > pool->stats.budget && cls >= 0) {
		entry = pool->idle[cls];
		if (entry == NULL) {
			cls--;
			continue;
		}
		pool->idle[cls] = entry->next;
		pool->stats.held -= entry->capacity;
		pool->stats.idle -= entry->capacity;
		pool->stats.evicted++;
		entry->next = victims;
		victims = entry;
	}
	return victims;
}

struct defw2_bulk_pool *defw2_bulk_pool_create(uint64_t budget)
{
	struct defw2_bulk_pool *pool = calloc(1, sizeof(*pool));

	if (pool == NULL)
		return NULL;
	pthread_mutex_init(&pool->lock, NULL);
	pool->stats.budget = budget;
	return pool;
}

defw2_rc_t defw2_bulk_get(struct defw2_rt *rt, hg_size_t size,
			  defw2_bulk_buf_t *buf)
{
	struct defw2_bulk_entry *entry = NULL, *victims = NULL;
	struct defw2_bulk_pool *pool;
	hg_size_t capacity = 0;
	bool keep = false;
	defw2_rc_t rc;
	int cls;

	if (buf != NULL)
		memset(buf, 0, sizeof(*buf));
	if (rt == NULL || rt->bulk_pool == NULL || buf == NULL || size == 0)
		return DEFW2_ERR_INVALID;
	pool = rt->bulk_pool;
	cls = class_of(size);
	if (cls >= 0)
		capacity = class_size(cls);

	pthread_mutex_lock(&pool->lock);
	if (cls >= 0 && pool->idle[cls] != NULL) {
		entry = pool->idle[cls];
		pool->idle[cls] = entry->next;
		pool->stats.idle -= entry->capacity;
		pool->stats.reused++;
	} else if (cls >= 0 && !pool->drained &&
		   capacity <= pool->stats.budget &&
		   pool->stats.held - pool->stats.idle + capacity <=
			   pool->stats.budget) {
		victims = evict(pool, capacity);
		pool->stats.held += capacity;
		pool->stats.made++;
		keep = true;
	} else {
		pool->stats.bypassed++;
	}
	pthread_mutex_unlock(&pool->lock);
	free_entries(victims);

	if (entry == NULL && keep) {
		entry = calloc(1, sizeof(*entry));
		rc = entry != NULL ? make_buffer(rt, capacity, true,
						 &entry->data, &entry->bulk)
				   : DEFW2_ERR_NOMEM;
		if (rc != DEFW2_OK) {
			free(entry);
			pthread_mutex_lock(&pool->lock);
			pool->stats.held -= capacity;
			pool->stats.made--;
			pthread_mutex_unlock(&pool->lock);
			return rc;
		}
		entry->capacity = capacity;
		entry->cls = cls;
	}
	if (entry != NULL) {
		entry->next = NULL;
		buf->data = entry->data;
		buf->capacity = entry->capacity;
		buf->bulk = entry->bulk;
		buf->entry = entry;
		return DEFW2_OK;
	}

	/* Made for this call alone, exactly the size asked for. */
	rc = make_buffer(rt, size, false, &buf->data, &buf->bulk);
	if (rc == DEFW2_OK)
		buf->capacity = size;
	return rc;
}

void defw2_bulk_put(struct defw2_rt *rt, defw2_bulk_buf_t *buf)
{
	struct defw2_bulk_entry *entry;
	struct defw2_bulk_pool *pool;

	if (buf == NULL || buf->data == NULL)
		return;
	entry = buf->entry;
	if (entry == NULL) {
		margo_bulk_free(buf->bulk);
		free(buf->data);
		memset(buf, 0, sizeof(*buf));
		return;
	}
	if (rt == NULL || rt->bulk_pool == NULL)
		return;
	pool = rt->bulk_pool;
	memset(buf, 0, sizeof(*buf));

	/*
	 * Back after the pool was drained, which only a caller that held a
	 * buffer across defw2_finalize can do, the buffer is left as it is:
	 * Margo may be gone, and freeing its handle would reach it.
	 */
	pthread_mutex_lock(&pool->lock);
	if (!pool->drained) {
		entry->next = pool->idle[entry->cls];
		pool->idle[entry->cls] = entry;
		pool->stats.idle += entry->capacity;
	}
	pthread_mutex_unlock(&pool->lock);
}

void defw2_bulk_pool_stats(struct defw2_rt *rt,
			   defw2_bulk_pool_stats_t *out)
{
	if (out == NULL)
		return;
	memset(out, 0, sizeof(*out));
	if (rt == NULL || rt->bulk_pool == NULL)
		return;
	pthread_mutex_lock(&rt->bulk_pool->lock);
	*out = rt->bulk_pool->stats;
	pthread_mutex_unlock(&rt->bulk_pool->lock);
}

void defw2_bulk_pool_drain(struct defw2_rt *rt)
{
	struct defw2_bulk_entry *idle = NULL, *entry;
	struct defw2_bulk_pool *pool;
	uint64_t lent;
	int cls;

	if (rt == NULL || rt->bulk_pool == NULL)
		return;
	pool = rt->bulk_pool;
	pthread_mutex_lock(&pool->lock);
	pool->drained = true;
	for (cls = 0; cls < DEFW2_BULK_CLASSES; cls++) {
		while ((entry = pool->idle[cls]) != NULL) {
			pool->idle[cls] = entry->next;
			entry->next = idle;
			idle = entry;
		}
	}
	pool->stats.held -= pool->stats.idle;
	pool->stats.idle = 0;
	lent = pool->stats.held;
	pthread_mutex_unlock(&pool->lock);
	free_entries(idle);
	if (lent != 0)
		defw2_log(rt, DEFW2_LOG_WARNING,
			  "%llu bytes of bulk buffers were still lent when "
			  "the runtime stopped", (unsigned long long)lent);
}

void defw2_bulk_pool_destroy(struct defw2_bulk_pool *pool)
{
	if (pool == NULL)
		return;
	pthread_mutex_destroy(&pool->lock);
	free(pool);
}
