/*
 * OTLP JSON, written by hand.
 *
 * Nothing here knows what a span means. It knows how OTLP JSON is spelled:
 * hex identifiers, integers as decimal strings, one export request per line.
 */
#include <inttypes.h>
#include <string.h>

#include <defw2/defw2.h>

#include "defw2_otlp.h"

static bool attrs_grow(struct defw2_attrs *attrs)
{
	size_t capacity = attrs->capacity ? attrs->capacity * 2 : 8;
	struct defw2_attr *items;

	items = realloc(attrs->items, capacity * sizeof(*items));
	if (items == NULL)
		return false;
	attrs->items = items;
	attrs->capacity = capacity;
	return true;
}

static struct defw2_attr *attrs_slot(struct defw2_attrs *attrs,
				     const char *key)
{
	struct defw2_attr *slot;
	size_t i;

	for (i = 0; i < attrs->count; i++) {
		if (strcmp(attrs->items[i].key, key) == 0) {
			free(attrs->items[i].text);
			attrs->items[i].text = NULL;
			return &attrs->items[i];
		}
	}
	if (attrs->count == attrs->capacity && !attrs_grow(attrs))
		return NULL;
	slot = &attrs->items[attrs->count];
	memset(slot, 0, sizeof(*slot));
	slot->key = strdup(key);
	if (slot->key == NULL)
		return NULL;
	attrs->count++;
	return slot;
}

bool defw2_attrs_set(struct defw2_attrs *attrs, const char *key,
		     const char *value)
{
	struct defw2_attr *slot = attrs_slot(attrs, key);

	if (slot == NULL)
		return false;
	slot->kind = DEFW2_ATTR_STRING;
	slot->text = strdup(value ? value : "");
	return slot->text != NULL;
}

bool defw2_attrs_set_int(struct defw2_attrs *attrs, const char *key,
			 int64_t value)
{
	struct defw2_attr *slot = attrs_slot(attrs, key);

	if (slot == NULL)
		return false;
	slot->kind = DEFW2_ATTR_INT;
	slot->number = value;
	return true;
}

void defw2_attrs_free(struct defw2_attrs *attrs)
{
	size_t i;

	for (i = 0; i < attrs->count; i++) {
		free(attrs->items[i].key);
		free(attrs->items[i].text);
	}
	free(attrs->items);
	attrs->items = NULL;
	attrs->count = 0;
	attrs->capacity = 0;
}

bool defw2_histogram_init(struct defw2_histogram *histogram,
			  const double *bounds, size_t bound_count)
{
	histogram->counts = calloc(bound_count + 1, sizeof(*histogram->counts));
	if (histogram->counts == NULL)
		return false;
	histogram->bounds = bounds;
	histogram->bound_count = bound_count;
	histogram->count = 0;
	histogram->sum = 0.0;
	histogram->low = 0.0;
	histogram->high = 0.0;
	return true;
}

void defw2_histogram_add(struct defw2_histogram *histogram, double value)
{
	size_t bucket = 0;

	while (bucket < histogram->bound_count &&
	       value > histogram->bounds[bucket])
		bucket++;
	histogram->counts[bucket]++;
	if (histogram->count == 0 || value < histogram->low)
		histogram->low = value;
	if (histogram->count == 0 || value > histogram->high)
		histogram->high = value;
	histogram->count++;
	histogram->sum += value;
}

void defw2_histogram_free(struct defw2_histogram *histogram)
{
	free(histogram->counts);
	histogram->counts = NULL;
	histogram->count = 0;
}

void defw2_json_string(FILE *out, const char *text)
{
	const unsigned char *at = (const unsigned char *)(text ? text : "");

	fputc('"', out);
	for (; *at != '\0'; at++) {
		switch (*at) {
		case '"':
			fputs("\\\"", out);
			break;
		case '\\':
			fputs("\\\\", out);
			break;
		case '\n':
			fputs("\\n", out);
			break;
		case '\r':
			fputs("\\r", out);
			break;
		case '\t':
			fputs("\\t", out);
			break;
		default:
			if (*at < 0x20)
				fprintf(out, "\\u%04x", *at);
			else
				fputc((char)*at, out);
			break;
		}
	}
	fputc('"', out);
}

static void json_key_string(FILE *out, const char *key, const char *value)
{
	fputs("{\"key\":", out);
	defw2_json_string(out, key);
	fputs(",\"value\":{\"stringValue\":", out);
	defw2_json_string(out, value);
	fputs("}}", out);
}

/* OTLP JSON carries 64-bit integers as decimal strings. */
static void json_key_int(FILE *out, const char *key, int64_t value)
{
	fputs("{\"key\":", out);
	defw2_json_string(out, key);
	fprintf(out, ",\"value\":{\"intValue\":\"%" PRId64 "\"}}", value);
}

void defw2_json_attrs(FILE *out, const struct defw2_attrs *attrs)
{
	size_t i;

	fputc('[', out);
	for (i = 0; attrs != NULL && i < attrs->count; i++) {
		const struct defw2_attr *attr = &attrs->items[i];

		if (i > 0)
			fputc(',', out);
		switch (attr->kind) {
		case DEFW2_ATTR_INT:
			json_key_int(out, attr->key, attr->number);
			break;
		case DEFW2_ATTR_DOUBLE:
			fputs("{\"key\":", out);
			defw2_json_string(out, attr->key);
			fprintf(out, ",\"value\":{\"doubleValue\":%.9g}}",
				attr->real);
			break;
		default:
			json_key_string(out, attr->key, attr->text);
			break;
		}
	}
	fputc(']', out);
}

static void json_trace_id(FILE *out, uint64_t high, uint64_t low)
{
	fprintf(out, "\"%016" PRIx64 "%016" PRIx64 "\"", high, low);
}

static void json_span_id(FILE *out, uint64_t id)
{
	fprintf(out, "\"%016" PRIx64 "\"", id);
}

static void spans_open(FILE *out, const struct defw2_attrs *resource,
		       const char *scope, const char *scope_version)
{
	fputs("{\"resourceSpans\":[{\"resource\":{\"attributes\":", out);
	defw2_json_attrs(out, resource);
	fputs("},\"scopeSpans\":[{\"scope\":{\"name\":", out);
	defw2_json_string(out, scope);
	fputs(",\"version\":", out);
	defw2_json_string(out, scope_version);
	fputs("},\"spans\":[", out);
}

static void request_close(FILE *out)
{
	fputs("]}]}]}\n", out);
	fflush(out);
}

/* Metrics use scopeMetrics where traces use scopeSpans. */
static void metrics_open(FILE *out, const struct defw2_attrs *resource,
			 const char *scope, const char *scope_version)
{
	fputs("{\"resourceMetrics\":[{\"resource\":{\"attributes\":", out);
	defw2_json_attrs(out, resource);
	fputs("},\"scopeMetrics\":[{\"scope\":{\"name\":", out);
	defw2_json_string(out, scope);
	fputs(",\"version\":", out);
	defw2_json_string(out, scope_version);
	fputs("},\"metrics\":[", out);
}

static void json_event(FILE *out, bool first, uint64_t at_ns, const char *name,
		       uint64_t duration_ns)
{
	if (!first)
		fputc(',', out);
	fprintf(out, "{\"timeUnixNano\":\"%" PRIu64 "\",\"name\":", at_ns);
	defw2_json_string(out, name);
	fputs(",\"attributes\":[", out);
	json_key_int(out, "qfw.rpc.phase.duration_ns", (int64_t)duration_ns);
	fputs("]}", out);
}

/*
 * The server records where its time went as events on its own span, which
 * is what makes the per-hop breakdown readable without cross-process clock
 * alignment: the phases are all measured on one machine.
 */
static void json_rpc_events(FILE *out, const struct defw2_rpc_span *span)
{
	uint64_t at = span->start_ns;
	bool first = true;

	if (span->decode_ns == 0 && span->queue_ns == 0 &&
	    span->handler_ns == 0 && span->encode_ns == 0)
		return;

	fputs(",\"events\":[", out);
	if (span->decode_ns > 0) {
		json_event(out, first, at, "decode", span->decode_ns);
		at += span->decode_ns;
		first = false;
	}
	if (span->queue_ns > 0) {
		json_event(out, first, at, "queue", span->queue_ns);
		at += span->queue_ns;
		first = false;
	}
	if (span->handler_ns > 0) {
		json_event(out, first, at, "handler", span->handler_ns);
		at += span->handler_ns;
		first = false;
	}
	if (span->encode_ns > 0)
		json_event(out, first, at, "encode", span->encode_ns);
	fputc(']', out);
}

static void json_status(FILE *out, int32_t code, uint32_t category)
{
	if (code == 0) {
		fprintf(out, ",\"status\":{\"code\":%d}", DEFW2_STATUS_OK);
		return;
	}
	fprintf(out, ",\"status\":{\"code\":%d,\"message\":", DEFW2_STATUS_ERROR);
	fprintf(out, "\"%s\"}", defw2_category_name(category));
}

void defw2_otlp_write_rpc_spans(FILE *out, const struct defw2_attrs *resource,
				const char *scope, const char *scope_version,
				const char *transport,
				const struct defw2_rpc_span *spans,
				size_t count)
{
	size_t start;

	for (start = 0; start < count; start += DEFW2_SPANS_PER_BATCH) {
		size_t end = start + DEFW2_SPANS_PER_BATCH;
		size_t i;

		if (end > count)
			end = count;
		spans_open(out, resource, scope, scope_version);
		for (i = start; i < end; i++) {
			const struct defw2_rpc_span *span = &spans[i];

			if (i > start)
				fputc(',', out);
			fputs("{\"traceId\":", out);
			json_trace_id(out, span->trace_hi, span->trace_lo);
			fputs(",\"spanId\":", out);
			json_span_id(out, span->span_id);
			if (span->parent_id != 0) {
				fputs(",\"parentSpanId\":", out);
				json_span_id(out, span->parent_id);
			}
			fprintf(out,
				",\"name\":\"qfw.transport.rpc\",\"kind\":%u,"
				"\"startTimeUnixNano\":\"%" PRIu64 "\","
				"\"endTimeUnixNano\":\"%" PRIu64 "\","
				"\"attributes\":[",
				span->kind, span->start_ns, span->end_ns);
			json_key_string(out, "qfw.rpc.api", span->api);
			fputc(',', out);
			json_key_string(out, "qfw.rpc.method", span->method);
			fputc(',', out);
			json_key_string(out, "qfw.transport.kind", transport);
			fputc(',', out);
			json_key_string(out, "qfw.rpc.tier",
					span->tier == DEFW2_TIER_DOCUMENT ?
						"document" : "typed");
			fputc(',', out);
			json_key_int(out, "qfw.rpc.request.bytes",
				     (int64_t)span->request_bytes);
			fputc(',', out);
			json_key_int(out, "qfw.rpc.response.bytes",
				     (int64_t)span->response_bytes);
			if (span->bulk_bytes > 0) {
				fputc(',', out);
				json_key_int(out, "qfw.rpc.bulk.bytes",
					     (int64_t)span->bulk_bytes);
			}
			fputc(',', out);
			json_key_string(out, "qfw.rpc.status.category",
					defw2_category_name(span->category));
			fputc(']', out);
			json_rpc_events(out, span);
			json_status(out, span->code, span->category);
			fputc('}', out);
		}
		request_close(out);
	}
}

void defw2_otlp_write_span(FILE *out, const struct defw2_attrs *resource,
			   const char *scope, const char *scope_version,
			   const struct defw2_otlp_span *span)
{
	spans_open(out, resource, scope, scope_version);
	fputs("{\"traceId\":", out);
	json_trace_id(out, span->trace_hi, span->trace_lo);
	fputs(",\"spanId\":", out);
	json_span_id(out, span->span_id);
	if (span->parent_id != 0) {
		fputs(",\"parentSpanId\":", out);
		json_span_id(out, span->parent_id);
	}
	fputs(",\"name\":", out);
	defw2_json_string(out, span->name);
	fprintf(out, ",\"kind\":%u,\"startTimeUnixNano\":\"%" PRIu64 "\","
		"\"endTimeUnixNano\":\"%" PRIu64 "\",\"attributes\":",
		span->kind, span->start_ns, span->end_ns);
	defw2_json_attrs(out, span->attrs);
	if (span->error != NULL) {
		fprintf(out, ",\"status\":{\"code\":%d,\"message\":",
			DEFW2_STATUS_ERROR);
		defw2_json_string(out, span->error);
		fputc('}', out);
	} else {
		fprintf(out, ",\"status\":{\"code\":%d}", DEFW2_STATUS_OK);
	}
	fputc('}', out);
	request_close(out);
}

void defw2_otlp_write_histogram(FILE *out, const struct defw2_attrs *resource,
				const char *scope, const char *scope_version,
				const char *name, const char *unit,
				const char *description,
				const struct defw2_attrs *point_attrs,
				const struct defw2_histogram *histogram,
				uint64_t start_ns, uint64_t now_ns)
{
	size_t i;

	metrics_open(out, resource, scope, scope_version);
	fputs("{\"name\":", out);
	defw2_json_string(out, name);
	fputs(",\"unit\":", out);
	defw2_json_string(out, unit);
	fputs(",\"description\":", out);
	defw2_json_string(out, description);
	/* 2 is cumulative temporality: the totals are for the whole run. */
	fputs(",\"histogram\":{\"aggregationTemporality\":2,\"dataPoints\":[{",
	      out);
	fputs("\"attributes\":", out);
	defw2_json_attrs(out, point_attrs);
	fprintf(out, ",\"startTimeUnixNano\":\"%" PRIu64 "\","
		"\"timeUnixNano\":\"%" PRIu64 "\",\"count\":\"%" PRIu64 "\","
		"\"sum\":%.9g", start_ns, now_ns, histogram->count,
		histogram->sum);
	fputs(",\"bucketCounts\":[", out);
	for (i = 0; i <= histogram->bound_count; i++)
		fprintf(out, "%s\"%" PRIu64 "\"", i ? "," : "",
			histogram->counts[i]);
	fputs("],\"explicitBounds\":[", out);
	for (i = 0; i < histogram->bound_count; i++)
		fprintf(out, "%s%.9g", i ? "," : "", histogram->bounds[i]);
	fprintf(out, "],\"min\":%.9g,\"max\":%.9g}]}}", histogram->low,
		histogram->high);
	fputs("]}]}]}\n", out);
	fflush(out);
}

void defw2_otlp_write_gauge(FILE *out, const struct defw2_attrs *resource,
			    const char *scope, const char *scope_version,
			    const char *name, const char *unit,
			    const char *description,
			    const struct defw2_attrs *point_attrs,
			    double value, uint64_t now_ns)
{
	metrics_open(out, resource, scope, scope_version);
	fputs("{\"name\":", out);
	defw2_json_string(out, name);
	fputs(",\"unit\":", out);
	defw2_json_string(out, unit);
	fputs(",\"description\":", out);
	defw2_json_string(out, description);
	fputs(",\"gauge\":{\"dataPoints\":[{\"attributes\":", out);
	defw2_json_attrs(out, point_attrs);
	fprintf(out, ",\"timeUnixNano\":\"%" PRIu64 "\",\"asDouble\":%.9g}]}}",
		now_ns, value);
	fputs("]}]}]}\n", out);
	fflush(out);
}
