/*
 * The directory API, qfw.directory.
 *
 * The directory is the part of DEFw that Mercury does not replace. It answers
 * one question, "where is the service that can do this", and it is the only
 * service every other process must be able to find without asking anyone.
 *
 * A service registers, heartbeats while it lives, and deregisters. A client
 * resolves once, keeps the binding, and re-resolves when a call fails at the
 * transport. Nothing here tells a client which language implements a service.
 *
 *	defw2_dir_t *dir = NULL;
 *	defw2_dir_query_t query = {
 *		.service_type = "qfw.qpm",
 *		.binding_name = "execution",
 *	};
 *	defw2_dir_result_t result = { 0 };
 *
 *	defw2_dir_open(rt, defw2_dirsvc(rt), &dir);
 *	defw2_dir_resolve(dir, &query, NULL, &result, &status);
 *	// result.records[0].address, .binding.provider_id
 *	defw2_dir_result_free(&result);
 *	defw2_dir_close(dir);
 *
 * The record model is the one in QFw's detailed design, moved from Python and
 * SQLite into a C daemon with in-memory state. Nothing here changes that
 * model.
 *
 * The directory keeps no vocabulary of its own. It does not know what a QPM
 * is, what a qubit is, or what any property means; openQSE/DEFw #20 settled
 * that for v1 and v2 keeps it. Everything domain specific travels in the
 * properties map, and a caller that needs a property matched a particular way
 * says so in the query.
 */
#ifndef DEFW2_DIR_H
#define DEFW2_DIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <defw2/defw2_event.h>
#include <defw2/defw2_rpc.h>
#include <defw2/defw2_service.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFW2_API_DIR		"qfw.directory"

/*
 * The directory always serves on provider 0. Every other provider identifier
 * is assigned by whoever composes a process, but bootstrap needs one that is
 * known before anything has been resolved, and this is it.
 */
#define DEFW2_PROVIDER_DIR	0

/*
 * Record lifecycle. Wire visible, so values may be appended but never
 * renumbered.
 *
 * UP is the only state resolve returns. DOWN is a clean stop that kept its
 * record, TIMED_OUT is the liveness scan's verdict, and DEREGISTERED is a
 * service that said goodbye. The last three stay queryable until their
 * retention deadline passes, because the operator question "what happened to
 * it" outlives the record's usefulness to a client.
 */
typedef enum {
	DEFW2_DIR_STATE_UP		= 0,
	DEFW2_DIR_STATE_DOWN		= 1,
	DEFW2_DIR_STATE_TIMED_OUT	= 2,
	DEFW2_DIR_STATE_DEREGISTERED	= 3,
} defw2_dir_state_t;

const char *defw2_dir_state_name(defw2_dir_state_t state);

/*
 * One API a service serves, and the Margo provider that serves it.
 *
 * This is where the binding record changed shape from v1: there is no module
 * or class to import any more, so a binding names an API by identifier and
 * version and gives the provider identifier. A client resolves the binding,
 * takes the record's address, and forwards typed RPCs there.
 */
typedef struct {
	const char	*binding_name;	/* "execution" */
	const char	*api_id;	/* "qfw.qpm.execution" */
	uint32_t	api_version;
	uint16_t	provider_id;
} defw2_dir_binding_t;

/*
 * Where the process is. Diagnostic rather than routable: the address is what
 * a caller connects to, and this says which host and pid to look at when the
 * address stops answering.
 */
typedef struct {
	const char	*node_name;
	const char	*hostname;
	int32_t		pid;
} defw2_dir_endpoint_t;

/*
 * How a human or a job names this service. The directory matches on name and
 * on the aliases and resources lists, and attaches no meaning to any of them.
 */
typedef struct {
	const char		*name;
	const char *const	*aliases;
	size_t			alias_count;
	const char *const	*resources;
	size_t			resource_count;
} defw2_dir_selector_t;

/*
 * An opaque name and value the service chose. The directory stores and
 * returns these without defining either, so adding one needs no DEFw release.
 * Values are strings on the wire; a bitmask filter parses one as an unsigned
 * integer when it is asked to, and fails the match rather than the request if
 * it will not parse.
 */
typedef struct {
	const char	*name;
	const char	*value;
} defw2_dir_property_t;

/*
 * A directory record. Every string and array belongs to whoever created the
 * record: the caller when registering, and the result when resolving.
 *
 * generation distinguishes one run of a service from the next. A restarted
 * service gets a new runtime_id and a higher generation, which is what lets
 * the directory ignore a late heartbeat from the process it replaced.
 */
typedef struct {
	const char			*service_id;
	const char			*service_type;
	const char			*runtime_id;
	uint64_t			generation;
	defw2_dir_state_t		state;
	const char			*address;
	defw2_dir_endpoint_t		endpoint;
	const defw2_dir_binding_t	*bindings;
	size_t				binding_count;
	defw2_dir_selector_t		selector;
	const defw2_dir_property_t	*properties;
	size_t				property_count;
	/* Nanoseconds since the epoch, filled by the directory. */
	uint64_t			registered_at_ns;
	uint64_t			last_heartbeat_ns;
	uint64_t			retention_deadline_ns;	/* 0 when none */
} defw2_dir_record_t;

/*
 * How a property filter compares.
 *
 * EQUAL is the default and the only one a caller needs unless it is carrying
 * a bitmask. v1 hard-coded bitmask matching for qpm_type and qpm_capabilities,
 * which put QPM vocabulary inside the directory; naming the mode per property
 * moves that decision to the caller and leaves the directory generic. BITS_ALL
 * matches when every bit the caller asked for is set in the record's value,
 * which is what v1's __record_matches did.
 */
typedef enum {
	DEFW2_DIR_MATCH_EQUAL		= 0,
	DEFW2_DIR_MATCH_BITS_ALL	= 1,
	DEFW2_DIR_MATCH_BITS_ANY	= 2,
} defw2_dir_match_t;

typedef struct {
	const char		*name;
	const char		*value;
	defw2_dir_match_t	match;
} defw2_dir_filter_t;

/*
 * What to look for. A zero initialised query matches every UP record, and
 * every field that is set narrows it. selector_name and resource match the
 * record's selector, including its aliases.
 *
 * binding_name selects which of a record's bindings comes back in
 * defw2_dir_entry_t.binding. When it is NULL the first binding is selected,
 * which is the common case of a service that serves one API.
 *
 * api_version, when non-zero, requires the selected binding to declare it.
 */
typedef struct {
	const char			*service_id;
	const char			*service_type;
	const char			*selector_name;
	const char			*resource;
	const char			*binding_name;
	uint32_t			api_version;
	const defw2_dir_filter_t	*filters;
	size_t				filter_count;
	/*
	 * Include records that are not UP. Operators want this, clients never
	 * do, and it is what separates query_directory from resolve_services.
	 */
	bool				include_inactive;
	/* 0 for no limit. */
	size_t				limit;
} defw2_dir_query_t;

/*
 * One answer: the record, and the binding the query selected out of it. The
 * binding is a copy rather than a pointer into record.bindings so that a
 * caller can take the two fields it actually needs, the address and the
 * provider, without walking the list again.
 */
typedef struct {
	defw2_dir_record_t	record;
	defw2_dir_binding_t	binding;
} defw2_dir_entry_t;

/*
 * A resolve result. Everything in it, including every string the records
 * point at, is released by defw2_dir_result_free.
 */
typedef struct {
	defw2_dir_entry_t	*entries;
	size_t			entry_count;
	void			*arena;	/* internal; holds the strings */
} defw2_dir_result_t;

void defw2_dir_result_free(defw2_dir_result_t *result);

/* --- client side ---------------------------------------------------- */

/*
 * A handle on the directory. It owns a binding to DEFW2_PROVIDER_DIR at the
 * given address, so the address bootstrap has already happened by the time
 * one exists: defw2_config_from_env resolved DEFW2_DIRSVC, or composed the
 * address from v1's parent variables, or read the address file a provider
 * without IP addressing needs.
 *
 * A handle may be shared between threads.
 */
typedef struct defw2_dir defw2_dir_t;

defw2_rc_t defw2_dir_open(defw2_rt_t *rt, const char *address,
			  defw2_dir_t **dir);
void defw2_dir_close(defw2_dir_t *dir);

/*
 * Register. The directory validates the record, refuses a second live runtime
 * for the same service_id, assigns or increments the generation and marks the
 * record UP. The generation it assigned comes back in generation, and that is
 * the value every later heartbeat must carry.
 *
 * The record's state, timestamps and generation are the directory's to set
 * and are ignored on the way in.
 */
defw2_rc_t defw2_dir_register(defw2_dir_t *dir,
			      const defw2_dir_record_t *record,
			      const defw2_call_opts_t *opts,
			      uint64_t *generation, defw2_status_t *status);

/*
 * Refresh last_heartbeat. Carries the runtime_id and generation so that a
 * process which has already been replaced cannot keep its successor's record
 * alive. A heartbeat for an unknown or superseded runtime fails with
 * DEFW2_CAT_NOT_FOUND, which is the service's signal to register again.
 */
defw2_rc_t defw2_dir_heartbeat(defw2_dir_t *dir, const char *service_id,
			       const char *runtime_id, uint64_t generation,
			       const defw2_call_opts_t *opts,
			       defw2_status_t *status);

/* Mark DEREGISTERED, clear the address and start the retention clock. */
defw2_rc_t defw2_dir_deregister(defw2_dir_t *dir, const char *service_id,
				const char *runtime_id, uint64_t generation,
				const defw2_call_opts_t *opts,
				defw2_status_t *status);

/*
 * Resolve. Returns UP records only, each with the binding the query selected.
 * An empty result is success with entry_count 0, not an error: "nothing
 * serves that yet" is a normal answer a caller retries.
 */
defw2_rc_t defw2_dir_resolve(defw2_dir_t *dir, const defw2_dir_query_t *query,
			     const defw2_call_opts_t *opts,
			     defw2_dir_result_t *result,
			     defw2_status_t *status);

/*
 * Everything, including records that are not UP, until retention expires.
 * This is the operator's view and the one defw2-dirsvc's own dump uses.
 * Equivalent to resolve with include_inactive set, and named separately
 * because the design names it separately.
 */
defw2_rc_t defw2_dir_query(defw2_dir_t *dir, const defw2_dir_query_t *query,
			   const defw2_call_opts_t *opts,
			   defw2_dir_result_t *result,
			   defw2_status_t *status);

/*
 * The current generation for a service_id, whatever its state. A client that
 * holds a cached binding uses this to notice a restart without fetching the
 * whole record.
 */
defw2_rc_t defw2_dir_generation(defw2_dir_t *dir, const char *service_id,
				const defw2_call_opts_t *opts,
				uint64_t *generation, defw2_status_t *status);

/* --- the binding cache ---------------------------------------------- */

/*
 * Resolve once, call many times, and notice when that stops being valid.
 *
 * A typed call needs a defw2_binding_t, and resolving one every time would
 * put the directory on the hot path of every RPC in the system. The cache
 * keeps the binding a query resolved to and hands the same one back until
 * something says it is stale.
 *
 * What says so is the caller. Mercury reports failures per call, not per
 * peer, so nothing else can know: a client whose forward fails at the
 * transport calls defw2_dir_cache_failed and the next lookup re-resolves.
 * That is the loop the design describes, and it is the whole invalidation
 * story apart from the optional generation check below.
 *
 *	defw2_dir_cache_binding(cache, &query, &opts, &binding, &status);
 *	rc = defw2_echo(binding, ...);
 *	if (rc == DEFW2_ERR_TRANSPORT)
 *		defw2_dir_cache_failed(cache, binding);
 *
 * A cache may be shared between threads. The bindings it returns belong to
 * it and must not be freed by a caller; they stay valid until the cache is
 * destroyed or the entry is invalidated, so a caller that keeps one across an
 * invalidate should look it up again rather than hold it.
 */
typedef struct defw2_dir_cache defw2_dir_cache_t;

/*
 * The cache borrows dir, which must outlive it. One cache per directory
 * handle is the expected shape; there is nothing to gain from two.
 */
defw2_rc_t defw2_dir_cache_create(defw2_dir_t *dir,
				  defw2_dir_cache_t **cache);
void defw2_dir_cache_destroy(defw2_dir_cache_t *cache);

/*
 * The binding this query resolves to, from the cache when it is there and by
 * resolving when it is not. Queries are matched on every field, so passing
 * the same query gets the same entry.
 *
 * A query that resolves to nothing is DEFW2_ERR_NOT_FOUND with the status
 * carrying the directory's own answer, and nothing is cached: "not yet" is a
 * state a caller retries, and remembering it would mean never noticing when
 * the service arrives.
 */
defw2_rc_t defw2_dir_cache_binding(defw2_dir_cache_t *cache,
				   const defw2_dir_query_t *query,
				   const defw2_call_opts_t *opts,
				   defw2_binding_t **binding,
				   defw2_status_t *status);

/*
 * Report that a call through this binding failed at the transport, so the
 * next lookup resolves again. Safe with a binding the cache does not know,
 * which is what lets a caller report every failure without checking.
 */
void defw2_dir_cache_failed(defw2_dir_cache_t *cache,
			    const defw2_binding_t *binding);

/*
 * Drop everything. For a client that has reason to believe the whole
 * directory moved, such as one that just reconnected.
 */
void defw2_dir_cache_clear(defw2_dir_cache_t *cache);

/*
 * Ask the directory whether the services behind the cached bindings are still
 * the same generation, and invalidate the ones that are not.
 *
 * This is the cheap half of the design's liveness story: get_generation
 * carries no record, so a client can notice a restart it has not yet called
 * into for the cost of one small RPC per cached service. It is optional, and
 * a client that only calls defw2_dir_cache_failed is still correct, just one
 * failed call slower to notice.
 *
 * Returns the number of entries it invalidated.
 */
size_t defw2_dir_cache_revalidate(defw2_dir_cache_t *cache,
				  const defw2_call_opts_t *opts);

/* How many bindings are held, which is for tests and for an operator. */
size_t defw2_dir_cache_size(const defw2_dir_cache_t *cache);

/* --- events ---------------------------------------------------------- */

/*
 * A service coming and going, told to a sink as the directory records it.
 *
 * A caller subscribes a sink (defw2_event.h), and the directory sends it an
 * event each time a record the subscription matches becomes UP, by
 * registering or by its heartbeat resuming, and each time it stops being
 * UP, by deregistering or timing out. A subscriber gets them in the order
 * the directory recorded them. v1 called the two SERVICE_CONNECTED and
 * SERVICE_DISCONNECTED, and an event's type is one of those names.
 *
 *	defw2_dir_subscribe_req_t req = {
 *		.target = { defw2_event_sink_address(sink),
 *			    DEFW2_PROVIDER_EVENT, "qpm" },
 *		.service_id = "qpm:iqm:q20",
 *	};
 *
 *	defw2_dir_event_accept(sink);
 *	defw2_dir_subscribe(dir, &req, &opts, &id, &status);
 *
 * A subscription lasts until it is cancelled or a delivery to its sink
 * fails. It lives in the directory's memory, so a restarted directory has
 * none. Each event's source is the directory's runtime, so a subscriber
 * that sees a new one subscribes again.
 */
#define DEFW2_DIR_EVENT_SERVICE		"service"
#define DEFW2_DIR_SERVICE_CONNECTED	"SERVICE_CONNECTED"
#define DEFW2_DIR_SERVICE_DISCONNECTED	"SERVICE_DISCONNECTED"

/* Which changes a subscription hears about. 0 means both. */
#define DEFW2_DIR_CONNECTED		(1u << 0)
#define DEFW2_DIR_DISCONNECTED		(1u << 1)

/*
 * service_id and service_type each narrow a subscription to the records
 * that match exactly, and NULL matches any. target is the sink, and its tag
 * comes back on every event.
 */
typedef struct {
	defw2_event_target_t	target;
	const char		*service_id;
	const char		*service_type;
	uint32_t		changes;	/* DEFW2_DIR_*, 0 for both */
} defw2_dir_subscribe_req_t;

/*
 * What a directory event carries: which way the record went, why, and the
 * record as the directory now holds it, its new generation on the way up
 * and its final state on the way down. reason is "registered",
 * "heartbeat-resumed", "deregistered" or "heartbeat-timeout".
 */
typedef struct {
	bool			connected;
	const char		*reason;
	defw2_dir_record_t	record;
} defw2_dir_change_t;

/*
 * Subscribe, and learn the id the directory gave the subscription, which is
 * what unsubscribe takes. Fails with DEFW2_CAT_INVALID_ARGUMENT for a
 * target with no address or one on the directory's own provider, and with
 * DEFW2_CAT_PENDING_CAPACITY when the directory holds as many subscriptions
 * as it will.
 */
defw2_rc_t defw2_dir_subscribe(defw2_dir_t *dir,
			       const defw2_dir_subscribe_req_t *req,
			       const defw2_call_opts_t *opts,
			       uint64_t *subscription_id,
			       defw2_status_t *status);

/* End one. DEFW2_CAT_NOT_FOUND when the directory holds no such id. */
defw2_rc_t defw2_dir_unsubscribe(defw2_dir_t *dir, uint64_t subscription_id,
				 const defw2_call_opts_t *opts,
				 defw2_status_t *status);

/*
 * Which runtime the directory is, copied into runtime_id, a buffer of len
 * bytes, at least DEFW2_DIR_RUNTIME_ID_LEN. It is new each time the
 * directory starts, so a client that keeps subscriptions asks for it now
 * and then: a different one is a restarted directory, which holds none of
 * them. It is also every directory event's source.
 */
#define DEFW2_DIR_RUNTIME_ID_LEN	64

defw2_rc_t defw2_dir_runtime_id(defw2_dir_t *dir,
				const defw2_call_opts_t *opts,
				char *runtime_id, size_t len,
				defw2_status_t *status);

/*
 * Make a sink take directory events, and read one. The change belongs to
 * the event: free the event, never the change. NULL for any other event.
 */
defw2_rc_t defw2_dir_event_accept(defw2_event_sink_t *sink);
const defw2_dir_change_t *defw2_dir_event_change(const defw2_event_t *event);

/* --- service side --------------------------------------------------- */

/*
 * Register qfw.directory on a service, backed by an in-memory store.
 *
 * This is what defw2-dirsvc binds and what a test binds to get a directory in
 * process. The store is created with the service and destroyed with it.
 *
 * heartbeat_timeout_ms is how long a record may go unrefreshed before the
 * liveness scan marks it TIMED_OUT, and scan_interval_ms is how often that
 * scan runs. Zero for either takes the default. retention_ms is how long an
 * inactive record stays queryable.
 *
 * snapshot_path, when it is not NULL, is a JSON file rewritten on every
 * change. It exists for operators and for post-mortems, and is never read
 * back: a restarted directory is a new generation of everything, which is the
 * QFw design's position and the reason the store is in memory at all.
 */
typedef struct {
	uint32_t	heartbeat_timeout_ms;
	uint32_t	scan_interval_ms;
	uint64_t	retention_ms;
	const char	*snapshot_path;
} defw2_dir_store_opts_t;

defw2_rc_t defw2_dir_bind(defw2_service_t *svc,
			  const defw2_dir_store_opts_t *opts);

/* --- registering the process that serves --------------------------- */

/*
 * Keep a service registered for as long as it runs.
 *
 * This is the half of the directory a service author should never have to
 * write: it registers the record, starts a Margo timer that heartbeats at
 * interval_ms, and registers again whenever the directory says it does not
 * know this registration. On a clean stop it deregisters.
 *
 * Re-registering reuses this process's runtime_id rather than minting one.
 * The runtime_id identifies the process, and it is what lets the directory
 * refuse a heartbeat from a process that has already been replaced; a new one
 * per attempt would give that up for nothing. A new runtime_id comes from a
 * new process, which is exactly when the generation should move.
 *
 * The record's address, endpoint and runtime_id are filled from the runtime,
 * so a caller supplies the parts only it knows: service_id, service_type,
 * bindings, selector and properties.
 *
 * interval_ms of 0 takes the default, which is a third of the directory's
 * default timeout so that one lost heartbeat is not enough to be declared
 * dead.
 *
 * It registers before it returns. When that fails it returns why: the
 * transport's code when the registration did not reach a directory, such as
 * DEFW2_ERR_TIMEOUT, or DEFW2_ERR_NOT_FOUND from a process that serves no
 * directory, or the directory's own code when it refused, such as
 * DEFW2_ERR_BUSY for a service_id that a live runtime already holds.
 */
typedef struct defw2_dir_agent defw2_dir_agent_t;

defw2_rc_t defw2_dir_agent_start(defw2_service_t *svc, const char *dir_address,
				 const defw2_dir_record_t *record,
				 uint32_t interval_ms,
				 defw2_dir_agent_t **agent);

/*
 * Stop heartbeating, deregister and free the agent. Safe on NULL, so a
 * service with no directory needs no special case. It frees the agent, so a
 * caller calls it once and forgets the pointer.
 *
 * Call it BEFORE defw2_service_shutdown. Deregistering is an RPC, and
 * shutdown stops the Margo instance, so an agent stopped afterwards cannot
 * say goodbye: the record stays UP at an address nobody is serving until the
 * heartbeat timeout retires it, and a restart in the meantime is refused as a
 * conflict with a live runtime. A signal handler that stops the service
 * should stop the agent first.
 */
void defw2_dir_agent_stop(defw2_dir_agent_t *agent);

/* What the directory assigned. 0 before the first successful register. */
uint64_t defw2_dir_agent_generation(const defw2_dir_agent_t *agent);
const char *defw2_dir_agent_runtime_id(const defw2_dir_agent_t *agent);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_DIR_H */
