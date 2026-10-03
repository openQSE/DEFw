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
 *
 * W5 and W6 time whole QPM jobs instead of calls: async_run, then read_cq
 * until the completion is ready. Their subject is QFw's fake IQM QPM, which
 * this finds through a directory, since it is a service QFw runs.
 */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <defw2/defw2_dir.h>
#include <defw2/defw2_echo.h>
#include <defw2/defw2_qpm.h>
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
	/*
	 * W4. The address is then the directory's and every call is a
	 * resolve, so what is measured is the control plane rather than the
	 * echo path. resolve_type narrows the query the way a real client
	 * would, since resolving everything is not what anyone does.
	 */
	bool		resolve;
	const char	*resolve_type;
	/*
	 * W5 and W6. The address is a directory's, service_id names the QPM
	 * in it, or is NULL for the directory's only one, and every measured
	 * call is a whole job. W6 asks for the statevector and lends a buffer
	 * for it.
	 */
	bool		qpm;
	const char	*service_id;
	uint32_t	qubits;
	uint32_t	shots;
	bool		statevector;
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
		"  --resolve           W4: call the directory at --address\n"
		"                      instead of an echo service\n"
		"  --resolve-type S    narrow the resolve to this service_type\n"
		"  --qpm               W5 and W6: run whole jobs on the QPM\n"
		"                      --service-id names in the directory at\n"
		"                      --address\n"
		"  --service-id S      the QPM (default: the directory's only one)\n"
		"  --qubits N          per job (default 4)\n"
		"  --shots N           per job (default 1024)\n"
		"  --statevector       W6: return the statevector into a buffer\n"
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
	/*
	 * W5 and W6, per job: the QPM's own run time as it reported it, the
	 * read_cq that collected the job, and how many read_cq calls that
	 * took. Absent for the other workloads.
	 */
	uint64_t	*backend_ns;
	uint64_t	*collect_ns;
	uint64_t	*polls;
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

/*
 * W4: directory resolve. The control plane's latency, which is what a client
 * pays before it can call anything at all.
 *
 * An empty answer is not a failure here. The directory may legitimately have
 * nothing matching, and counting that as an error would make the workload
 * depend on what else happens to be registered; what W4 measures is the cost
 * of asking.
 */
static void run_resolve(defw2_dir_t *dir, const struct options *opts,
			struct results *results,
			const defw2_call_opts_t *call)
{
	defw2_dir_query_t query;
	long i;

	memset(&query, 0, sizeof(query));
	query.service_type = opts->resolve_type;

	for (i = 0; i < opts->calls; i++) {
		defw2_dir_result_t result = { 0 };
		defw2_status_t status = { 0 };
		uint64_t started = mono_ns();
		defw2_rc_t rc;

		rc = defw2_dir_resolve(dir, &query, call, &result, &status);
		results->durations[i] = mono_ns() - started;

		if (rc != DEFW2_OK)
			note_failure(results, i, defw2_strerror(rc));
		else if (status.category != DEFW2_CAT_OK)
			note_failure(results, i,
				     defw2_category_name(status.category));

		defw2_dir_result_free(&result);
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

static void write_u64_array(FILE *out, const char *name,
			    const uint64_t *values, long count)
{
	long i;

	fprintf(out, "\"%s\":[", name);
	for (i = 0; i < count; i++)
		fprintf(out, "%s%" PRIu64, i ? "," : "", values[i]);
	fputs("],", out);
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
			  const struct results *results,
			  const char *service_id)
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
	if (service_id != NULL) {
		fputs("\"service_id\":\"", out);
		json_escape(out, service_id);
		fputs("\",", out);
	}

	fputs("\"durations_ns\":[", out);
	for (i = 0; i < opts->calls; i++)
		fprintf(out, "%s%" PRIu64, i ? "," : "", results->durations[i]);
	fputs("],", out);
	if (results->polls != NULL) {
		write_u64_array(out, "backend_ns", results->backend_ns,
				opts->calls);
		write_u64_array(out, "collect_ns", results->collect_ns,
				opts->calls);
		write_u64_array(out, "polls", results->polls, opts->calls);
	}

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
		if (strcmp(name, "--resolve") == 0) {
			opts->resolve = true;
			continue;
		}
		if (strcmp(name, "--qpm") == 0) {
			opts->qpm = true;
			continue;
		}
		if (strcmp(name, "--statevector") == 0) {
			opts->statevector = true;
			continue;
		}
		if (value == NULL)
			return false;
		i++;
		if (strcmp(name, "--address") == 0)
			opts->address = value;
		else if (strcmp(name, "--traceparent") == 0)
			opts->traceparent = value;
		else if (strcmp(name, "--resolve-type") == 0)
			opts->resolve_type = value;
		else if (strcmp(name, "--service-id") == 0)
			opts->service_id = value;
		else if (strcmp(name, "--qubits") == 0)
			opts->qubits = (uint32_t)strtoul(value, NULL, 10);
		else if (strcmp(name, "--shots") == 0)
			opts->shots = (uint32_t)strtoul(value, NULL, 10);
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
	       opts->calls > 0 && opts->payload > 0 &&
	       (!opts->qpm || (opts->qubits > 0 && opts->qubits <= 30 &&
			       opts->shots > 0));
}

/* --- W5 and W6: whole jobs on a QPM ----------------------------------- */

/* Points of a statevector checked after each W6 job. */
#define QPM_CHECK_POINTS	256
/*
 * Tries at the checked job. QFw's QPM fails a job now and then under
 * concurrent callers, which should not cost a whole run. A broken service
 * fails every try.
 */
#define QPM_CHECK_TRIES		3

struct qpm_client {
	defw2_binding_t		*admission;
	defw2_binding_t		*execution;
	char			service_id[256];	/* the one measured */
	uint64_t		reservation_id;
	char			qasm[256];
	char			why[512];		/* the last failure */
	defw2_result_buffer_t	lent;		/* W6 only */
};

/*
 * One unsigned number from an answer's extra JSON. The fake reports its own
 * run time there, and that is the only value this client needs from it, so
 * a key search stands in for a JSON parser.
 */
static bool json_u64(const char *json, const char *key, uint64_t *out)
{
	char pattern[128];
	const char *at;
	char *end;

	if (json == NULL)
		return false;
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	at = strstr(json, pattern);
	if (at == NULL)
		return false;
	at += strlen(pattern);
	while (*at == ' ')
		at++;
	*out = strtoull(at, &end, 10);
	return end != at;
}

/*
 * Amplitude k of the fake IQM QPM's statevector is
 * exp(2 pi i frac(k phi)) / sqrt(2^n), as QFw's fake_statevector defines it,
 * with phi the golden ratio's fractional part. Checking a few hundred points
 * spread across the vector catches a wrong layout, scale or offset without
 * costing more than the job did.
 */
static bool amplitude_matches(const double *amplitudes, uint64_t k,
			      double norm)
{
	const double phi = (sqrt(5.0) - 1.0) / 2.0;
	double phase = 2.0 * M_PI * fmod((double)k * phi, 1.0);

	return fabs(amplitudes[2 * k] - cos(phase) * norm) <= 1e-9 * norm &&
	       fabs(amplitudes[2 * k + 1] - sin(phase) * norm) <= 1e-9 * norm;
}

static bool statevector_matches(const void *data, uint32_t qubits)
{
	const double *amplitudes = data;
	uint64_t count = 1ull << qubits;
	uint64_t stride = count / QPM_CHECK_POINTS + 1;
	double norm = 1.0 / sqrt((double)count);
	uint64_t k;

	for (k = 0; k < count; k += stride)
		if (!amplitude_matches(amplitudes, k, norm))
			return false;
	return amplitude_matches(amplitudes, count - 1, norm);
}

/*
 * Find the QPM by its service_id in the directory, or take the directory's
 * only one, as in a QFw run, which names its QPM for the run. Then bind the
 * two APIs a job uses at the providers its record names.
 */
static bool qpm_open(defw2_rt_t *rt, const struct options *opts,
		     struct qpm_client *qpm)
{
	defw2_call_opts_t call = { .timeout_ms = opts->timeout_ms };
	defw2_dir_result_t found = { 0 };
	defw2_status_t status = { 0 };
	const defw2_dir_record_t *record;
	defw2_dir_query_t query;
	defw2_dir_t *dir = NULL;
	bool ok = false;
	size_t i;

	memset(&query, 0, sizeof(query));
	query.service_id = opts->service_id;
	query.service_type = "qfw.qpm";
	if (defw2_dir_open(rt, opts->address, &dir) != DEFW2_OK ||
	    defw2_dir_resolve(dir, &query, &call, &found, &status) !=
	    DEFW2_OK || status.category != DEFW2_CAT_OK ||
	    found.entry_count == 0) {
		fprintf(stderr, "no QPM %s in the directory at %s\n",
			opts->service_id ? opts->service_id : "at all",
			opts->address);
		goto out;
	}
	record = &found.entries[0].record;
	if (found.entry_count > 1)
		fprintf(stderr, "%zu QPMs in the directory, measuring %s\n",
			found.entry_count, record->service_id);
	snprintf(qpm->service_id, sizeof(qpm->service_id), "%s",
		 record->service_id ? record->service_id : "");
	for (i = 0; i < record->binding_count; i++) {
		const defw2_dir_binding_t *binding = &record->bindings[i];
		defw2_binding_t **slot = NULL;

		if (binding->api_id == NULL)
			continue;
		if (strcmp(binding->api_id, DEFW2_API_QPM_ADMISSION) == 0)
			slot = &qpm->admission;
		else if (strcmp(binding->api_id, DEFW2_API_QPM_EXECUTION) == 0)
			slot = &qpm->execution;
		if (slot == NULL || *slot != NULL)
			continue;
		if (defw2_binding_create(rt, record->address,
					 binding->provider_id, slot) !=
		    DEFW2_OK) {
			fprintf(stderr, "cannot reach %s at %s\n",
				binding->api_id, record->address);
			goto out;
		}
	}
	ok = qpm->admission != NULL && qpm->execution != NULL;
	if (!ok)
		fprintf(stderr, "%s serves no QPM admission and execution\n",
			qpm->service_id);
out:
	defw2_dir_result_free(&found);
	defw2_status_free(&status);
	defw2_dir_close(dir);
	return ok;
}

/*
 * One reservation for every job the client runs, as an application holds
 * one for its allocation, so admission stays out of the measured loop.
 */
static bool qpm_reserve(struct qpm_client *qpm, const struct options *opts,
			uint64_t jobs)
{
	defw2_call_opts_t call = { .timeout_ms = opts->timeout_ms };
	defw2_qpm_decision_t out = { 0 };
	defw2_status_t status = { 0 };
	defw2_qpm_reserve_req_t req;
	char job_id[64];
	bool ok;

	snprintf(job_id, sizeof(job_id), "defw2-bench-%ld", opts->index);
	memset(&req, 0, sizeof(req));
	req.job_id = job_id;
	req.allocation_id = job_id;
	req.workload_kind = "quantum";
	req.num_qubits = opts->qubits;
	req.walltime_ns = 3600ull * 1000000000ull;
	req.ttl_ns = req.walltime_ns;
	req.has_task_class = true;
	req.task_class.count = jobs;
	req.task_class.qubit_count = opts->qubits;
	req.task_class.depth = 1;
	req.task_class.one_q_gate_count = 1;
	req.task_class.shots = opts->shots;
	req.task_class.measurement_count = opts->qubits;
	req.extra = "{\"owner\":{\"user\":\"defw2-bench\"},"
		    "\"run_context\":{\"operation\":\"async_run\"}}";

	ok = defw2_qpm_reserve(qpm->admission, &req, &call, &out, &status) ==
	     DEFW2_OK && status.category == DEFW2_CAT_OK &&
	     out.decision != NULL &&
	     strcmp(out.decision, DEFW2_QPM_DECISION_ACCEPTED) == 0 &&
	     out.reservation_id != 0;
	if (ok)
		qpm->reservation_id = out.reservation_id;
	else
		fprintf(stderr, "the reservation was not accepted: %s\n",
			out.message ? out.message :
			out.reason ? out.reason :
			defw2_category_name(status.category));
	defw2_qpm_decision_free(&out);
	defw2_status_free(&status);
	return ok;
}

static void qpm_release(struct qpm_client *qpm, const struct options *opts)
{
	defw2_call_opts_t call = { .timeout_ms = opts->timeout_ms };
	defw2_qpm_decision_t out = { 0 };
	defw2_status_t status = { 0 };
	defw2_qpm_close_req_t req;

	if (qpm->reservation_id == 0)
		return;
	memset(&req, 0, sizeof(req));
	req.ctx.reservation_id = qpm->reservation_id;
	defw2_qpm_release(qpm->admission, &req, &call, &out, &status);
	defw2_qpm_decision_free(&out);
	defw2_status_free(&status);
}

/* What failed, in the words of the transport or the service. */
static const char *qpm_failure(struct qpm_client *qpm, const char *what,
			       defw2_rc_t rc, const defw2_status_t *status)
{
	snprintf(qpm->why, sizeof(qpm->why), "%s failed: %s%s%s", what,
		 rc != DEFW2_OK ? defw2_strerror(rc) :
		 defw2_category_name(status->category),
		 status->message ? ": " : "",
		 status->message ? status->message : "");
	return qpm->why;
}

/*
 * One job: async_run, then read_cq until the completion is ready. It polls
 * back to back, as fast as the QPM answers, so what is measured is the
 * framework's latency rather than a sleep between polls. A W6 job lends its
 * buffer to every read_cq, which is how the API is meant to be called when
 * the size is known: the call that finds the completion delivers it. A job
 * gets the call timeout to complete in, so a completion the QPM loses fails
 * the job rather than the run.
 *
 * Returns NULL for a job that completed, and what went wrong otherwise.
 */
static const char *qpm_job(struct qpm_client *qpm, const struct options *opts,
			   const defw2_call_opts_t *call, uint64_t *collect_ns,
			   uint64_t *backend_ns, uint64_t *polls)
{
	defw2_result_buffer_t *lent = opts->statevector ? &qpm->lent : NULL;
	defw2_qpm_task_t task = { 0 };
	defw2_status_t status = { 0 };
	defw2_qpm_task_req_t read;
	defw2_qpm_run_req_t run;
	const char *why = NULL;
	uint64_t deadline;
	char cid[128];
	defw2_rc_t rc;

	memset(&run, 0, sizeof(run));
	run.ctx.reservation_id = qpm->reservation_id;
	run.circuit.format = DEFW2_QPM_FORMAT_OPENQASM2;
	run.circuit.data = qpm->qasm;
	run.circuit.len = strlen(qpm->qasm);
	run.num_qubits = opts->qubits;
	run.num_shots = opts->shots;
	run.return_statevector = opts->statevector;
	rc = defw2_qpm_async_run(qpm->execution, &run, call, &task, &status);
	if (rc != DEFW2_OK || status.category != DEFW2_CAT_OK ||
	    task.cid == NULL) {
		why = qpm_failure(qpm, "async_run", rc, &status);
		goto out;
	}
	snprintf(cid, sizeof(cid), "%s", task.cid);
	defw2_qpm_task_free(&task);
	defw2_status_free(&status);

	memset(&read, 0, sizeof(read));
	read.ctx.reservation_id = qpm->reservation_id;
	read.cid = cid;
	*polls = 0;
	deadline = mono_ns() + (uint64_t)opts->timeout_ms * 1000000ull;
	for (;;) {
		uint64_t started = mono_ns();

		rc = defw2_qpm_read_cq(qpm->execution, &read, lent, call,
				       &task, &status);
		*collect_ns = mono_ns() - started;
		(*polls)++;
		if (rc != DEFW2_OK || status.category != DEFW2_CAT_OK) {
			why = qpm_failure(qpm, "read_cq", rc, &status);
			goto out;
		}
		if (task.completion_ready)
			break;
		defw2_qpm_task_free(&task);
		defw2_status_free(&status);
		if (mono_ns() > deadline) {
			why = "the job did not complete within the call timeout";
			goto out;
		}
	}

	if (task.outcome == NULL ||
	    strcmp(task.outcome, DEFW2_QPM_COMPLETED) != 0)
		why = "the job did not complete";
	else if (opts->statevector &&
		 (!task.statevector_delivered ||
		  task.statevector.nbytes != (16ull << opts->qubits)))
		why = "the statevector was not delivered";
	/* A QPM that does not say how long it ran counts as no time. */
	if (!json_u64(task.extra, "observed_fake_runtime_ns", backend_ns))
		*backend_ns = 0;
out:
	defw2_qpm_task_free(&task);
	defw2_status_free(&status);
	return why;
}

/*
 * W5 and W6. The same shape as the echo run: one checked job and the warmup
 * outside the run's trace, then the measured jobs between ready and the end.
 */
static int run_qpm(const struct options *opts, defw2_rt_t *rt,
		   struct results *results, const defw2_call_opts_t *call,
		   const defw2_call_opts_t *warm)
{
	uint64_t unused_collect, unused_backend, unused_polls, loop_start;
	defw2_process_stats_t before, after;
	struct qpm_client qpm = { 0 };
	const char *why;
	int rc = 1;
	long i;

	snprintf(qpm.qasm, sizeof(qpm.qasm),
		 "OPENQASM 2.0;\ninclude \"qelib1.inc\";\nqreg q[%u];\n"
		 "creg c[%u];\nh q[0];\nmeasure q -> c;\n",
		 opts->qubits, opts->qubits);
	results->backend_ns = calloc((size_t)opts->calls, sizeof(uint64_t));
	results->collect_ns = calloc((size_t)opts->calls, sizeof(uint64_t));
	results->polls = calloc((size_t)opts->calls, sizeof(uint64_t));
	if (opts->statevector) {
		qpm.lent.capacity = 16ull << opts->qubits;
		qpm.lent.data = malloc(qpm.lent.capacity);
	}
	if (results->backend_ns == NULL || results->collect_ns == NULL ||
	    results->polls == NULL ||
	    (opts->statevector && qpm.lent.data == NULL)) {
		fprintf(stderr, "out of memory for %ld jobs\n", opts->calls);
		goto out;
	}
	if (!qpm_open(rt, opts, &qpm) ||
	    !qpm_reserve(&qpm, opts, (uint64_t)(opts->calls + opts->warmup) + 1))
		goto out;

	/* One checked job before anything is measured. */
	for (i = 0; i < QPM_CHECK_TRIES; i++) {
		if (opts->statevector)
			memset(qpm.lent.data, 0, qpm.lent.capacity);
		why = qpm_job(&qpm, opts, warm, &unused_collect,
			      &unused_backend, &unused_polls);
		if (why == NULL && opts->statevector &&
		    !statevector_matches(qpm.lent.data, opts->qubits))
			why = "the statevector is not the fake's";
		if (why == NULL)
			break;
		fprintf(stderr, "the checked job failed: %s\n", why);
	}
	if (why != NULL)
		goto out;
	for (i = 0; i < opts->warmup; i++)
		qpm_job(&qpm, opts, warm, &unused_collect, &unused_backend,
			&unused_polls);

	if (!touch(opts->ready_path) || !wait_for(opts->go_path, opts->wait_s))
		goto out;

	defw2_process_stats(&before);
	results->loop_start_unix_ns = wall_ns();
	loop_start = mono_ns();
	for (i = 0; i < opts->calls; i++) {
		uint64_t started;

		/*
		 * Cleared so the check below proves this job's statevector
		 * arrived, since every job returns the same one. Outside the
		 * timed region, but inside the loop, as for W3.
		 */
		if (opts->statevector)
			memset(qpm.lent.data, 0, qpm.lent.capacity);
		started = mono_ns();
		why = qpm_job(&qpm, opts, call, &results->collect_ns[i],
			      &results->backend_ns[i], &results->polls[i]);
		results->durations[i] = mono_ns() - started;

		/* Checked after the clock stops. */
		if (why == NULL && opts->statevector &&
		    !statevector_matches(qpm.lent.data, opts->qubits))
			why = "the statevector is not the fake's";
		if (why != NULL)
			note_failure(results, i, why);
		else if (opts->statevector)
			results->bytes_moved += qpm.lent.capacity;
	}
	results->loop_ns = mono_ns() - loop_start;
	defw2_process_stats(&after);

	results->cpu_ns = (after.user_us + after.system_us -
			   before.user_us - before.system_us) * 1000ull;
	results->max_rss_kib = after.peak_rss_kib;
	if (write_results(opts, results, qpm.service_id))
		rc = results->failures ? 1 : 0;
	if (results->failures)
		fprintf(stderr, "client %ld: %ld of %ld jobs failed (%s)\n",
			opts->index, results->failures, opts->calls,
			results->message);
out:
	qpm_release(&qpm, opts);
	defw2_binding_free(qpm.admission);
	defw2_binding_free(qpm.execution);
	free(qpm.lent.data);
	free(results->backend_ns);
	free(results->collect_ns);
	free(results->polls);
	return rc;
}

int main(int argc, char **argv)
{
	struct options opts = {
		.provider_id = DEFW2_PROVIDER_ECHO,
		.calls = 1000,
		.warmup = 100,
		.payload = 64,
		.qubits = 4,
		.shots = 1024,
		.timeout_ms = 60000,
		.wait_s = 300,
	};
	defw2_process_stats_t before, after;
	struct results results = { 0 };
	defw2_binding_t *echo = NULL;
	defw2_dir_t *dir = NULL;	/* W4 only */
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
	/*
	 * W4 talks to the directory, and W5 and W6 to a QPM found there, so
	 * neither has an echo service to bind.
	 */
	if (!opts.resolve && !opts.qpm &&
	    defw2_binding_create(rt, opts.address, opts.provider_id, &echo) !=
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
	if (opts.qpm) {
		rc = run_qpm(&opts, rt, &results, &call, &warm);
		goto out;
	}
	if (opts.resolve) {
		/*
		 * W4 takes a different path from here: a directory handle
		 * rather than an echo binding, and no payload to verify, so
		 * the echo warm-up and its data check do not apply.
		 */
		defw2_dir_query_t warm_query;
		long w;

		memset(&warm_query, 0, sizeof(warm_query));
		warm_query.service_type = opts.resolve_type;
		if (defw2_dir_open(rt, opts.address, &dir) != DEFW2_OK) {
			fprintf(stderr, "cannot reach the directory at %s\n",
				opts.address);
			goto out;
		}
		for (w = 0; w < opts.warmup; w++) {
			defw2_dir_result_t warm_result = { 0 };

			defw2_dir_resolve(dir, &warm_query, &warm,
					  &warm_result, &status);
			defw2_dir_result_free(&warm_result);
			defw2_status_free(&status);
		}
		if (!touch(opts.ready_path))
			goto out;
		if (!wait_for(opts.go_path, opts.wait_s))
			goto out;

		defw2_process_stats(&before);
		results.loop_start_unix_ns = wall_ns();
		loop_start = mono_ns();
		run_resolve(dir, &opts, &results, &call);
		results.loop_ns = mono_ns() - loop_start;
		defw2_process_stats(&after);

		results.cpu_ns = (after.user_us + after.system_us -
				  before.user_us - before.system_us) * 1000ull;
		results.max_rss_kib = after.peak_rss_kib;
		if (write_results(&opts, &results, NULL))
			rc = results.failures ? 1 : 0;
		if (results.failures)
			fprintf(stderr,
				"client %ld: %ld of %ld resolves failed (%s)\n",
				opts.index, results.failures, opts.calls,
				results.message);
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

	if (write_results(&opts, &results, NULL))
		rc = results.failures ? 1 : 0;
	if (results.failures)
		fprintf(stderr, "client %ld: %ld of %ld calls failed (%s)\n",
			opts.index, results.failures, opts.calls,
			results.message);
out:
	defw2_buffer_free(&reply);
	defw2_status_free(&status);
	defw2_dir_close(dir);
	defw2_binding_free(echo);
	defw2_finalize(rt);
	free(results.durations);
	free(results.failed);
	free(payload);
	free(sink);
	return rc;
}
