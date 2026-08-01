%module cdefw_agent

%{
#include <netinet/in.h>
#include "defw_agent.h"
%}

%include "compat_charpp.i"
%include "compat_charppp.i"

%include "defw_agent.h"
