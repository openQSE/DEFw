#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <libgen.h>
#include <sys/time.h>
#include <sys/socket.h>
#include "defw_listener.h"
#include "defw_message.h"
#include "defw_python.h"
#include "defw.h"
#include "defw_sl.h"
#include "libdefw_agent.h"
#include "defw_print.h"

extern defw_config_params_t g_defw_cfg;

static void
defw_help_usage(const struct option *long_options, const char *const description[])
{
	int i = 0;

	fprintf(stderr,
		BOLDCYAN "defwp [option] ... [-c cmd | -m mod | file] [arg] ...\n"
		RESET "Options and arguments (and corresponding environment variables):\n");

	while ((long_options[i].name != NULL) && (description[i] != NULL)) {
		fprintf(stderr, "\t-%c or --%s %s\n",
			(char) long_options[i].val,
			long_options[i].name,
			description[i]);
		i++;
	}

	fprintf(stderr, "\n");
}

defw_run_mode_t handle_cmd_line_opt(int argc, char *argv[], char **module, char **pycmd,
				    bool *spawned)
{
	int cOpt;
	defw_run_mode_t shell = EN_DEFW_RUN_CMD_LINE;
	/* If followed by a ':', the option requires an argument*/
	const char *const short_options = "c:m:vdxh";
	const struct option long_options[] = {
		{.name = "py-version", .has_arg = no_argument, .val = 'v'},
		{.name = "cmd", .has_arg = required_argument, .val = 'c'},
		{.name = "module", .has_arg = required_argument, .val = 'm'},
		{.name = "deamon", .has_arg = no_argument, .val = 'd'},
		{.name = "execvp", .has_arg = no_argument, .val = 'x'},
		{.name = "help", .has_arg = no_argument, .val = 'h'},
		{NULL, 0, NULL, 0}
	};

	static const char * const description[] = {
		/*'c'*/":\n\t\tprogram passed in as string (terminates option list)",
		/*'m'*/":\n\t\trun library module as a script (terminates option list)",
		/*'h'*/":\n\t\tPrint this help",
		NULL
	};

	/* sanity check */
	if (argc <= 1)
		return EN_DEFW_RUN_INTERACTIVE;

	/* don't print error if you find an option parameter which is not
	 * in the above list. python modules can take arguments specified
	 * in the command line and there is no way to exhaustively handle
	 * them.
	 */
	opterr = 0;
	/*now process command line arguments*/
	if (argc > 1) {
		while ((cOpt = getopt_long(argc, argv,
					   short_options,
					   long_options,
					   NULL)) != -1) {
			switch (cOpt) {
			case 'm':
				*module = optarg;
				break;
			case 'c':
				*pycmd = optarg;
				break;
			case 'h':
				defw_help_usage(long_options, description);
				exit(DEFW_EXIT_NORMAL);
			case 'd':
				shell = EN_DEFW_RUN_DAEMON;
				break;
			case 'v':
				python_print_version();
				exit(DEFW_EXIT_NORMAL);
			case 'x':
				*spawned = true;
				break;
			case '?':
				break;
			default:
				PERROR("Bad parameter");
				exit(DEFW_EXIT_ERR_BAD_PARAM);
				break;
			}
		}
	}

	return shell;
}
#include <signal.h>

void handle_interrupt(int signum)
{
	if (g_defw_cfg.shell == EN_DEFW_RUN_DAEMON) {
		fprintf(stderr, "Received signal: %d. Shutting down\n",
			signum);
		defw_shutdown();
	}
}

defw_rc_t defw_start(int argc, char *argv[])
{
	pthread_t l_thread_id;
	defw_rc_t rc;
	char *module = NULL, *cmd = NULL;
	defw_run_mode_t shell;
	char **local_argv = NULL;
	char *bin_name;
	bool spawned = false;
	bool pure_python = false;
	bool listener_started = false;

	if (!python_check_version())
		exit(DEFW_PYTHON_VERSION_MISMATCH);

	memset(&g_defw_cfg, 0, sizeof(g_defw_cfg));

	defw_init_logging();

	/* if argv[0] starts with python, then run the DEFw as a pure
	 * python interpreter. Pass all the arguments directly to the
	 * python interpreter. This allows applications to run within the
	 * context of DEFw seemlessly
	 */
	bin_name = basename(argv[0]);
	if (!strncmp(bin_name, "python", 6)) {
		pure_python = true;
		goto run_pure_python;
	}

	local_argv = calloc(1, (argc+1)*sizeof(*local_argv));
	memcpy(local_argv, argv, argc*sizeof(*local_argv));

	shell = handle_cmd_line_opt(argc, argv, &module, &cmd, &spawned);

	if (shell == EN_DEFW_RUN_DAEMON && !spawned) {
		pid_t process_id = 0;
		pid_t sid = 0;

		/* create the child process */
		process_id = fork();
		if (process_id < 0) {
			PERROR("Failed to run defw as deamon");
			return EN_DEFW_RC_ERR_THREAD_STARTUP;
		}

		if (process_id > 0) {
			/*
			 * We're in the parent process so let's kill it
			 * off
			 */
			PDEBUG("Shutting down parent process");
			exit(DEFW_EXIT_NORMAL);
		}

		umask(0);
		sid = setsid();
		if (sid < 0) {
			PERROR("forking child failed");
			return EN_DEFW_RC_ERR_THREAD_STARTUP;
		}

		rc = chdir("/");
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		if (rc) {
			PERROR("chdir failed");
			return EN_DEFW_RC_ERR_THREAD_STARTUP;
		}

		local_argv[argc] = "-x";
		local_argv[argc+1] = NULL;
		execvp(local_argv[0], local_argv);

		PERROR("Failed to execvp");
		exit(DEFW_EXIT_ERR_DEAMEON_STARTUP);
	}

run_pure_python:

	/* generate global uuid for this instance */
	uuid_generate(g_defw_cfg.uuid);

	defw_agent_init();

	rc = python_init(bin_name);
	if (rc) {
		PERROR("Failed to initialize Python Module");
		return rc;
	}

	/* create the log file */
	g_defw_cfg.out = stdout;
	if (strlen(g_defw_cfg.tmp_dir) == 0)
		getcwd(g_defw_cfg.tmp_dir, sizeof(g_defw_cfg.tmp_dir));
	g_defw_cfg.outlog = calloc(strlen(g_defw_cfg.tmp_dir) + strlen(OUT_LOG_NAME) + 2, 1);
	if (!g_defw_cfg.outlog) {
		PERROR("out of memory");
		return EN_DEFW_RC_LOG_CREATION_FAILURE;
	}
	sprintf(g_defw_cfg.outlog, "%s/%s", g_defw_cfg.tmp_dir, OUT_LOG_NAME);
	g_defw_cfg.out = fopen(g_defw_cfg.outlog, "w");
	if (!g_defw_cfg.out) {
		PERROR("Failed to open log files: %s\n",
			g_defw_cfg.outlog);
		return EN_DEFW_RC_LOG_CREATION_FAILURE;
	}

	if (g_defw_cfg.l_info.listen_address.sin_port != 0) {
		if (defw_spawn_listener(&l_thread_id)) {
			PERROR("Failed to initialize listener thread");
			return EN_DEFW_RC_ERR_THREAD_STARTUP;
		}
		listener_started = true;
	}

	if (pure_python) {
		/* we don't need to finalize python since Py_Main() will do that */
		rc = python_run_interpreter(argc, argv);
		if (rc)
			PERROR("Interprter failed to execute: %d\n", rc);
		if (listener_started)
			defw_listener_shutdown();
		goto out;
	}

	if (g_defw_cfg.shell == EN_DEFW_RUN_INTERACTIVE)
		shell = EN_DEFW_RUN_INTERACTIVE;

	switch (shell) {
	case EN_DEFW_RUN_INTERACTIVE:
		rc = python_run_interactive_shell();
		if (rc) {
			PERROR("Failed to run python interactively");
			defw_shutdown();
			return rc;
		}
		defw_shutdown();
		break;
	case EN_DEFW_RUN_CMD_LINE:
		rc = python_run_cmd_line(argc-1, local_argv+1, module, cmd);
		/* once Python is done execution of whatever was passed to
		 * it, it's assumed there is no longer a reason to keep
		 * the defw around
		 */
		defw_shutdown();
		free(local_argv);
		return rc;
	case EN_DEFW_RUN_DAEMON:
		rc = python_run_telnet_server();
		if (rc) {
			PERROR("Failed to run python interactively");
			defw_shutdown();
			return rc;
		}
		defw_shutdown();
		break;
	default:
		PERROR("Unexpected shell type: %d\n", g_defw_cfg.shell);
		defw_shutdown();
		free(local_argv);
		return EN_DEFW_RC_BAD_PARAM;
	}

	if (!g_defw_cfg.initialized) {
		PERROR("Python didn't initialize the system");
		defw_shutdown();
		return EN_DEFW_RC_ERR_THREAD_STARTUP;
	}

out:
	if (local_argv)
		free(local_argv);
	if (g_defw_cfg.safe_shutdown && listener_started)
		pthread_join(l_thread_id, NULL);

	PDEBUG("%d: Exiting Framework\n", getpid());

	pthread_spin_lock(&g_defw_cfg.log_lock);
	fclose(g_defw_cfg.out);
	g_defw_cfg.out = NULL;
	free(g_defw_cfg.outlog);
	g_defw_cfg.outlog = NULL;
	pthread_spin_unlock(&g_defw_cfg.log_lock);

	return EN_DEFW_RC_OK;
}

defw_rc_t defw_exec_py(char *py_code)
{
	return python_exec(py_code);
}

void defw_shutdown(void)
{
	defw_listener_shutdown();
	python_finalize();
}
