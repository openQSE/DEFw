from defw_agent import *

class ServiceDescriptor:
	def __init__(self, name, uuid, service_type, capabilities):
		self.name = name
		self.service_type = service_type
		self.caps = capabilities
		self.uuid = uuid

class Service:
	def __init__(self, agent, service_descriptor):
		self.agent_descriptor = agent_descriptor
		self.service_descriptor = service_descriptor

# Directory Service binding API takes a ServiceDescriptor object and
# returns endpoint metadata the client can use to construct a service API.
# The infrastructure takes care of all the object instantiation and
# handling of Agents, effectively abstracting away all communication
# information from the user.

class ServiceCollection:
class DataChannel(Endpoint):
class CntrlChannel(Endpoint):

class ExAgent():
	def __init__(self, ip, port,):
		self.__data_channel = DataChannel()
		
