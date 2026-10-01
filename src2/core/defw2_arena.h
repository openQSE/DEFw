/*
 * A bag of allocations freed together. Shared inside libdefw2 and not
 * installed.
 *
 * Anything that hands a caller a structure full of strings needs a way to
 * free all of them in one call. A directory result does, and so does a typed
 * answer: the client copies what came back into an arena the answer owns,
 * and a service builds its answer in the call's arena, which the provider
 * frees once the reply is on the wire.
 *
 * It is deliberately not a bump allocator. The sizes are small and
 * irregular, and one pointer per allocation is cheaper to get right than
 * arithmetic on a block.
 */
#ifndef DEFW2_ARENA_H
#define DEFW2_ARENA_H

#include <stddef.h>

struct defw2_arena {
	void	**blocks;
	size_t	count;
	size_t	cap;
};

/*
 * Zeroed storage of the given size, owned by the arena, or NULL when there
 * is no memory. A zero-size request still returns a distinct pointer, so a
 * caller can tell "an empty list" from "no list" without a separate flag.
 */
void *defw2_arena_alloc(struct defw2_arena *arena, size_t size);

/*
 * Copy s into the arena and return the copy, or NULL when s is NULL so that
 * an absent field stays absent. Returns NULL on allocation failure too, so a
 * caller that needs to tell the two apart checks s first.
 */
char *defw2_arena_strdup(struct defw2_arena *arena, const char *s);

/* The first len bytes of s, terminated. s need not be. */
char *defw2_arena_strndup(struct defw2_arena *arena, const char *s,
			  size_t len);

/* Free everything and leave the arena empty and reusable. */
void defw2_arena_free(struct defw2_arena *arena);

#endif /* DEFW2_ARENA_H */
