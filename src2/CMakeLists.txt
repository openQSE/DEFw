# DEFw v2 prototype. Built only when DEFW_BUILD_V2 is ON, so every existing
# build and CI job stays exactly as it was.
#
# Mercury, Margo and Argobots come from pkg-config, which finds either a
# source build such as the QFw-SLURM-Cluster image's /opt/qfw/mochi or a
# Spack mochi-margo. Load the module first, or point PKG_CONFIG_PATH at it.

find_package(PkgConfig REQUIRED)
pkg_check_modules(MARGO REQUIRED IMPORTED_TARGET margo)
pkg_check_modules(ARGOBOTS REQUIRED IMPORTED_TARGET argobots>=1.2)

message(STATUS "DEFw v2: margo ${MARGO_VERSION}, argobots ${ARGOBOTS_VERSION}")

add_library(defw2 SHARED
	core/defw2_config.c
	core/defw2_log.c
	core/defw2_runtime.c
	rpc/defw2_wire.c
	rpc/defw2_binding.c
	rpc/defw2_echo_client.c
	telemetry/defw2_otlp.c
	telemetry/defw2_trace.c
	host/defw2_service.c
	services/echo/defw2_echo_svc.c)

target_include_directories(defw2
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/core
		${CMAKE_CURRENT_SOURCE_DIR}/rpc
		${CMAKE_CURRENT_SOURCE_DIR}/telemetry
		${CMAKE_CURRENT_SOURCE_DIR}/host)

target_compile_options(defw2
	PRIVATE
		-Wall
		-Wextra
		-fPIC)

target_link_libraries(defw2
	PUBLIC
		PkgConfig::MARGO
	PRIVATE
		PkgConfig::ARGOBOTS
		Threads::Threads
		"${UUID_LIBRARY}")

set_target_properties(defw2
	PROPERTIES
		INSTALL_RPATH "$ORIGIN")

# The echo tool serves the reference service and calls it, which is what
# makes the RPC path usable from a shell and across two nodes. The benchmark
# client that records spans is a separate program.
add_executable(defw2-echo services/echo/defw2_echo_main.c)
target_link_libraries(defw2-echo PRIVATE defw2)
target_compile_options(defw2-echo PRIVATE -Wall -Wextra)
set_target_properties(defw2-echo
	PROPERTIES
		INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

install(TARGETS defw2 defw2-echo
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
	LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/defw2
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The Python binding, built when cffi is there to build it. Everything
# below is skipped without it, so a C-only build needs no Python at all.
find_package(Python3 QUIET COMPONENTS Interpreter)
set(DEFW2_PYTHON_DIR "${CMAKE_CURRENT_BINARY_DIR}/python")
set(DEFW2_HAVE_CFFI FALSE)
if(Python3_Interpreter_FOUND)
	execute_process(
		COMMAND "${Python3_EXECUTABLE}" -c "import cffi"
		RESULT_VARIABLE DEFW2_CFFI_MISSING
		OUTPUT_QUIET ERROR_QUIET)
	if(DEFW2_CFFI_MISSING EQUAL 0)
		set(DEFW2_HAVE_CFFI TRUE)
	endif()
endif()

if(DEFW2_HAVE_CFFI)
	message(STATUS "DEFw v2: building the Python binding with cffi")
	add_custom_target(defw2-python ALL
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/defw2"
			"${DEFW2_PYTHON_DIR}/defw2"
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/cffi_build.py"
			--include "${CMAKE_CURRENT_SOURCE_DIR}/include"
			--library "$<TARGET_FILE_DIR:defw2>"
			--out "${DEFW2_PYTHON_DIR}"
		DEPENDS defw2
		COMMENT "Building the defw2 Python binding")
else()
	message(STATUS "DEFw v2: no cffi, skipping the Python binding")
endif()

if(BUILD_TESTING)
	add_executable(defw2_runtime_smoke tests/defw2_runtime_smoke.c)
	target_link_libraries(defw2_runtime_smoke PRIVATE defw2)
	add_test(NAME defw2_runtime_smoke COMMAND defw2_runtime_smoke)

	add_executable(defw2_echo_smoke tests/defw2_echo_smoke.c)
	target_link_libraries(defw2_echo_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_echo_smoke COMMAND defw2_echo_smoke)

	add_executable(defw2_queue_smoke tests/defw2_queue_smoke.c)
	target_link_libraries(defw2_queue_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_queue_smoke COMMAND defw2_queue_smoke)

	# The telemetry test leaves its files behind for the checker below.
	set(DEFW2_TELEMETRY_DIR "${CMAKE_CURRENT_BINARY_DIR}/telemetry")
	file(MAKE_DIRECTORY "${DEFW2_TELEMETRY_DIR}")
	add_executable(defw2_telemetry_smoke tests/defw2_telemetry_smoke.c)
	target_link_libraries(defw2_telemetry_smoke PRIVATE defw2)
	add_test(NAME defw2_telemetry_smoke
		COMMAND defw2_telemetry_smoke "${DEFW2_TELEMETRY_DIR}")
	set_tests_properties(defw2_telemetry_smoke
		PROPERTIES
			FIXTURES_SETUP defw2_otlp_files)

	# Shared memory needs no network and no port, so the tests say
	# nothing about the machine they run on.
	set_tests_properties(defw2_runtime_smoke defw2_echo_smoke
			     defw2_queue_smoke defw2_telemetry_smoke
		PROPERTIES
			ENVIRONMENT "DEFW2_ADDRESS=na+sm://")

	# The C test counts what it reads. This one parses it the way the
	# report tooling will, which is the check that matters.
	if(Python3_Interpreter_FOUND)
		add_test(NAME defw2_otlp_check
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_otlp_check.py"
				"${DEFW2_TELEMETRY_DIR}")
		set_tests_properties(defw2_otlp_check
			PROPERTIES
				FIXTURES_REQUIRED defw2_otlp_files)
	else()
		message(STATUS "DEFw v2: no python3, skipping defw2_otlp_check")
	endif()

	# The binding's own test: a Python service, a Python client, and the
	# check that a blocking call does not hold the interpreter lock.
	if(DEFW2_HAVE_CFFI)
		add_test(NAME defw2_python_smoke
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_python_smoke.py")
		set_tests_properties(defw2_python_smoke
			PROPERTIES
				ENVIRONMENT
					"DEFW2_ADDRESS=na+sm://;PYTHONPATH=${DEFW2_PYTHON_DIR}")
	endif()
endif()
