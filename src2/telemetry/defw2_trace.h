/*
 * Recording spans, shared inside libdefw2 and not installed.
 *
 * A call site pairs defw2_trace_begin with defw2_trace_end and fills in what
 * it knows between them. When profiling is off, begin returns false having
 * done nothing but test a boolean, and end returns at once.
 */
#ifndef DEFW2_TRACE_H
#define DEFW2_TRACE_H

#include <defw2/defw2_telemetry.h>

#include "defw2_internal.h"
#include "defw2_otlp.h"

/* "00-" + 32 + "-" + 16 + "-" + 2, and the terminator. */
#define DEFW2_TRACEPARENT_LEN	56

struct defw2_trace {
	struct defw2_rpc_span	span;
	uint64_t		mono_ns;	/* for the duration */
	char			traceparent[DEFW2_TRACEPARENT_LEN];
	bool			recording;
};

/*
 * Start a span. kind is DEFW2_SPAN_CLIENT or DEFW2_SPAN_SERVER. inherited
 * is the W3C traceparent to join, which is the caller's own on a client and
 * the request header's on a server, and may be NULL or empty. On return
 * trace->traceparent describes this span, which is what a client sends so
 * the service's span becomes its child.
 */
bool defw2_trace_begin(struct defw2_rt *rt, struct defw2_trace *trace,
		       uint32_t kind, const char *inherited);

/*
 * Move a span's start back to when the work really began. A server reads
 * the traceparent out of the request, so it can only begin the span after
 * the decode it wants to measure.
 */
void defw2_trace_backdate(struct defw2_trace *trace, uint64_t wall_ns,
			  uint64_t mono_ns);

/* Close the span and record it. Safe to call when begin returned false. */
void defw2_trace_end(struct defw2_rt *rt, struct defw2_trace *trace);

/*
 * Where node-local telemetry goes, or NULL when there is nowhere. The
 * runtime needs this before Margo starts, so that Margo's own monitor can
 * write beside the spans.
 */
const char *defw2_telemetry_dir(const defw2_config_t *cfg);

/*
 * Set up and tear down the recorder. Called from defw2_init and finalize.
 * close writes everything out and stops recording, and free releases the
 * recorder once Margo has stopped, so a ULT still finishing a call between
 * the two ends its span against a recorder that drops it.
 */
defw2_rc_t defw2_telemetry_open(struct defw2_rt *rt,
				const defw2_config_t *cfg);
void defw2_telemetry_close(struct defw2_rt *rt);
void defw2_telemetry_free(struct defw2_rt *rt);

/* The monotonic clock, for durations that must not follow the wall clock. */
uint64_t defw2_mono_ns(void);

#endif /* DEFW2_TRACE_H */
