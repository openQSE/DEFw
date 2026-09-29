/*
 * Does a run leave behind what the comparison reads?
 *
 * One process, a server and a client, profiling on, a handful of calls, and
 * then the files are read back. It checks what the benchmarking design asks
 * for: a run span, a client and a server span per call, the phase timings on
 * the server's, and the histograms and process totals in the metrics file.
 *
 * The directory comes from argv[1] so the Python checker beside this test
 * can read the same files and validate the JSON itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2_echo.h>
#include <defw2/defw2_telemetry.h>

#define CALLS		16
#define BULK_BYTES	(64 * 1024)

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-36s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

/* How many times does needle appear? The files are small and flat. */
static long occurrences(const char *text, const char *needle)
{
	size_t length = strlen(needle);
	const char *at = text;
	long found = 0;

	while ((at = strstr(at, needle)) != NULL) {
		found++;
		at += length;
	}
	return found;
}

static char *slurp(const char *dir, const char *name)
{
	char path[1024];
	char *text;
	long size;
	FILE *in;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	in = fopen(path, "r");
	if (in == NULL) {
		printf("  cannot read %s\n", path);
		return NULL;
	}
	fseek(in, 0, SEEK_END);
	size = ftell(in);
	rewind(in);
	text = malloc((size_t)size + 1);
	if (text != NULL)
		text[fread(text, 1, (size_t)size, in)] = '\0';
	fclose(in);
	return text;
}

static defw2_rt_t *start(const char *name, defw2_role_t role)
{
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;

	defw2_config_from_env(&cfg);
	cfg.role = role;
	cfg.profile = true;
	/* Each runtime writes its own files, so each needs its own name. */
	cfg.node_name = name;
	if (defw2_init(&cfg, &rt) != DEFW2_OK) {
		printf("%s init FAILED\n", name);
		exit(1);
	}
	return rt;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	defw2_call_opts_t opts = { .timeout_ms = 10000 };
	defw2_rt_t *server, *client;
	defw2_service_t *svc = NULL;
	defw2_binding_t *echo = NULL;
	unsigned char payload[64];
	unsigned char *bulk;
	defw2_status_t status = { 0 };
	char *spans, *metrics;
	int i;

	setenv("DEFW2_TELEMETRY_DIR", dir, 1);
	memset(payload, 0x5a, sizeof(payload));

	server = start("telemetry-server", DEFW2_ROLE_SERVER);
	check("profiling is on", defw2_profiling(server));
	check("the run has a trace", defw2_trace_id(server) != NULL &&
				     strlen(defw2_trace_id(server)) == 32);
	defw2_service_create(server, "echo", DEFW2_API_ECHO,
			     DEFW2_PROVIDER_ECHO, &svc);
	defw2_echo_bind(svc, NULL);

	client = start("telemetry-client", DEFW2_ROLE_CLIENT);
	defw2_telemetry_resource(client, "qfw.bench.harness", "telemetry-smoke");
	defw2_binding_create(client, defw2_service_address(svc),
			     DEFW2_PROVIDER_ECHO, &echo);

	check("run span opens",
	      defw2_telemetry_run_begin(client, "qfw.bench.run") == DEFW2_OK);
	defw2_telemetry_run_attr(client, "qfw.bench.workload", "W1");
	defw2_telemetry_run_attr_int(client, "qfw.bench.calls", CALLS);

	for (i = 0; i < CALLS; i++) {
		defw2_buffer_t reply = { 0 };

		if (defw2_echo(echo, payload, sizeof(payload), &opts, &reply,
			       &status) != DEFW2_OK)
			failures++;
		defw2_buffer_free(&reply);
		defw2_status_free(&status);
	}

	bulk = malloc(BULK_BYTES);
	memset(bulk, 0x27, BULK_BYTES);
	check("bulk call",
	      defw2_echo_bulk(echo, bulk, bulk, BULK_BYTES, &opts, NULL,
			      &status) == DEFW2_OK);
	defw2_status_free(&status);

	defw2_telemetry_run_end(client, NULL);
	defw2_binding_free(echo);
	defw2_finalize(client);

	defw2_service_destroy(svc);
	defw2_finalize(server);
	free(bulk);

	spans = slurp(dir, "spans-telemetry-client.jsonl");
	check("the client wrote spans", spans != NULL);
	if (spans != NULL) {
		check("one transport span per call",
		      occurrences(spans, "\"qfw.transport.rpc\"") == CALLS + 1);
		check("the run span is there",
		      occurrences(spans, "\"qfw.bench.run\"") == 1);
		check("spans name the api",
		      occurrences(spans, "qfw.rpc.api") == CALLS + 1);
		check("spans name the transport",
		      occurrences(spans, "qfw.transport.kind") >= CALLS + 1);
		check("the bulk span counts its bytes",
		      occurrences(spans, "qfw.rpc.bulk.bytes") == 1);
		check("the run attributes are kept",
		      occurrences(spans, "qfw.bench.workload") == 1);
		check("client spans are client kind",
		      occurrences(spans, "\"kind\":3") == CALLS + 1);
		free(spans);
	}

	spans = slurp(dir, "spans-telemetry-server.jsonl");
	check("the server wrote spans", spans != NULL);
	if (spans != NULL) {
		check("one server span per call",
		      occurrences(spans, "\"kind\":2") == CALLS + 1);
		check("the server records its phases",
		      occurrences(spans, "\"name\":\"decode\"") == CALLS + 1 &&
		      occurrences(spans, "\"name\":\"encode\"") == CALLS + 1);
		free(spans);
	}

	metrics = slurp(dir, "metrics-telemetry-client.jsonl");
	check("the client wrote metrics", metrics != NULL);
	if (metrics != NULL) {
		check("round trip histogram",
		      occurrences(metrics, "qfw.transport.rpc.duration") == 1);
		check("byte histogram",
		      occurrences(metrics, "qfw.transport.rpc.bytes") == 1);
		check("process cpu time",
		      occurrences(metrics, "process.cpu.time") == 2);
		check("peak resident set",
		      occurrences(metrics, "process.memory.peak_rss") == 1);
		free(metrics);
	}

	printf("%s\n", failures ? "TELEMETRY SMOKE FAILED" : "TELEMETRY SMOKE PASSED");
	return failures ? 1 : 0;
}
