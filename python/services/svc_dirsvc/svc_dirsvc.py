from svc_resmgr.svc_resmgr import DEFwResMgr
from defw_agent_info import Capability, DEFwServiceInfo, get_bit_desc, \
			    get_bit_list
from api_resmgr import ResMgrType, ResMgrCapability


class DEFwDirSvc(DEFwResMgr):
	def query(self):
		t = get_bit_list(ResMgrType.RESMGR_TYPE_DEFW, ResMgrType)
		c = get_bit_list(ResMgrCapability.RESMGR_CAP_DEFW, ResMgrCapability)
		cap = Capability(ResMgrType.RESMGR_TYPE_DEFW,
				 ResMgrCapability.RESMGR_CAP_DEFW,
				 get_bit_desc(t, c))
		return DEFwServiceInfo('DEFwDirSvc', 'DEFw Directory Service',
				       self.__class__.__name__,
				       self.__class__.__module__,
				       cap, -1)
