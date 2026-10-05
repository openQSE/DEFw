/*
 * The SPANK plugin's reserve and release, against libdefw2's public headers
 * alone. QFw's plugin has a gateway make these calls today, over QSGP, in
 * Python. Each is a process here, as each is a callback in the plugin, and
 * the reservation's id passes from one to the other. DEFW2_DIRSVC names the
 * directory, and reserve prints the reservation's id.
 *
 *	defw2-spank-flow reserve SERVICE_ID JOB_ID USER QUBITS SHOTS
 *	defw2-spank-flow release SERVICE_ID RESERVATION_ID
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <defw2/defw2_dir.h>
#include <defw2/defw2_qpm.h>

static const defw2_call_opts_t opts = { .timeout_ms = 10000 };

/* The QPM's binding for one of its APIs, as the directory names it. */
static defw2_binding_t *qpm_api(defw2_dir_cache_t *cache, const char *qpm,
				const char *api)
{
	defw2_dir_query_t query = { .service_id = qpm, .binding_name = api };
	defw2_binding_t *binding = NULL;
	defw2_status_t status = { 0 };

	defw2_dir_cache_binding(cache, &query, &opts, &binding, &status);
	defw2_status_free(&status);
	return binding;
}

/* Reserve once the QPM says it is ready, as the gateway does. */
static const char *reserve(defw2_binding_t *control,
			   defw2_binding_t *admission, char **argv,
			   defw2_qpm_decision_t *decision,
			   defw2_status_t *status)
{
	uint64_t walltime_ns = 3600 * 1000000000ull;
	defw2_qpm_reserve_req_t req = {
		.job_id = argv[3], .allocation_id = argv[3], .user = argv[4],
		.workload_kind = "quantum", .num_qubits = atoi(argv[5]),
		.walltime_ns = walltime_ns, .ttl_ns = walltime_ns,
		.has_task_class = true, .task_class = { .count = 1,
			.qubit_count = atoi(argv[5]),
			.shots = atoi(argv[6]) } };
	defw2_qpm_service_status_t ready = { 0 };
	defw2_qpm_ctx_t ctx = { 0 };
	char owner[128];
	bool up;

	snprintf(owner, sizeof(owner), "{\"owner\":{\"user\":\"%s\",\"uid\":%u,"
		 "\"gid\":%u},\"scheduler\":\"slurm\"}", argv[4], getuid(),
		 getgid());
	req.extra = owner;
	up = defw2_qpm_is_ready(control, &ctx, &opts, &ready, status) ==
	     DEFW2_OK && ready.ready;
	defw2_qpm_service_status_free(&ready);
	if (!up)
		return "the QPM is not ready";
	if (defw2_qpm_reserve(admission, &req, &opts, decision, status) !=
	    DEFW2_OK || decision->decision == NULL ||
	    strcmp(decision->decision, DEFW2_QPM_DECISION_ACCEPTED) != 0)
		return decision->message ? decision->message : "refused";
	printf("%" PRIu64 "\n", decision->reservation_id);
	return NULL;
}

int main(int argc, char **argv)
{
	int reserving = argc == 7 && strcmp(argv[1], "reserve") == 0;
	defw2_binding_t *control = NULL, *admission = NULL;
	defw2_qpm_close_req_t req = { 0 };
	defw2_qpm_decision_t decision = { 0 };
	defw2_status_t status = { 0 };
	defw2_dir_cache_t *cache = NULL;
	const char *why = NULL;
	defw2_dir_t *dir = NULL;
	defw2_rt_t *rt = NULL;
	defw2_config_t cfg;

	if (!reserving && (argc != 4 || strcmp(argv[1], "release") != 0)) {
		fprintf(stderr, "usage: see the top of defw2_spank_flow.c\n");
		return 2;
	}
	defw2_config_from_env(&cfg);
	if (defw2_init(&cfg, &rt) == DEFW2_OK &&
	    defw2_dir_open(rt, defw2_dirsvc(rt), &dir) == DEFW2_OK &&
	    defw2_dir_cache_create(dir, &cache) == DEFW2_OK) {
		control = qpm_api(cache, argv[2], "control");
		admission = qpm_api(cache, argv[2], "admission");
	}
	req.ctx.reservation_id = reserving ? 0 : strtoull(argv[3], NULL, 10);
	if (control == NULL || admission == NULL)
		why = "the directory in DEFW2_DIRSVC has no such QPM";
	else if (reserving)
		why = reserve(control, admission, argv, &decision, &status);
	else if (defw2_qpm_release(admission, &req, &opts, &decision,
				   &status) != DEFW2_OK ||
		 status.category != DEFW2_CAT_OK)
		why = status.message ? status.message : "release failed";
	if (why != NULL)
		fprintf(stderr, "%s %s: %s\n", argv[1], argv[2], why);
	defw2_qpm_decision_free(&decision);
	defw2_status_free(&status);
	defw2_dir_cache_destroy(cache);
	defw2_dir_close(dir);
	defw2_finalize(rt);
	return why != NULL;
}
