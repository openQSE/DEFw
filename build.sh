#!/bin/bash

# Configure and build DEFw.
# Requires: CMake, a C compiler, libuuid, and Margo and Argobots through
# pkg-config. The Python binding is built when Python has cffi.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${DEFW_BUILD_DIR:-${SCRIPT_DIR}/build}"
INSTALL_PREFIX=""

usage() {
	echo "Usage: $0 [-b build-dir] [-i install-prefix]"
}

while getopts "b:i:h" opt; do
	case "${opt}" in
		b) BUILD_DIR="${OPTARG}" ;;
		i) INSTALL_PREFIX="${OPTARG}" ;;
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

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}"
