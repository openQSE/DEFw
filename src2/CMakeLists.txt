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
	core/defw2_arena.c
	core/defw2_config.c
	core/defw2_log.c
	core/defw2_runtime.c
	rpc/defw2_wire.c
	rpc/defw2_binding.c
	rpc/defw2_bulk.c
	rpc/defw2_bulk_pool.c
	rpc/defw2_typed.c
	rpc/defw2_doc.c
	rpc/defw2_echo_client.c
	telemetry/defw2_otlp.c
	telemetry/defw2_trace.c
	host/defw2_service.c
	event/defw2_event.c
	event/defw2_event_pub.c
	event/defw2_event_sink.c
	dir/defw2_dir_store.c
	dir/defw2_dir_svc.c
	dir/defw2_dir_client.c
	dir/defw2_dir_agent.c
	dir/defw2_dir_cache.c
	dir/defw2_dir_event.c
	services/echo/defw2_echo_svc.c
	qpm/defw2_qpm_client.c
	qpm/defw2_qpm_event.c
	qpm/defw2_qpm_svc.c
	qpm/defw2_qpm_task.c)

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

# An install finds its own libdefw2 through $ORIGIN, and Margo, Argobots and
# libfabric through the directories the build linked them from, so it runs
# with nothing on LD_LIBRARY_PATH. A process started over ssh gets a bare
# environment, which is exactly when that matters.
set_target_properties(defw2
	PROPERTIES
		INSTALL_RPATH "$ORIGIN"
		INSTALL_RPATH_USE_LINK_PATH TRUE)

# The echo tool serves the reference service and calls it, which is what
# makes the RPC path usable from a shell and across two nodes. The benchmark
# client that records spans is a separate program.
add_executable(defw2-echo services/echo/defw2_echo_main.c)
target_link_libraries(defw2-echo PRIVATE defw2)
target_compile_options(defw2-echo PRIVATE -Wall -Wextra)
set_target_properties(defw2-echo
	PROPERTIES
		INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
		INSTALL_RPATH_USE_LINK_PATH TRUE)

# The directory. One process, one provider, and the one address that cannot
# be resolved and so has to be announced: on stdout, and optionally to a file
# for a provider where there is no port to agree on in advance.
add_executable(defw2-dirsvc services/dirsvc/defw2_dirsvc_main.c)
target_link_libraries(defw2-dirsvc PRIVATE defw2 Threads::Threads)
target_compile_options(defw2-dirsvc PRIVATE -Wall -Wextra)
set_target_properties(defw2-dirsvc
	PROPERTIES
		INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
		INSTALL_RPATH_USE_LINK_PATH TRUE)

# The measured client. The launcher beside it, bench/v2/defw2_bench.py, runs
# as many of these as the workload asks for and writes the report.
add_executable(defw2-bench bench/v2/defw2_bench.c)
# libm for W6, which checks the statevector it receives.
target_link_libraries(defw2-bench PRIVATE defw2 m)
target_compile_options(defw2-bench PRIVATE -Wall -Wextra)
set_target_properties(defw2-bench
	PROPERTIES
		INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
		INSTALL_RPATH_USE_LINK_PATH TRUE)

# The SPANK plugin's reserve and release, against the public headers
# alone: the C caller experience the design asks for. An example, so it is
# built and tested but not installed.
add_executable(defw2-spank-flow examples/defw2_spank_flow.c)
target_link_libraries(defw2-spank-flow PRIVATE defw2)
target_compile_options(defw2-spank-flow PRIVATE -Wall -Wextra)

install(TARGETS defw2 defw2-echo defw2-dirsvc defw2-bench
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
	set(DEFW2_PYTHON_INSTALL_DIR
		"${CMAKE_INSTALL_LIBDIR}/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages"
		CACHE STRING
		"Where the defw2 Python package installs, under the prefix")
	# The installed copy is built apart, so its extension finds libdefw2
	# only relative to itself: the install can move, and never reaches
	# back into the build tree.
	set(DEFW2_PYTHON_STAGE "${CMAKE_CURRENT_BINARY_DIR}/python-install")
	file(RELATIVE_PATH DEFW2_PYTHON_TO_LIBDIR
		"${CMAKE_INSTALL_PREFIX}/${DEFW2_PYTHON_INSTALL_DIR}/defw2"
		"${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
	configure_file(bindings/python/defw2-python.in
		"${CMAKE_CURRENT_BINARY_DIR}/defw2-python" @ONLY)
	add_custom_target(defw2-python ALL
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/defw2"
			"${DEFW2_PYTHON_DIR}/defw2"
		COMMAND "${CMAKE_COMMAND}" -E copy
			"${CMAKE_CURRENT_BINARY_DIR}/defw2-python"
			"${DEFW2_PYTHON_DIR}/defw2-python"
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/cffi_build.py"
			--include "${CMAKE_CURRENT_SOURCE_DIR}/include"
			--library "$<TARGET_FILE_DIR:defw2>"
			--out "${DEFW2_PYTHON_DIR}"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/defw2"
			"${DEFW2_PYTHON_STAGE}/defw2"
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/cffi_build.py"
			--include "${CMAKE_CURRENT_SOURCE_DIR}/include"
			--library "$<TARGET_FILE_DIR:defw2>"
			--rpath "\$ORIGIN/${DEFW2_PYTHON_TO_LIBDIR}"
			--out "${DEFW2_PYTHON_STAGE}"
		DEPENDS defw2
		COMMENT "Building the defw2 Python binding"
		VERBATIM)
	install(DIRECTORY "${DEFW2_PYTHON_STAGE}/defw2"
		DESTINATION "${DEFW2_PYTHON_INSTALL_DIR}"
		USE_SOURCE_PERMISSIONS
		PATTERN "__pycache__" EXCLUDE
		PATTERN "*.c" EXCLUDE
		PATTERN "*.o" EXCLUDE)
	install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/defw2-python"
		DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
	message(STATUS "DEFw v2: no cffi, skipping the Python binding")
endif()

if(BUILD_TESTING)
	add_executable(defw2_runtime_smoke tests/defw2_runtime_smoke.c)
	# It reads the configuration Margo runs with, through the runtime.
	target_include_directories(defw2_runtime_smoke
		PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core)
	target_link_libraries(defw2_runtime_smoke PRIVATE defw2)
	add_test(NAME defw2_runtime_smoke COMMAND defw2_runtime_smoke)

	# It reaches the pool through the runtime's internals.
	add_executable(defw2_bulk_pool_smoke tests/defw2_bulk_pool_smoke.c)
	target_include_directories(defw2_bulk_pool_smoke
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/core
			${CMAKE_CURRENT_SOURCE_DIR}/rpc)
	target_link_libraries(defw2_bulk_pool_smoke
		PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_bulk_pool_smoke COMMAND defw2_bulk_pool_smoke)

	# It looks RPCs up through the runtime's internals, and replaces two
	# of Mercury's functions with its own, which Margo finds only when
	# the executable exports them.
	add_executable(defw2_rpc_register_smoke
		tests/defw2_rpc_register_smoke.c)
	target_include_directories(defw2_rpc_register_smoke
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/core
			${CMAKE_CURRENT_SOURCE_DIR}/rpc)
	target_link_libraries(defw2_rpc_register_smoke
		PRIVATE defw2 Threads::Threads ${CMAKE_DL_LIBS})
	set_target_properties(defw2_rpc_register_smoke
		PROPERTIES ENABLE_EXPORTS ON)
	add_test(NAME defw2_rpc_register_smoke
		COMMAND defw2_rpc_register_smoke)

	add_executable(defw2_echo_smoke tests/defw2_echo_smoke.c)
	target_link_libraries(defw2_echo_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_echo_smoke COMMAND defw2_echo_smoke)

	add_executable(defw2_queue_smoke tests/defw2_queue_smoke.c)
	target_link_libraries(defw2_queue_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_queue_smoke COMMAND defw2_queue_smoke)

	# Drives the store directly, so it needs no runtime and no provider.
	# The argument is where the snapshot test writes; without it that one
	# case is skipped.
	add_executable(defw2_dir_store_smoke tests/defw2_dir_store_smoke.c)
	target_link_libraries(defw2_dir_store_smoke PRIVATE defw2)
	add_test(NAME defw2_dir_store_smoke
		COMMAND defw2_dir_store_smoke
			"${CMAKE_CURRENT_BINARY_DIR}/dir-snapshot.json")

	# Runs a directory and a client in one process over na+sm.
	add_executable(defw2_dir_rpc_smoke tests/defw2_dir_rpc_smoke.c)
	target_link_libraries(defw2_dir_rpc_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_dir_rpc_smoke COMMAND defw2_dir_rpc_smoke)

	# Waits out several heartbeat timeouts, so it is the slow one.
	add_executable(defw2_dir_agent_smoke tests/defw2_dir_agent_smoke.c)
	target_link_libraries(defw2_dir_agent_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_dir_agent_smoke COMMAND defw2_dir_agent_smoke)

	# A directory, a service and a client, one runtime each.
	add_executable(defw2_dir_cache_smoke tests/defw2_dir_cache_smoke.c)
	target_link_libraries(defw2_dir_cache_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_dir_cache_smoke COMMAND defw2_dir_cache_smoke)

	# A directory and a subscriber whose sinks take its events.
	add_executable(defw2_dir_event_smoke tests/defw2_dir_event_smoke.c)
	target_link_libraries(defw2_dir_event_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_dir_event_smoke COMMAND defw2_dir_event_smoke)

	# One runtime that both serves an API and calls it. Guards the
	# registration collision that silently unbound a provider.
	# The QPM test profiles into a directory of its own, which the OTLP
	# checker then reads back, so the shared typed code's spans are
	# checked as well as its calls.
	set(DEFW2_QPM_TELEMETRY_DIR "${CMAKE_CURRENT_BINARY_DIR}/telemetry-qpm")
	file(MAKE_DIRECTORY "${DEFW2_QPM_TELEMETRY_DIR}")
	add_executable(defw2_qpm_smoke tests/defw2_qpm_smoke.c)
	target_link_libraries(defw2_qpm_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_qpm_smoke
		COMMAND defw2_qpm_smoke "${DEFW2_QPM_TELEMETRY_DIR}")
	set_tests_properties(defw2_qpm_smoke
		PROPERTIES
			FIXTURES_SETUP defw2_qpm_otlp_files)

	# Sinks and publishers across two runtimes, and a sink in a second
	# process that the test starts and then stops with SIGSTOP. It too
	# profiles into a directory of its own for the OTLP checker.
	set(DEFW2_EVENT_TELEMETRY_DIR "${CMAKE_CURRENT_BINARY_DIR}/telemetry-event")
	file(MAKE_DIRECTORY "${DEFW2_EVENT_TELEMETRY_DIR}")
	add_executable(defw2_event_smoke tests/defw2_event_smoke.c)
	target_link_libraries(defw2_event_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_event_smoke
		COMMAND defw2_event_smoke "${DEFW2_EVENT_TELEMETRY_DIR}")
	set_tests_properties(defw2_event_smoke
		PROPERTIES
			FIXTURES_SETUP defw2_event_otlp_files)

	# The document tier against a C service, a queued one and one with
	# neither, profiled into a directory of its own for the OTLP checker.
	set(DEFW2_DOC_TELEMETRY_DIR "${CMAKE_CURRENT_BINARY_DIR}/telemetry-doc")
	file(MAKE_DIRECTORY "${DEFW2_DOC_TELEMETRY_DIR}")
	add_executable(defw2_doc_smoke tests/defw2_doc_smoke.c)
	target_link_libraries(defw2_doc_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_doc_smoke
		COMMAND defw2_doc_smoke "${DEFW2_DOC_TELEMETRY_DIR}")
	set_tests_properties(defw2_doc_smoke
		PROPERTIES
			ENVIRONMENT "DEFW2_ADDRESS=na+sm://"
			FIXTURES_SETUP defw2_doc_otlp_files)

	# The wire test reaches the procs directly, so it sees the internal
	# headers the library is built from.
	add_executable(defw2_wire_smoke tests/defw2_wire_smoke.c)
	target_include_directories(defw2_wire_smoke
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/core
			${CMAKE_CURRENT_SOURCE_DIR}/rpc
			${CMAKE_CURRENT_SOURCE_DIR}/telemetry
			${CMAKE_CURRENT_SOURCE_DIR}/host
			${CMAKE_CURRENT_SOURCE_DIR}/qpm
			${CMAKE_CURRENT_SOURCE_DIR}/dir
			${CMAKE_CURRENT_SOURCE_DIR}/event)
	target_link_libraries(defw2_wire_smoke
		PRIVATE defw2 PkgConfig::ARGOBOTS Threads::Threads)
	add_test(NAME defw2_wire_smoke COMMAND defw2_wire_smoke)

	add_executable(defw2_self_call_smoke tests/defw2_self_call_smoke.c)
	target_link_libraries(defw2_self_call_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_self_call_smoke COMMAND defw2_self_call_smoke)

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
			     defw2_bulk_pool_smoke defw2_rpc_register_smoke
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
		add_test(NAME defw2_qpm_otlp_check
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_otlp_check.py"
				"${DEFW2_QPM_TELEMETRY_DIR}")
		set_tests_properties(defw2_qpm_otlp_check
			PROPERTIES
				FIXTURES_REQUIRED defw2_qpm_otlp_files)
		add_test(NAME defw2_doc_otlp_check
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_otlp_check.py"
				"${DEFW2_DOC_TELEMETRY_DIR}" --tier document)
		set_tests_properties(defw2_doc_otlp_check
			PROPERTIES
				FIXTURES_REQUIRED defw2_doc_otlp_files)
		add_test(NAME defw2_event_otlp_check
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_otlp_check.py"
				"${DEFW2_EVENT_TELEMETRY_DIR}")
		set_tests_properties(defw2_event_otlp_check
			PROPERTIES
				FIXTURES_REQUIRED defw2_event_otlp_files)

		# The line counter the comparison's line-count criterion
		# rests on, and both versions counted with it, so a new
		# source file that belongs to no area fails here.
		add_test(NAME defw2_loc_check
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_loc_check.py"
				"${CMAKE_CURRENT_SOURCE_DIR}/..")
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

		# Python's sinks and publishers, in one process.
		add_test(NAME defw2_python_event_smoke
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_python_event_smoke.py")
		set_tests_properties(defw2_python_event_smoke
			PROPERTIES
				ENVIRONMENT
					"DEFW2_ADDRESS=na+sm://;PYTHONPATH=${DEFW2_PYTHON_DIR}")

		# The QPM in both languages, against callers in both, the
		# control queue under an execution backlog, and a Python QPM
		# in the directory. It runs the C fake and the directory as
		# processes of their own, so it needs both built.
		# The SPANK flow in C against the Python fake QPM, found
		# through a directory as a plugin would find a site's.
		add_test(NAME defw2_spank_flow_check
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_spank_flow_check.py"
				--flow $<TARGET_FILE:defw2-spank-flow>
				--dirsvc $<TARGET_FILE:defw2-dirsvc>
				--fake "${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_qpm_fake.py")
		set_tests_properties(defw2_spank_flow_check
			PROPERTIES
				ENVIRONMENT
					"DEFW2_ADDRESS=na+sm://;PYTHONPATH=${DEFW2_PYTHON_DIR}")

		# The benchmark harness end to end, with few calls: the
		# launcher, both clients, a directory, and the Python fake as
		# W5's QPM, polling and on events.
		add_test(NAME defw2_bench_smoke
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_bench_smoke.py"
				--bin-dir $<TARGET_FILE_DIR:defw2-bench>
				--fake "${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_qpm_fake.py")
		set_tests_properties(defw2_bench_smoke
			PROPERTIES
				ENVIRONMENT
					"DEFW2_ADDRESS=na+sm://;PYTHONPATH=${DEFW2_PYTHON_DIR}")

		add_test(NAME defw2_python_qpm_smoke
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_python_qpm_smoke.py"
				--qpm-smoke $<TARGET_FILE:defw2_qpm_smoke>
				--dirsvc $<TARGET_FILE:defw2-dirsvc>)
		set_tests_properties(defw2_python_qpm_smoke
			PROPERTIES
				ENVIRONMENT
					"DEFW2_ADDRESS=na+sm://;PYTHONPATH=${DEFW2_PYTHON_DIR}")

		# v1 QFw code on v2: a v1 QPM module served by defw2-python
		# --serve and a v1 client run by defw2-python, compared call
		# for call with v1's answers. The v1 modules compat reuses
		# come from this repository's python tree.
		add_test(NAME defw2_compat_smoke
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_compat_smoke.py"
				--dirsvc $<TARGET_FILE:defw2-dirsvc>
				--launcher "${DEFW2_PYTHON_DIR}/defw2-python")
		set_tests_properties(defw2_compat_smoke
			PROPERTIES
				ENVIRONMENT
					"DEFW2_ADDRESS=na+sm://;PYTHONPATH=${DEFW2_PYTHON_DIR};DEFW2_PYTHON=${Python3_EXECUTABLE};DEFW_PATH=${CMAKE_CURRENT_SOURCE_DIR}/..")

		# The same compat test again, from an install of src2 into a
		# scratch prefix, with no LD_LIBRARY_PATH and no PYTHONPATH.
		add_test(NAME defw2_install_smoke
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/defw2_install_smoke.py"
				--build "${CMAKE_CURRENT_BINARY_DIR}"
				--cmake "${CMAKE_COMMAND}"
				--python "${Python3_EXECUTABLE}")
		set_tests_properties(defw2_install_smoke
			PROPERTIES
				ENVIRONMENT
					"DEFW2_ADDRESS=na+sm://;DEFW_PATH=${CMAKE_CURRENT_SOURCE_DIR}/..")
	endif()
endif()
