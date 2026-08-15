module use /sw/crusher/ums/ompix/DEVELOP/cce/13.0.0/modules/
module load DEFw/v0.1

export DEFW_LISTEN_PORT=8090
export DEFW_AGENT_NAME=dirsvc_$(hostname)
export DEFW_AGENT_TYPE=dirsvc
export DEFW_SHELL_TYPE=daemon
export DEFW_TELNET_PORT=8091
export DEFW_PARENT_HOSTNAME=$hostname
export DEFW_ONLY_LOAD_MODULE=svc_dirsvc
export DEFW_LOG_DIR=/tmp/${DEFW_AGENT_NAME}
export DEFW_ONLY_LOAD_MODULE=svc_dirsvc,svc_libfabric

# start the directory service
echo "Starting Directory Service"
defwp


