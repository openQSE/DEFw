%module defw_typemap_fixture

%{
#include "defw_typemap_fixture.h"
%}

%include "compat_charpp.i"
%include "compat_charppp.i"
%include "owned_string.i"
%include "owned_string_list_counted.i"
%include "opaque_handle.i"

typedef unsigned long size_t;

%apply char **DEFW_OWNED_STRING { char **owned_string };
%apply char **DEFW_OWNED_STRING { char **missing_string };
%apply (char ***DEFW_OWNED_STRING_LIST, size_t *DEFW_OWNED_STRING_LIST_COUNT) {
	(char ***items, size_t *count)
};

typedef struct defw_typemap_handle defw_typemap_handle_t;
DEFW_OPAQUE_HANDLE(defw_typemap_handle_t)

int defw_typemap_make_compat_string(char **compat_string);
int defw_typemap_make_compat_pointer(char ***compat_pointer);
int defw_typemap_make_owned_string(char **owned_string);
int defw_typemap_make_null_owned_string(char **missing_string);
int defw_typemap_make_owned_string_list(char ***items, size_t *count);
defw_typemap_handle_t *defw_typemap_get_handle(void);

%clear char **owned_string;
%clear char **missing_string;
%clear (char ***items, size_t *count);
