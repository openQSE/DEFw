import logging

from .svc_dirsvc import DEFwDirSvc


SERVICE_NAME = 'DEFwDirSvc'
SERVICE_DESC = 'DEFw Directory Service'

svc_info = {
	'name': 'Directory Service',
	'module': __name__,
	'description': SERVICE_DESC,
	'version': 1.0,
}

service_classes = [DEFwDirSvc]


def initialize():
	logging.defw_service("registering the DEFw Directory Service")


def uninitialize():
	logging.defw_service("unregistering the DEFw Directory Service")
