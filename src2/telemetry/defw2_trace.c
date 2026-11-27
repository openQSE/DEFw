/*
 * What v2 records about itself.
 *
 * One qfw.transport.rpc span per call on each side, two histograms, and the
 * process totals at exit, all written as OTLP JSON to node-local files. The
 * vocabulary is the QFw benchmarking design's, so the reports come from
 * tooling QFw already has.
 *
 * The rule from that design is flag-guarded, not sampled. Profiling is
 * fixed at init and every call site is one boolean test when it is off,
 * because a sampled-out span is not free and at fabric latencies it would
 * tax the quantity under measurement.
 */
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <uuid/uuid.h>

#include <defw2/defw2_telemetry.h>

#include "defw2_trace.h"

#define DEFW2_SCOPE		"defw2"
#define DEFW2_SPAN_BUFFER	DEFW2_SPANS_PER_BATCH

/*
 * How many names the recorder keeps for document spans. A run names few
 * methods, and one that names more records the rest under the tier's name,
 * so a caller cannot grow the table without bound.
 */
#define DEFW2_SPAN_NAMES_MAX	256

/* One microsecond to ten seconds, which spans shared memory to a stall. */
static const double duration_bounds[] = {
	1e-6, 2e-6, 5e-6, 1e-5, 2e-5, 5e-5, 1e-4, 2e-4, 5e-4,
	1e-3, 2e-3, 5e-3, 1e-2, 2e-2, 5e-2, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0,
};

/* 64 bytes to a gigabyte, which spans W1's payload to W3's largest. */
static const double byte_bounds[] = {
	64, 256, 1024, 4096, 16384, 65536, 262144, 1048576,
	4194304, 16777216, 67108864, 268435456, 1073741824,
};

struct defw2_telemetry {
	FILE			*spans;
	struct defw2_attrs	resource;
	char			dir[DEFW2_NAME_MAX];
	char			trace_id[33];
	char			transport[32];
	uint64_t		trace_hi;
	uint64_t		trace_lo;
	uint64_t		next_span_id;
	uint64_t		started_ns;

	struct defw2_otlp_span	run;
	struct defw2_attrs	run_attrs;
	bool			run_open;

	struct defw2_rpc_span	*buffer;
	size_t			count;
	char			*names[DEFW2_SPAN_NAMES_MAX];
	size_t			name_count;

	struct defw2_histogram	duration;
	struct defw2_histogram	bytes;
	bool			histograms;
	bool			closed;		/* written out, now idle */

	pthread_mutex_t		lock;
};

uint64_t defw2_wall_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

uint64_t defw2_mono_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

void defw2_process_stats(defw2_process_stats_t *stats)
{
	struct rusage usage;

	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	if (getrusage(RUSAGE_SELF, &usage) != 0)
		return;
	stats->user_us = (uint64_t)usage.ru_utime.tv_sec * 1000000ull +
			 (uint64_t)usage.ru_utime.tv_usec;
	stats->system_us = (uint64_t)usage.ru_stime.tv_sec * 1000000ull +
			   (uint64_t)usage.ru_stime.tv_usec;
	/* Linux reports this in kibibytes. */
	stats->peak_rss_kib = (uint64_t)usage.ru_maxrss;
}

/*
 * The provider part of a Margo address, which is what qfw.transport.kind
 * names: ofi+tcp, ofi+cxi, na+sm.
 */
static void transport_kind(char *out, size_t size, const char *address)
{
	const char *separator = address ? strstr(address, "://") : NULL;
	size_t length = separator ? (size_t)(separator - address)
				  : (address ? strlen(address) : 0);

	if (length == 0 || length >= size) {
		snprintf(out, size, "unknown");
		return;
	}
	memcpy(out, address, length);
	out[length] = '\0';
}

static void random_128(uint64_t *high, uint64_t *low)
{
	uuid_t raw;

	uuid_generate(raw);
	memcpy(high, raw, sizeof(*high));
	memcpy(low, raw + sizeof(*high), sizeof(*low));
}

static uint64_t next_span_id(struct defw2_telemetry *telemetry)
{
	uint64_t id;

	/* Never zero: OTLP reads an all-zero identifier as absent. */
	do {
		id = __atomic_add_fetch(&telemetry->next_span_id, 1,
					__ATOMIC_RELAXED);
	} while (id == 0);
	return id;
}

/*
 * version "-" trace-id "-" parent-id "-" flags, as W3C writes it. Anything
 * that is not a sampled version 00 header is ignored rather than guessed at.
 */
static bool parse_traceparent(const char *text, uint64_t *high, uint64_t *low,
			      uint64_t *parent)
{
	unsigned long long hi, lo, id;
	char buffer[17];

	if (text == NULL || strlen(text) < 55 || text[0] != '0' ||
	    text[1] != '0' || text[2] != '-' || text[35] != '-' ||
	    text[52] != '-')
		return false;

	memcpy(buffer, text + 3, 16);
	buffer[16] = '\0';
	if (sscanf(buffer, "%16llx", &hi) != 1)
		return false;
	memcpy(buffer, text + 19, 16);
	buffer[16] = '\0';
	if (sscanf(buffer, "%16llx", &lo) != 1)
		return false;
	memcpy(buffer, text + 36, 16);
	buffer[16] = '\0';
	if (sscanf(buffer, "%16llx", &id) != 1)
		return false;

	*high = hi;
	*low = lo;
	*parent = id;
	return true;
}

static void write_traceparent(char *out, uint64_t high, uint64_t low,
			      uint64_t span_id)
{
	snprintf(out, DEFW2_TRACEPARENT_LEN,
		 "00-%016" PRIx64 "%016" PRIx64 "-%016" PRIx64 "-01",
		 high, low, span_id);
}

/* The caller holds the lock. */
static void flush_spans(struct defw2_telemetry *telemetry)
{
	if (telemetry->spans == NULL || telemetry->count == 0)
		return;
	defw2_otlp_write_rpc_spans(telemetry->spans, &telemetry->resource,
				   DEFW2_SCOPE, DEFW2_VERSION_STRING,
				   telemetry->transport, telemetry->buffer,
				   telemetry->count);
	telemetry->count = 0;
}

bool defw2_profiling(const defw2_rt_t *rt)
{
	return rt != NULL && rt->profile && rt->telemetry != NULL;
}

const char *defw2_trace_id(const defw2_rt_t *rt)
{
	if (rt == NULL || rt->telemetry == NULL)
		return NULL;
	return rt->telemetry->trace_id;
}

bool defw2_trace_begin(struct defw2_rt *rt, struct defw2_trace *trace,
		       uint32_t kind, const char *inherited)
{
	struct defw2_telemetry *telemetry;

	trace->recording = false;
	trace->traceparent[0] = '\0';
	if (!defw2_profiling(rt))
		return false;

	telemetry = rt->telemetry;
	memset(&trace->span, 0, sizeof(trace->span));
	if (!parse_traceparent(inherited, &trace->span.trace_hi,
			       &trace->span.trace_lo, &trace->span.parent_id)) {
		trace->span.trace_hi = telemetry->trace_hi;
		trace->span.trace_lo = telemetry->trace_lo;
		/* A run span, when one is open, is the root of the trace. */
		trace->span.parent_id = telemetry->run_open ?
						telemetry->run.span_id : 0;
	}
	trace->span.span_id = next_span_id(telemetry);
	trace->span.kind = kind;
	trace->span.start_ns = defw2_wall_ns();
	trace->mono_ns = defw2_mono_ns();
	write_traceparent(trace->traceparent, trace->span.trace_hi,
			  trace->span.trace_lo, trace->span.span_id);
	trace->recording = true;
	return true;
}

void defw2_trace_backdate(struct defw2_trace *trace, uint64_t wall_ns,
			  uint64_t mono_ns)
{
	if (!trace->recording)
		return;
	trace->span.start_ns = wall_ns;
	trace->mono_ns = mono_ns;
}

/*
 * A typed span names its API and method with literals. A document span's
 * method is whatever the call named, and on the caller's side so is its
 * API, and neither outlives the call, while the span waits in the buffer
 * until a flush. So the recorder keeps one copy of each such name for its
 * own life. Called with the lock held.
 */
static const char *kept_name(struct defw2_telemetry *telemetry,
			     const char *name, const char *fallback)
{
	size_t i;

	if (name == NULL)
		return fallback;
	for (i = 0; i < telemetry->name_count; i++) {
		if (strcmp(telemetry->names[i], name) == 0)
			return telemetry->names[i];
	}
	if (telemetry->name_count == DEFW2_SPAN_NAMES_MAX)
		return fallback;
	telemetry->names[telemetry->name_count] = strdup(name);
	if (telemetry->names[telemetry->name_count] == NULL)
		return fallback;
	return telemetry->names[telemetry->name_count++];
}

void defw2_trace_end(struct defw2_rt *rt, struct defw2_trace *trace)
{
	struct defw2_telemetry *telemetry;
	uint64_t elapsed;

	if (!trace->recording)
		return;
	trace->recording = false;
	telemetry = rt->telemetry;

	/*
	 * The span's ends are wall-clock times, because they are compared
	 * across processes, but its length is measured on the monotonic
	 * clock, which no adjustment moves.
	 */
	elapsed = defw2_mono_ns() - trace->mono_ns;
	trace->span.end_ns = trace->span.start_ns + elapsed;

	pthread_mutex_lock(&telemetry->lock);
	if (trace->span.tier == DEFW2_TIER_DOCUMENT) {
		trace->span.api = kept_name(telemetry, trace->span.api, "?");
		trace->span.method = kept_name(telemetry, trace->span.method,
					       "document");
	}
	if (telemetry->buffer != NULL) {
		telemetry->buffer[telemetry->count++] = trace->span;
		if (telemetry->count == DEFW2_SPAN_BUFFER)
			flush_spans(telemetry);
	}
	/*
	 * Round-trip time is the caller's measure, so only the client side
	 * feeds the histograms. The service's own time is on its span.
	 */
	if (telemetry->histograms && trace->span.kind == DEFW2_SPAN_CLIENT) {
		defw2_histogram_add(&telemetry->duration,
				    (double)elapsed / 1e9);
		defw2_histogram_add(&telemetry->bytes,
				    (double)(trace->span.request_bytes +
					     trace->span.response_bytes +
					     trace->span.bulk_bytes));
	}
	pthread_mutex_unlock(&telemetry->lock);
}

defw2_rc_t defw2_telemetry_resource(defw2_rt_t *rt, const char *key,
				    const char *value)
{
	defw2_rc_t rc = DEFW2_OK;

	if (rt == NULL || rt->telemetry == NULL || key == NULL)
		return DEFW2_ERR_INVALID;
	pthread_mutex_lock(&rt->telemetry->lock);
	if (!defw2_attrs_set(&rt->telemetry->resource, key, value))
		rc = DEFW2_ERR_NOMEM;
	pthread_mutex_unlock(&rt->telemetry->lock);
	return rc;
}

defw2_rc_t defw2_telemetry_resource_int(defw2_rt_t *rt, const char *key,
					int64_t value)
{
	defw2_rc_t rc = DEFW2_OK;

	if (rt == NULL || rt->telemetry == NULL || key == NULL)
		return DEFW2_ERR_INVALID;
	pthread_mutex_lock(&rt->telemetry->lock);
	if (!defw2_attrs_set_int(&rt->telemetry->resource, key, value))
		rc = DEFW2_ERR_NOMEM;
	pthread_mutex_unlock(&rt->telemetry->lock);
	return rc;
}

defw2_rc_t defw2_telemetry_run_begin(defw2_rt_t *rt, const char *name)
{
	struct defw2_telemetry *telemetry;

	if (!defw2_profiling(rt) || name == NULL)
		return DEFW2_ERR_INVALID;
	telemetry = rt->telemetry;

	pthread_mutex_lock(&telemetry->lock);
	if (telemetry->run_open) {
		pthread_mutex_unlock(&telemetry->lock);
		return DEFW2_ERR_INVALID;
	}
	memset(&telemetry->run, 0, sizeof(telemetry->run));
	telemetry->run.trace_hi = telemetry->trace_hi;
	telemetry->run.trace_lo = telemetry->trace_lo;
	telemetry->run.span_id = next_span_id(telemetry);
	telemetry->run.start_ns = defw2_wall_ns();
	telemetry->run.name = name;
	telemetry->run.kind = DEFW2_SPAN_INTERNAL;
	telemetry->run.attrs = &telemetry->run_attrs;
	telemetry->run_open = true;
	pthread_mutex_unlock(&telemetry->lock);
	return DEFW2_OK;
}

void defw2_telemetry_run_attr(defw2_rt_t *rt, const char *key,
			      const char *value)
{
	if (!defw2_profiling(rt) || key == NULL)
		return;
	pthread_mutex_lock(&rt->telemetry->lock);
	defw2_attrs_set(&rt->telemetry->run_attrs, key, value);
	pthread_mutex_unlock(&rt->telemetry->lock);
}

void defw2_telemetry_run_attr_int(defw2_rt_t *rt, const char *key,
				  int64_t value)
{
	if (!defw2_profiling(rt) || key == NULL)
		return;
	pthread_mutex_lock(&rt->telemetry->lock);
	defw2_attrs_set_int(&rt->telemetry->run_attrs, key, value);
	pthread_mutex_unlock(&rt->telemetry->lock);
}

void defw2_telemetry_run_end(defw2_rt_t *rt, const char *error)
{
	struct defw2_telemetry *telemetry;

	if (!defw2_profiling(rt))
		return;
	telemetry = rt->telemetry;

	pthread_mutex_lock(&telemetry->lock);
	if (telemetry->run_open) {
		telemetry->run.end_ns = defw2_wall_ns();
		telemetry->run.error = error;
		/* The children go out first, so a reader that stops early
		 * still has the calls rather than only the summary. */
		flush_spans(telemetry);
		if (telemetry->spans != NULL)
			defw2_otlp_write_span(telemetry->spans,
					      &telemetry->resource,
					      DEFW2_SCOPE,
					      DEFW2_VERSION_STRING,
					      &telemetry->run);
		telemetry->run_open = false;
	}
	pthread_mutex_unlock(&telemetry->lock);
}

defw2_rc_t defw2_telemetry_flush(defw2_rt_t *rt)
{
	if (rt == NULL || rt->telemetry == NULL)
		return DEFW2_ERR_INVALID;
	pthread_mutex_lock(&rt->telemetry->lock);
	flush_spans(rt->telemetry);
	pthread_mutex_unlock(&rt->telemetry->lock);
	return DEFW2_OK;
}

/*
 * The metrics file is written once, at the end. It holds the two transport
 * histograms when profiling was on, and the process totals always, because
 * cost per call and peak memory are what the comparison weighs against the
 * latency.
 */
static void write_metrics(struct defw2_rt *rt)
{
	struct defw2_telemetry *telemetry = rt->telemetry;
	defw2_process_stats_t stats;
	struct defw2_attrs point = { 0 };
	char path[DEFW2_NAME_MAX * 3];
	uint64_t now = defw2_wall_ns();
	FILE *out;

	snprintf(path, sizeof(path), "%s/metrics-%s.jsonl", telemetry->dir,
		 rt->node_name);
	out = fopen(path, "w");
	if (out == NULL) {
		defw2_log(rt, DEFW2_LOG_WARNING, "cannot write %s (%s)", path,
			  strerror(errno));
		return;
	}

	if (telemetry->histograms && telemetry->duration.count > 0) {
		defw2_attrs_set(&point, "qfw.transport.kind",
				telemetry->transport);
		defw2_attrs_set(&point, "qfw.rpc.tier", "typed");
		defw2_otlp_write_histogram(out, &telemetry->resource,
					   DEFW2_SCOPE, DEFW2_VERSION_STRING,
					   "qfw.transport.rpc.duration", "s",
					   "RPC round trip, client side",
					   &point, &telemetry->duration,
					   telemetry->started_ns, now);
		defw2_otlp_write_histogram(out, &telemetry->resource,
					   DEFW2_SCOPE, DEFW2_VERSION_STRING,
					   "qfw.transport.rpc.bytes", "By",
					   "Payload carried per RPC",
					   &point, &telemetry->bytes,
					   telemetry->started_ns, now);
		defw2_attrs_free(&point);
	}

	defw2_process_stats(&stats);
	defw2_attrs_set(&point, "process.cpu.state", "user");
	defw2_otlp_write_gauge(out, &telemetry->resource, DEFW2_SCOPE,
			       DEFW2_VERSION_STRING, "process.cpu.time", "s",
			       "Process CPU time at exit", &point,
			       (double)stats.user_us / 1e6, now);
	defw2_attrs_set(&point, "process.cpu.state", "system");
	defw2_otlp_write_gauge(out, &telemetry->resource, DEFW2_SCOPE,
			       DEFW2_VERSION_STRING, "process.cpu.time", "s",
			       "Process CPU time at exit", &point,
			       (double)stats.system_us / 1e6, now);
	defw2_attrs_free(&point);
	defw2_otlp_write_gauge(out, &telemetry->resource, DEFW2_SCOPE,
			       DEFW2_VERSION_STRING, "process.memory.peak_rss",
			       "By", "Maximum resident set at exit", NULL,
			       (double)stats.peak_rss_kib * 1024.0, now);
	fclose(out);
}

const char *defw2_telemetry_dir(const defw2_config_t *cfg)
{
	const char *dir = getenv("DEFW2_TELEMETRY_DIR");

	if (dir == NULL || dir[0] == '\0')
		dir = cfg->log_dir;
	if (dir == NULL || dir[0] == '\0')
		return NULL;
	return dir;
}

defw2_rc_t defw2_telemetry_open(struct defw2_rt *rt,
				const defw2_config_t *cfg)
{
	struct defw2_telemetry *telemetry;
	const char *dir = defw2_telemetry_dir(cfg);
	char path[DEFW2_NAME_MAX * 3];
	uint64_t scratch;

	/*
	 * With nowhere node-local to write there is nothing to record, so
	 * the runtime says so once rather than at every call site.
	 */
	if (dir == NULL) {
		if (cfg->profile)
			defw2_log(rt, DEFW2_LOG_WARNING,
				  "DEFW2_PROFILE is set but neither DEFW2_TELEMETRY_DIR nor DEFW_LOG_DIR is");
		return DEFW2_OK;
	}

	telemetry = calloc(1, sizeof(*telemetry));
	if (telemetry == NULL)
		return DEFW2_ERR_NOMEM;
	pthread_mutex_init(&telemetry->lock, NULL);
	snprintf(telemetry->dir, sizeof(telemetry->dir), "%s", dir);
	telemetry->started_ns = defw2_wall_ns();
	transport_kind(telemetry->transport, sizeof(telemetry->transport),
		       cfg->address);
	random_128(&telemetry->trace_hi, &telemetry->trace_lo);
	/* Span identifiers start somewhere random and count up, so two
	 * processes in one trace cannot collide. */
	random_128(&telemetry->next_span_id, &scratch);
	snprintf(telemetry->trace_id, sizeof(telemetry->trace_id),
		 "%016" PRIx64 "%016" PRIx64, telemetry->trace_hi,
		 telemetry->trace_lo);

	defw2_attrs_set(&telemetry->resource, "service.name", rt->node_name);
	defw2_attrs_set(&telemetry->resource, "service.version",
			DEFW2_VERSION_STRING);
	defw2_attrs_set(&telemetry->resource, "host.name", rt->hostname);
	defw2_attrs_set_int(&telemetry->resource, "process.pid", rt->pid);
	defw2_attrs_set(&telemetry->resource, "process.runtime.name", "defw2");
	defw2_attrs_set(&telemetry->resource, "qfw.transport.kind",
			telemetry->transport);
	defw2_attrs_set(&telemetry->resource, "qfw.defw.runtime_id",
			rt->runtime_id);
	if (getenv("SLURM_JOB_ID") != NULL)
		defw2_attrs_set(&telemetry->resource, "qfw.slurm.job_id",
				getenv("SLURM_JOB_ID"));

	if (cfg->profile) {
		snprintf(path, sizeof(path), "%s/spans-%s.jsonl", dir,
			 rt->node_name);
		telemetry->spans = fopen(path, "w");
		if (telemetry->spans == NULL)
			defw2_log(rt, DEFW2_LOG_WARNING,
				  "cannot write %s (%s), spans are dropped",
				  path, strerror(errno));
		telemetry->buffer = calloc(DEFW2_SPAN_BUFFER,
					   sizeof(*telemetry->buffer));
		telemetry->histograms =
			defw2_histogram_init(&telemetry->duration,
					     duration_bounds,
					     sizeof(duration_bounds) /
						     sizeof(*duration_bounds)) &&
			defw2_histogram_init(&telemetry->bytes, byte_bounds,
					     sizeof(byte_bounds) /
						     sizeof(*byte_bounds));
	}

	rt->telemetry = telemetry;
	defw2_log(rt, DEFW2_LOG_DEBUG, "telemetry in %s, trace %s, profile %s",
		  dir, telemetry->trace_id, cfg->profile ? "on" : "off");
	return DEFW2_OK;
}

/*
 * Write everything out and stop recording, but keep the recorder. A ULT can
 * still be finishing a call when the runtime closes its telemetry, which it
 * does before Margo stops so that a hang there loses nothing, and that ULT
 * ends its span against this recorder. With the buffer and the histograms
 * gone, trace_end drops the span rather than writing to freed memory.
 */
void defw2_telemetry_close(struct defw2_rt *rt)
{
	struct defw2_telemetry *telemetry = rt->telemetry;

	if (telemetry == NULL)
		return;

	defw2_telemetry_run_end(rt, NULL);
	pthread_mutex_lock(&telemetry->lock);
	if (telemetry->closed) {
		pthread_mutex_unlock(&telemetry->lock);
		return;
	}
	flush_spans(telemetry);
	write_metrics(rt);
	if (telemetry->spans != NULL)
		fclose(telemetry->spans);
	telemetry->spans = NULL;
	free(telemetry->buffer);
	telemetry->buffer = NULL;
	telemetry->count = 0;
	telemetry->histograms = false;
	defw2_histogram_free(&telemetry->duration);
	defw2_histogram_free(&telemetry->bytes);
	telemetry->closed = true;
	pthread_mutex_unlock(&telemetry->lock);
}

/* Once Margo has stopped, nothing can end a span, so the recorder goes. */
void defw2_telemetry_free(struct defw2_rt *rt)
{
	struct defw2_telemetry *telemetry = rt->telemetry;

	if (telemetry == NULL)
		return;
	defw2_telemetry_close(rt);
	defw2_attrs_free(&telemetry->resource);
	defw2_attrs_free(&telemetry->run_attrs);
	while (telemetry->name_count > 0)
		free(telemetry->names[--telemetry->name_count]);
	pthread_mutex_destroy(&telemetry->lock);
	free(telemetry);
	rt->telemetry = NULL;
}
