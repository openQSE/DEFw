#ifndef DEFW_H
#define DEFW_H

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <pthread.h>
#include "defw_common.h"
#include "defw_agent.h"
#include "defw_message.h"
#include "libdefw_connect.h"

#define DEFW_UUID_STR_LEN		(UUID_STR_LEN+12)

typedef struct hb_info_s {
	struct sockaddr_in parent_address;
	int agent_telnet_port;
	char node_name[MAX_STR_LEN];
} hb_info_t;

typedef struct defw_listener_info_s {
	defw_type_t type;
	struct sockaddr_in listen_address;
	hb_info_t hb_info;
} defw_listener_info_t;

typedef struct defw_config_params_s {
	bool initialized;
	bool safe_shutdown;
	bool disable_dirsvc_connect;
	bool in_daemon_mode;
	defw_listener_info_t l_info;
	uuid_t uuid;
	defw_run_mode_t shell; /* run in [non]-interactive or daemon mode */
	char cfg_path[MAX_STR_LEN]; /* path to config file */
	char defw_path[MAX_STR_LEN]; /* path to defw */
	char py_path[MAX_STR_LEN]; /* other python specific paths */
	char parent_name[MAX_STR_LEN]; /* name of master. Important if I'm an agent */
	char parent_hostname[MAX_STR_LEN]; /* hostname of master. Important if I'm an agent */
	char hostname[MAX_STR_LEN]; /* local hostname. */
	char suite[MAX_STR_LEN]; /* name of suite to run. Run all if not present */
	char suite_list[MAX_STR_LEN]; /* list of suites to run. Takes precedence
			     over single suite parameter */
	char script[MAX_STR_LEN]; /* name of script to run. Suite must be specified */
	char pattern[MAX_STR_LEN]; /* file match pattern */
	char results_file[MAX_STR_LEN]; /* path to results file */
	char tmp_dir[MAX_STR_LEN]; /* directory to put temporary files */
	char *agents[MAX_NUM_AGENTS]; /* list of agents to wait for before
			       * starting the test
			       */
	int loglevel;
	pthread_spinlock_t log_lock;
	FILE *out;
	char *outlog;
} defw_config_params_t;

extern defw_config_params_t g_defw_cfg;

#endif /* DEFW_H */
