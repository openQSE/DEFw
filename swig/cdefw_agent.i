%module cdefw_agent

%{
#include <netinet/in.h>
#include "defw_agent.h"
%}

%include "stdint.i"
%include "owned_string.i"
%include "rma_buffer.i"

%apply char **DEFW_OWNED_STRING { char **remote_uuid };
%apply char **DEFW_OWNED_STRING { char **blk_uuid };
%apply char **DEFW_OWNED_STRING { char **rma_desc };

%include "defw_agent.h"

%clear char **remote_uuid;
%clear char **blk_uuid;
%clear char **rma_desc;
