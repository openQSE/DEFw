#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "defw.h"
#include "defw_python.h"

defw_config_params_t g_defw_cfg;

void disable_dirsvc(void)
{
	g_defw_cfg.disable_dirsvc_connect = true;
}

int dirsvc_disabled(void)
{
	return g_defw_cfg.disable_dirsvc_connect;
}

void set_log_level(defw_log_level_t level)
{
	g_defw_cfg.loglevel = level;
}

char *get_defw_path(void)
{
	return g_defw_cfg.defw_path;
}

void set_defw_path(char *path)
{
	strncpy(g_defw_cfg.defw_path, path, MAX_STR_LEN);
}

void set_defw_safe_shutdown(int safe)
{
	g_defw_cfg.safe_shutdown = safe;
}

char *get_py_path(void)
{
	return g_defw_cfg.py_path;
}

void set_py_path(char *path)
{
	strncpy(g_defw_cfg.py_path, path, MAX_STR_LEN);
}

char *get_parent_name(void)
{
	return g_defw_cfg.parent_name;
}

void set_parent_name(char *name)
{
	strncpy(g_defw_cfg.parent_name, name, MAX_STR_LEN);
}

char *get_parent_hostname(void)
{
	return g_defw_cfg.parent_hostname;
}

void set_parent_hostname(char *name)
{
	strncpy(g_defw_cfg.parent_hostname, name, MAX_STR_LEN);
}

char *get_hostname(void)
{
	return g_defw_cfg.hostname;
}

void set_hostname(char *name)
{
	strncpy(g_defw_cfg.hostname, name, MAX_STR_LEN);
}

char *get_suite_name(void)
{
	return g_defw_cfg.suite;
}

void set_suite_name(char *name)
{
	strncpy(g_defw_cfg.suite, name, MAX_STR_LEN);
}

char *get_script_name(void)
{
	return g_defw_cfg.script;
}

void set_script_name(char *name)
{
	strncpy(g_defw_cfg.script, name, MAX_STR_LEN);
}

char *get_matching_pattern(void)
{
	return g_defw_cfg.pattern;
}

void set_matching_pattern(char *pattern)
{
	strncpy(g_defw_cfg.pattern, pattern, MAX_STR_LEN);
}

char *get_listen_address(void)
{
	return inet_ntoa(g_defw_cfg.l_info.listen_address.sin_addr);
}

defw_rc_t set_listen_address(char *addr)
{
	if (strlen(addr) == 0) {
		g_defw_cfg.l_info.listen_address.sin_addr.s_addr = INADDR_ANY;
	} else {
		if (!inet_aton(addr, &g_defw_cfg.l_info.listen_address.sin_addr))
				return EN_DEFW_RC_BAD_ADDR;
	}
	return EN_DEFW_RC_OK;
}

int get_listen_port(void)
{
	return g_defw_cfg.l_info.listen_address.sin_port;
}

void set_listen_port(int port)
{
	g_defw_cfg.l_info.listen_address.sin_port = port;
}

char *get_node_name(void)
{
	return g_defw_cfg.l_info.hb_info.node_name;
}

void set_node_name(char *name)
{
	strncpy(g_defw_cfg.l_info.hb_info.node_name, name, MAX_STR_LEN);
}

int get_agent_telnet_port(void)
{
	return g_defw_cfg.l_info.hb_info.agent_telnet_port;
}

void set_agent_telnet_port(int port)
{
	g_defw_cfg.l_info.hb_info.agent_telnet_port = port;
}

char *get_parent_address(void)
{
	return inet_ntoa(g_defw_cfg.l_info.hb_info.parent_address.sin_addr);
}

defw_rc_t set_parent_address(const char *addr)
{
	if (!inet_aton(addr, &g_defw_cfg.l_info.hb_info.parent_address.sin_addr))
		return EN_DEFW_RC_BAD_ADDR;
	return EN_DEFW_RC_OK;
}

int get_parent_port(void)
{
	return g_defw_cfg.l_info.hb_info.parent_address.sin_port;
}

void set_parent_port(int port)
{
	g_defw_cfg.l_info.hb_info.parent_address.sin_port = port;
}

defw_run_mode_t get_defw_mode(void)
{
	return g_defw_cfg.shell;
}

void set_defw_mode(char *mode)
{
	if (!strcasecmp(mode, "daemon"))
		g_defw_cfg.shell = EN_DEFW_RUN_DAEMON;
	else if (!strcasecmp(mode, "interactive"))
		g_defw_cfg.shell = EN_DEFW_RUN_INTERACTIVE;
	else if (!strcasecmp(mode, "cmdline"))
		g_defw_cfg.shell = EN_DEFW_RUN_CMD_LINE;
}

defw_type_t get_defw_type(void)
{
	return g_defw_cfg.l_info.type;
}

void set_defw_type(char *type)
{
	if (!strcasecmp(type, "agent"))
		g_defw_cfg.l_info.type = EN_DEFW_AGENT;
	else if (!strcasecmp(type, "service"))
		g_defw_cfg.l_info.type = EN_DEFW_SERVICE;
	else if (!strcasecmp(type, "dirsvc"))
		g_defw_cfg.l_info.type = EN_DEFW_DIRSVC;
	else
		g_defw_cfg.l_info.type = EN_DEFW_DIRSVC;
}

char *get_defw_results_file_path(void)
{
	return g_defw_cfg.results_file;
}

void set_defw_results_file_path(char *path)
{
	strncpy(g_defw_cfg.results_file, path, MAX_STR_LEN);
}

char *get_defw_cfg_file_path(void)
{
	return g_defw_cfg.cfg_path;
}

void set_defw_cfg_file_path(char *path)
{
	strncpy(g_defw_cfg.cfg_path, path, MAX_STR_LEN);
}

char *get_defw_tmp_dir(void)
{
	return g_defw_cfg.tmp_dir;
}

void set_defw_tmp_dir(char *path)
{
	strncpy(g_defw_cfg.tmp_dir, path, MAX_STR_LEN);
}

void set_defw_initialized(int initialized)
{
	if (initialized)
		g_defw_cfg.initialized = true;
	else
		g_defw_cfg.initialized = false;
}

int get_defw_initialized(void)
{
	return g_defw_cfg.initialized;
}


void get_defw_uuid(char **uuid)
{
	*uuid = calloc(1, UUID_STR_LEN);
	uuid_unparse_lower(g_defw_cfg.uuid, *uuid);
}

void update_py_interactive_shell(void)
{
}
