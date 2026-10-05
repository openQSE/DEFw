/*
 * Writing OTLP JSON, one export request per line.
 *
 * This is the whole of v2's knowledge of the wire format the benchmarking
 * design's file profile asks for. It has no dependency beyond stdio, which
 * is the point: a benchmark run must not need a collector, a library or a
 * build option to record what it did.
 *
 * Identifiers are lowercase hex, enums are integers and 64-bit numbers are
 * decimal strings, as OTLP JSON requires.
 */
#ifndef DEFW2_OTLP_H
#define DEFW2_OTLP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* OTLP span kinds and status codes, from the protocol. */
#define DEFW2_SPAN_INTERNAL	1
#define DEFW2_SPAN_SERVER	2
#define DEFW2_SPAN_CLIENT	3
#define DEFW2_STATUS_OK		1
#define DEFW2_STATUS_ERROR	2

/* Export requests hold at most this many spans, as the v1 harness does. */
#define DEFW2_SPANS_PER_BATCH	2000

enum defw2_attr_kind {
	DEFW2_ATTR_STRING,
	DEFW2_ATTR_INT,
	DEFW2_ATTR_DOUBLE,
};

struct defw2_attr {
	char			*key;	/* owned */
	char			*text;	/* owned, when the kind is string */
	int64_t			number;
	double			real;
	enum defw2_attr_kind	kind;
};

/* A small growable attribute list. Setting a key again replaces it. */
struct defw2_attrs {
	struct defw2_attr	*items;
	size_t			count;
	size_t			capacity;
};

bool defw2_attrs_set(struct defw2_attrs *attrs, const char *key,
		     const char *value);
bool defw2_attrs_set_int(struct defw2_attrs *attrs, const char *key,
			 int64_t value);
void defw2_attrs_free(struct defw2_attrs *attrs);

/*
 * One RPC round trip. The high-rate record, so it holds fixed fields rather
 * than an attribute list: a run recording ten thousand calls allocates
 * nothing per call. The server-side timings are zero on a client span.
 */
struct defw2_rpc_span {
	uint64_t	trace_hi;
	uint64_t	trace_lo;
	uint64_t	span_id;
	uint64_t	parent_id;	/* 0 when there is no parent */
	uint64_t	start_ns;
	uint64_t	end_ns;
	/*
	 * The two messages as Mercury encoded them, its own headers aside,
	 * which is what wire bytes per call counts, and what moved by bulk
	 * transfer outside them.
	 */
	uint64_t	request_bytes;
	uint64_t	response_bytes;
	uint64_t	bulk_bytes;
	uint64_t	decode_ns;
	uint64_t	queue_ns;	/* a Python service's hand-off */
	uint64_t	handler_ns;
	uint64_t	encode_ns;
	/* Literals, or for a document span names the recorder keeps. */
	const char	*api;
	const char	*method;
	uint32_t	kind;
	uint32_t	category;
	int32_t		code;
	uint8_t		tier;		/* DEFW2_TIER_* */
};

#define DEFW2_TIER_TYPED	0
#define DEFW2_TIER_DOCUMENT	1

/* A one-off span with its own attributes, such as a benchmark's run span. */
struct defw2_otlp_span {
	uint64_t		trace_hi;
	uint64_t		trace_lo;
	uint64_t		span_id;
	uint64_t		parent_id;
	uint64_t		start_ns;
	uint64_t		end_ns;
	const char		*name;
	uint32_t		kind;
	const char		*error;	/* NULL when it succeeded */
	struct defw2_attrs	*attrs;
};

/* An explicit-bucket histogram. counts holds bound_count + 1 entries. */
struct defw2_histogram {
	const double	*bounds;
	size_t		bound_count;
	uint64_t	*counts;
	uint64_t	count;
	double		sum;
	double		low;
	double		high;
};

bool defw2_histogram_init(struct defw2_histogram *histogram,
			  const double *bounds, size_t bound_count);
void defw2_histogram_add(struct defw2_histogram *histogram, double value);
void defw2_histogram_free(struct defw2_histogram *histogram);

void defw2_json_string(FILE *out, const char *text);
void defw2_json_attrs(FILE *out, const struct defw2_attrs *attrs);

/*
 * One export request per call, written whole and flushed, so a run that is
 * killed still leaves every complete line readable.
 */
void defw2_otlp_write_rpc_spans(FILE *out, const struct defw2_attrs *resource,
				const char *scope, const char *scope_version,
				const char *transport,
				const struct defw2_rpc_span *spans,
				size_t count);
void defw2_otlp_write_span(FILE *out, const struct defw2_attrs *resource,
			   const char *scope, const char *scope_version,
			   const struct defw2_otlp_span *span);
void defw2_otlp_write_histogram(FILE *out, const struct defw2_attrs *resource,
				const char *scope, const char *scope_version,
				const char *name, const char *unit,
				const char *description,
				const struct defw2_attrs *point_attrs,
				const struct defw2_histogram *histogram,
				uint64_t start_ns, uint64_t now_ns);
void defw2_otlp_write_gauge(FILE *out, const struct defw2_attrs *resource,
			    const char *scope, const char *scope_version,
			    const char *name, const char *unit,
			    const char *description,
			    const struct defw2_attrs *point_attrs,
			    double value, uint64_t now_ns);

#endif /* DEFW2_OTLP_H */
