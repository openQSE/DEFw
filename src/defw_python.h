#ifndef DEFW_PYTHON_H
#define DEFW_PYTHON_H

#include <pthread.h>
#include "defw.h"

typedef struct python_thread_data_s {
	char **argv;
} python_thread_data_t;

/*
 * python_init
 *    initialize the python interpreter
 *    It can then be run interactively or not
 */
defw_rc_t python_init(char *pname);

/*
 * python_finalize
 *   Finalize the python interpreter
 */
defw_rc_t python_finalize(void);

/*
 * python_print_version
 *   Print the version of python DEFw was compiled with
 */
void python_print_version(void);

/*
 * python_check_version
 *   Check the version of python DEFw was compiled against the runtime
 *   version
 */
int python_check_version(void);

/*
 * python_run_interpreter
 *	this runs the interpreter passing it whatever command line
 *	arguments were passed to the program. The intent is to make the
 *	DEFw look exactly like a python interpreter
 */
defw_rc_t python_run_interpreter(int argc, char *argv[]);

/*
 * python_run_cmd_line
 *   this is intended to run python as you would from the command line
 */
defw_rc_t python_run_cmd_line(int argc, char *argv[], char *module, char *cmd);

/*
 * python_exec
 *    Run a python code. This is not meant for complex use
 */
defw_rc_t python_exec(char *code);

/*
 * python_run_interactive_shell
 *    Run the interactive shell
 */
defw_rc_t python_run_interactive_shell(void);

/*
 * python_run_telnet_server
 *	Run the telnet server as our main loop
 */
defw_rc_t python_run_telnet_server(void);

/*
 * python_collect_agent_core
 *   Collect core information from the specified agent
 */
defw_rc_t python_collect_agent_core(char *ip);

/*
 * python_handle_connect_complete
 *	A connection requested by python has completed
 */
defw_rc_t python_handle_connect_complete(defw_rc_t status, char *uuid);

/*
 * python_handle_peer_event
 *	Dispatch a C-owned peer lifecycle event into the python worker queue
 */
defw_rc_t python_handle_peer_event(const defw_peer_event_t *event);

/*
 * python_update_interactive_shell
 *	update the interactive shell environment
 */
void python_update_interactive_shell(void);

#endif /* DEFW_PYTHON_H */
