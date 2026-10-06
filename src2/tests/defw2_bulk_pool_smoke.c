/*
 * Does the bulk pool lend its buffers again and keep to its budget?
 *
 * One process. Runtimes with small budgets show how the pool decides. It
 * uses na+sm, so it needs no network and no port.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defw2_internal.h"
#include "defw2_bulk_pool.h"

#define KIB		((hg_size_t)1024)
#define MIB		(1024 * KIB)
#define THREADS		8
#define ROUNDS		300

static int failures;

static int check(const char *what, bool ok)
{
	printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
	return ok ? 0 : 1;
}

/* A runtime whose pool holds pool_mib, or the default when it is -1. */
static defw2_rt_t *runtime(int pool_mib, defw2_role_t role)
{
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;

	defw2_config_from_env(&cfg);
	cfg.role = role;
	if (pool_mib >= 0) {
		cfg.has_bulk_pool_mib = true;
		cfg.bulk_pool_mib = pool_mib;
	}
	if (defw2_init(&cfg, &rt) != DEFW2_OK) {
		printf("runtime init FAILED\n");
		exit(1);
	}
	return rt;
}

static defw2_bulk_pool_stats_t stats_of(defw2_rt_t *rt)
{
	defw2_bulk_pool_stats_t stats;

	defw2_bulk_pool_stats(rt, &stats);
	return stats;
}

static void lending(void)
{
	defw2_rt_t *rt = runtime(8, DEFW2_ROLE_CLIENT);
	defw2_bulk_buf_t one, two;
	defw2_bulk_pool_stats_t stats;
	void *first;

	check("a buffer is lent, registered",
	      defw2_bulk_get(rt, MIB, &one) == DEFW2_OK && one.data != NULL &&
		      one.bulk != HG_BULK_NULL && one.entry != NULL &&
		      one.capacity == MIB);
	check("a size between powers of two gets the next one",
	      defw2_bulk_get(rt, MIB + 1, &two) == DEFW2_OK &&
		      two.capacity == 2 * MIB);
	first = one.data;
	defw2_bulk_put(rt, &one);
	defw2_bulk_put(rt, &two);
	check("put clears what it was given", one.data == NULL);
	check("the buffer is lent again once returned",
	      defw2_bulk_get(rt, MIB, &one) == DEFW2_OK && one.data == first);
	defw2_bulk_put(rt, &one);
	check("and to a smaller request of its size",
	      defw2_bulk_get(rt, 600 * KIB, &one) == DEFW2_OK &&
		      one.data == first);
	defw2_bulk_put(rt, &one);
	stats = stats_of(rt);
	check("two made, two lent again",
	      stats.made == 2 && stats.reused == 2 && stats.bypassed == 0);
	check("all of it idle once returned",
	      stats.held == 3 * MIB && stats.idle == stats.held);
	check("a zero size is refused",
	      defw2_bulk_get(rt, 0, &one) == DEFW2_ERR_INVALID);
	defw2_finalize(rt);
}

static void budget(void)
{
	defw2_rt_t *rt = runtime(4, DEFW2_ROLE_CLIENT);
	defw2_bulk_buf_t x, y, z;
	defw2_bulk_pool_stats_t stats;

	defw2_bulk_get(rt, 2 * MIB, &x);
	defw2_bulk_get(rt, 2 * MIB, &y);
	check("past the budget a buffer is made for its one call",
	      defw2_bulk_get(rt, 2 * MIB, &z) == DEFW2_OK && z.entry == NULL &&
		      z.data != NULL && z.capacity == 2 * MIB);
	stats = stats_of(rt);
	check("and the pool still holds only its budget",
	      stats.held == 4 * MIB && stats.bypassed == 1);
	defw2_bulk_put(rt, &z);
	defw2_bulk_put(rt, &x);
	defw2_bulk_put(rt, &y);

	check("a larger size frees idle buffers to fit",
	      defw2_bulk_get(rt, 4 * MIB, &x) == DEFW2_OK && x.entry != NULL);
	stats = stats_of(rt);
	check("both 2 MiB buffers went for it",
	      stats.evicted == 2 && stats.held == 4 * MIB && stats.idle == 0);
	check("a size beyond the budget is made for its one call",
	      defw2_bulk_get(rt, 8 * MIB, &y) == DEFW2_OK && y.entry == NULL);
	defw2_bulk_put(rt, &y);
	defw2_bulk_put(rt, &x);
	defw2_finalize(rt);

	rt = runtime(0, DEFW2_ROLE_CLIENT);
	check("with no budget every buffer is for one call",
	      defw2_bulk_get(rt, 64 * KIB, &x) == DEFW2_OK && x.entry == NULL);
	defw2_bulk_put(rt, &x);
	stats = stats_of(rt);
	check("and the pool holds nothing", stats.held == 0 && stats.made == 0);
	defw2_finalize(rt);

	rt = runtime(-1, DEFW2_ROLE_CLIENT);
	check("the default budget is 1024 MiB",
	      stats_of(rt).budget == 1024 * MIB);
	defw2_finalize(rt);
}

static void *borrower(void *arg)
{
	defw2_rt_t *rt = arg;
	defw2_bulk_buf_t buf;
	long bad = 0;
	int i;

	for (i = 0; i < ROUNDS; i++) {
		hg_size_t size = (64 * KIB) << (i % 6);
		unsigned char *bytes;

		if (defw2_bulk_get(rt, size, &buf) != DEFW2_OK ||
		    buf.capacity < size) {
			bad++;
			continue;
		}
		bytes = buf.data;
		bytes[0] = (unsigned char)i;
		bytes[size - 1] = (unsigned char)i;
		defw2_bulk_put(rt, &buf);
	}
	return (void *)bad;
}

static void threads(void)
{
	defw2_rt_t *rt = runtime(8, DEFW2_ROLE_CLIENT);
	defw2_bulk_pool_stats_t stats;
	pthread_t thread[THREADS];
	long bad = 0;
	void *result;
	int i;

	for (i = 0; i < THREADS; i++)
		pthread_create(&thread[i], NULL, borrower, rt);
	for (i = 0; i < THREADS; i++) {
		pthread_join(thread[i], &result);
		bad += (long)result;
	}
	stats = stats_of(rt);
	check("eight threads lend and return at once", bad == 0);
	check("within the budget, every buffer back",
	      stats.held <= 8 * MIB && stats.idle == stats.held);
	check("and lends reused buffers",
	      stats.reused > (uint64_t)THREADS * ROUNDS / 4);
	defw2_finalize(rt);
}

static void config(void)
{
	defw2_config_t cfg;

	setenv("DEFW2_BULK_POOL_MIB", "64", 1);
	check("DEFW2_BULK_POOL_MIB sets the budget",
	      defw2_config_from_env(&cfg) == DEFW2_OK &&
		      cfg.has_bulk_pool_mib && cfg.bulk_pool_mib == 64);
	setenv("DEFW2_BULK_POOL_MIB", "lots", 1);
	check("and one that is not a count is refused",
	      defw2_config_from_env(&cfg) == DEFW2_ERR_CONFIG);
	unsetenv("DEFW2_BULK_POOL_MIB");
	check("unset, the runtime chooses",
	      defw2_config_from_env(&cfg) == DEFW2_OK &&
		      !cfg.has_bulk_pool_mib);
}

int main(void)
{
	lending();
	budget();
	threads();
	config();
	printf("%s\n", failures ? "BULK POOL SMOKE FAILED"
				: "BULK POOL SMOKE PASSED");
	return failures ? 1 : 0;
}
