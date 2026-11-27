/*
 * Profiling switches and export.
 *
 * v2 records what the QFw benchmarking design asks for, in that design's
 * vocabulary, so the reports come from tooling QFw already has. One
 * qfw.transport.rpc span per call on the client, one on the server carrying
 * its own timings as events, two histograms, and the process's CPU and
 * memory at exit. Everything is written as OTLP JSON to node-local files,
 * one export request per line, which is the design's file profile 1.
 *
 * Profiling is guarded by a flag, not sampled. DEFW2_PROFILE turns it on
 * for a benchmark run, and when it is off every call site is one test of a
 * boolean. A sampled-out span is not free, and at fabric latencies it would
 * tax the quantity under measurement.
 *
 * Files are written under DEFW2_TELEMETRY_DIR, or DEFW_LOG_DIR when that is
 * unset, named after the agent, which defaults to the host and the process
 * identifier:
 *
 *	spans-<agent>.jsonl	the spans, when profiling is on
 *	metrics-<agent>.jsonl	the histograms, and the process totals
 *
 * Margo's own statistics join them under DEFW2_MARGO_MONITOR, which is off
 * by default. Before Margo 0.24.3 the default monitor crashed a service
 * under concurrent load.
 *
 * Two runtimes sharing a directory and an agent name would overwrite each
 * other's files, the same way they would share a v1 log file. With no
 * directory to write to there is nothing to record.
 */
#ifndef DEFW2_TELEMETRY_H
#define DEFW2_TELEMETRY_H

#include <defw2/defw2.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Is profiling on? Call sites test this before they touch a clock. It is
 * fixed at defw2_init, so the answer does not change under a caller.
 */
bool defw2_profiling(const defw2_rt_t *rt);

/*
 * The trace this process records under, 32 lowercase hex characters, or
 * NULL when profiling is off. A caller that supplies its own traceparent
 * per call joins that trace instead, and this one is then only the default.
 */
const char *defw2_trace_id(const defw2_rt_t *rt);

/*
 * Resource attributes describe the process and are repeated on every export
 * request: service.name, the image, the Slurm job, the library versions. Key
 * and value are copied. Setting a key again replaces it.
 */
defw2_rc_t defw2_telemetry_resource(defw2_rt_t *rt, const char *key,
				    const char *value);
defw2_rc_t defw2_telemetry_resource_int(defw2_rt_t *rt, const char *key,
					int64_t value);

/*
 * A run span, which is what a benchmark's qfw.bench.run is. While one is
 * open every transport span this process records is a child of it, so one
 * run is one trace with one root. One at a time per runtime.
 */
defw2_rc_t defw2_telemetry_run_begin(defw2_rt_t *rt, const char *name);
void defw2_telemetry_run_attr(defw2_rt_t *rt, const char *key,
			      const char *value);
void defw2_telemetry_run_attr_int(defw2_rt_t *rt, const char *key,
				  int64_t value);
/* error is NULL when the run succeeded, and the span's message otherwise. */
void defw2_telemetry_run_end(defw2_rt_t *rt, const char *error);

/*
 * Write what has been recorded so far. defw2_finalize does this, so a
 * caller only needs it to bound memory during a long run.
 */
defw2_rc_t defw2_telemetry_flush(defw2_rt_t *rt);

/*
 * The process totals, from getrusage. These are the CPU microseconds per
 * call and the peak resident set the comparison reports, and they are
 * recorded whether or not profiling is on.
 */
typedef struct {
	uint64_t	user_us;
	uint64_t	system_us;
	uint64_t	peak_rss_kib;
} defw2_process_stats_t;

void defw2_process_stats(defw2_process_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_TELEMETRY_H */
