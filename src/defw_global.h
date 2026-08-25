#ifndef DEFW_CONNECT_H
#define DEFW_CONNECT_H

#include "defw_common.h"

/* accessor functions to set/get global information */

void disable_dirsvc(void);
int dirsvc_disabled(void);
void set_log_level(defw_log_level_t level);
void set_defw_path(char *path);
void set_py_path(char *path);
void set_parent_name(char *name);
void set_parent_hostname(char *name);
void set_hostname(char *name);
void set_suite_name(char *name);
void set_script_name(char *name);
void set_matching_pattern(char *pattern);
void set_listen_port(int port);
void set_node_name(char *name);
void set_agent_telnet_port(int port);
defw_rc_t set_parent_address(char *addr);
void set_parent_port(int port);
defw_rc_t set_listen_address(char *addr);
void set_listen_port(int port);
void set_defw_mode(char *mode);
void set_defw_type(char *type);
void set_defw_results_file_path(char *path);
void set_defw_cfg_file_path(char *path);
void set_defw_tmp_dir(char *path);
void set_defw_initialized(int initialized);
void set_defw_safe_shutdown(int safe);

char *get_defw_path(void);
char *get_py_path(void);
char *get_parent_name(void);
char *get_parent_hostname(void);
char *get_hostname(void);
char *get_suite_name(void);
char *get_script_name(void);
char *get_matching_pattern(void);
int get_listen_port(void);
char *get_node_name(void);
int get_agent_telnet_port(void);
char *get_parent_address(void);
int get_parent_port(void);
char *get_listen_address(void);
int get_listen_port(void);
defw_run_mode_t get_defw_mode(void);
defw_type_t get_defw_type(void);
char *get_defw_results_file_path(void);
char *get_defw_cfg_file_path(void);
char *get_defw_tmp_dir(void);
int get_defw_initialized(void);
void get_defw_uuid(char **uuid);

static inline const char *defw_rc2str(defw_rc_t rc)
{
	static const char * const str[] = {
		[EN_DEFW_RC_OK] = "RC_OK",
		[EN_DEFW_RC_FAIL*-1] = "RC_FAIL",
		[EN_DEFW_RC_SYS_ERR*-1] = "RC_SYSTEM_ERROR",
		[EN_DEFW_RC_BAD_VERSION*-1] = "RC_BAD_VERSION",
		[EN_DEFW_RC_SOCKET_FAIL*-1] = "RC_SOCKET_FAIL",
		[EN_DEFW_RC_BIND_FAILED*-1] = "RC_BIND_FAIL",
		[EN_DEFW_RC_LISTEN_FAILED*-1] = "RC_LISTEN_FAIL",
		[EN_DEFW_RC_CLIENT_CLOSED*-1] = "RC_CLIENT_CLOSED",
		[EN_DEFW_RC_ERR_THREAD_STARTUP*-1] = "RC_ERR_THREAD_START",
		[EN_DEFW_RC_AGENT_NOT_FOUND*-1] = "RC_AGENT_NOT_FOUND",
		[EN_DEFW_RC_PY_IMPORT_FAIL*-1] = "RC_PY_IMPORT_FAIL",
		[EN_DEFW_RC_PY_SCRIPT_FAIL*-1] = "RC_PY_SCRIPT_FAIL",
		[EN_DEFW_RC_RPC_FAIL*-1] = "RC_RPC_FAIL",
		[EN_DEFW_RC_OOM*-1] = "RC_OOM",
		[EN_DEFW_RC_BAD_PARAM*-1] = "RC_BAD_PARAM",
		[EN_DEFW_RC_BAD_ADDR*-1] = "RC_BAD_ADDR",
		[EN_DEFW_RC_MISSING_PARAM*-1] = "RC_MISSING_PARAM",
		[EN_DEFW_RC_TIMEOUT*-1] = "RC_TIMEOUT",
		[EN_DEFW_RC_LOG_CREATION_FAILURE*-1] = "RC_LOG_CREATION_FAILURE",
		[EN_DEFW_RC_PROTO_ERROR*-1] = "RC_PROTO_ERROR",
		[EN_DEFW_RC_IN_PROGRESS*-1] = "RC_IN_PROGRESS",
		[EN_DEFW_RC_BAD_UUID*-1] = "RC_BAD_UUID",
		[EN_DEFW_RC_NO_DATA_ON_SOCKET*-1] = "RC_NO_DATA_ON_SOCKET",
		[EN_DEFW_RC_KEEP_DATA*-1] = "RC_KEEP_DATA",
		[EN_DEFW_RC_UNKNOWN_MESSAGE*-1] = "RC_UNKNOWN_MESSAGE",
	};

	if (rc <= EN_DEFW_RC_MAX)
		return "BAD RC";

	rc *= -1;

	return str[rc];
}

#endif /* DEFW_CONNECT_H */
