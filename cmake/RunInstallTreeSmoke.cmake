if(NOT DEFW_BINARY_DIR)
	message(FATAL_ERROR "DEFW_BINARY_DIR is required")
endif()
if(NOT DEFW_INSTALL_PREFIX)
	message(FATAL_ERROR "DEFW_INSTALL_PREFIX is required")
endif()
if(NOT DEFW_PYTHON)
	message(FATAL_ERROR "DEFW_PYTHON is required")
endif()
if(NOT DEFW_PYTHON_INSTALL_DIR)
	message(FATAL_ERROR "DEFW_PYTHON_INSTALL_DIR is required")
endif()

file(REMOVE_RECURSE "${DEFW_INSTALL_PREFIX}")

execute_process(
	COMMAND "${CMAKE_COMMAND}" --install "${DEFW_BINARY_DIR}"
		--prefix "${DEFW_INSTALL_PREFIX}"
	RESULT_VARIABLE install_rc)
if(NOT install_rc EQUAL 0)
	message(FATAL_ERROR "DEFw install-tree smoke install failed")
endif()

set(smoke_dir "${DEFW_BINARY_DIR}/package-smoke")
file(REMOVE_RECURSE "${smoke_dir}")
file(MAKE_DIRECTORY "${smoke_dir}")
file(WRITE "${smoke_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.20)
project(defw_package_smoke C)
find_package(DEFw CONFIG REQUIRED)
add_executable(defw_package_smoke main.c)
target_link_libraries(defw_package_smoke PRIVATE DEFw::defw)
")
file(WRITE "${smoke_dir}/main.c"
"#include <defw_global.h>
int main(void)
{
	return get_defw_initialized();
}
")

execute_process(
	COMMAND "${CMAKE_COMMAND}" -S "${smoke_dir}" -B "${smoke_dir}/build"
		-DCMAKE_PREFIX_PATH=${DEFW_INSTALL_PREFIX}
	RESULT_VARIABLE configure_rc)
if(NOT configure_rc EQUAL 0)
	message(FATAL_ERROR "DEFw package smoke configure failed")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" --build "${smoke_dir}/build"
	RESULT_VARIABLE build_rc)
if(NOT build_rc EQUAL 0)
	message(FATAL_ERROR "DEFw package smoke build failed")
endif()

set(site_packages "${DEFW_INSTALL_PREFIX}/${DEFW_PYTHON_INSTALL_DIR}")
set(typemap_test
	"${CMAKE_CURRENT_LIST_DIR}/../tests/swig_typemaps/test_typemap_contracts.py")
set(wrapper_app_marker "${smoke_dir}/installed_wrapper_app.out")
set(wrapper_app_code
"marker = os.environ['DEFW_APP_SMOKE_OUTPUT']
with open(marker, 'w', encoding='utf-8') as stream:
    stream.write(me.my_name() + '\\n')
    stream.write(str('defw' in globals()) + '\\n')
")

execute_process(
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_INSTALL_PREFIX}"
		"DEFW_CONFIG_PATH=${DEFW_INSTALL_PREFIX}/python/config/defw_generic.yaml"
		"DEFW_AGENT_NAME=install-executor"
		"DEFW_AGENT_TYPE=agent"
		"DEFW_SHELL_TYPE=cmdline"
		"DEFW_LISTEN_PORT=0"
		"DEFW_TELNET_PORT=0"
		"DEFW_PARENT_NAME=None"
		"DEFW_PARENT_HOSTNAME=None"
		"DEFW_PARENT_ADDR=0.0.0.0"
		"DEFW_PARENT_PORT=0"
		"DEFW_DISABLE_RESMGR=yes"
		"DEFW_LOG_DIR=${DEFW_BINARY_DIR}/install-executor-log"
		"DEFW_LOG_LEVEL=error"
		"DEFW_EXTERNAL_SERVICES_PATH="
		"DEFW_EXTERNAL_SERVICE_APIS_PATH="
		"DEFW_EXTERNAL_EXPERIMENTS_PATH="
		"DEFW_EXPECTED_AGENT_COUNT=0"
		"PYTHONPATH=${site_packages}"
		"${DEFW_INSTALL_PREFIX}/bin/defwp" -c
		"import cdefw_global; print('install executor ok')"
	RESULT_VARIABLE executor_rc)
if(NOT executor_rc EQUAL 0)
	message(FATAL_ERROR "DEFw install executor smoke failed")
endif()

execute_process(
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_INSTALL_PREFIX}"
		"DEFW_CONFIG_PATH=${DEFW_INSTALL_PREFIX}/python/config/defw_generic.yaml"
		"DEFW_AGENT_NAME=install-wrapper"
		"DEFW_AGENT_TYPE=agent"
		"DEFW_SHELL_TYPE=cmdline"
		"DEFW_LISTEN_PORT=0"
		"DEFW_TELNET_PORT=0"
		"DEFW_PARENT_NAME=None"
		"DEFW_PARENT_HOSTNAME=None"
		"DEFW_PARENT_ADDR=0.0.0.0"
		"DEFW_PARENT_PORT=0"
		"DEFW_DISABLE_RESMGR=yes"
		"DEFW_LOG_DIR=${DEFW_BINARY_DIR}/install-wrapper-log"
		"DEFW_LOG_LEVEL=error"
		"DEFW_EXTERNAL_SERVICES_PATH="
		"DEFW_EXTERNAL_SERVICE_APIS_PATH="
		"DEFW_EXTERNAL_EXPERIMENTS_PATH="
		"DEFW_EXPECTED_AGENT_COUNT=0"
		"DEFW_APP_SMOKE_OUTPUT=${wrapper_app_marker}"
		"PYTHONPATH=${site_packages}"
		"${DEFW_INSTALL_PREFIX}/bin/defwp-wrapper" -c "${wrapper_app_code}"
	RESULT_VARIABLE wrapper_app_rc)
if(NOT wrapper_app_rc EQUAL 0)
	message(FATAL_ERROR "DEFw install wrapper application smoke failed")
endif()
if(NOT EXISTS "${wrapper_app_marker}")
	message(FATAL_ERROR "DEFw install wrapper application smoke did not run")
endif()
file(READ "${wrapper_app_marker}" wrapper_app_output)
string(FIND "${wrapper_app_output}" "install-wrapper" wrapper_agent_index)
if(wrapper_agent_index EQUAL -1)
	message(FATAL_ERROR "DEFw install wrapper application smoke used wrong runtime")
endif()
string(FIND "${wrapper_app_output}" "True" wrapper_defw_index)
if(wrapper_defw_index EQUAL -1)
	message(FATAL_ERROR "DEFw install wrapper application smoke missed DEFw globals")
endif()

execute_process(
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_INSTALL_PREFIX}"
		"PYTHONPATH=${site_packages}"
		"${DEFW_PYTHON}" -c
		"import defw; print('install direct defw import ok')"
	RESULT_VARIABLE import_rc)
if(NOT import_rc EQUAL 0)
	message(FATAL_ERROR "DEFw install Python import smoke failed")
endif()

execute_process(
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_INSTALL_PREFIX}"
		"DEFW_CONFIG_PATH=${DEFW_INSTALL_PREFIX}/python/config/defw_generic.yaml"
		"DEFW_AGENT_NAME=install-direct"
		"DEFW_AGENT_TYPE=agent"
		"DEFW_SHELL_TYPE=cmdline"
		"DEFW_LISTEN_PORT=0"
		"DEFW_TELNET_PORT=0"
		"DEFW_PARENT_NAME=None"
		"DEFW_PARENT_HOSTNAME=None"
		"DEFW_PARENT_ADDR=0.0.0.0"
		"DEFW_PARENT_PORT=0"
		"DEFW_DISABLE_RESMGR=yes"
		"DEFW_LOG_DIR=${DEFW_BINARY_DIR}/install-direct-log"
		"DEFW_LOG_LEVEL=error"
		"DEFW_EXTERNAL_SERVICES_PATH="
		"DEFW_EXTERNAL_SERVICE_APIS_PATH="
		"DEFW_EXTERNAL_EXPERIMENTS_PATH="
		"DEFW_EXPECTED_AGENT_COUNT=0"
		"PYTHONPATH=${site_packages}"
		"${DEFW_PYTHON}" -c
		"import defw; print('install configured defw import ok')"
	RESULT_VARIABLE configured_import_rc)
if(NOT configured_import_rc EQUAL 0)
	message(FATAL_ERROR "DEFw install configured Python import smoke failed")
endif()

execute_process(
	COMMAND
		"${CMAKE_COMMAND}" -E env
		"DEFW_PATH=${DEFW_INSTALL_PREFIX}"
		"PYTHONPATH=${site_packages}"
		"${DEFW_PYTHON}" "${typemap_test}"
	RESULT_VARIABLE typemap_rc)
if(NOT typemap_rc EQUAL 0)
	message(FATAL_ERROR "DEFw install typemap contract smoke failed")
endif()
