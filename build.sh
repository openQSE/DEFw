#!/bin/bash

# Ensure environment is setup before running the build script.
# Requires: CMake, a C compiler, SWIG, Python, and libuuid.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${DEFW_BUILD_DIR:-${SCRIPT_DIR}/build}"
INSTALL_PREFIX=""
EXTERNAL_SWIG_CONFIG=""

usage() {
	echo "Usage: $0 [-b build-dir] [-i install-prefix] [-p external-swig-config]"
}

while getopts "b:i:p:h" opt; do
	case "${opt}" in
		b) BUILD_DIR="${OPTARG}" ;;
		i) INSTALL_PREFIX="${OPTARG}" ;;
		p) EXTERNAL_SWIG_CONFIG="${OPTARG}" ;;
		h) usage; exit 0 ;;
		*) usage; exit 1 ;;
	esac
done

cmake_args=(
	-S "${SCRIPT_DIR}"
	-B "${BUILD_DIR}"
)

if [[ -n "${INSTALL_PREFIX}" ]]; then
	cmake_args+=("-DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}")
fi
if [[ -n "${EXTERNAL_SWIG_CONFIG}" ]]; then
	cmake_args+=("-DDEFW_EXTERNAL_SWIG_CONFIG=${EXTERNAL_SWIG_CONFIG}")
fi

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}"
