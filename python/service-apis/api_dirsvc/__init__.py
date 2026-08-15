from .api_dirsvc import *


svc_info = {
	'name': 'Directory Service',
	'description': 'DEFw Framework Directory Service',
	'version': 1.0,
}

service_classes = [DEFwDirSvc]


def initialize():
	pass


def uninitialize():
	pass
