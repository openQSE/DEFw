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
	host/defw2_service.c
	services/echo/defw2_echo_svc.c)

target_include_directories(defw2
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/core
		${CMAKE_CURRENT_SOURCE_DIR}/rpc
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

if(BUILD_TESTING)
	add_executable(defw2_runtime_smoke tests/defw2_runtime_smoke.c)
	target_link_libraries(defw2_runtime_smoke PRIVATE defw2)
	add_test(NAME defw2_runtime_smoke COMMAND defw2_runtime_smoke)

	add_executable(defw2_echo_smoke tests/defw2_echo_smoke.c)
	target_link_libraries(defw2_echo_smoke PRIVATE defw2 Threads::Threads)
	add_test(NAME defw2_echo_smoke COMMAND defw2_echo_smoke)

	# Shared memory needs no network and no port, so the tests say
	# nothing about the machine they run on.
	set_tests_properties(defw2_runtime_smoke defw2_echo_smoke
		PROPERTIES
			ENVIRONMENT "DEFW2_ADDRESS=na+sm://")
endif()
