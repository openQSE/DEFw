#!/bin/bash

# Initialize variables
param_NUM_NODES=""
param_NUM_PROCS=""
param_SCRIPT_P=""
param_SCRIPT=""

# Parse command-line arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -N)
            param_NUM_NODES="$2"
            shift 2
            ;;
        -n)
            param_NUM_PROCS="$2"
            shift 2
            ;;
        -s)
            param_SCRIPT="$2"
            shift 2
            ;;
        -p)
            param_SCRIPT_P="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 -N <num of nodes> -n <num of procs> -s <script> [-p <script_param>]"
            exit 1
            ;;
    esac
done

if [[ -z "$param_NUM_NODES" || -z "$param_NUM_PROCS" || -z "$param_SCRIPT" ]]; then
    echo "Usage: $0 -N <num of nodes> -n <num of procs> -s <script> [-p <script_param>]"
    exit 1
fi

echo "Starting test"

module load rocm/6.2.4
module use /sw/crusher/ums/ompix/DEVELOP/cce/13.0.0/modules/
module load DEFw/v0.1
module load libfabric/ompix-upstream-borg-r

export DEFW_LISTEN_PORT=8090
export DEFW_AGENT_NAME=dirsvc_$(hostname)
export DEFW_AGENT_TYPE=dirsvc
export DEFW_SHELL_TYPE=daemon
export DEFW_TELNET_PORT=8091
export DEFW_ONLY_LOAD_MODULE=svc_dirsvc
export DEFW_LOG_DIR=/tmp/${DEFW_AGENT_NAME}
export DEFW_ONLY_LOAD_MODULE=svc_dirsvc,svc_libfabric
export DEFW_EXPECTED_AGENT_COUNT=$param_NUM_PROCS
export DEFW_PARENT_HOSTNAME=$(hostname)

ml

# start the directory service
echo "Starting Directory Service"
defwp -d

set -xe
srun --unbuffered -N $param_NUM_NODES -n $param_NUM_NODES bash -c 'rm -Rf /tmp/libfabric_*'
srun --unbuffered -N $param_NUM_NODES -n $param_NUM_NODES bash -c 'rm -Rf /tmp/test_out_*'
srun --unbuffered -N $param_NUM_NODES -n $param_NUM_PROCS libfabric_agent.sh -b 8094 \
	-r ${DEFW_PARENT_HOSTNAME} \
	-p "$DEFW_PATH/python/experiments/suite_libfabric/$param_SCRIPT $param_SCRIPT_P"

pkill -9 -f 'defwp -d -x'
