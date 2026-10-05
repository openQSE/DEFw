/*
 * DEFw v2 runtime: joining, identity, status and logging.
 *
 * A process joins by filling a config, usually from the environment, and
 * calling defw2_init. That starts one Margo instance, generates the runtime
 * identity and installs the logging sink. Everything else in libdefw2 takes
 * the runtime handle this produces.
 *
 *	defw2_config_t cfg;
 *	defw2_rt_t *rt = NULL;
 *
 *	defw2_config_from_env(&cfg);
 *	if (defw2_init(&cfg, &rt) != DEFW2_OK)
 *		return 1;
 *	...
 *	defw2_finalize(rt);
 *
 * The handle is not thread safe to create or destroy, but the calls that
 * take a const handle may be made from any thread.
 */
#ifndef DEFW2_H
#define DEFW2_H

#include <stdarg.h>
#include <stdbool.h>

#include <defw2/defw2_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct defw2_rt defw2_rt_t;

/*
 * A server serves RPCs and so runs handler threads, a client only forwards
 * them. defw2_config_from_env reads this from DEFW_AGENT_TYPE, where
 * service and dirsvc mean server.
 */
typedef enum {
	DEFW2_ROLE_CLIENT	= 0,
	DEFW2_ROLE_SERVER	= 1,
} defw2_role_t;

/* The v1 levels, so one log file reads the same in both versions. */
typedef enum {
	DEFW2_LOG_ERROR		= 0,
	DEFW2_LOG_WARNING	= 1,
	DEFW2_LOG_MESSAGE	= 2,
	DEFW2_LOG_DEBUG		= 3,
	DEFW2_LOG_ALL		= 4,
} defw2_log_level_t;

/*
 * Runtime settings. The string fields point into the environment when the
 * config came from defw2_config_from_env, so they last as long as the
 * process. defw2_init copies what it needs.
 */
typedef struct {
	const char		*address;	/* DEFW2_ADDRESS */
	const char		*dirsvc;	/* DEFW2_DIRSVC, may be NULL */
	const char		*margo_config;	/* DEFW2_MARGO_CONFIG path */
	const char		*node_name;	/* DEFW_AGENT_NAME */
	const char		*log_dir;	/* DEFW_LOG_DIR, may be NULL */
	defw2_role_t		role;		/* DEFW_AGENT_TYPE */
	defw2_log_level_t	log_level;	/* DEFW_LOG_LEVEL */
	int			rpc_thread_count; /* DEFW2_RPC_THREADS */
	/*
	 * DEFW2_PROGRESS_SPINDOWN_MS: how long Margo's progress loop spins
	 * after it has handled something before it waits again, when
	 * has_progress_spindown says so. Otherwise a server does not spin,
	 * so a process that listens does not hold a CPU between calls, and a
	 * client keeps Margo's own default, 10 ms, which shortens a call
	 * made right after another.
	 */
	bool			has_progress_spindown;
	int			progress_spindown_ms;
	bool			profile;	/* DEFW2_PROFILE */
} defw2_config_t;

/*
 * Fill cfg from the DEFW_ and DEFW2_ environment, applying the defaults in
 * the design's environment contract. Fails only on a value that cannot be
 * parsed, so an empty environment is a valid client config.
 */
defw2_rc_t defw2_config_from_env(defw2_config_t *cfg);

defw2_rc_t defw2_init(const defw2_config_t *cfg, defw2_rt_t **rt);
void defw2_finalize(defw2_rt_t *rt);

/* Identity. Every string belongs to the runtime and lives until finalize. */
const char *defw2_runtime_id(const defw2_rt_t *rt);
const char *defw2_address(const defw2_rt_t *rt);
const char *defw2_node_name(const defw2_rt_t *rt);
/*
 * Where the directory is, as the config resolved it, or NULL when this
 * process was told there is none. This is the whole of the address bootstrap
 * a caller sees: defw2_dir_open takes it directly.
 */
const char *defw2_dirsvc(const defw2_rt_t *rt);
const char *defw2_hostname(const defw2_rt_t *rt);
int defw2_pid(const defw2_rt_t *rt);
defw2_role_t defw2_role(const defw2_rt_t *rt);

/* Status helpers. */
void defw2_status_free(defw2_status_t *status);
const char *defw2_category_name(uint32_t category);
const char *defw2_strerror(defw2_rc_t rc);

/*
 * Log through the runtime's sink, which is also where Margo's own messages
 * go. rt may be NULL before init, in which case the message goes to stderr.
 */
void defw2_log(const defw2_rt_t *rt, defw2_log_level_t level,
	       const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

const char *defw2_version(void);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_H */
