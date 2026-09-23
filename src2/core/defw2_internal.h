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

/* 36 characters and the terminator. */
#define DEFW2_UUID_STR_LEN	37
#define DEFW2_NAME_MAX		256

struct defw2_rt {
	margo_instance_id	mid;
	char			runtime_id[DEFW2_UUID_STR_LEN];
	char			*address;	/* owned */
	char			*dirsvc;	/* owned, may be NULL */
	char			node_name[DEFW2_NAME_MAX];
	char			hostname[DEFW2_NAME_MAX];
	int			pid;
	defw2_role_t		role;
	defw2_log_level_t	log_level;
	bool			profile;
	FILE			*log_stream;	/* stderr, or an owned file */
	bool			log_owned;
	pthread_mutex_t		log_lock;
};

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
