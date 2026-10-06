/*
 * Does a call register the identifier it goes out as, once, before Margo
 * would?
 *
 * Margo puts the provider into an RPC's identifier when it forwards a
 * call. If the process has not registered that identifier, Margo registers
 * it there, on the first forward, with no lock between its check and its
 * registration. Threads making that first call at once can all register
 * it, and Mercury then frees a registration that a handle still uses.
 * libdefw2 registers the identifier itself, under its own lock, when it
 * looks the RPC up.
 *
 * The window is a few instructions wide, so an idle machine almost never
 * hits it. This test holds it open. It replaces HG_Registered, and while
 * threads make their first calls it sleeps 2 ms whenever the answer is
 * "not registered", so every thread that checks then finds the identifier
 * missing. It replaces HG_Register too, and counts the registrations, and
 * those of an identifier the class already had. Both call Mercury's own.
 * They come ahead of Mercury's because the test exports its symbols.
 *
 * One process: a server runtime, and client runtimes made fresh for each
 * round, all over na+sm, so it needs no network and no port.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <defw2/defw2_doc.h>
#include <defw2/defw2_echo.h>

#include "defw2_internal.h"
#include "defw2_wire.h"

#define THREADS		8
#define ROUNDS		5
#define HOLD_US		2000
#define BULK_BYTES	(64 * 1024)
/* The RPCs each round calls first: echo, the bulk echo and a document. */
#define FIRST_CALLS	3
#define PROVIDER_DOC	11
#define API_DOC		"qfw.test"
/* A provider nothing serves, which only lookups use. */
#define PROVIDER_SPARE	9

typedef hg_return_t (*registered_fn)(hg_class_t *, hg_id_t, hg_bool_t *);
typedef hg_return_t (*register_fn)(hg_class_t *, hg_id_t, hg_proc_cb_t,
				   hg_proc_cb_t, hg_rpc_cb_t);

static registered_fn real_registered;
static register_fn real_register;

static atomic_bool holding;
static atomic_int held;		/* "not registered" answers held open */
static atomic_int registrations;
static atomic_int duplicates;	/* of an identifier the class had */

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-60s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

__attribute__((constructor)) static void find_mercury(void)
{
	*(void **)&real_registered = dlsym(RTLD_NEXT, "HG_Registered");
	*(void **)&real_register = dlsym(RTLD_NEXT, "HG_Register");
}

hg_return_t HG_Registered(hg_class_t *cls, hg_id_t id, hg_bool_t *flag)
{
	hg_return_t ret = real_registered(cls, id, flag);

	if (ret == HG_SUCCESS && !*flag && atomic_load(&holding)) {
		atomic_fetch_add(&held, 1);
		usleep(HOLD_US);
	}
	return ret;
}

hg_return_t HG_Register(hg_class_t *cls, hg_id_t id, hg_proc_cb_t in_cb,
			hg_proc_cb_t out_cb, hg_rpc_cb_t rpc_cb)
{
	hg_bool_t had = HG_FALSE;

	if (real_registered(cls, id, &had) == HG_SUCCESS && had)
		atomic_fetch_add(&duplicates, 1);
	atomic_fetch_add(&registrations, 1);
	return real_register(cls, id, in_cb, out_cb, rpc_cb);
}

static defw2_rt_t *runtime(defw2_role_t role)
{
	defw2_config_t cfg;
	defw2_rt_t *rt = NULL;

	defw2_config_from_env(&cfg);
	cfg.role = role;
	cfg.dirsvc = NULL;
	/* A full cache warns on every call, which here is the point. */
	cfg.log_level = DEFW2_LOG_ERROR;
	if (defw2_init(&cfg, &rt) != DEFW2_OK) {
		printf("runtime init FAILED\n");
		exit(1);
	}
	return rt;
}

static hg_id_t echo_lookup(defw2_rt_t *rt, uint16_t provider_id)
{
	return defw2_rpc_lookup(rt, DEFW2_RPC_ECHO, provider_id,
				hg_proc_defw2_echo_in_t,
				hg_proc_defw2_echo_out_t);
}

static bool registered(defw2_rt_t *rt, uint16_t provider_id, hg_id_t *id)
{
	hg_bool_t flag = HG_FALSE;

	return margo_provider_registered_name(rt->mid, DEFW2_RPC_ECHO,
					      provider_id, id, &flag) ==
		       HG_SUCCESS && flag;
}

static void lookups(defw2_rt_t *server)
{
	defw2_rt_t *client = runtime(DEFW2_ROLE_CLIENT);
	hg_id_t id, spare, found = 0;
	char name[64];
	int before, cached, i;

	before = atomic_load(&registrations);
	id = echo_lookup(client, DEFW2_PROVIDER_ECHO);
	check("a lookup registers the identifier the call goes out as",
	      id != 0 && registered(client, DEFW2_PROVIDER_ECHO, &found) &&
		      found == id);
	check("and nothing else",
	      atomic_load(&registrations) - before == 1);

	cached = client->rpc_cached;
	check("a second lookup is answered from the cache",
	      echo_lookup(client, DEFW2_PROVIDER_ECHO) == id &&
		      client->rpc_cached == cached &&
		      atomic_load(&registrations) - before == 1);

	spare = echo_lookup(client, PROVIDER_SPARE);
	check("another provider gets an identifier of its own",
	      spare != 0 && spare != id &&
		      registered(client, PROVIDER_SPARE, &found) &&
		      found == spare && client->rpc_cached == cached + 1);

	/* Fill the cache, and a lookup it has no room for still works. */
	for (i = client->rpc_cached; i < DEFW2_RPC_CACHE_MAX; i++) {
		snprintf(name, sizeof(name), "defw2.test.fill%d", i);
		defw2_rpc_lookup(client, name, 0, NULL, NULL);
	}
	before = atomic_load(&registrations);
	id = echo_lookup(client, PROVIDER_SPARE + 1);
	check("with the cache full, a lookup still registers",
	      client->rpc_cached == DEFW2_RPC_CACHE_MAX && id != 0 &&
		      registered(client, PROVIDER_SPARE + 1, &found) &&
		      found == id &&
		      atomic_load(&registrations) - before == 1);
	check("and asks Margo again rather than registering twice",
	      echo_lookup(client, PROVIDER_SPARE + 1) == id &&
		      atomic_load(&registrations) - before == 1);
	defw2_finalize(client);

	/*
	 * Where the service is, its registration carries the handler, and a
	 * lookup that registered again as a caller would take it away. The
	 * rounds below call this service, so they would fail too.
	 */
	before = atomic_load(&registrations);
	id = echo_lookup(server, DEFW2_PROVIDER_ECHO);
	check("where the service is, a lookup reuses its registration",
	      id != 0 && registered(server, DEFW2_PROVIDER_ECHO, &found) &&
		      found == id && atomic_load(&registrations) == before);
}

static defw2_rc_t answer(void *arg, defw2_call_t *call, const char *method,
			 const char *request, const char **out)
{
	(void)arg;
	(void)method;
	(void)request;
	*out = defw2_call_strdup(call, "\"pong\"");
	return *out != NULL ? DEFW2_OK : DEFW2_ERR_NOMEM;
}

struct round {
	defw2_binding_t		*echo;
	defw2_binding_t		*doc;
	pthread_barrier_t	start;
	atomic_int		answered;
};

/* Each thread's first call to each RPC, all threads at once. */
static void *first_calls(void *arg)
{
	struct round *r = arg;
	defw2_call_opts_t opts = { .timeout_ms = 10000 };
	defw2_status_t status = { 0 };
	defw2_buffer_t reply = { 0 };
	unsigned char *source = malloc(BULK_BYTES);
	unsigned char *sink = calloc(1, BULK_BYTES);
	char *text = NULL;
	int ok = 0;

	pthread_barrier_wait(&r->start);
	if (defw2_echo(r->echo, "ping", 4, &opts, &reply, &status) ==
		    DEFW2_OK &&
	    reply.len == 4 && memcmp(reply.data, "ping", 4) == 0)
		ok++;
	defw2_buffer_free(&reply);

	pthread_barrier_wait(&r->start);
	if (defw2_doc_call(r->doc, API_DOC, "ping", NULL, &opts, &text,
			   &status) == DEFW2_OK &&
	    status.code == DEFW2_OK && text != NULL &&
	    strcmp(text, "\"pong\"") == 0)
		ok++;
	free(text);

	pthread_barrier_wait(&r->start);
	if (source != NULL && sink != NULL) {
		memset(source, 0x5a, BULK_BYTES);
		if (defw2_echo_bulk(r->echo, source, sink, BULK_BYTES, &opts,
				    NULL, &status) == DEFW2_OK &&
		    memcmp(source, sink, BULK_BYTES) == 0)
			ok++;
	}
	defw2_status_free(&status);
	free(source);
	free(sink);
	atomic_fetch_add(&r->answered, ok);
	return NULL;
}

static void rounds(defw2_rt_t *server)
{
	pthread_t thread[THREADS];
	int answered = 0, extra = 0;
	int round, i, before;

	for (round = 0; round < ROUNDS; round++) {
		defw2_rt_t *client = runtime(DEFW2_ROLE_CLIENT);
		struct round r;

		memset(&r, 0, sizeof(r));
		if (defw2_binding_create(client, defw2_address(server),
					 DEFW2_PROVIDER_ECHO, &r.echo) !=
			    DEFW2_OK ||
		    defw2_binding_create(client, defw2_address(server),
					 PROVIDER_DOC, &r.doc) != DEFW2_OK) {
			printf("binding FAILED\n");
			exit(1);
		}
		pthread_barrier_init(&r.start, NULL, THREADS);

		before = atomic_load(&registrations);
		atomic_store(&holding, true);
		for (i = 0; i < THREADS; i++)
			pthread_create(&thread[i], NULL, first_calls, &r);
		for (i = 0; i < THREADS; i++)
			pthread_join(thread[i], NULL);
		atomic_store(&holding, false);
		if (atomic_load(&registrations) - before != FIRST_CALLS)
			extra++;
		answered += atomic_load(&r.answered);

		pthread_barrier_destroy(&r.start);
		defw2_binding_free(r.echo);
		defw2_binding_free(r.doc);
		defw2_finalize(client);
	}
	check("eight threads' first calls all answered",
	      answered == ROUNDS * THREADS * FIRST_CALLS);
	check("with the window held open",
	      atomic_load(&held) >= ROUNDS * FIRST_CALLS);
	check("each RPC registered once a round",
	      extra == 0);
	check("and no identifier registered twice",
	      atomic_load(&duplicates) == 0);
}

int main(void)
{
	defw2_doc_ops_t ops = { .call = answer };
	defw2_service_t *echo_svc = NULL, *doc_svc = NULL;
	defw2_rt_t *server;

	if (real_registered == NULL || real_register == NULL) {
		printf("cannot find Mercury's registration functions\n");
		return 1;
	}
	server = runtime(DEFW2_ROLE_SERVER);
	check("an echo service",
	      defw2_service_create(server, "echo-register", DEFW2_API_ECHO,
				   DEFW2_PROVIDER_ECHO, &echo_svc) ==
			      DEFW2_OK &&
		      defw2_echo_bind(echo_svc, NULL) == DEFW2_OK);
	check("and a document service",
	      defw2_service_create(server, "doc-register", API_DOC,
				   PROVIDER_DOC, &doc_svc) == DEFW2_OK &&
		      defw2_doc_bind(doc_svc, API_DOC, &ops) == DEFW2_OK);
	check("this test's HG_Register is the one Margo calls",
	      atomic_load(&registrations) > 0);

	lookups(server);
	rounds(server);

	defw2_service_destroy(doc_svc);
	defw2_service_destroy(echo_svc);
	defw2_finalize(server);
	printf("%s\n", failures ? "RPC REGISTER SMOKE FAILED"
				: "RPC REGISTER SMOKE PASSED");
	return failures ? 1 : 0;
}
