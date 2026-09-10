#include "defw_typemap_fixture.h"

#include <stdlib.h>
#include <string.h>

struct defw_typemap_handle {
	int marker;
};

static struct defw_typemap_handle g_handle = {
	.marker = 17,
};

static char *copy_string(const char *text)
{
	char *copy;
	size_t len;

	len = strlen(text) + 1;
	copy = malloc(len);
	if (!copy)
		return NULL;

	memcpy(copy, text, len);
	return copy;
}

int defw_typemap_make_owned_string(char **owned_string)
{
	if (!owned_string)
		return -1;

	*owned_string = copy_string("owned-string");
	return *owned_string ? 0 : -1;
}

int defw_typemap_make_null_owned_string(char **missing_string)
{
	if (!missing_string)
		return -1;

	*missing_string = NULL;
	return 0;
}

int defw_typemap_make_owned_string_list(char ***items, size_t *count)
{
	static const char * const values[] = {
		"alpha",
		"beta",
		"gamma",
	};
	size_t idx;

	if (!items || !count)
		return -1;

	*count = sizeof(values) / sizeof(values[0]);
	*items = calloc(*count, sizeof(**items));
	if (!*items)
		return -1;

	for (idx = 0; idx < *count; idx++) {
		(*items)[idx] = copy_string(values[idx]);
		if (!(*items)[idx]) {
			while (idx > 0) {
				idx--;
				free((*items)[idx]);
			}
			free(*items);
			*items = NULL;
			*count = 0;
			return -1;
		}
	}

	return 0;
}

defw_typemap_handle_t *defw_typemap_get_handle(void)
{
	return &g_handle;
}
