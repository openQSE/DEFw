/*
 * Internals shared inside libdefw2. Not installed.
 */
#ifndef DEFW2_INTERNAL_H
#define DEFW2_INTERNAL_H

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include <margo.h>

#include <defw2/defw2.h>

/* The settings a process gets when the environment says nothing. */
#define DEFW2_DEFAULT_ADDRESS		"ofi+tcp://"
#define DEFW2_DEFAULT_RPC_THREADS	2

/* 36 characters and the terminator. */
#define DEFW2_UUID_STR_LEN	37
#define DEFW2_NAME_MAX		256

/*
 * Room for every RPC one process calls or serves. The design's typed
 * tier names two dozen methods, so a linear scan of this is cheaper than
 * anything with a hash in it.
 */
#define DEFW2_RPC_CACHE_MAX	64

struct defw2_rpc_entry {
	char		*name;		/* owned, freed in defw2_finalize */
	hg_id_t		id;
};

struct defw2_telemetry;

struct defw2_rt {
	margo_instance_id	mid;
	char			runtime_id[DEFW2_UUID_STR_LEN];
	char			*address;	/* owned */
	char			*dirsvc;	/* owned, may be NULL */
	char			node_name[DEFW2_NAME_MAX];
	char			hostname[DEFW2_NAME_MAX];
	int			pid;
	uint64_t		correlation;	/* the next RPC correlation id */
	int			stopping;	/* set once, by whoever stops first */
	defw2_role_t		role;
	defw2_log_level_t	log_level;
	bool			profile;
	FILE			*log_stream;	/* stderr, or an owned file */
	bool			log_owned;
	pthread_mutex_t		log_lock;
	/*
	 * A memo of what this runtime has registered, so a lookup on the hot
	 * path is a scan of a short array rather than a call into Margo.
	 *
	 * It is a cache, not the source of truth. defw2_rpc_lookup asks Margo
	 * through margo_provider_registered_name when a name is not in here,
	 * because a provider registration made elsewhere in the process never
	 * passes through this table and must not be overwritten.
	 */
	struct defw2_rpc_entry	rpc_cache[DEFW2_RPC_CACHE_MAX];
	int			rpc_cached;
	pthread_mutex_t		rpc_lock;
	struct defw2_telemetry	*telemetry;	/* NULL when nothing records */
};

/*
 * Stop the Margo instance, exactly once however many callers ask. A
 * service shutting itself down and defw2_finalize both come through
 * here, so neither has to know whether the other ran first.
 */
void defw2_runtime_stop(struct defw2_rt *rt);

/* The wall clock, which is what a cross-process timestamp has to use. */
uint64_t defw2_wall_ns(void);

defw2_rc_t defw2_log_open(struct defw2_rt *rt, const char *log_dir);
void defw2_log_close(struct defw2_rt *rt);
void defw2_log_emit(const struct defw2_rt *rt, defw2_log_level_t level,
		    const char *fmt, va_list args);

/*
 * Send Margo's own messages to the runtime's sink, so one file tells the
 * whole story. Call once the Margo instance exists.
 */
void defw2_log_attach_margo(struct defw2_rt *rt);

#endif /* DEFW2_INTERNAL_H */
