#ifndef DEFW_COMMON_H
#define DEFW_COMMON_H

#define RESET   "\033[0m"
#define BLACK   "\033[30m"      /* Black */
#define RED     "\033[31m"      /* Red */
#define GREEN   "\033[32m"      /* Green */
#define YELLOW  "\033[33m"      /* Yellow */
#define BLUE    "\033[34m"      /* Blue */
#define MAGENTA "\033[35m"      /* Magenta */
#define CYAN    "\033[36m"      /* Cyan */
#define WHITE   "\033[37m"      /* White */
#define BOLDBLACK   "\033[1m\033[30m"      /* Bold Black */
#define BOLDRED     "\033[1m\033[31m"      /* Bold Red */
#define BOLDGREEN   "\033[1m\033[32m"      /* Bold Green */
#define BOLDYELLOW  "\033[1m\033[33m"      /* Bold Yellow */
#define BOLDBLUE    "\033[1m\033[34m"      /* Bold Blue */
#define BOLDMAGENTA "\033[1m\033[35m"      /* Bold Magenta */
#define BOLDCYAN    "\033[1m\033[36m"      /* Bold Cyan */
#define BOLDWHITE   "\033[1m\033[37m"      /* Bold White */

/* Bumped to 2: the message header now carries the sender uuid instead of
 * the sender IP address.
 * Bumped to 3: the session message now carries the sender's OFI endpoint
 * address, so its size changed. Old and new builds are intentionally
 * incompatible (the header version check rejects a mismatched peer before
 * its differently-sized session body can be misread).
 */
#define DEFW_VERSION_NUMBER		 3

#define MAX_STR_LEN			1024
#define MAX_SHORT_STR_LEN		128
#define MAX_PATH_LEN 			256
#define MAX_MSG_SIZE			2048

#define DEFW_EXIT_NORMAL		 0
#define DEFW_EXIT_ERR_STARTUP		-1
#define DEFW_EXIT_ERR_BAD_PARAM		-2
#define DEFW_EXIT_ERR_THREAD_STARTUP	-3
#define DEFW_EXIT_ERR_DEAMEON_STARTUP	-4
#define DEFW_PYTHON_VERSION_MISMATCH	-5

#define SYSTEMIPADDR			0x7f000001
#define INVALID_TCP_SOCKET		-1
#define SOCKET_TIMEOUT_USEC		900000
#define SOCKET_CONN_TIMEOUT_SEC		2
#define TCP_READ_TIMEOUT_SEC		20

#define DEFAULT_PARENT_PORT	8282

/* Framework Environment Variables needed from C */
#define DEFW_PATH 		"DEFW_PATH" /* base installation path */
#define DEFW_TRANSPORT_ENV	"DEFW_TRANSPORT" /* "tcp" (default) or "ofi" */
#define DEFW_OFI_PROVIDER_ENV	"DEFW_OFI_PROVIDER" /* optional fi provider filter */

#ifndef _UUID_UUID_H
typedef unsigned char uuid_t[16];
#endif

typedef enum {
	EN_DEFW_RC_OK = 0,
	EN_DEFW_RC_FAIL = -1,
	EN_DEFW_RC_SYS_ERR = -2,
	EN_DEFW_RC_BAD_VERSION = -3,
	EN_DEFW_RC_SOCKET_FAIL = -4,
	EN_DEFW_RC_BIND_FAILED = -5,
	EN_DEFW_RC_LISTEN_FAILED = -6,
	EN_DEFW_RC_CLIENT_CLOSED = -7,
	EN_DEFW_RC_ERR_THREAD_STARTUP = -8,
	EN_DEFW_RC_AGENT_NOT_FOUND = -9,
	EN_DEFW_RC_PY_IMPORT_FAIL = -10,
	EN_DEFW_RC_PY_SCRIPT_FAIL = -11,
	EN_DEFW_RC_RPC_FAIL = -12,
	EN_DEFW_RC_OOM = -13,
	EN_DEFW_RC_BAD_PARAM = -14,
	EN_DEFW_RC_BAD_ADDR = -15,
	EN_DEFW_RC_MISSING_PARAM = -16,
	EN_DEFW_RC_TIMEOUT = -17,
	EN_DEFW_RC_LOG_CREATION_FAILURE = -18,
	EN_DEFW_RC_PROTO_ERROR = -19,
	EN_DEFW_RC_IN_PROGRESS = -20,
	EN_DEFW_RC_BAD_UUID = -21,
	EN_DEFW_RC_NO_DATA_ON_SOCKET = -22,
	EN_DEFW_RC_KEEP_DATA = -23,
	EN_DEFW_RC_UNKNOWN_MESSAGE = -24,
	EN_DEFW_RC_MAX,
} defw_rc_t;

typedef enum defw_type {
	EN_DEFW_RESMGR = 1,
	EN_DEFW_AGENT = 2,
	EN_DEFW_SERVICE = 3,
	EN_DEFW_INVALID,
} defw_type_t;

#define INTERACTIVE "interactive"
#define BATCH "batch"
#define DAEMON "daemon"

typedef enum defw_run_mode {
	EN_DEFW_RUN_INTERACTIVE = 1,
	EN_DEFW_RUN_BATCH = 2,
	EN_DEFW_RUN_DAEMON = 3,
	EN_DEFW_RUN_CMD_LINE = 4,
	EN_DEFW_RUN_INVALID,
} defw_run_mode_t;

typedef enum defw_log_level {
	EN_LOG_LEVEL_ERROR = 1,
	EN_LOG_LEVEL_DEBUG,
	EN_LOG_LEVEL_MSG,
	EN_LOG_LEVEL_ALL,
} defw_log_level_t;

#endif /* DEFW_COMMON_H */
