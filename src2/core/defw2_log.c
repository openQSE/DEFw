/*
 * One logging sink for the runtime and for Margo.
 *
 * Messages go to stderr by default, and to defw2_out.log under DEFW_LOG_DIR
 * when that is set, which is where v1 puts its own log. Margo's logger is
 * pointed at the same sink so a single file tells the whole story.
 */
#include <errno.h>
#include <string.h>
#include <time.h>

#include "defw2_internal.h"

static const char *const level_names[] = {
	"ERROR", "WARNING", "MESSAGE", "DEBUG", "ALL",
};

static const char *level_name(defw2_log_level_t level)
{
	if ((int)level < 0 || level > DEFW2_LOG_ALL)
		return "?";
	return level_names[level];
}

static void timestamp(char *buffer, size_t size)
{
	struct timespec now;
	struct tm parts;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
	    gmtime_r(&now.tv_sec, &parts) == NULL) {
		snprintf(buffer, size, "-");
		return;
	}
	snprintf(buffer, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
		 parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
		 parts.tm_hour, parts.tm_min, parts.tm_sec,
		 (int)(now.tv_nsec / 1000000));
}

void defw2_log_emit(const struct defw2_rt *rt, defw2_log_level_t level,
		    const char *fmt, va_list args)
{
	defw2_log_level_t threshold = rt ? rt->log_level : DEFW2_LOG_ERROR;
	FILE *stream = (rt && rt->log_stream) ? rt->log_stream : stderr;
	const char *name = rt ? rt->node_name : "defw2";
	char when[64];

	if (level > threshold)
		return;

	timestamp(when, sizeof(when));
	/* The lock keeps one message on one line under concurrency. It is
	 * not held across the caller's formatting. */
	if (rt)
		pthread_mutex_lock((pthread_mutex_t *)&rt->log_lock);
	fprintf(stream, "%s %-7s %s: ", when, level_name(level), name);
	vfprintf(stream, fmt, args);
	fputc('\n', stream);
	fflush(stream);
	if (rt)
		pthread_mutex_unlock((pthread_mutex_t *)&rt->log_lock);
}

void defw2_log(const defw2_rt_t *rt, defw2_log_level_t level,
	       const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	defw2_log_emit(rt, level, fmt, args);
	va_end(args);
}

defw2_rc_t defw2_log_open(struct defw2_rt *rt, const char *log_dir)
{
	char path[DEFW2_NAME_MAX * 2];
	FILE *stream;

	rt->log_stream = stderr;
	rt->log_owned = false;
	if (log_dir == NULL || log_dir[0] == '\0')
		return DEFW2_OK;

	snprintf(path, sizeof(path), "%s/defw2_out.log", log_dir);
	stream = fopen(path, "a");
	if (stream == NULL) {
		/* Losing the file is not worth refusing to start over, so
		 * say so on stderr and carry on there. */
		defw2_log(NULL, DEFW2_LOG_ERROR,
			  "cannot open %s (%s), logging to stderr",
			  path, strerror(errno));
		return DEFW2_OK;
	}
	rt->log_stream = stream;
	rt->log_owned = true;
	return DEFW2_OK;
}

void defw2_log_close(struct defw2_rt *rt)
{
	if (rt->log_owned && rt->log_stream != NULL)
		fclose(rt->log_stream);
	rt->log_stream = NULL;
	rt->log_owned = false;
}

/* Margo hands us a formatted message, so these only pick the level. */
#define DEFW2_MARGO_SINK(suffix, level)					\
	static void margo_log_##suffix(void *uargs, const char *message) \
	{								\
		defw2_log((const struct defw2_rt *)uargs, level,		\
			  "margo: %s", message);			\
	}

DEFW2_MARGO_SINK(trace, DEFW2_LOG_ALL)
DEFW2_MARGO_SINK(debug, DEFW2_LOG_DEBUG)
DEFW2_MARGO_SINK(info, DEFW2_LOG_MESSAGE)
DEFW2_MARGO_SINK(warning, DEFW2_LOG_WARNING)
DEFW2_MARGO_SINK(error, DEFW2_LOG_ERROR)
DEFW2_MARGO_SINK(critical, DEFW2_LOG_ERROR)

static margo_log_level margo_level_for(defw2_log_level_t level)
{
	switch (level) {
	case DEFW2_LOG_ERROR:
		return MARGO_LOG_ERROR;
	case DEFW2_LOG_WARNING:
		return MARGO_LOG_WARNING;
	case DEFW2_LOG_MESSAGE:
		return MARGO_LOG_INFO;
	case DEFW2_LOG_DEBUG:
		return MARGO_LOG_DEBUG;
	default:
		return MARGO_LOG_TRACE;
	}
}

void defw2_log_attach_margo(struct defw2_rt *rt)
{
	struct margo_logger logger = {
		.uargs		= rt,
		.trace		= margo_log_trace,
		.debug		= margo_log_debug,
		.info		= margo_log_info,
		.warning	= margo_log_warning,
		.error		= margo_log_error,
		.critical	= margo_log_critical,
	};

	margo_set_logger(rt->mid, &logger);
	margo_set_log_level(rt->mid, margo_level_for(rt->log_level));
}
