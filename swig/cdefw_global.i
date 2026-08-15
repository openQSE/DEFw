%module cdefw_global

%{
#include <netinet/in.h>
typedef unsigned int bool;
#define true 1
#define false 0
#include "defw_global.h"
%}

%include "owned_string.i"

%apply char **DEFW_OWNED_STRING { char **uuid };

%include "defw_global.h"

%clear char **uuid;
