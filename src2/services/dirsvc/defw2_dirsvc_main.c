/*
 * defw2-dirsvc: the directory service.
 *
 * One process, one provider, the whole of qfw.directory. Everything a client
 * or a service needs to find anything else goes through here, which is why it
 * is the one address that cannot itself be resolved and has to be told.
 *
 *	defw2-dirsvc
 *	defw2-dirsvc --address-file /var/run/qfw/dirsvc.addr
 *	defw2-dirsvc --snapshot /var/lib/qfw/directory.json --timeout 30000
 *	defw2-dirsvc dump ofi+tcp://10.0.0.5:8090
 *
 * The address it serves on comes from DEFW2_ADDRESS, and it prints that
 * address on stdout so a launcher can read it back. --address-file writes the
 * same string to a file, which is the bootstrap the design calls for on a
 * provider without IP addressing: there is no fixed port to agree on in
 * advance, so the launcher exports the path instead and
 * defw2_config_from_env's DEFW2_DIRSVC picks up what it finds there.
 *
 * dump is the operator's view, which is query_directory rather than
 * resolve_services, so it shows records a client can no longer see.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2_dir.h>

struct options {
	const char	*address_file;
	const char	*snapshot;
	uint32_t	timeout_ms;
	uint32_t	scan_ms;
	uint64_t	retention_ms;
};

static void usage(void)
{
	fprintf(stderr,
		"usage: defw2-dirsvc [--address-file path] [--snapshot path]\n"
		"                    [--timeout ms] [--scan ms]\n"
		"                    [--retention ms]\n"
		"       defw2-dirsvc dump <address> [--service-id id]\n"
		"\n"
		"Serves qfw.directory on DEFW2_ADDRESS and prints the\n"
		"address it bound. --address-file writes that address to a\n"
		"file, for a provider where there is no port to agree on in\n"
		"advance; a launcher exports the path as DEFW2_DIRSVC.\n");
}

/*
 * Write the address where a launcher told us to. Through a temporary and a
 * rename, so a reader never sees a partial address, which matters because
 * everything else in the deployment is waiting on this file to appear.
 */
static int write_address_file(const char *path, const char *address)
{
	char temp[1024];
	FILE *out;

	if ((size_t)snprintf(temp, sizeof(temp), "%s.tmp", path) >=
	    sizeof(temp)) {
		fprintf(stderr, "address file path is too long\n");
		return 1;
	}
	out = fopen(temp, "w");
	if (out == NULL) {
		fprintf(stderr, "cannot write %s: %s\n", temp,
			strerror(errno));
		return 1;
	}
	fprintf(out, "%s\n", address);
	if (fclose(out) != 0 || rename(temp, path) != 0) {
		fprintf(stderr, "cannot replace %s: %s\n", path,
			strerror(errno));
		return 1;
	}
	return 0;
}

/*
 * Wait for a signal on a thread of its own, so the shutdown path is ordinary
 * code rather than a signal handler. The main thread belongs to
 * defw2_service_run. Same shape as defw2-echo.
 */
static void *wait_for_signal(void *arg)
{
	defw2_service_t *svc = arg;
	sigset_t stopping;
	int signo = 0;

	sigemptyset(&stopping);
	sigaddset(&stopping, SIGINT);
	sigaddset(&stopping, SIGTERM);
	sigwait(&stopping, &signo);

	defw2_log(defw2_service_runtime(svc), DEFW2_LOG_MESSAGE,
		  "signal %d, stopping the directory", signo);
	defw2_service_shutdown(svc);
	return NULL;
}

static int serve(const struct options *opts)
{
	defw2_dir_store_opts_t store_opts;
	defw2_service_t *svc = NULL;
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;
	pthread_t waiter;
	sigset_t stopping;

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

	memset(&store_opts, 0, sizeof(store_opts));
	store_opts.heartbeat_timeout_ms = opts->timeout_ms;
	store_opts.scan_interval_ms = opts->scan_ms;
	store_opts.retention_ms = opts->retention_ms;
	store_opts.snapshot_path = opts->snapshot;

	if (defw2_service_create(rt, "dirsvc", DEFW2_API_DIR,
				 DEFW2_PROVIDER_DIR, &svc) != DEFW2_OK ||
	    defw2_dir_bind(svc, &store_opts) != DEFW2_OK) {
		fprintf(stderr, "cannot bind %s\n", DEFW2_API_DIR);
		defw2_service_destroy(svc);
		defw2_finalize(rt);
		return 1;
	}

	/*
	 * The file before the line on stdout: a launcher that is polling for
	 * the file should find it the moment anything at all has been
	 * announced.
	 */
	if (opts->address_file != NULL &&
	    write_address_file(opts->address_file,
			       defw2_service_address(svc)) != 0) {
		defw2_service_shutdown(svc);
		defw2_service_destroy(svc);
		defw2_finalize(rt);
		return 1;
	}
	printf("%s\n", defw2_service_address(svc));
	fflush(stdout);

	if (pthread_create(&waiter, NULL, wait_for_signal, svc) != 0) {
		fprintf(stderr, "cannot wait for a signal: %s\n",
			strerror(errno));
		defw2_service_shutdown(svc);
		defw2_service_destroy(svc);
		defw2_finalize(rt);
		return 1;
	}

	defw2_service_run(svc);
	pthread_join(waiter, NULL);
	/*
	 * The address file named a process that has stopped, so leaving it
	 * would send the next client somewhere that will not answer.
	 */
	if (opts->address_file != NULL)
		remove(opts->address_file);
	defw2_service_destroy(svc);
	defw2_finalize(rt);
	return 0;
}

/* --- dump ------------------------------------------------------------ */

static const char *or_dash(const char *s)
{
	return (s != NULL && s[0] != '\0') ? s : "-";
}

static int dump(const char *address, const char *service_id)
{
	defw2_dir_query_t query;
	defw2_dir_result_t result;
	defw2_status_t status;
	defw2_call_opts_t opts;
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;
	defw2_dir_t *dir = NULL;
	size_t i, j;
	int rc = 0;

	defw2_config_from_env(&cfg);
	cfg.role = DEFW2_ROLE_CLIENT;
	if (defw2_init(&cfg, &rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start a client runtime\n");
		return 1;
	}
	if (defw2_dir_open(rt, address, &dir) != DEFW2_OK) {
		fprintf(stderr, "cannot reach the directory at %s\n",
			or_dash(address));
		defw2_finalize(rt);
		return 1;
	}

	memset(&query, 0, sizeof(query));
	memset(&result, 0, sizeof(result));
	memset(&status, 0, sizeof(status));
	memset(&opts, 0, sizeof(opts));
	opts.timeout_ms = 10000;
	query.service_id = service_id;

	if (defw2_dir_query(dir, &query, &opts, &result, &status) !=
	    DEFW2_OK) {
		fprintf(stderr, "the directory did not answer\n");
		rc = 1;
		goto out;
	}
	if (status.code != DEFW2_OK) {
		fprintf(stderr, "the directory refused: %s\n",
			or_dash(status.message));
		rc = 1;
		goto out;
	}

	printf("%-22s %-14s %-12s %4s  %s\n", "SERVICE_ID", "TYPE", "STATE",
	       "GEN", "ADDRESS");
	for (i = 0; i < result.entry_count; i++) {
		const defw2_dir_record_t *r = &result.entries[i].record;

		printf("%-22s %-14s %-12s %4llu  %s\n",
		       or_dash(r->service_id), or_dash(r->service_type),
		       defw2_dir_state_name(r->state),
		       (unsigned long long)r->generation,
		       or_dash(r->address));
		for (j = 0; j < r->binding_count; j++)
			printf("  binding %-12s %-26s v%u provider %u\n",
			       or_dash(r->bindings[j].binding_name),
			       or_dash(r->bindings[j].api_id),
			       r->bindings[j].api_version,
			       r->bindings[j].provider_id);
		for (j = 0; j < r->property_count; j++)
			printf("  property %-20s %s\n",
			       or_dash(r->properties[j].name),
			       or_dash(r->properties[j].value));
	}
	printf("\n%zu record%s\n", result.entry_count,
	       result.entry_count == 1 ? "" : "s");

out:
	defw2_dir_result_free(&result);
	defw2_status_free(&status);
	defw2_dir_close(dir);
	defw2_finalize(rt);
	return rc;
}

/* --- arguments ------------------------------------------------------- */

/*
 * Returns whether the value parsed, so an unreadable number is a usage error
 * rather than silently becoming zero and then the default.
 */
static bool parse_u64(const char *value, uint64_t *out)
{
	char *end = NULL;
	unsigned long long parsed;

	if (value == NULL || value[0] == '\0')
		return false;
	parsed = strtoull(value, &end, 10);
	if (end == value || *end != '\0')
		return false;
	*out = (uint64_t)parsed;
	return true;
}

int main(int argc, char **argv)
{
	struct options opts;
	const char *service_id = NULL;
	int i;

	memset(&opts, 0, sizeof(opts));

	if (argc > 1 && strcmp(argv[1], "dump") == 0) {
		if (argc < 3) {
			usage();
			return 2;
		}
		for (i = 3; i + 1 < argc; i += 2) {
			if (strcmp(argv[i], "--service-id") == 0) {
				service_id = argv[i + 1];
			} else {
				usage();
				return 2;
			}
		}
		return dump(argv[2], service_id);
	}
	if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
			 strcmp(argv[1], "--help") == 0)) {
		usage();
		return 0;
	}

	for (i = 1; i + 1 < argc; i += 2) {
		uint64_t value = 0;

		if (strcmp(argv[i], "--address-file") == 0) {
			opts.address_file = argv[i + 1];
		} else if (strcmp(argv[i], "--snapshot") == 0) {
			opts.snapshot = argv[i + 1];
		} else if (strcmp(argv[i], "--timeout") == 0) {
			if (!parse_u64(argv[i + 1], &value)) {
				usage();
				return 2;
			}
			opts.timeout_ms = (uint32_t)value;
		} else if (strcmp(argv[i], "--scan") == 0) {
			if (!parse_u64(argv[i + 1], &value)) {
				usage();
				return 2;
			}
			opts.scan_ms = (uint32_t)value;
		} else if (strcmp(argv[i], "--retention") == 0) {
			if (!parse_u64(argv[i + 1], &value)) {
				usage();
				return 2;
			}
			opts.retention_ms = value;
		} else {
			usage();
			return 2;
		}
	}
	if (i != argc) {
		usage();
		return 2;
	}
	return serve(&opts);
}
