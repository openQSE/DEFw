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
	core/defw2_runtime.c)

target_include_directories(defw2
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/core)

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

install(TARGETS defw2
	LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/defw2
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(BUILD_TESTING)
	add_executable(defw2_runtime_smoke tests/defw2_runtime_smoke.c)
	target_link_libraries(defw2_runtime_smoke PRIVATE defw2)
	add_test(NAME defw2_runtime_smoke COMMAND defw2_runtime_smoke)
	# Shared memory needs no network and no port, so the test says
	# nothing about the machine it runs on.
	set_tests_properties(defw2_runtime_smoke
		PROPERTIES
			ENVIRONMENT "DEFW2_ADDRESS=na+sm://")
endif()
