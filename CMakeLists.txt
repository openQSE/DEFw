cmake_minimum_required(VERSION 3.20)

project(DEFw
	VERSION 0.1.0
	LANGUAGES C)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
find_package(SWIG REQUIRED)
find_package(Threads REQUIRED)
find_library(UUID_LIBRARY uuid REQUIRED)

option(DEFW_ENABLE_LIBFABRIC "Enable the optional libfabric transport" ON)
set(DEFW_HAVE_LIBFABRIC OFF)
if(DEFW_ENABLE_LIBFABRIC)
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(LIBFABRIC QUIET IMPORTED_TARGET libfabric)
	endif()
	if(TARGET PkgConfig::LIBFABRIC)
		set(DEFW_LIBFABRIC_TARGET PkgConfig::LIBFABRIC)
		set(DEFW_HAVE_LIBFABRIC ON)
	else()
		find_path(DEFW_LIBFABRIC_INCLUDE_DIR rdma/fabric.h
			HINTS
				"$ENV{LIBFABRIC_DIR}"
				"$ENV{LIBFABRIC_INSTALL_DIR}"
			PATH_SUFFIXES include)
		find_library(DEFW_LIBFABRIC_LIBRARY fabric
			HINTS
				"$ENV{LIBFABRIC_DIR}"
				"$ENV{LIBFABRIC_INSTALL_DIR}"
			PATH_SUFFIXES lib lib64)
		if(DEFW_LIBFABRIC_INCLUDE_DIR AND DEFW_LIBFABRIC_LIBRARY)
			add_library(defw_libfabric UNKNOWN IMPORTED)
			set_target_properties(defw_libfabric PROPERTIES
				IMPORTED_LOCATION "${DEFW_LIBFABRIC_LIBRARY}"
				INTERFACE_INCLUDE_DIRECTORIES
					"${DEFW_LIBFABRIC_INCLUDE_DIR}")
			set(DEFW_LIBFABRIC_TARGET defw_libfabric)
			set(DEFW_HAVE_LIBFABRIC ON)
		endif()
	endif()
endif()
if(DEFW_HAVE_LIBFABRIC)
	message(STATUS "DEFw libfabric transport enabled")
else()
	message(STATUS "DEFw libfabric not found; building TCP-only transport")
endif()

set(DEFW_BUILD_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime")
set(DEFW_BUILD_SRC_DIR "${DEFW_BUILD_RUNTIME_DIR}/src")
set(DEFW_BUILD_PYTHON_DIR "${DEFW_BUILD_RUNTIME_DIR}/python")
set(DEFW_SWIG_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/swig")
set(DEFW_PYTHON_INSTALL_DIR
	"${CMAKE_INSTALL_LIBDIR}/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages")
set(DEFW_EXTERNAL_SWIG_CONFIG "" CACHE FILEPATH
	"YAML config for optional external SWIG wrapper generation")

file(MAKE_DIRECTORY
	"${DEFW_BUILD_SRC_DIR}"
	"${DEFW_BUILD_PYTHON_DIR}"
	"${DEFW_SWIG_BINARY_DIR}")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${DEFW_BUILD_SRC_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${DEFW_BUILD_SRC_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DEFW_BUILD_SRC_DIR}")

set(DEFW_PUBLIC_HEADERS
	src/defw.h
	src/defw_agent.h
	src/defw_common.h
	src/defw_global.h
	src/defw_list.h
	src/defw_listener.h
	src/defw_message.h
	src/defw_print.h
	src/defw_python.h
	src/defw_sl.h
	src/defw_transport.h
	src/libdefw_agent.h
	src/libdefw_connect.h)

set(DEFW_SWIG_TYPEMAPS
	swig/typemaps/opaque_handle.i
	swig/typemaps/owned_string.i
	swig/typemaps/owned_string_list_counted.i
	swig/typemaps/rma_buffer.i)

set(DEFW_BUILD_RPATH "${DEFW_BUILD_SRC_DIR}")
set(DEFW_LIBRARY_INSTALL_RPATH "$ORIGIN")
set(DEFW_EXECUTABLE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
set(DEFW_PYTHON_EXTENSION_INSTALL_RPATH
	"$ORIGIN/../.."
	"$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
	"$ORIGIN")

function(defw_common_target_settings target)
	target_include_directories(${target}
		PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
			$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/defw>)
	target_compile_options(${target}
		PRIVATE
			-Wall
			-fPIC)
	set_target_properties(${target}
		PROPERTIES
			BUILD_RPATH "${DEFW_BUILD_RPATH}"
			INSTALL_RPATH "${DEFW_LIBRARY_INSTALL_RPATH}")
endfunction()

function(defw_configure_transport target)
	target_sources(${target}
		PRIVATE
			src/defw_transport_ofi.c
			src/defw_transport_tcp.c)
	if(DEFW_HAVE_LIBFABRIC)
		target_compile_definitions(${target} PRIVATE HAVE_LIBFABRIC)
		target_link_libraries(${target} PRIVATE ${DEFW_LIBFABRIC_TARGET})
	endif()
endfunction()

add_library(defw_global SHARED
	src/libdefw_global.c)
defw_common_target_settings(defw_global)
target_link_libraries(defw_global
	PUBLIC
		"${UUID_LIBRARY}")

add_library(defw_connect SHARED
	src/libdefw_connect.c)
defw_common_target_settings(defw_connect)

add_library(defw_agent SHARED
	src/libdefw_agent.c)
defw_common_target_settings(defw_agent)
target_link_libraries(defw_agent
	PUBLIC
		defw_global
		defw_connect
		Threads::Threads
		"${UUID_LIBRARY}")

add_library(defw SHARED
	src/defw_listener.c
	src/defw_python.c
	src/defw_sl.c)
defw_common_target_settings(defw)
defw_configure_transport(defw)
target_include_directories(defw
	PRIVATE
		${Python3_INCLUDE_DIRS})
target_link_libraries(defw
	PUBLIC
		defw_global
		defw_connect
		defw_agent
		Threads::Threads
		"${UUID_LIBRARY}"
	PRIVATE
		Python3::Python)
set_target_properties(defw
	PROPERTIES
		OUTPUT_NAME defw)

add_executable(defwp
	src/defw.c)
target_link_libraries(defwp
	PRIVATE
		defw
		defw_global
		defw_connect
		defw_agent
		"${UUID_LIBRARY}"
		Python3::Python)
set_target_properties(defwp
	PROPERTIES
		BUILD_RPATH "${DEFW_BUILD_RPATH}"
		INSTALL_RPATH "${DEFW_EXECUTABLE_INSTALL_RPATH}")

function(defw_add_swig_module module_name interface_file)
	set(options NO_INSTALL)
	set(one_value_args)
	set(multi_value_args DEPENDS INCLUDE_DIRS LINK_LIBRARIES)
	cmake_parse_arguments(DEFW_SWIG
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN})

	set(wrapper_c "${DEFW_SWIG_BINARY_DIR}/${module_name}_wrap.c")
	set(proxy_py "${DEFW_BUILD_SRC_DIR}/${module_name}.py")
	set(swig_include_args)
	foreach(include_dir IN LISTS DEFW_SWIG_INCLUDE_DIRS)
		list(APPEND swig_include_args "-I${include_dir}")
	endforeach()

	add_custom_command(
		OUTPUT
			"${wrapper_c}"
			"${proxy_py}"
		COMMAND
			"${SWIG_EXECUTABLE}"
			-threads
			-python
			-includeall
			-D__x86_64__
			-D__arch_lib__
			-D_LARGEFILE64_SOURCE=1
			-I${CMAKE_CURRENT_SOURCE_DIR}/src
			-I${CMAKE_CURRENT_SOURCE_DIR}/swig/typemaps
			${swig_include_args}
			-outdir "${DEFW_BUILD_SRC_DIR}"
			-o "${wrapper_c}"
			"${CMAKE_CURRENT_SOURCE_DIR}/${interface_file}"
		DEPENDS
			"${interface_file}"
			${DEFW_PUBLIC_HEADERS}
			${DEFW_SWIG_TYPEMAPS}
			${DEFW_SWIG_DEPENDS}
		VERBATIM)

	add_library(_${module_name} MODULE
		"${wrapper_c}")
	target_include_directories(_${module_name}
		PRIVATE
			"${CMAKE_CURRENT_SOURCE_DIR}/src"
			${DEFW_SWIG_INCLUDE_DIRS}
			${Python3_INCLUDE_DIRS})
	target_compile_options(_${module_name}
		PRIVATE
			-Wall
			-fPIC)
	target_link_libraries(_${module_name}
		PRIVATE
			${DEFW_SWIG_LINK_LIBRARIES}
			Python3::Python)
	set_target_properties(_${module_name}
		PROPERTIES
			PREFIX ""
			OUTPUT_NAME "_${module_name}"
			LIBRARY_OUTPUT_DIRECTORY "${DEFW_BUILD_SRC_DIR}"
			BUILD_RPATH "${DEFW_BUILD_RPATH}"
			INSTALL_RPATH "${DEFW_PYTHON_EXTENSION_INSTALL_RPATH}")

	if(NOT DEFW_SWIG_NO_INSTALL)
		install(TARGETS _${module_name}
			LIBRARY DESTINATION "${DEFW_PYTHON_INSTALL_DIR}")
		install(FILES "${proxy_py}"
			DESTINATION "${DEFW_PYTHON_INSTALL_DIR}")
		install(TARGETS _${module_name}
			LIBRARY DESTINATION src)
		install(FILES "${proxy_py}"
			DESTINATION src)
	endif()
endfunction()

defw_add_swig_module(cdefw_global swig/cdefw_global.i
	LINK_LIBRARIES defw_global)
defw_add_swig_module(cdefw_agent swig/cdefw_agent.i
	LINK_LIBRARIES defw defw_agent defw_connect defw_global)
target_link_options(_cdefw_agent
	PRIVATE
		"LINKER:--no-as-needed")

set(DEFW_EXTERNAL_SWIG_SCRIPT
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/defw_external_swig.py")

if(BUILD_TESTING)
add_library(defw_typemap_fixture SHARED
	tests/swig_typemaps/defw_typemap_fixture.c)
defw_common_target_settings(defw_typemap_fixture)
target_include_directories(defw_typemap_fixture
	PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/swig_typemaps")
defw_add_swig_module(defw_typemap_fixture
	tests/swig_typemaps/defw_typemap_fixture.i
	NO_INSTALL
	DEPENDS
		tests/swig_typemaps/defw_typemap_fixture.h
	INCLUDE_DIRS
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/swig_typemaps"
	LINK_LIBRARIES
		defw_typemap_fixture)

add_library(defw_external_swig_fixture_lib SHARED
	tests/external_swig/fixture.c)
set_target_properties(defw_external_swig_fixture_lib
	PROPERTIES
		OUTPUT_NAME defw_external_swig_fixture
		LIBRARY_OUTPUT_DIRECTORY "${DEFW_BUILD_SRC_DIR}"
		BUILD_RPATH "${DEFW_BUILD_RPATH}"
		INSTALL_RPATH "${DEFW_LIBRARY_INSTALL_RPATH}")
target_include_directories(defw_external_swig_fixture_lib
	PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/external_swig")

set(DEFW_EXTERNAL_SWIG_FIXTURE_DIR
	"${CMAKE_CURRENT_BINARY_DIR}/external-swig-fixture")
set(DEFW_EXTERNAL_SWIG_FIXTURE_CONFIG
	"${CMAKE_CURRENT_BINARY_DIR}/defw_external_swig_fixture.yaml")
configure_file(
	tests/external_swig/defw_external_swig_fixture.yaml.in
	"${DEFW_EXTERNAL_SWIG_FIXTURE_CONFIG}"
	@ONLY)
add_custom_command(
	OUTPUT
		"${DEFW_EXTERNAL_SWIG_FIXTURE_DIR}/cdefw_external_swig_fixture.py"
		"${DEFW_EXTERNAL_SWIG_FIXTURE_DIR}/_cdefw_external_swig_fixture.so"
	COMMAND
		"${Python3_EXECUTABLE}"
		"${DEFW_EXTERNAL_SWIG_SCRIPT}"
		--config "${DEFW_EXTERNAL_SWIG_FIXTURE_CONFIG}"
		--binary-dir "${DEFW_EXTERNAL_SWIG_FIXTURE_DIR}"
		--source-dir "${CMAKE_CURRENT_SOURCE_DIR}"
		--swig "${SWIG_EXECUTABLE}"
		--cc "${CMAKE_C_COMPILER}"
	DEPENDS
		defw_external_swig_fixture_lib
		"${DEFW_EXTERNAL_SWIG_SCRIPT}"
		"${DEFW_EXTERNAL_SWIG_FIXTURE_CONFIG}"
		tests/external_swig/fixture.h
	VERBATIM)
add_custom_target(defw_external_swig_fixture_module ALL
	DEPENDS
		"${DEFW_EXTERNAL_SWIG_FIXTURE_DIR}/cdefw_external_swig_fixture.py"
		"${DEFW_EXTERNAL_SWIG_FIXTURE_DIR}/_cdefw_external_swig_fixture.so")
endif()

if(DEFW_EXTERNAL_SWIG_CONFIG)
	if(IS_ABSOLUTE "${DEFW_EXTERNAL_SWIG_CONFIG}")
		set(DEFW_EXTERNAL_SWIG_CONFIG_ABS "${DEFW_EXTERNAL_SWIG_CONFIG}")
	else()
		get_filename_component(DEFW_EXTERNAL_SWIG_CONFIG_ABS
			"${CMAKE_CURRENT_SOURCE_DIR}/${DEFW_EXTERNAL_SWIG_CONFIG}"
			ABSOLUTE)
	endif()
	set(DEFW_EXTERNAL_SWIG_OUTPUT_DIR
		"${CMAKE_CURRENT_BINARY_DIR}/external-swig")
	add_custom_target(defw_external_swig ALL
		COMMAND
			"${Python3_EXECUTABLE}"
			"${DEFW_EXTERNAL_SWIG_SCRIPT}"
			--config "${DEFW_EXTERNAL_SWIG_CONFIG_ABS}"
			--binary-dir "${DEFW_EXTERNAL_SWIG_OUTPUT_DIR}"
			--source-dir "${CMAKE_CURRENT_SOURCE_DIR}"
			--swig "${SWIG_EXECUTABLE}"
			--cc "${CMAKE_C_COMPILER}"
		DEPENDS
			"${DEFW_EXTERNAL_SWIG_SCRIPT}"
			"${DEFW_EXTERNAL_SWIG_CONFIG_ABS}"
		VERBATIM)
	install(DIRECTORY "${DEFW_EXTERNAL_SWIG_OUTPUT_DIR}/"
		DESTINATION "${DEFW_PYTHON_INSTALL_DIR}"
		FILES_MATCHING
			PATTERN "c*.py"
			PATTERN "_c*.so")
endif()

add_custom_target(defw_python_runtime ALL
	COMMAND
		"${CMAKE_COMMAND}" -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/python"
		"${DEFW_BUILD_PYTHON_DIR}"
	COMMENT "Copying DEFw Python runtime")

set(DEFW_BUILD_PYTHON_LIB_DIR "${Python3_LIBRARY_DIRS}")
configure_file(
	src/defwp-wrapper.in
	"${DEFW_BUILD_SRC_DIR}/defwp-wrapper"
	@ONLY)
file(CHMOD "${DEFW_BUILD_SRC_DIR}/defwp-wrapper"
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_EXECUTE
		WORLD_READ WORLD_EXECUTE)

if(BUILD_TESTING)
add_executable(defw_phase1_transport_contracts
	tests/transport/test_phase1_transport_contracts.c)
target_include_directories(defw_phase1_transport_contracts
	PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(defw_phase1_transport_contracts
	PRIVATE
		-Wall
		-fPIC)
target_link_libraries(defw_phase1_transport_contracts
	PRIVATE
		defw
		defw_agent
		defw_connect
		defw_global
		Threads::Threads
		"${UUID_LIBRARY}"
		Python3::Python)
set_target_properties(defw_phase1_transport_contracts
	PROPERTIES
		BUILD_RPATH "${DEFW_BUILD_RPATH}")

add_test(NAME defw_swig_imports
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"PYTHONPATH=${DEFW_BUILD_SRC_DIR}"
		"LD_LIBRARY_PATH=${DEFW_BUILD_SRC_DIR}"
		"${Python3_EXECUTABLE}" -c
		"import cdefw_global, cdefw_agent; print('swig imports ok')")
set_tests_properties(defw_swig_imports
	PROPERTIES
		DEPENDS "_cdefw_global;_cdefw_agent;_defw_typemap_fixture")

add_test(NAME defw_python_direct_import
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_BUILD_RUNTIME_DIR}"
		"PYTHONPATH=${DEFW_BUILD_SRC_DIR}:${DEFW_BUILD_PYTHON_DIR}/infra"
		"${Python3_EXECUTABLE}" -c
		"import defw; print('build direct defw import ok')")
set_tests_properties(defw_python_direct_import
	PROPERTIES
		DEPENDS "_cdefw_global;_cdefw_agent;defw_python_runtime")

add_test(NAME defw_swig_typemap_contracts
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"PYTHONPATH=${DEFW_BUILD_SRC_DIR}"
		"LD_LIBRARY_PATH=${DEFW_BUILD_SRC_DIR}"
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/swig_typemaps/test_typemap_contracts.py")
set_tests_properties(defw_swig_typemap_contracts
	PROPERTIES
		DEPENDS "_defw_typemap_fixture")

add_test(NAME defw_peer_event_contract
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_BUILD_RUNTIME_DIR}"
		"PYTHONPATH=${DEFW_BUILD_SRC_DIR}:${DEFW_BUILD_PYTHON_DIR}/infra"
		"LD_LIBRARY_PATH=${DEFW_BUILD_SRC_DIR}"
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/peer_events/test_peer_event_contract.py")
set_tests_properties(defw_peer_event_contract
	PROPERTIES
		DEPENDS "_cdefw_global;_cdefw_agent;defw_python_runtime")

add_test(NAME defw_directory_contract
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"PYTHONPATH=${DEFW_BUILD_SRC_DIR}:${DEFW_BUILD_PYTHON_DIR}/infra"
		"LD_LIBRARY_PATH=${DEFW_BUILD_SRC_DIR}"
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/directory/test_directory_contract.py")
set_tests_properties(defw_directory_contract
	PROPERTIES
		DEPENDS "_cdefw_global;defw_python_runtime")

add_test(NAME defw_binding_connect_contract
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_BUILD_RUNTIME_DIR}"
		"PYTHONPATH=${DEFW_BUILD_SRC_DIR}:${DEFW_BUILD_PYTHON_DIR}/infra"
		"LD_LIBRARY_PATH=${DEFW_BUILD_SRC_DIR}"
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/directory/test_binding_connect_contract.py")
set_tests_properties(defw_binding_connect_contract
	PROPERTIES
		DEPENDS "_cdefw_global;_cdefw_agent;defw_python_runtime")

add_test(NAME defw_phase1_transport_contracts
	COMMAND defw_phase1_transport_contracts)

add_test(NAME defw_external_swig_fixture
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"PYTHONPATH=${DEFW_EXTERNAL_SWIG_FIXTURE_DIR}"
		"LD_LIBRARY_PATH=${DEFW_BUILD_SRC_DIR}"
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/external_swig/test_external_swig.py")
set_tests_properties(defw_external_swig_fixture
	PROPERTIES
		DEPENDS "defw_external_swig_fixture_module")

add_test(NAME defw_executor_cmd
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_BUILD_RUNTIME_DIR}"
		"DEFW_CONFIG_PATH=${DEFW_BUILD_PYTHON_DIR}/config/defw_generic.yaml"
		"DEFW_AGENT_NAME=cmake-executor"
		"DEFW_AGENT_TYPE=agent"
		"DEFW_SHELL_TYPE=cmdline"
		"DEFW_LISTEN_PORT=0"
		"DEFW_TELNET_PORT=0"
		"DEFW_PARENT_NAME=None"
		"DEFW_PARENT_HOSTNAME=None"
		"DEFW_PARENT_ADDR=0.0.0.0"
		"DEFW_PARENT_PORT=0"
		"DEFW_DISABLE_DIRSVC=yes"
		"DEFW_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/defw-executor-log"
		"DEFW_LOG_LEVEL=error"
		"DEFW_EXTERNAL_SERVICES_PATH="
		"DEFW_EXTERNAL_SERVICE_APIS_PATH="
		"DEFW_EXTERNAL_EXPERIMENTS_PATH="
		"DEFW_EXPECTED_AGENT_COUNT=0"
		"${DEFW_BUILD_SRC_DIR}/defwp" -c
		"import cdefw_global; print(cdefw_global.get_defw_path())")
set_tests_properties(defw_executor_cmd
	PROPERTIES
		DEPENDS "_cdefw_global;_cdefw_agent;defw_python_runtime")

add_test(NAME defw_test_runner_dry_run
	COMMAND
		"${Python3_EXECUTABLE}"
		"${DEFW_BUILD_PYTHON_DIR}/tests/defw_test_runner.py"
		smoke
		--dry-run)
set_tests_properties(defw_test_runner_dry_run
	PROPERTIES
		DEPENDS "defw_python_runtime")

add_test(NAME defw_test_runner_smoke
	COMMAND
		"${CMAKE_COMMAND}"
		-DDEFW_RUNNER=${DEFW_BUILD_PYTHON_DIR}/tests/defw_test_runner.py
		-DDEFW_PYTHON=${Python3_EXECUTABLE}
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunDefwRunnerSmoke.cmake")
set_tests_properties(defw_test_runner_smoke
	PROPERTIES
		DEPENDS "defw_python_runtime;defwp"
		SKIP_REGULAR_EXPRESSION "DEFw socket smoke skipped")

add_test(NAME defw_test_runner_detects_failure
	COMMAND
		"${CMAKE_COMMAND}"
		-DDEFW_RUNNER=${DEFW_BUILD_PYTHON_DIR}/tests/defw_test_runner.py
		-DDEFW_PYTHON=${Python3_EXECUTABLE}
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunDefwRunnerFailureSmoke.cmake")
set_tests_properties(defw_test_runner_detects_failure
	PROPERTIES
		DEPENDS "defw_python_runtime;defwp")

add_test(NAME defw_install_tree_smoke
	COMMAND
		"${CMAKE_COMMAND}"
		-DDEFW_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
		-DDEFW_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/test-install
		-DDEFW_PYTHON=${Python3_EXECUTABLE}
		-DDEFW_PYTHON_INSTALL_DIR=${DEFW_PYTHON_INSTALL_DIR}
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunInstallTreeSmoke.cmake")
endif()

install(TARGETS
		defw
		defw_global
		defw_connect
		defw_agent
	EXPORT DEFwTargets
	LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

install(TARGETS defwp
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS "${DEFW_BUILD_SRC_DIR}/defwp-wrapper"
	DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(FILES ${DEFW_PUBLIC_HEADERS}
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/defw")
install(DIRECTORY python/
	DESTINATION python
	PATTERN "config" EXCLUDE
	PATTERN "__pycache__" EXCLUDE)
install(FILES python/config/defw_generic.yaml
	DESTINATION "${CMAKE_INSTALL_DATADIR}/defw/config")
install(DIRECTORY python/infra/
	DESTINATION "${DEFW_PYTHON_INSTALL_DIR}"
	FILES_MATCHING PATTERN "*.py")
install(DIRECTORY swig/typemaps/
	DESTINATION "${CMAKE_INSTALL_DATADIR}/defw/swig/typemaps"
	FILES_MATCHING PATTERN "*.i")

install(EXPORT DEFwTargets
	FILE DEFwTargets.cmake
	NAMESPACE DEFw::
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DEFw")

configure_package_config_file(
	cmake/DEFwConfig.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/DEFwConfig.cmake"
	INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DEFw")
write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/DEFwConfigVersion.cmake"
	VERSION "${PROJECT_VERSION}"
	COMPATIBILITY SameMajorVersion)

install(FILES
	"${CMAKE_CURRENT_BINARY_DIR}/DEFwConfig.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/DEFwConfigVersion.cmake"
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DEFw")
