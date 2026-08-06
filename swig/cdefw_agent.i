%module cdefw_agent

%{
#include <netinet/in.h>
#include "defw_agent.h"
%}

%include "owned_string.i"

%apply char **DEFW_OWNED_STRING { char **remote_uuid };
%apply char **DEFW_OWNED_STRING { char **blk_uuid };

%include "defw_agent.h"

%clear char **remote_uuid;
%clear char **blk_uuid;
