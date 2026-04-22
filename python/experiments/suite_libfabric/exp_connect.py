import yaml, sys, time, os
from defw_app_util import *
from defw import me
from util_data import *
from defw_exception import DEFwOperationFailure

def run():
	# connect to the resource manager
	rsmgr = defw_get_resource_mgr()
	logging.defw_app(f"{os.getpid()}: got resmgr {rsmgr}")

	# publish it to the resource manager
	rsmgr.register_agent(me.my_endpoint(), f"I'm {os.getpid()}")
	# Wait until all processes in the world has connected
	rsmgr.wait_agents()
	# get the addresses
	contexts = rsmgr.get_agents_context()

	rsmgr.deregister_agent(me.my_endpoint())

	rsmgr.wait_agents_deregistration()

	logging.defw_app(f"Agent Contexts: {contexts}")

if __name__ == '__main__':
	run()
