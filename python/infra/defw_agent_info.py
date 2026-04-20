from defw_exception import DEFwOutOfResources
from defw import me
from enum import IntFlag
import logging

def get_bit_list(bitstring, IntFlag_class):
	str_list = list(IntFlag_class.__members__.keys())
	set_list = []
	idx = 0
	for i in IntFlag_class.__members__.values():
		if i & bitstring:
			set_list.append(str_list[idx])
		idx += 1
	return set_list

def get_bit_desc(b1, b2):
	return f"{','.join(b1)} -> {','.join(b2)}"

# This is what an agent (either a service or a client) needs to return
# when queried about the services it offers:
# DEFwServiceInfo contains a list of ServiceDescr
# Each ServiceDescr has a list of Capability
# Each Capability describes a capacity it can handle
#
class Capability:
	def __init__(self, cap_type, cap_bitstr, cap_desc):
		self.__cap_type = cap_type
		self.__cap_bitstr = cap_bitstr
		self.__cap_desc = cap_desc

	def __repr__(self):
		return f"Capability({self.__cap_desc})"

	def get_capability_dict(self):
		return {'type': self.__cap_type,
			    'caps': self.__cap_bitstr,
				'description': self.__cap_desc}

	def get_capability(self):
		return self

	def get_cap_type(self):
		return self.__cap_type

	def get_caps(self):
		return self.__cap_bitstr

	def get_descr(self):
		return self.__cap_desc

class DEFwServiceInfo:
	def __init__(self, service_name, service_descr,
				 cname, mname, capabilities, max_capacity,
				 agent_descriptor=None):
		self.__service_name = service_name
		self.__service_descr = service_descr
		self.__capabilities = capabilities
		self.__agent_descriptor = agent_descriptor
		self.__max_capacity = max_capacity
		self.__cur_capacity = 0
		self.__module_name = mname
		self.__class_name = cname
		self.__my_ep = me.my_endpoint()
		self.__loc_db = None
		self.__key = 0

	def get_service_name(self):
		return self.__service_name

	def get_class_name(self):
		return self.__class_name

	def is_match(self, svc_name, svc_type, svc_caps):
		logging.defw_core(f"is_match {svc_name} <-> {self.__service_name}")
		if svc_name != self.__service_name:
			return False
		t = self.__capabilities.get_cap_type()
		c = self.__capabilities.get_caps()
		logging.defw_core(f"is_match {bin(t)} <-> {bin(svc_type)}")
		logging.defw_core(f"is_match {bin(c)} <-> {bin(svc_caps)}")
		if svc_type != -1:
			if not (svc_type & t) == svc_type:
				logging.defw_core("is_match didn't match svc_type")
				return False
		if svc_caps != -1:
			if not (svc_caps & c) == svc_caps:
				logging.defw_core("is_match didn't match svc_caps")
				return False
		return True

	def consume_capacity(self):
		if self.__cur_capacity == self.__max_capacity:
			err = f"Exceeded capacity on {self.__service_name}. " \
				  f"Current Capacity = {self.__cur_capacity}. " \
				  f"Maximum Capacity = {self.__max_capacity}"
			raise DEFwOutOfResources(err)
		self.__cur_capacity += 1

	def release_capacity(self):
		if self.__cur_capacity == 0:
			raise FIWOutOfResources(f"Release unreserved service {self.__service_name}")
		self.__cur_capacity -= 1

	def add_key(self, uuid_key):
		self.__key = uuid_key

	def get_key(self):
		return self.__key

	def add_loc_db(self, db_name):
		self.__loc_db = db_name

	def get_loc_db(self):
		return self.__loc_db

	def get_module_name(self):
		return self.__module_name

	def get_endpoint(self):
		return self.__my_ep

	def __repr__(self):
		return f"Service Info(name={self.__service_name}, " \
			   f"Residence={self.__my_ep}, caps={self.__capabilities}"

