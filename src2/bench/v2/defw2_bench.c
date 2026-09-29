/*
 * defw2-bench: one measured client.
 *
 * It runs the workloads in "Profiling and the v1 Comparison" in
 * docs/design_v2.md against the echo service, and writes its per-call
 * timings where the launcher can read them. The launcher runs as many of
 * these as the workload asks for and assembles the report, so this program
 * knows nothing about workload names, transports or reports.
 *
 * The spans are libdefw2's, not this program's. Setting DEFW2_PROFILE and a
 * telemetry directory is all it takes, and the traceparent the launcher
 * passes makes every call a child of the run.
 *
 * It measures the same thing the v1 client measures, the same way: the
 * monotonic clock around the public call, with the payload checked after
 * the clock has stopped, and getrusage around the whole loop.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <defw2/defw2_echo.h>
#include <defw2/defw2_telemetry.h>

#define POLL_NS			(5 * 1000 * 1000L)

struct options {
	const char	*address;
	const char	*traceparent;
	const char	*ready_path;
	const char	*go_path;
	const char	*result_path;
	uint16_t	provider_id;
	long		index;
	long		calls;
	long		warmup;
	size_t		payload;
	bool		bulk;
	uint32_t	timeout_ms;
	long		wait_s;
};

static void usage(void)
{
	fprintf(stderr,
		"usage: defw2-bench --address ADDR --result PATH [options]\n"
		"\n"
		"  --address ADDR      the echo service to call\n"
		"  --provider N        provider id (default %d)\n"
		"  --index N           this client's index (default 0)\n"
		"  --calls N           measured calls (default 1000)\n"
		"  --warmup N          unmeasured calls first (default 100)\n"
		"  --payload N         payload bytes (default 64)\n"
		"  --bulk              move the payload through bulk memory\n"
		"  --traceparent S     the run's W3C trace context\n"
		"  --ready PATH        created once warmed up\n"
		"  --go PATH           waited for before measuring\n"
		"  --result PATH       where the timings are written\n"
		"  --timeout-ms N      per call (default 60000)\n"
		"  --wait-s N          for the go signal (default 300)\n",
		DEFW2_PROVIDER_ECHO);
}

static uint64_t mono_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static uint64_t wall_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/*
 * The same bytes the v1 harness sends, so the two sides carry identical
 * payloads: the 256 byte pattern repeated.
 */
static void build_payload(unsigned char *buffer, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		buffer[i] = (unsigned char)(i % 256);
}

static bool touch(const char *path)
{
	FILE *out;

	if (path == NULL)
		return true;
	out = fopen(path, "w");
	if (out == NULL) {
		fprintf(stderr, "cannot create %s: %s\n", path,
			strerror(errno));
		return false;
	}
	fclose(out);
	return true;
}

static bool wait_for(const char *path, long seconds)
{
	struct timespec nap = { .tv_sec = 0, .tv_nsec = POLL_NS };
	uint64_t deadline;
	struct stat ignored;

	if (path == NULL)
		return true;
	deadline = mono_ns() + (uint64_t)seconds * 1000000000ull;
	while (stat(path, &ignored) != 0) {
		if (mono_ns() > deadline) {
			fprintf(stderr, "no go signal at %s after %ld s\n",
				path, seconds);
			return false;
		}
		nanosleep(&nap, NULL);
	}
	return true;
}

struct results {
	uint64_t	*durations;	/* per call, nanoseconds */
	long		*failed;	/* every failed call's index */
	long		failures;
	char		message[256];	/* the first failure's */
	uint64_t	loop_start_unix_ns;
	uint64_t	loop_ns;
	uint64_t	cpu_ns;
	uint64_t	max_rss_kib;
	uint64_t	bytes_moved;
};

static void note_failure(struct results *results, long call, const char *what)
{
	if (results->failures == 0)
		snprintf(results->message, sizeof(results->message), "%s",
			 what);
	results->failed[results->failures++] = call;
}

static void run_eager(defw2_binding_t *echo, const struct options *opts,
		      const unsigned char *payload, struct results *results,
		      const defw2_call_opts_t *call)
{
	long i;

	for (i = 0; i < opts->calls; i++) {
		defw2_buffer_t reply = { 0 };
		defw2_status_t status = { 0 };
		uint64_t started = mono_ns();
		defw2_rc_t rc;

		rc = defw2_echo(echo, payload, opts->payload, call, &reply,
				&status);
		results->durations[i] = mono_ns() - started;

		/* Checked after the clock stops, as the v1 client does. */
		if (rc != DEFW2_OK)
			note_failure(results, i, defw2_strerror(rc));
		else if (status.category != DEFW2_CAT_OK)
			note_failure(results, i,
				     defw2_category_name(status.category));
		else if (reply.len != opts->payload ||
			 memcmp(reply.data, payload, opts->payload) != 0)
			note_failure(results, i, "the echo returned different data");
		else
			results->bytes_moved += 2 * (uint64_t)opts->payload;

		defw2_buffer_free(&reply);
		defw2_status_free(&status);
	}
}

static void run_bulk(defw2_binding_t *echo, const struct options *opts,
		     const unsigned char *payload, unsigned char *sink,
		     struct results *results, const defw2_call_opts_t *call)
{
	long i;

	for (i = 0; i < opts->calls; i++) {
		defw2_status_t status = { 0 };
		uint64_t moved = 0;
		uint64_t started;
		defw2_rc_t rc;

		/*
		 * Cleared first, so the comparison afterwards proves the
		 * service wrote this call's data rather than the last
		 * call's. It is outside the timed region, but it is inside
		 * the loop, so W3's calls per second carries it. The bulk
		 * rate is computed from the call durations and does not.
		 */
		memset(sink, 0, opts->payload);
		started = mono_ns();
		rc = defw2_echo_bulk(echo, payload, sink, opts->payload, call,
				     &moved, &status);
		results->durations[i] = mono_ns() - started;

		if (rc != DEFW2_OK)
			note_failure(results, i, defw2_strerror(rc));
		else if (status.category != DEFW2_CAT_OK)
			note_failure(results, i,
				     defw2_category_name(status.category));
		else if (memcmp(sink, payload, opts->payload) != 0)
			note_failure(results, i, "the echo returned different data");
		else
			results->bytes_moved += moved;

		defw2_status_free(&status);
	}
}

static void json_escape(FILE *out, const char *text)
{
	for (; *text != '\0'; text++) {
		if (*text == '"' || *text == '\\')
			fputc('\\', out);
		fputc(*text, out);
	}
}

/*
 * Written to a partial file and renamed, so the launcher never reads half a
 * result. The keys are the v1 client's, so one report builder fits both.
 */
static bool write_results(const struct options *opts,
			  const struct results *results)
{
	char partial[4096];
	FILE *out;
	long i;

	snprintf(partial, sizeof(partial), "%s.partial", opts->result_path);
	out = fopen(partial, "w");
	if (out == NULL) {
		fprintf(stderr, "cannot write %s: %s\n", partial,
			strerror(errno));
		return false;
	}

	fprintf(out, "{\"index\":%ld,\"calls\":%ld,", opts->index, opts->calls);
	fprintf(out, "\"resource\":{\"process.pid\":%d},", (int)getpid());
	fprintf(out, "\"loop_start_unix_ns\":%" PRIu64 ",",
		results->loop_start_unix_ns);
	fprintf(out, "\"loop_ns\":%" PRIu64 ",", results->loop_ns);
	fprintf(out, "\"cpu_ns\":%" PRIu64 ",", results->cpu_ns);
	fprintf(out, "\"max_rss_kib\":%" PRIu64 ",", results->max_rss_kib);
	fprintf(out, "\"bytes_moved\":%" PRIu64 ",", results->bytes_moved);

	fputs("\"durations_ns\":[", out);
	for (i = 0; i < opts->calls; i++)
		fprintf(out, "%s%" PRIu64, i ? "," : "", results->durations[i]);
	fputs("],", out);

	fputs("\"failed_calls\":[", out);
	for (i = 0; i < results->failures; i++)
		fprintf(out, "%s%ld", i ? "," : "", results->failed[i]);
	fprintf(out, "],\"failed_call_count\":%ld,", results->failures);
	fputs("\"failure_messages\":{", out);
	if (results->failures > 0) {
		fprintf(out, "\"%ld\":\"", results->failed[0]);
		json_escape(out, results->message);
		fputs("\"", out);
	}
	fputs("}}\n", out);

	if (fclose(out) != 0) {
		fprintf(stderr, "cannot finish %s: %s\n", partial,
			strerror(errno));
		return false;
	}
	if (rename(partial, opts->result_path) != 0) {
		fprintf(stderr, "cannot rename %s: %s\n", partial,
			strerror(errno));
		return false;
	}
	return true;
}

static bool parse(int argc, char **argv, struct options *opts)
{
	int i;

	for (i = 1; i < argc; i++) {
		const char *name = argv[i];
		const char *value = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(name, "--bulk") == 0) {
			opts->bulk = true;
			continue;
		}
		if (value == NULL)
			return false;
		i++;
		if (strcmp(name, "--address") == 0)
			opts->address = value;
		else if (strcmp(name, "--traceparent") == 0)
			opts->traceparent = value;
		else if (strcmp(name, "--ready") == 0)
			opts->ready_path = value;
		else if (strcmp(name, "--go") == 0)
			opts->go_path = value;
		else if (strcmp(name, "--result") == 0)
			opts->result_path = value;
		else if (strcmp(name, "--provider") == 0)
			opts->provider_id = (uint16_t)strtoul(value, NULL, 10);
		else if (strcmp(name, "--index") == 0)
			opts->index = strtol(value, NULL, 10);
		else if (strcmp(name, "--calls") == 0)
			opts->calls = strtol(value, NULL, 10);
		else if (strcmp(name, "--warmup") == 0)
			opts->warmup = strtol(value, NULL, 10);
		else if (strcmp(name, "--payload") == 0)
			opts->payload = strtoull(value, NULL, 10);
		else if (strcmp(name, "--timeout-ms") == 0)
			opts->timeout_ms = (uint32_t)strtoul(value, NULL, 10);
		else if (strcmp(name, "--wait-s") == 0)
			opts->wait_s = strtol(value, NULL, 10);
		else
			return false;
	}
	return opts->address != NULL && opts->result_path != NULL &&
	       opts->calls > 0 && opts->payload > 0;
}

int main(int argc, char **argv)
{
	struct options opts = {
		.provider_id = DEFW2_PROVIDER_ECHO,
		.calls = 1000,
		.warmup = 100,
		.payload = 64,
		.timeout_ms = 60000,
		.wait_s = 300,
	};
	defw2_process_stats_t before, after;
	struct results results = { 0 };
	defw2_binding_t *echo = NULL;
	unsigned char *payload = NULL;
	unsigned char *sink = NULL;
	defw2_call_opts_t call, warm;
	defw2_status_t status = { 0 };
	defw2_buffer_t reply = { 0 };
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;
	uint64_t loop_start;
	int rc = 1;
	long i;

	if (!parse(argc, argv, &opts)) {
		usage();
		return 2;
	}
	if (!opts.bulk && opts.payload > DEFW2_EAGER_MAX) {
		fprintf(stderr, "a %zu byte payload needs --bulk\n",
			opts.payload);
		return 2;
	}

	call.timeout_ms = opts.timeout_ms;
	call.traceparent = opts.traceparent;
	/*
	 * The check and the warmup stay out of the run's trace, so that one
	 * run is one trace holding exactly the calls that were measured.
	 * Their spans are still recorded, under this process's own trace.
	 */
	warm.timeout_ms = opts.timeout_ms;
	warm.traceparent = NULL;

	defw2_config_from_env(&cfg);
	cfg.role = DEFW2_ROLE_CLIENT;
	if (defw2_init(&cfg, &rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start a client on %s\n", cfg.address);
		return 1;
	}
	if (defw2_binding_create(rt, opts.address, opts.provider_id, &echo) !=
	    DEFW2_OK) {
		fprintf(stderr, "cannot reach %s\n", opts.address);
		goto out;
	}

	payload = malloc(opts.payload);
	results.durations = calloc((size_t)opts.calls,
				   sizeof(*results.durations));
	results.failed = calloc((size_t)opts.calls, sizeof(*results.failed));
	if (opts.bulk)
		sink = malloc(opts.payload);
	if (payload == NULL || results.durations == NULL ||
	    results.failed == NULL || (opts.bulk && sink == NULL)) {
		fprintf(stderr, "out of memory for %ld calls of %zu bytes\n",
			opts.calls, opts.payload);
		goto out;
	}
	build_payload(payload, opts.payload);

	/* One checked call before anything is measured, so a broken service
	 * fails the run here rather than as ten thousand failures. */
	if (opts.bulk) {
		memset(sink, 0, opts.payload);
		if (defw2_echo_bulk(echo, payload, sink, opts.payload, &warm,
				    NULL, &status) != DEFW2_OK ||
		    memcmp(sink, payload, opts.payload) != 0) {
			fprintf(stderr, "the echo service returned different data\n");
			goto out;
		}
	} else {
		if (defw2_echo(echo, payload, opts.payload, &warm, &reply,
			       &status) != DEFW2_OK ||
		    reply.len != opts.payload ||
		    memcmp(reply.data, payload, opts.payload) != 0) {
			fprintf(stderr, "the echo service returned different data\n");
			goto out;
		}
	}
	defw2_buffer_free(&reply);
	defw2_status_free(&status);

	for (i = 0; i < opts.warmup; i++) {
		if (opts.bulk) {
			defw2_echo_bulk(echo, payload, sink, opts.payload,
					&warm, NULL, &status);
		} else {
			defw2_echo(echo, payload, opts.payload, &warm, &reply,
				   &status);
			defw2_buffer_free(&reply);
		}
		defw2_status_free(&status);
	}

	if (!touch(opts.ready_path))
		goto out;
	if (!wait_for(opts.go_path, opts.wait_s))
		goto out;

	defw2_process_stats(&before);
	results.loop_start_unix_ns = wall_ns();
	loop_start = mono_ns();
	if (opts.bulk)
		run_bulk(echo, &opts, payload, sink, &results, &call);
	else
		run_eager(echo, &opts, payload, &results, &call);
	results.loop_ns = mono_ns() - loop_start;
	defw2_process_stats(&after);

	results.cpu_ns = (after.user_us + after.system_us -
			  before.user_us - before.system_us) * 1000ull;
	results.max_rss_kib = after.peak_rss_kib;

	if (write_results(&opts, &results))
		rc = results.failures ? 1 : 0;
	if (results.failures)
		fprintf(stderr, "client %ld: %ld of %ld calls failed (%s)\n",
			opts.index, results.failures, opts.calls,
			results.message);
out:
	defw2_buffer_free(&reply);
	defw2_status_free(&status);
	defw2_binding_free(echo);
	defw2_finalize(rt);
	free(results.durations);
	free(results.failed);
	free(payload);
	free(sink);
	return rc;
}
