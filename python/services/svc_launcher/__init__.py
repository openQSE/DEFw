from .svc_launcher import Launcher
import sys, os, logging
sys.path.append(os.path.split(os.path.abspath(__file__))[0])
import launcher_common as common

# This is used by the infrastructure to display information about
# the service module. The name is also used as a key through out the
# infrastructure. Without it the service module will not load.
svc_info = {'name': 'Launcher',
			'description': 'Process Launcher',
			'version': 1.0}

# This is used by the infrastructure to define all the service classes.
# Each class should be a separate service. Each class should implement the
# following methods:
#	query()
#	reserve()
#	release()
service_classes = [Launcher]

def initialize():
	logging.defw_service("Initializing the launcher module")
	pass

def uninitialize():
	logging.defw_service("Uninitializing the launcher module")
	common.shutdown = True
	pass
