#!/bin/bash

# Initialize variables
r_VALUE=""
p_VALUE=""
b_VALUE=""
lnx_cfg="shm+cxi:cxi0|shm+cxi:cxi1|shm+cxi:cxi2|shm+cxi:cxi3"

if [[ $# -ne 6 ]]; then
  echo "Error: Exactly three parameters (-b, -p and -r) are required."
  echo "Usage: $0 -b <base port> -r <directory service hostname> -p <path>"
  exit 1
fi


# Parse command-line arguments
while getopts "b:r:p:l:" opt; do
  case ${opt} in
    r ) r_VALUE=$OPTARG ;;
    p ) p_VALUE=$OPTARG ;;
    b ) b_VALUE=$OPTARG ;;
    l ) lnx_cfg=$OPTARG ;;
    * ) echo "Usage: $0 -r <parent hostname> -b <base port> -l <links> -p <script>"; exit 1 ;;
  esac
done

module use /sw/crusher/ums/ompix/DEVELOP/cce/13.0.0/modules/
module load DEFw/v0.1

BASE_PORT=$b_VALUE
NODE_ID=$SLURM_NODEID
PMI_RANK=${PMI_RANK:-0}
LISTEN_PORT=$((BASE_PORT + NODE_ID * 1000 + PMI_RANK * 2))
TELNET_PORT=$((LISTEN_PORT + 1))

export DEFW_AGENT_NAME=libfabric_$$
export DEFW_LISTEN_PORT=$LISTEN_PORT
export DEFW_TELNET_PORT=$TELNET_PORT
export DEFW_AGENT_TYPE=agent
export DEFW_SHELL_TYPE=daemon
export DEFW_LOG_DIR=/tmp/${DEFW_AGENT_NAME}
export DEFW_PARENT_HOSTNAME=$r_VALUE
export DEFW_PREF_PATH=/ccs/home/shehataa/tmp

export FI_LNX_PROV_LINKS=$lnx_cfg
export FI_LNX_DISABLE_SHM=0
export FI_CXI_RDZV_THRESHOLD=16384
export FI_CXI_RDZV_EAGER_SIZE=2048
export FI_CXI_OFLOW_BUF_SIZE=12582912
export FI_CXI_OFLOW_BUF_COUNT=3
export FI_CXI_DEFAULT_CQ_SIZE=131072
export FI_CXI_REQ_BUF_MAX_CACHED=0
export FI_CXI_REQ_BUF_MIN_POSTED=6
export FI_CXI_REQ_BUF_SIZE=12582912
export FI_CXI_RX_MATCH_MODE=software
export FI_MR_CACHE_MAX_SIZE=-1
export FI_MR_CACHE_MAX_COUNT=524288
export FI_LNX_SRQ_SUPPORT=1
export FI_SHM_USE_XPMEM=1

echo "PMI_RANK:          $PMI_RANK"
echo "agent name:        $DEFW_AGENT_NAME"
echo "agent listen port: $DEFW_LISTEN_PORT"
echo "agent telnet port: $DEFW_TELNET_PORT"

set -xe
#fi_info -p lnx
#env | grep -i vni
#/bin/rm -Rf /tmp/libfabric*
#python3 --version
#echo $LD_LIBRARY_PATH
python3 $p_VALUE 2>&1 | tee /tmp/test_out_$$
