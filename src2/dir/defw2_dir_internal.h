/*
 * Directory internals: the owned record, the string arena and the store.
 * Not installed.
 *
 * The public record in defw2_dir.h points at strings its creator owns. The
 * store cannot keep those, because the caller's request buffer is gone the
 * moment the handler answers, so it holds this parallel shape instead, where
 * every string is its own allocation and the record owns all of them.
 *
 * Going the other way, a resolve result hands a caller records it must be
 * able to free in one call. Each result therefore carries an arena holding
 * every allocation the decode made, and defw2_dir_result_free walks it. That
 * is the whole ownership story: records point into an arena, and the arena is
 * freed as a unit.
 */
#ifndef DEFW2_DIR_INTERNAL_H
#define DEFW2_DIR_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <defw2/defw2_dir.h>

struct defw2_rt;

/* The settings a store gets when the caller asks for the defaults. */
#define DEFW2_DIR_DEFAULT_TIMEOUT_MS	15000u
#define DEFW2_DIR_DEFAULT_SCAN_MS	2000u
#define DEFW2_DIR_DEFAULT_RETENTION_MS	300000ull

/*
 * A heartbeat interval a service picks for itself when it says nothing. A
 * third of the timeout, so losing one heartbeat to a scheduling hiccup is not
 * enough to be declared dead.
 */
#define DEFW2_DIR_DEFAULT_HEARTBEAT_MS	(DEFW2_DIR_DEFAULT_TIMEOUT_MS / 3u)

/* --- string arena ---------------------------------------------------- */

/*
 * A bag of allocations freed together. It is deliberately not a bump
 * allocator: the sizes here are small and irregular, and one pointer per
 * string is cheaper to get right than arithmetic on a block.
 */
typedef struct {
	void	**blocks;
	size_t	count;
	size_t	cap;
} defw2_dir_arena_t;

/*
 * Copy s into the arena and return the copy, or NULL when s is NULL so that
 * an absent field stays absent. Returns NULL on allocation failure too,
 * which every caller treats as fatal for the whole decode, so the arena is
 * freed as a unit and no partial record escapes.
 */
char *defw2_dir_arena_str(defw2_dir_arena_t *arena, const char *s);

/* Zeroed storage of the given size, owned by the arena. */
void *defw2_dir_arena_alloc(defw2_dir_arena_t *arena, size_t size);

void defw2_dir_arena_free(defw2_dir_arena_t *arena);

/* --- the owned record ------------------------------------------------ */

/*
 * The binding and property lists are the public structures, not parallel
 * ones with the const dropped. Two structs that differ only in a qualifier
 * are layout compatible but still distinct types, so casting an array of one
 * to the other to build a view would be an aliasing violation. Owning the
 * public shape means the view needs no cast at all, and the only place that
 * has to remember these strings came from strdup is the free below.
 */
typedef struct defw2_dir_record_own {
	char				*service_id;
	char				*service_type;
	char				*runtime_id;
	uint64_t			generation;
	defw2_dir_state_t		state;
	char				*address;
	char				*node_name;
	char				*hostname;
	int32_t				pid;
	defw2_dir_binding_t		*bindings;	/* owned strings */
	size_t				binding_count;
	char				*selector_name;
	char				**aliases;
	size_t				alias_count;
	char				**resources;
	size_t				resource_count;
	defw2_dir_property_t		*properties;	/* owned strings */
	size_t				property_count;
	uint64_t			registered_at_ns;
	uint64_t			last_heartbeat_ns;
	uint64_t			retention_deadline_ns;
	struct defw2_dir_record_own	*next;
} defw2_dir_record_own_t;

void defw2_dir_record_own_free(defw2_dir_record_own_t *record);

/*
 * A public view of an owned record. Nothing is copied: view borrows every
 * string and array from record, so it is valid only while record is. The
 * agent uses it to re-send the registration it is holding on to.
 */
void defw2_dir_record_own_view(const defw2_dir_record_own_t *record,
			       defw2_dir_record_t *view);

/*
 * Deep copy a public record into an owned one. Ignores the fields the
 * directory owns: state, the three timestamps and the generation, all of
 * which the store sets itself.
 */
defw2_rc_t defw2_dir_record_own_from(const defw2_dir_record_t *src,
				     defw2_dir_record_own_t **out);

/* --- the store ------------------------------------------------------- */

typedef struct defw2_dir_store defw2_dir_store_t;

defw2_rc_t defw2_dir_store_create(struct defw2_rt *rt,
				  const defw2_dir_store_opts_t *opts,
				  defw2_dir_store_t **store);
void defw2_dir_store_destroy(defw2_dir_store_t *store);

uint32_t defw2_dir_store_scan_interval_ms(const defw2_dir_store_t *store);

/*
 * Register. Assigns a generation for a service_id never seen, or one past the
 * highest this store has issued for it, and marks the record UP.
 *
 * Refuses, with DEFW2_CAT_INVALID_ARGUMENT, a record whose service_id is
 * already held by a different runtime that is still UP. That is the "rejects
 * a live conflicting runtime" rule: two processes claiming one service_id is
 * a deployment mistake, and silently letting the second win would make which
 * one serves depend on startup order.
 *
 * A record in any other state is replaced, because that is a restart, and a
 * restart is exactly what should succeed.
 */
defw2_rc_t defw2_dir_store_register(defw2_dir_store_t *store,
				    const defw2_dir_record_t *record,
				    uint64_t *generation,
				    defw2_status_t *status);

/*
 * Refresh last_heartbeat. Fails with DEFW2_CAT_NOT_FOUND when the service_id
 * is unknown, when the runtime_id is not the one registered, or when the
 * generation is not current. All three mean the same thing to the sender,
 * that its registration no longer stands, and the answer is to register
 * again rather than to keep beating.
 */
defw2_rc_t defw2_dir_store_heartbeat(defw2_dir_store_t *store,
				     const char *service_id,
				     const char *runtime_id,
				     uint64_t generation,
				     defw2_status_t *status);

defw2_rc_t defw2_dir_store_deregister(defw2_dir_store_t *store,
				      const char *service_id,
				      const char *runtime_id,
				      uint64_t generation,
				      defw2_status_t *status);

/*
 * Fill result with deep copies of the matching records. include_inactive in
 * the query is what separates the client view from the operator one.
 */
defw2_rc_t defw2_dir_store_resolve(defw2_dir_store_t *store,
				   const defw2_dir_query_t *query,
				   defw2_dir_result_t *result,
				   defw2_status_t *status);

/*
 * As above, and also reports which binding each record's query selected, as
 * an index into that record's bindings or DEFW2_DIR_NO_BINDING_INDEX.
 *
 * Only the service handler wants this: it has to put the selection on the
 * wire, and deriving it again on the client would be the selection rules
 * implemented twice. selected is malloc'd with entry_count entries and is
 * the caller's to free; pass NULL to not be told.
 */
#define DEFW2_DIR_NO_BINDING_INDEX	0xffffffffu

defw2_rc_t defw2_dir_store_resolve_indexed(defw2_dir_store_t *store,
					   const defw2_dir_query_t *query,
					   defw2_dir_result_t *result,
					   uint32_t **selected,
					   defw2_status_t *status);

defw2_rc_t defw2_dir_store_generation(defw2_dir_store_t *store,
				      const char *service_id,
				      uint64_t *generation,
				      defw2_status_t *status);

/*
 * One liveness pass. Marks every UP record whose last_heartbeat is older than
 * the timeout as TIMED_OUT and gives it a retention deadline, then drops
 * records whose deadline has passed. Returns how many records changed state,
 * which is what the caller logs and what a test asserts on.
 *
 * Safe to call from a Margo timer or from a test, as often as wanted.
 */
size_t defw2_dir_store_scan(defw2_dir_store_t *store);

#endif /* DEFW2_DIR_INTERNAL_H */
