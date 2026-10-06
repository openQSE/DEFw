/*
 * Registered buffers for bulk transfers, kept for reuse.
 *
 * A buffer allocated for each transfer can cost more than the transfer.
 * glibc maps a block over 32 MiB afresh every time, so each call faults in
 * and zeroes new pages and unmaps them after, and on a fabric such as
 * Slingshot registering memory pins it. W3 moved 256 MiB 18% to 20%
 * faster with its buffer kept between calls.
 *
 * So a runtime keeps the buffers it lends, registered, and lends them
 * again. A buffer is the size asked for rounded up to a power of two, at
 * least 64 KiB, and the pool holds at most its budget, lent or idle,
 * DEFW2_BULK_POOL_MIB. To fit a new buffer it frees idle ones, and when
 * even that would not fit it, the buffer is made for the one call and
 * freed after it, as before the pool. Idle buffers stay until
 * defw2_finalize.
 *
 * A lent buffer holds whatever its last user left in it, so a caller
 * writes every byte it sends.
 */
#ifndef DEFW2_BULK_POOL_H
#define DEFW2_BULK_POOL_H

#include <stdint.h>

#include <margo.h>

#include <defw2/defw2.h>

/* The budget when DEFW2_BULK_POOL_MIB does not set one. */
#define DEFW2_BULK_POOL_DEFAULT_MIB	1024

struct defw2_rt;
struct defw2_bulk_pool;
struct defw2_bulk_entry;

/* A lent buffer, registered for reading and writing. */
typedef struct {
	void			*data;
	hg_size_t		capacity;	/* at least the size asked */
	hg_bulk_t		bulk;
	struct defw2_bulk_entry	*entry;		/* NULL if made for one call */
} defw2_bulk_buf_t;

typedef struct {
	uint64_t	budget;		/* bytes the pool may hold */
	uint64_t	held;		/* bytes it holds, lent or idle */
	uint64_t	idle;		/* of those, bytes not lent */
	uint64_t	reused;		/* lends of a buffer it already had */
	uint64_t	made;		/* buffers it made to keep */
	uint64_t	evicted;	/* idle buffers freed to fit another */
	uint64_t	bypassed;	/* buffers made for one call */
} defw2_bulk_pool_stats_t;

struct defw2_bulk_pool *defw2_bulk_pool_create(uint64_t budget);

/*
 * Lends buf at least size bytes. Every buffer lent goes back through
 * defw2_bulk_put, from any thread.
 */
defw2_rc_t defw2_bulk_get(struct defw2_rt *rt, hg_size_t size,
			  defw2_bulk_buf_t *buf);
void defw2_bulk_put(struct defw2_rt *rt, defw2_bulk_buf_t *buf);

void defw2_bulk_pool_stats(struct defw2_rt *rt,
			   defw2_bulk_pool_stats_t *out);

/*
 * Frees the idle buffers and keeps no more. defw2_finalize calls it once
 * Margo's last handler has finished and before the instance is released,
 * while the bulk handles can still be freed.
 */
void defw2_bulk_pool_drain(struct defw2_rt *rt);
void defw2_bulk_pool_destroy(struct defw2_bulk_pool *pool);

#endif
