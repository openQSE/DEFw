if(NOT DEFW_RUNNER)
	message(FATAL_ERROR "DEFW_RUNNER is required")
endif()
if(NOT DEFW_PYTHON)
	message(FATAL_ERROR "DEFW_PYTHON is required")
endif()

execute_process(
	COMMAND "${DEFW_PYTHON}" "${DEFW_RUNNER}" intentional-failure
	RESULT_VARIABLE runner_rc
	OUTPUT_VARIABLE runner_stdout
	ERROR_VARIABLE runner_stderr)

if(runner_rc EQUAL 0)
	message(FATAL_ERROR
		"DEFw runner succeeded even though an experiment reported FAIL")
endif()

set(runner_output "${runner_stdout}\n${runner_stderr}")
string(FIND "${runner_output}"
	"DEFw experiment failures: framework::intentional_failure"
	runner_failure_index)
if(runner_failure_index EQUAL -1)
	message(FATAL_ERROR
		"DEFw runner failed without reporting the failing experiment")
endif()
