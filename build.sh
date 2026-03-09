#!/bin/bash

# Ensure environment is setup before running the build script
# Require: gcc, swig, rocm, python and libfabric if building libfabric
# tests

# Initialize variables
build_cfg=""

# script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd $SCRIPT_DIR

# Parse command-line arguments
while getopts "p:" opt; do
  case ${opt} in
    p ) build_cfg=$OPTARG ;;
    * ) echo "Usage: $0 -p <scons parameter>"; exit 1 ;;
  esac
done

ml

python3 defw_cleanup_build.py $build_cfg

scons CONFIG=$build_cfg
