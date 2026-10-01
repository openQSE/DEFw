/*
 * defw2-echo: serve the echo API, or call it.
 *
 * This is the development tool that makes the echo path usable from a shell
 * and across two nodes. It is deliberately small. The benchmark client,
 * which records spans and writes the OTLP files the comparison reads, is
 * defw2-bench and is a separate thing.
 *
 *	defw2-echo serve
 *	defw2-echo ping na+sm://1234-0 -n 10000 -s 64
 *	defw2-echo ping ofi+tcp://10.0.0.5:8090 -b 16777216
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <defw2/defw2_dir.h>
#include <defw2/defw2_echo.h>

struct options {
	const char	*address;
	uint16_t	provider_id;
	unsigned long	calls;
	size_t		payload;
	size_t		bulk;
	uint32_t	timeout_ms;
	bool		calls_set;
};

static void usage(void)
{
	fprintf(stderr,
		"usage: defw2-echo serve\n"
		"       defw2-echo ping <address> [-n calls] [-s bytes]\n"
		"                                 [-b bytes] [-p provider]\n"
		"                                 [-t timeout_ms]\n"
		"\n"
		"The address to serve on comes from DEFW2_ADDRESS, and the\n"
		"rest of the environment is the contract in src2/README.md.\n");
}

static double now_s(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int compare_double(const void *a, const void *b)
{
	double left = *(const double *)a, right = *(const double *)b;

	return (left > right) - (left < right);
}

/* Nearest rank, the same rule the v1 harness uses, so the two agree. */
static double percentile(const double *sorted, size_t count, double fraction)
{
	size_t rank = (size_t)(fraction * (double)count + 0.999999);

	if (rank == 0)
		rank = 1;
	if (rank > count)
		rank = count;
	return sorted[rank - 1];
}

/*
 * Wait for a signal on a thread of its own, so the shutdown path is ordinary
 * code rather than a signal handler, where almost nothing is legal to call.
 * The main thread belongs to defw2_service_run.
 */
struct stopping {
	defw2_service_t		*svc;
	defw2_dir_agent_t	*agent;	/* NULL without a directory */
};

static void *wait_for_signal(void *arg)
{
	struct stopping *ctx = arg;
	sigset_t stopping;
	int signo = 0;

	sigemptyset(&stopping);
	sigaddset(&stopping, SIGINT);
	sigaddset(&stopping, SIGTERM);
	sigwait(&stopping, &signo);

	defw2_log(defw2_service_runtime(ctx->svc), DEFW2_LOG_MESSAGE,
		  "signal %d, stopping", signo);
	/*
	 * Deregister first. Shutdown stops the Margo instance, so an agent
	 * stopped after it cannot send its goodbye, and the record would sit
	 * UP at an address nobody serves until the directory timed it out.
	 */
	defw2_dir_agent_stop(ctx->agent);
	defw2_service_shutdown(ctx->svc);
	return NULL;
}

static int serve(void)
{
	defw2_dir_agent_t *agent = NULL;
	defw2_service_t *svc = NULL;
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;
	struct stopping ctx = { 0 };
	pthread_t waiter;
	sigset_t stopping;

	/* Blocked here, and so in every thread made after this point. */
	sigemptyset(&stopping);
	sigaddset(&stopping, SIGINT);
	sigaddset(&stopping, SIGTERM);
	if (pthread_sigmask(SIG_BLOCK, &stopping, NULL) != 0) {
		fprintf(stderr, "cannot block signals: %s\n", strerror(errno));
		return 1;
	}

	defw2_config_from_env(&cfg);
	cfg.role = DEFW2_ROLE_SERVER;
	if (defw2_init(&cfg, &rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start on %s\n", cfg.address);
		return 1;
	}
	/*
	 * The node name, so several echo services can register at once. One
	 * hardcoded service_id meant the second instance was refused as a
	 * conflict with the first, which is the directory being right about a
	 * mistake this program was making.
	 */
	if (defw2_service_create(rt, defw2_node_name(rt) != NULL ?
					 defw2_node_name(rt) : "echo",
				 DEFW2_API_ECHO,
				 DEFW2_PROVIDER_ECHO, &svc) != DEFW2_OK ||
	    defw2_echo_bind(svc, NULL) != DEFW2_OK) {
		fprintf(stderr, "cannot bind %s\n", DEFW2_API_ECHO);
		defw2_service_destroy(svc);
		defw2_finalize(rt);
		return 1;
	}

	/*
	 * Register, when this deployment has a directory. Without one the
	 * service is still perfectly usable by a caller that was given the
	 * address, which is how the benchmarks drive it, so an absent
	 * directory is a configuration choice rather than a failure.
	 */
	if (defw2_dirsvc(rt) != NULL) {
		defw2_dir_record_t record;
		const defw2_dir_binding_t bindings[] = {
			{ "echo", DEFW2_API_ECHO, DEFW2_API_VERSION_MAJOR,
			  DEFW2_PROVIDER_ECHO },
		};

		memset(&record, 0, sizeof(record));
		record.service_type = DEFW2_API_ECHO;
		record.selector.name = defw2_node_name(rt);
		record.bindings = bindings;
		record.binding_count = 1;
		if (defw2_dir_agent_start(svc, defw2_dirsvc(rt), &record, 0,
					  &agent) != DEFW2_OK) {
			fprintf(stderr, "cannot register with the directory "
				"at %s\n", defw2_dirsvc(rt));
			defw2_service_shutdown(svc);
			defw2_service_destroy(svc);
			defw2_finalize(rt);
			return 1;
		}
	}

	/* One line, on stdout, so a script can read the address back. */
	printf("%s\n", defw2_service_address(svc));
	fflush(stdout);

	ctx.svc = svc;
	ctx.agent = agent;
	if (pthread_create(&waiter, NULL, wait_for_signal, &ctx) != 0) {
		fprintf(stderr, "cannot wait for a signal: %s\n",
			strerror(errno));
		defw2_dir_agent_stop(agent);
		defw2_service_shutdown(svc);
		defw2_service_destroy(svc);
		defw2_finalize(rt);
		return 1;
	}

	defw2_service_run(svc);
	pthread_join(waiter, NULL);
	/* The waiter already stopped the agent, while the runtime could send. */
	defw2_service_destroy(svc);
	defw2_finalize(rt);
	return 0;
}

static int ping_eager(defw2_binding_t *binding, const struct options *opts)
{
	defw2_call_opts_t call = { .timeout_ms = opts->timeout_ms };
	unsigned char *payload = calloc(1, opts->payload ? opts->payload : 1);
	double *samples = calloc(opts->calls, sizeof(*samples));
	unsigned long bad = 0, i;
	double started, elapsed;

	if (payload == NULL || samples == NULL) {
		fprintf(stderr, "out of memory\n");
		free(payload);
		free(samples);
		return 1;
	}

	started = now_s();
	for (i = 0; i < opts->calls; i++) {
		defw2_buffer_t reply = { 0 };
		defw2_status_t status = { 0 };
		double call_started = now_s();

		if (defw2_echo(binding, payload, opts->payload, &call,
			       &reply, &status) != DEFW2_OK ||
		    reply.len != opts->payload)
			bad++;
		samples[i] = now_s() - call_started;
		defw2_buffer_free(&reply);
		defw2_status_free(&status);
	}
	elapsed = now_s() - started;

	qsort(samples, opts->calls, sizeof(*samples), compare_double);
	printf("echo %zu bytes, %lu calls, %lu failed\n", opts->payload,
	       opts->calls, bad);
	printf("  p50 %.3f ms, p99 %.3f ms, min %.3f ms, max %.3f ms\n",
	       percentile(samples, opts->calls, 0.50) * 1e3,
	       percentile(samples, opts->calls, 0.99) * 1e3,
	       samples[0] * 1e3, samples[opts->calls - 1] * 1e3);
	printf("  %.1f calls/s over %.3f s\n", (double)opts->calls / elapsed,
	       elapsed);

	free(payload);
	free(samples);
	return bad ? 1 : 0;
}

static int ping_bulk(defw2_binding_t *binding, const struct options *opts)
{
	defw2_call_opts_t call = { .timeout_ms = opts->timeout_ms };
	unsigned char *source = malloc(opts->bulk);
	unsigned char *sink = malloc(opts->bulk);
	unsigned long bad = 0, i;
	double started, best = 0.0, total = 0.0;
	uint64_t moved = 0;

	if (source == NULL || sink == NULL) {
		fprintf(stderr, "out of memory for %zu bytes\n", opts->bulk);
		free(source);
		free(sink);
		return 1;
	}
	memset(source, 0x5a, opts->bulk);

	for (i = 0; i < opts->calls; i++) {
		defw2_status_t status = { 0 };
		double elapsed;

		started = now_s();
		if (defw2_echo_bulk(binding, source, sink, opts->bulk, &call,
				    &moved, &status) != DEFW2_OK)
			bad++;
		elapsed = now_s() - started;
		total += elapsed;
		if (best == 0.0 || elapsed < best)
			best = elapsed;
		defw2_status_free(&status);
	}

	printf("bulk echo %zu bytes, %lu calls, %lu failed\n", opts->bulk,
	       opts->calls, bad);
	printf("  mean %.3f ms, best %.3f ms\n", total / (double)opts->calls * 1e3,
	       best * 1e3);
	/* Both directions moved, which is what the bytes count reports. */
	printf("  %.1f MiB/s round trip\n",
	       (double)moved / best / (1024.0 * 1024.0));
	if (bad == 0 && memcmp(source, sink, opts->bulk) != 0) {
		fprintf(stderr, "  the payload came back changed\n");
		bad++;
	}

	free(source);
	free(sink);
	return bad ? 1 : 0;
}

static int ping(struct options *opts)
{
	defw2_binding_t *binding = NULL;
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;
	int rc;

	defw2_config_from_env(&cfg);
	cfg.role = DEFW2_ROLE_CLIENT;
	if (defw2_init(&cfg, &rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start a client on %s\n", cfg.address);
		return 1;
	}
	if (defw2_binding_create(rt, opts->address, opts->provider_id,
				 &binding) != DEFW2_OK) {
		fprintf(stderr, "cannot reach %s\n", opts->address);
		defw2_finalize(rt);
		return 1;
	}

	rc = opts->bulk ? ping_bulk(binding, opts) : ping_eager(binding, opts);

	defw2_binding_free(binding);
	defw2_finalize(rt);
	return rc;
}

int main(int argc, char **argv)
{
	struct options opts = {
		.provider_id = DEFW2_PROVIDER_ECHO,
		.calls = 1000,
		.payload = 64,
		.timeout_ms = 60000,
	};
	int i;

	if (argc < 2) {
		usage();
		return 2;
	}
	if (strcmp(argv[1], "serve") == 0)
		return serve();
	if (strcmp(argv[1], "ping") != 0 || argc < 3) {
		usage();
		return 2;
	}

	opts.address = argv[2];
	for (i = 3; i + 1 < argc; i += 2) {
		const char *value = argv[i + 1];

		if (strcmp(argv[i], "-n") == 0) {
			opts.calls = strtoul(value, NULL, 10);
			opts.calls_set = true;
		}
		else if (strcmp(argv[i], "-s") == 0)
			opts.payload = strtoull(value, NULL, 10);
		else if (strcmp(argv[i], "-b") == 0)
			opts.bulk = strtoull(value, NULL, 10);
		else if (strcmp(argv[i], "-p") == 0)
			opts.provider_id = (uint16_t)strtoul(value, NULL, 10);
		else if (strcmp(argv[i], "-t") == 0)
			opts.timeout_ms = (uint32_t)strtoul(value, NULL, 10);
		else {
			usage();
			return 2;
		}
	}
	if (opts.calls == 0 || opts.payload > DEFW2_EAGER_MAX ||
	    opts.bulk > DEFW2_BULK_MAX) {
		usage();
		return 2;
	}
	/* A 256 MiB round trip is not a loop, so bulk asks for few. */
	if (opts.bulk && !opts.calls_set)
		opts.calls = 3;
	return ping(&opts);
}
