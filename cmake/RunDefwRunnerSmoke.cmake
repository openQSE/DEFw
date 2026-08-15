if(NOT DEFW_RUNNER)
	message(FATAL_ERROR "DEFW_RUNNER is required")
endif()
if(NOT DEFW_PYTHON)
	message(FATAL_ERROR "DEFW_PYTHON is required")
endif()

execute_process(
	COMMAND "${DEFW_PYTHON}" -c
		"import socket; probe = socket.socket(); probe.close()"
	RESULT_VARIABLE socket_rc
	ERROR_VARIABLE socket_stderr)
if(NOT socket_rc EQUAL 0)
	string(STRIP "${socket_stderr}" socket_error)
	message("DEFw socket smoke skipped: ${socket_error}")
	return()
endif()

execute_process(
	COMMAND "${DEFW_PYTHON}" "${DEFW_RUNNER}" smoke
	RESULT_VARIABLE runner_rc)
if(NOT runner_rc EQUAL 0)
	message(FATAL_ERROR "DEFw runner smoke failed")
endif()
