/*
 * The arena. See defw2_arena.h.
 */
#include <stdlib.h>
#include <string.h>

#include "defw2_arena.h"

static int arena_push(struct defw2_arena *arena, void *block)
{
	if (arena->count == arena->cap) {
		size_t cap = arena->cap ? arena->cap * 2 : 16;
		void **blocks = realloc(arena->blocks, cap * sizeof(*blocks));

		if (blocks == NULL)
			return -1;
		arena->blocks = blocks;
		arena->cap = cap;
	}
	arena->blocks[arena->count++] = block;
	return 0;
}

void *defw2_arena_alloc(struct defw2_arena *arena, size_t size)
{
	void *block;

	if (arena == NULL)
		return NULL;
	block = calloc(1, size ? size : 1);
	if (block == NULL)
		return NULL;
	if (arena_push(arena, block) != 0) {
		free(block);
		return NULL;
	}
	return block;
}

char *defw2_arena_strndup(struct defw2_arena *arena, const char *s,
			  size_t len)
{
	char *copy;

	if (arena == NULL || s == NULL)
		return NULL;
	/* Zeroed, so the terminator is already there. */
	copy = defw2_arena_alloc(arena, len + 1);
	if (copy == NULL)
		return NULL;
	memcpy(copy, s, len);
	return copy;
}

char *defw2_arena_strdup(struct defw2_arena *arena, const char *s)
{
	if (s == NULL)
		return NULL;
	return defw2_arena_strndup(arena, s, strlen(s));
}

void defw2_arena_free(struct defw2_arena *arena)
{
	size_t i;

	if (arena == NULL)
		return;
	for (i = 0; i < arena->count; i++)
		free(arena->blocks[i]);
	free(arena->blocks);
	arena->blocks = NULL;
	arena->count = 0;
	arena->cap = 0;
}
