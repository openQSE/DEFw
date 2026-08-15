import yaml, sys, time, os
from defw_app_util import *
from defw import me
from util_data import *
from defw_exception import DEFwOperationFailure

def run():
	# connect to the directory service
	dirsvc = defw_get_directory_service()
	logging.defw_app(f"{os.getpid()}: got dirsvc {dirsvc}")

	# publish it to the directory service
	dirsvc.register_agent(me.my_endpoint(), f"I'm {os.getpid()}")
	# Wait until all processes in the world has connected
	dirsvc.wait_agents()
	# get the addresses
	contexts = dirsvc.get_agents_context()

	dirsvc.deregister_agent(me.my_endpoint())

	dirsvc.wait_agents_deregistration()

	logging.defw_app(f"Agent Contexts: {contexts}")

if __name__ == '__main__':
	run()
