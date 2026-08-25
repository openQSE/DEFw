#include <Python.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include "defw.h"
#include "defw_python.h"
#include "defw_listener.h"
#include "defw_global.h"
#include "defw_print.h"

extern defw_config_params_t g_defw_cfg;
static atomic_long g_py_gil_refcount;

/*
 * python_handle_[request | response]
 *   Received an RPC now execute the operation in the python interpreter
 */
defw_rc_t python_handle_request(char *rpc, char *uuid);
defw_rc_t python_handle_response(char *rpc, char *uuid);
defw_rc_t python_handle_event(char *rpc, char *uuid);
defw_rc_t python_handle_peer_event(const defw_peer_event_t *event);

#define RUN_PYTHON_CMD(cmd) {						\
	int py_rc;							\
	py_rc = PyRun_SimpleString(cmd);				\
	if (py_rc) {							\
		PERROR("Python execution failed: %d", py_rc);		\
		return EN_DEFW_RC_PY_SCRIPT_FAIL;			\
	}								\
}

#define RUN_PYTHON_CMD_OBJ(cmd, obj, globals, locals) {			\
	PyObject *type, *value, *traceback, *str_value;			\
	obj = PyRun_String(cmd, 0, globals, locals);			\
	if (!obj) {							\
		PERROR("Python cmd failed: %s", cmd);			\
		PyErr_Fetch(&type, &value, &traceback);			\
		PERROR("Exception type: %s", Py_TYPE(type)->tp_name);	\
		str_value = PyObject_Str(value);			\
		if (str_value)						\
			printf("Exception value: %s",			\
			       PyUnicode_AsUTF8(str_value));		\
		return EN_DEFW_RC_PY_SCRIPT_FAIL;			\
	}								\
}

defw_rc_t python_exec(char *code)
{
	RUN_PYTHON_CMD(code);
	return EN_DEFW_RC_OK;
}

defw_rc_t python_run_interpreter(int argc, char *argv[])
{
	int i, rc;
	wchar_t **wargv;

	RUN_PYTHON_CMD("sys.ps1 = 'defw>>> '\n");
	RUN_PYTHON_CMD("sys.ps2 = 'defw... '\n");

	wargv = calloc(argc + 1, sizeof(*wargv));
	if (!wargv)
		return EN_DEFW_RC_OOM;

	for (i = 0; i < argc; i++) {
		wargv[i] = Py_DecodeLocale(argv[i], NULL);
		if (!wargv[i]) {
			rc = EN_DEFW_RC_BAD_PARAM;
			goto out;
		}
	}
	PySys_SetArgvEx(argc, wargv, 0);

	rc = Py_Main(argc, wargv);

out:
	for (int i = 0; i < argc; i++)
		PyMem_RawFree(wargv[i]);
	free(wargv);

	return (rc) ? EN_DEFW_RC_PY_SCRIPT_FAIL : EN_DEFW_RC_OK;
}

static defw_rc_t python_setup(void)
{
	char buf[MAX_STR_LEN * 4];
	char cwd[MAX_STR_LEN];
	char *infra = getenv(DEFW_PATH);

	if (!infra) {
		if (getcwd(cwd, sizeof(cwd)) != NULL) {
			PDEBUG("Current working directory: %s", cwd);
		} else {
			PERROR("getcwd");
			exit(DEFW_EXIT_ERR_STARTUP);
		}
		infra = cwd;
	}

	RUN_PYTHON_CMD("import code\n");
	RUN_PYTHON_CMD("import os\n");
	RUN_PYTHON_CMD("import sys\n");
	RUN_PYTHON_CMD("import readline\n");

	/* all other paths are figured out within python */
	snprintf(buf, sizeof(buf),
		"sys.path.append('.')");
	RUN_PYTHON_CMD(buf);
	snprintf(buf, sizeof(buf),
		"sys.path.append(os.path.join('%s', 'python', 'infra'))", infra);
	RUN_PYTHON_CMD(buf);
	snprintf(buf, sizeof(buf),
		"sys.path.append(os.path.join('%s', 'src'))", infra);
	RUN_PYTHON_CMD(buf);

	RUN_PYTHON_CMD("from defw_venv import add_venv_sitepackages\n");
	RUN_PYTHON_CMD("add_venv_sitepackages()\n");

	RUN_PYTHON_CMD("import defw\n");
	RUN_PYTHON_CMD("from defw import me,experiments,"
		"services,service_apis,dirsvc,"
		"dump_all_agents,dumpGlobalTestResults,R,I,X\n"
		"from defw_workers import worker_thread\n");

	return EN_DEFW_RC_OK;
}

defw_rc_t python_run_interactive_shell(void)
{
	char *intro;

	PDEBUG("Running in Interactive mode");
	/*
	 * start an independent shell
	 * Since we imported all the necessary modules to start in
	 * the main interpreter, copying the globals should copy
	 * them in the interactive shell.
	 */
	RUN_PYTHON_CMD("vars = globals().copy()\n");
	RUN_PYTHON_CMD("vars.update(locals())\n");
	RUN_PYTHON_CMD("shell = code.InteractiveConsole(vars)\n");
	RUN_PYTHON_CMD("shell.push('sys.ps1 = \"defw>>> \"')\n");
	RUN_PYTHON_CMD("shell.push('sys.ps2 = \"defw... \"')\n");

	/* import base defw module */
	g_defw_cfg.initialized = true;

	intro = "shell.interact('Welcome to the Distributed Execution Framework (DEFw)\\n"
		"Convenience Functions: \\n"
		"\tR() = dumpGlobalTestResults()\\n"
		"\tdump_all_agents() = peer table dump\\n"
		"\tI() = me.dump_intfs()\\n"
		"\tX() = me.exit()\\n"
		"Convenience Objects: \\n"
		"\\tme - Instance of this DEFw\\n"
		"\\texperiments - All experiment scripts\\n"
		"\\tservices - Available services to offer\\n"
		"\\tservice_apis - Available service interface APIs\\n')";
	RUN_PYTHON_CMD(intro);

	return EN_DEFW_RC_OK;
}

#define TEL_SRV_LOG "defw_telsrv.out"

defw_rc_t python_run_telnet_server(void)
{
	char buf[MAX_STR_LEN * 4];
	/* run the telnet server. This becomes our main process
	 * now
	 */
	PDEBUG("Running in Daemon mode");
	sprintf(buf, "fname = os.path.join('%s', '%s')\n",
		g_defw_cfg.tmp_dir[0] != '\0' ? g_defw_cfg.tmp_dir : "/tmp", TEL_SRV_LOG);
	RUN_PYTHON_CMD(buf);

	sprintf(buf, "logfile = open(fname, 'w')\n");
	RUN_PYTHON_CMD(buf);

	RUN_PYTHON_CMD("sys.stdout = sys.stderr = logfile\n");

	RUN_PYTHON_CMD("from defw_telnet_sr import DefwTelnetServer\n");

	sprintf(buf, "tns = DefwTelnetServer(%d)\n",
		g_defw_cfg.l_info.hb_info.agent_telnet_port);
	RUN_PYTHON_CMD(buf);

	RUN_PYTHON_CMD("tns.run()\n");

	RUN_PYTHON_CMD("logfile.close()");

	return EN_DEFW_RC_OK;
}

defw_rc_t python_run_cmd_line(int argc, char *argv[], char *module, char *cmd)
{
	char buf[MAX_STR_LEN * 4];
	PyObject *pSysArgv;
	PyObject *pArg;
	ssize_t len;
	FILE *f;

	if (!g_defw_cfg.initialized)
		return EN_DEFW_RC_PY_SCRIPT_FAIL;

	/* skip the option */
	if (module) {
		argc -= 1;
		argv += 1;
	/* skip the option and its arg */
	} else if (cmd) {
		argc -= 2;
		argv += 2;
	}

	pSysArgv = PySys_GetObject("argv");
	for (int i = 0; i < argc; ++i) {
		pArg = PyUnicode_DecodeUTF8(argv[i], strlen(argv[i]), "surrogateescape");
		PyList_Append(pSysArgv, pArg);
		Py_DECREF(pArg);
	}
	len = readlink("/proc/self/exe", buf, sizeof(buf)-1);
	if (len < sizeof(buf)-1)
		buf[len] = '\0';
	else
		buf[sizeof(buf)-1] = '\0';

	PySys_SetObject("executable", PyUnicode_DecodeFSDefault(buf));
	PySys_SetObject("_base_executable", PyUnicode_DecodeFSDefault(buf));

	RUN_PYTHON_CMD("sys.argv.pop(0)\n");

	if (module) {
		sprintf(buf,
			"import runpy\n"
			"try:\n"
			"    runpy.run_module('%s', run_name='__main__', alter_sys=False)\n"
			"except SystemExit as e:\n"
			"    pass\n"
			"except Exception as e:\n"
			"    print(e)\n"
			"print(f'Running module %s with args: {sys.argv}')\n", module, module);
		RUN_PYTHON_CMD(buf);
	} else if (cmd) {
		RUN_PYTHON_CMD(cmd);
	} else if (argc >= 1) {
		f = fopen(argv[0], "r");
		if (!f)
			return EN_DEFW_RC_PY_SCRIPT_FAIL;

		PyRun_SimpleFile(f, argv[0]);
		fclose(f);
	}

	return EN_DEFW_RC_OK;
}

static defw_rc_t process_msg_py_request(char *msg, defw_agent_blk_t *agent)
{
	defw_rc_t rc;
	char *uuid = calloc(1, UUID_STR_LEN);
	uuid_unparse_lower(agent->id.blk_uuid, uuid);

	agent->state |= DEFW_AGENT_WORK_IN_PROGRESS;
	rc = python_handle_request(msg, uuid);
	agent->state &= ~DEFW_AGENT_WORK_IN_PROGRESS;

	return rc;
}

/* An RPC reponse means the agent that there a thread waiting on the
 * response to arrive. Signal the agent that a response has arrived.
 *
 * There could be one outstanding response per agent.
 */
static defw_rc_t process_msg_py_response(char *msg, defw_agent_blk_t *agent)
{
	defw_rc_t rc;
	char *uuid = calloc(1, UUID_STR_LEN);
	uuid_unparse_lower(agent->id.blk_uuid, uuid);

	agent->state |= DEFW_AGENT_WORK_IN_PROGRESS;
	rc = python_handle_response(msg, uuid);
	agent->state &= ~DEFW_AGENT_WORK_IN_PROGRESS;

	return rc;
}

static defw_rc_t process_msg_py_event(char *msg, defw_agent_blk_t *agent)
{
	defw_rc_t rc;
	char *uuid = calloc(1, UUID_STR_LEN);
	uuid_unparse_lower(agent->id.blk_uuid, uuid);

	agent->state |= DEFW_AGENT_WORK_IN_PROGRESS;
	rc = python_handle_event(msg, uuid);
	agent->state &= ~DEFW_AGENT_WORK_IN_PROGRESS;

	return rc;
}

static void py_connect_status(defw_rc_t status, uuid_t uuid)
{
	defw_rc_t rc;
	char *uuid_str = calloc(1, UUID_STR_LEN);

	uuid_unparse(uuid, uuid_str);

	rc = python_handle_connect_complete(status, uuid_str);
	if (rc)
		PERROR("Python connect request failed: %s", defw_rc2str(rc));
}

/*
 * gcc py.c -o py -I/usr/local/include/python2.7
 * -L/usr/local/lib/python2.7/config -lm -ldl -lpthread -lutil -lpython2.7
 */
defw_rc_t python_init(char *pname)
{
	static wchar_t program[MAX_STR_LEN];
	size_t converted;

	/* register with listener */
	defw_register_msg_callback(EN_MSG_TYPE_PY_REQUEST, process_msg_py_request);
	defw_register_msg_callback(EN_MSG_TYPE_PY_RESPONSE, process_msg_py_response);
	defw_register_msg_callback(EN_MSG_TYPE_PY_EVENT, process_msg_py_event);
	defw_register_connect_complete(py_connect_status);
	defw_register_peer_event_callback(python_handle_peer_event);

	atomic_init(&g_py_gil_refcount, 0);

	converted = mbstowcs(program, pname,
		sizeof(program) / sizeof(program[0]) - 1);
	if (converted == (size_t)-1)
		return EN_DEFW_RC_BAD_PARAM;
	program[converted] = L'\0';

	Py_SetProgramName(program);

	Py_Initialize();

	return python_setup();
}

defw_rc_t python_finalize()
{
	PDEBUG("Python finalizing");

	if (g_defw_cfg.shell == EN_DEFW_RUN_CMD_LINE)
		RUN_PYTHON_CMD("me.exit()");
	Py_Finalize();

	PDEBUG("Python finalized");

	return EN_DEFW_RC_OK;
}

void python_print_version()
{
	const char *version;
	char pyversion[MAX_STR_LEN];
	char *end;

	Py_Initialize();
	version = Py_GetVersion();

	end = strchr(version, ' ');
	if (end) {
		int len = (end - version) + 1;
		snprintf(pyversion, len, "%s", version);
	} else {
		strcpy(pyversion, version);
	}

	fprintf(stdout, "%s\n", pyversion);
	Py_Finalize();
}

int python_check_version()
{
	const char *version;
	int check = 1;
	char pyversion[MAX_STR_LEN];
	char *end;

	Py_Initialize();
	version = Py_GetVersion();
	end = strchr(version, ' ');
	if (end) {
		int len = (end - version) + 1;
		snprintf(pyversion, len, "%s", version);
	} else {
		strcpy(pyversion, version);
	}

	if (strncmp(version, PY_VERSION, strlen(PY_VERSION)) != 0) {
		fprintf(stderr, "Version mismatch! Headers %s vs runtime %s\n",
			PY_VERSION, pyversion);
		check = 0;
	}
	Py_Finalize();

	return check;
}

typedef enum python_callbacks {
	EN_PY_CB_REQUEST,
	EN_PY_CB_RESPONSE,
	EN_PY_CB_CONNECT,
	EN_PY_CB_EVENT,
	EN_PY_CB_MAX,
} python_callbacks_t;

char *python_callback_str[EN_PY_CB_MAX] = {
	"put_request",
	"put_response",
	"put_connect_complete",
	"put_event",
};

PyGILState_STATE python_gil_ensure()
{
	PyGILState_STATE gstate;
	gstate = PyGILState_Ensure();
	(void)atomic_fetch_add(&g_py_gil_refcount, 1);
	return gstate;
}

void python_gil_release(PyGILState_STATE gstate)
{
	PyGILState_Release(gstate);
	(void) atomic_fetch_sub(&g_py_gil_refcount, 1);
}

static int py_dict_set_string(PyObject *dict, const char *key,
			      const char *value)
{
	int rc;
	PyObject *obj = PyUnicode_FromString(value ? value : "");

	if (!obj)
		return -1;
	rc = PyDict_SetItemString(dict, key, obj);
	Py_DECREF(obj);
	return rc;
}

static int py_dict_set_long(PyObject *dict, const char *key, long value)
{
	int rc;
	PyObject *obj = PyLong_FromLong(value);

	if (!obj)
		return -1;
	rc = PyDict_SetItemString(dict, key, obj);
	Py_DECREF(obj);
	return rc;
}

static int py_dict_set_bool(PyObject *dict, const char *key, int value)
{
	int rc;
	PyObject *obj = value ? Py_True : Py_False;

	Py_INCREF(obj);
	rc = PyDict_SetItemString(dict, key, obj);
	Py_DECREF(obj);
	return rc;
}

static int py_dict_set_double(PyObject *dict, const char *key, double value)
{
	int rc;
	PyObject *obj = PyFloat_FromDouble(value);

	if (!obj)
		return -1;
	rc = PyDict_SetItemString(dict, key, obj);
	Py_DECREF(obj);
	return rc;
}

static PyObject *python_peer_event_to_dict(const defw_peer_event_t *event)
{
	PyObject *py_event;
	PyObject *endpoint;
	double timestamp;

	py_event = PyDict_New();
	endpoint = PyDict_New();
	if (!py_event || !endpoint)
		goto fail;

	timestamp = (double)event->timestamp_sec +
		    ((double)event->timestamp_usec / 1000000.0);

	if (py_dict_set_string(py_event, "event_type",
			       defw_peer_event_type2str(event->event_type)) ||
	    py_dict_set_string(py_event, "peer_handle", event->peer_handle) ||
	    py_dict_set_string(py_event, "remote_runtime_id",
			       event->remote_runtime_id) ||
	    py_dict_set_bool(py_event, "is_self", event->is_self) ||
	    py_dict_set_string(py_event, "transport_context",
			       event->transport_context) ||
	    py_dict_set_string(py_event, "connection_direction",
			       event->connection_direction) ||
	    py_dict_set_long(py_event, "node_type", event->node_type) ||
	    py_dict_set_string(py_event, "reason", event->reason) ||
	    py_dict_set_double(py_event, "timestamp", timestamp) ||
	    py_dict_set_string(endpoint, "address", event->address) ||
	    py_dict_set_long(endpoint, "listen_port", event->listen_port) ||
	    py_dict_set_long(endpoint, "node_type", event->node_type) ||
	    py_dict_set_string(endpoint, "node_name", event->node_name) ||
	    py_dict_set_string(endpoint, "hostname", event->hostname) ||
	    py_dict_set_long(endpoint, "pid", event->pid) ||
	    PyDict_SetItemString(py_event, "endpoint", endpoint))
		goto fail;

	Py_DECREF(endpoint);
	return py_event;

fail:
	Py_XDECREF(endpoint);
	Py_XDECREF(py_event);
	return NULL;
}

defw_rc_t python_handle_peer_event(const defw_peer_event_t *event)
{
	defw_rc_t rc = EN_DEFW_RC_OK;
	PyGILState_STATE gstate;
	PyObject *module_name = NULL;
	PyObject *defw = NULL;
	PyObject *py_handler = NULL;
	PyObject *py_event = NULL;
	PyObject *args = NULL;
	PyObject *result = NULL;

	if (!event)
		return EN_DEFW_RC_BAD_PARAM;
	if (!g_defw_cfg.initialized)
		return EN_DEFW_RC_PY_SCRIPT_FAIL;

	gstate = python_gil_ensure();

	module_name = PyUnicode_FromString("defw_workers");
	if (!module_name)
		goto fail;
	defw = PyImport_Import(module_name);
	if (!defw)
		goto fail;
	py_handler = PyObject_GetAttrString(defw, "put_peer_event");
	if (!py_handler)
		goto fail;
	py_event = python_peer_event_to_dict(event);
	if (!py_event)
		goto fail;
	args = PyTuple_Pack(1, py_event);
	if (!args)
		goto fail;

	result = PyObject_CallObject(py_handler, args);
	if (!result)
		goto fail;

	goto out;

fail:
	rc = EN_DEFW_RC_PY_SCRIPT_FAIL;
	if (PyErr_Occurred())
		PyErr_Print();

out:
	Py_XDECREF(result);
	Py_XDECREF(args);
	Py_XDECREF(py_event);
	Py_XDECREF(py_handler);
	Py_XDECREF(defw);
	Py_XDECREF(module_name);
	python_gil_release(gstate);

	return rc;
}

static defw_rc_t
python_handle_op(char *msg, defw_rc_t status, char *uuid, python_callbacks_t cb)
{
	defw_rc_t rc = EN_DEFW_RC_OK;
	PyGILState_STATE gstate;
	PyObject *py_handler, *defw, *pystatus, *pymsg, *pyuuid,
		 *str, *args = NULL, *result;
	char *func = python_callback_str[cb];

	if (!g_defw_cfg.initialized)
		return EN_DEFW_RC_PY_SCRIPT_FAIL;

	if (msg && uuid)
		PMSG("Handling %s from %s\n%s", func, uuid, msg);

	gstate = python_gil_ensure();

	str = PyUnicode_FromString((char *)"defw_workers");
	defw = PyImport_Import(str);
	Py_DECREF(str);

	py_handler = PyObject_GetAttrString(defw, func);
	Py_DECREF(defw);

	switch (cb) {
	/* All strings passed to python via the CPython API
	 * are now owned and managed by python. No need to
	 * free them.
	 */
	case EN_PY_CB_REQUEST:
	case EN_PY_CB_RESPONSE:
		pymsg = PyUnicode_FromString(msg);
		pyuuid = PyUnicode_FromString(uuid);

		args = PyTuple_Pack(2, pymsg, pyuuid);
		Py_DECREF(pymsg);
		Py_DECREF(pyuuid);
		break;
	case EN_PY_CB_CONNECT:
		pystatus = PyLong_FromLong((long)status);
		pyuuid = PyUnicode_FromString(uuid);

		args = PyTuple_Pack(2, pystatus, pyuuid);
		Py_DECREF(pystatus);
		Py_DECREF(pyuuid);
		break;
	default:
		break;
	}

	result = PyObject_CallObject(py_handler, args);

	if (!result)
		PDEBUG("%s didn't return any values", func);
	else
		Py_DECREF(result);

	if (args)
		Py_DECREF(args);

	Py_DECREF(py_handler);

	python_gil_release(gstate);

	return rc;
}

defw_rc_t python_handle_request(char *msg, char *uuid)
{
	return python_handle_op(msg, EN_DEFW_RC_OK, uuid, EN_PY_CB_REQUEST);
}

defw_rc_t python_handle_response(char *msg, char *uuid)
{
	return python_handle_op(msg, EN_DEFW_RC_OK, uuid, EN_PY_CB_RESPONSE);
}

defw_rc_t python_handle_event(char *msg, char *uuid)
{
	return python_handle_op(msg, EN_DEFW_RC_OK, uuid, EN_PY_CB_EVENT);
}

defw_rc_t python_handle_connect_complete(defw_rc_t status, char *uuid)
{
	return python_handle_op(NULL, status, uuid, EN_PY_CB_CONNECT);
}
