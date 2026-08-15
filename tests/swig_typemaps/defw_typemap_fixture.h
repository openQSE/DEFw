#ifndef DEFW_TYPEMAP_FIXTURE_H
#define DEFW_TYPEMAP_FIXTURE_H

#include <stddef.h>

typedef struct defw_typemap_handle defw_typemap_handle_t;

int defw_typemap_make_owned_string(char **owned_string);
int defw_typemap_make_null_owned_string(char **missing_string);
int defw_typemap_make_owned_string_list(char ***items, size_t *count);
defw_typemap_handle_t *defw_typemap_get_handle(void);

#endif /* DEFW_TYPEMAP_FIXTURE_H */
