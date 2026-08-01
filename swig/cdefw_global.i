%module cdefw_global

%{
#include <netinet/in.h>
typedef unsigned int bool;
#define true 1
#define false 0
#include "defw_global.h"
%}

%include "compat_charpp.i"
%include "compat_charppp.i"

%include "defw_global.h"

