#ifndef DEFW_PRINT_H
#define DEFW_PRINT_H

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include "defw.h"

#define OUT_LOG_NAME "defw_out.log"
#define OUT_PY_LOG "defw_py.log"
#define LARGE_LOG_FILE 400000000 /* 400 MB */
#define DEFW_LOG_PAYLOAD_EDGE_BYTES 4096
#define DEFW_LOG_PAYLOAD_LIMIT (DEFW_LOG_PAYLOAD_EDGE_BYTES * 2)

static inline bool defw_log_enabled(int loglevel)
{
	if (g_defw_cfg.loglevel == EN_LOG_LEVEL_MSG &&
	    loglevel != EN_LOG_LEVEL_MSG)
		return false;

	if (g_defw_cfg.loglevel < loglevel)
		return false;

	return true;
}

static inline void defw_init_logging(void)
{
	pthread_spin_init(&g_defw_cfg.log_lock, PTHREAD_PROCESS_PRIVATE);
}

static inline void defw_log_print(int loglevel, bool error, char *color1,
				 char *color2, char *file, int line,
				 char *fmt, ...)
{
	time_t debugnow;
	int di;
	char debugtimestr[30];
	struct stat st;
	va_list args;
	FILE *print = stderr;

	pthread_spin_lock(&g_defw_cfg.log_lock);

	if (g_defw_cfg.loglevel == EN_LOG_LEVEL_MSG &&
	    loglevel != EN_LOG_LEVEL_MSG)
		goto out;

	if (g_defw_cfg.loglevel < loglevel)
		goto out;

	if (!g_defw_cfg.outlog || !g_defw_cfg.out)
		goto print_err;

	/* check if the log file has grown too large */
	print = g_defw_cfg.out;
	stat(g_defw_cfg.outlog, &st);
	if (st.st_size > LARGE_LOG_FILE)
		g_defw_cfg.out = freopen(g_defw_cfg.outlog, "w", g_defw_cfg.out);

print_err:
	time(&debugnow);
	ctime_r(&debugnow, debugtimestr);
	for (di = 0; di < 30; di++) {
		if (debugtimestr[di] == '\n')
			debugtimestr[di] = '\0';
	}

	fprintf(print, "%s%lu %s %s:%s:%d " RESET "%s- ", color1,
		pthread_self(), (error) ? "ERROR" : "", debugtimestr, file, line, color2);
	va_start(args, fmt);
	vfprintf(print, fmt, args);
	va_end(args);
	fprintf(print, RESET"\n");
	fflush(print);
out:
	pthread_spin_unlock(&g_defw_cfg.log_lock);
}

static inline void defw_log_payload(int loglevel, bool error, char *color1,
				    char *color2, char *file, int line,
				    const char *prefix,
				    const char *payload)
{
	size_t len;
	size_t omitted;
	const char *safe_prefix = "";
	const char *suffix;

	if (!defw_log_enabled(loglevel))
		return;

	if (prefix)
		safe_prefix = prefix;

	if (!payload) {
		defw_log_print(loglevel, error, color1, color2, file, line,
			       "%s(null)", safe_prefix);
		return;
	}

	len = strlen(payload);
	if (len <= DEFW_LOG_PAYLOAD_LIMIT) {
		defw_log_print(loglevel, error, color1, color2, file, line,
			       "%s%s", safe_prefix, payload);
		return;
	}

	omitted = len - DEFW_LOG_PAYLOAD_LIMIT;
	suffix = payload + len - DEFW_LOG_PAYLOAD_EDGE_BYTES;
	defw_log_print(loglevel, error, color1, color2, file, line,
		       "%s%.*s\n... truncated RPC payload: "
		       "original=%zu bytes, shown=%d+%d bytes, "
		       "omitted=%zu bytes ...\n%.*s",
		       safe_prefix, (int)DEFW_LOG_PAYLOAD_EDGE_BYTES,
		       payload, len, (int)DEFW_LOG_PAYLOAD_EDGE_BYTES,
		       (int)DEFW_LOG_PAYLOAD_EDGE_BYTES, omitted,
		       (int)DEFW_LOG_PAYLOAD_EDGE_BYTES, suffix);
}

#define PERROR(fmt, args...) \
	defw_log_print(EN_LOG_LEVEL_ERROR, true, BOLDRED, RED, __FILE__, \
		       __LINE__, fmt, ## args)
#define PDEBUG(fmt, args...) \
	defw_log_print(EN_LOG_LEVEL_DEBUG, false, BOLDGREEN, GREEN, __FILE__, \
		       __LINE__, fmt, ## args)
#define PMSG(fmt, args...) \
	defw_log_print(EN_LOG_LEVEL_MSG, false, BOLDMAGENTA, BOLDBLUE, \
		       __FILE__, __LINE__, fmt, ## args)
#define PERROR_PAYLOAD(prefix, payload) \
	defw_log_payload(EN_LOG_LEVEL_ERROR, true, BOLDRED, RED, \
			 __FILE__, __LINE__, prefix, payload)
#define PMSG_PAYLOAD(prefix, payload) \
	defw_log_payload(EN_LOG_LEVEL_MSG, false, BOLDMAGENTA, \
			 BOLDBLUE, __FILE__, __LINE__, prefix, payload)

#endif /* DEFW_PRINT_H */
