/*
 * The document tier: any method of any API as JSON, over the typed path.
 *
 * One server runtime with three providers, and a client runtime calling
 * them over na+sm:
 *
 *   provider 11 serves qfw.test.doc from a C operations table,
 *   provider 12 serves qfw.test.queued from a consumer thread, with echo on
 *     the same queue, which is how a consumer learns to tell a document
 *     from bytes,
 *   provider 13 binds qfw.test.none with neither, so it answers no
 *     documents.
 *
 * Given a directory, both runtimes profile into it, and the OTLP checker
 * then requires document spans on both sides, each naming its own method.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2_doc.h>
#include <defw2/defw2_echo.h>

#define API_DOC		"qfw.test.doc"
#define API_QUEUED	"qfw.test.queued"
#define API_NONE	"qfw.test.none"
#define PROVIDER_DOC	11
#define PROVIDER_QUEUED	12
#define PROVIDER_NONE	13

#define THREADS		4
#define CALLS		50

static int failures;
static long doc_calls;	/* what the operations table was asked */
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

static void check(const char *what, bool ok)
{
	printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

static long asked(void)
{
	long n;

	pthread_mutex_lock(&count_lock);
	n = doc_calls;
	pthread_mutex_unlock(&count_lock);
	return n;
}

/* A string of n bytes of c, terminated. */
static char *filled(size_t n, char c)
{
	char *s = malloc(n + 1);

	if (s != NULL) {
		memset(s, c, n);
		s[n] = '\0';
	}
	return s;
}

/* --- the C operations table ------------------------------------------ */

static defw2_rc_t answer_doc(void *arg, defw2_call_t *call,
			     const char *method, const char *request,
			     const char **answer)
{
	size_t len;
	char *text;

	(void)arg;
	pthread_mutex_lock(&count_lock);
	doc_calls++;
	pthread_mutex_unlock(&count_lock);

	if (strcmp(method, "echo") == 0) {
		len = strlen(request) + 64;
		text = defw2_call_alloc(call, len);
		if (text == NULL)
			return DEFW2_ERR_NOMEM;
		snprintf(text, len, "{\"method\": \"echo\", \"request\": %s}",
			 request);
		*answer = text;
		return DEFW2_OK;
	}
	if (strcmp(method, "length") == 0) {
		text = defw2_call_alloc(call, 64);
		if (text == NULL)
			return DEFW2_ERR_NOMEM;
		snprintf(text, 64, "{\"length\": %zu}", strlen(request));
		*answer = text;
		return DEFW2_OK;
	}
	if (strcmp(method, "nothing") == 0)
		return DEFW2_OK;
	if (strcmp(method, "refuse") == 0) {
		defw2_call_set_status(call, DEFW2_ERR_INVALID,
				      DEFW2_CAT_INVALID_ARGUMENT,
				      "the service refused");
		return DEFW2_ERR_INVALID;
	}
	if (strcmp(method, "biggest") == 0 || strcmp(method, "huge") == 0) {
		/* The largest answer a document carries, and one byte more. */
		len = strcmp(method, "huge") == 0 ? DEFW2_EAGER_MAX
						  : DEFW2_EAGER_MAX - 1;
		text = defw2_call_alloc(call, len + 1);
		if (text == NULL)
			return DEFW2_ERR_NOMEM;
		memset(text, 'x', len);
		text[0] = '"';
		text[len - 1] = '"';
		*answer = text;
		return DEFW2_OK;
	}
	defw2_call_set_status(call, DEFW2_ERR_NOT_FOUND, DEFW2_CAT_NOT_FOUND,
			      "no such method");
	return DEFW2_ERR_NOT_FOUND;
}

/* --- the consumer ----------------------------------------------------- */

static long bytes_told_apart;

static void *consume(void *arg)
{
	defw2_service_t *svc = arg;

	for (;;) {
		const char *document, *method;
		defw2_call_t *call = NULL;
		const void *request;
		char *text;
		size_t len;
		defw2_rc_t rc;

		rc = defw2_service_next_call(svc, 500, &call);
		if (rc == DEFW2_ERR_NOT_FOUND)
			break;		/* the queue closed */
		if (rc != DEFW2_OK)
			continue;	/* nothing arrived in time */

		document = defw2_call_document(call);
		if (document == NULL) {
			/* Echo's bytes. A document answer does not fit. */
			if (defw2_service_respond_document(call, "1") ==
			    DEFW2_ERR_INVALID)
				__atomic_add_fetch(&bytes_told_apart, 1,
						   __ATOMIC_RELAXED);
			request = defw2_call_request(call, &len);
			defw2_service_respond(call, request, len);
			continue;
		}
		method = defw2_call_method(call);
		if (strcmp(method, "boom") == 0) {
			defw2_service_fail(call, DEFW2_ERR_INTERNAL,
					   DEFW2_CAT_PROVIDER_FAILURE,
					   "the consumer went boom");
			continue;
		}
		len = strlen(document) + strlen(method) +
		      strlen(defw2_call_api(call)) + 64;
		text = malloc(len);
		if (text == NULL) {
			defw2_service_fail(call, DEFW2_ERR_NOMEM,
					   DEFW2_CAT_PROVIDER_FAILURE,
					   "no memory");
			continue;
		}
		snprintf(text, len,
			 "{\"api\": \"%s\", \"method\": \"%s\", "
			 "\"request\": %s}",
			 defw2_call_api(call), method, document);
		defw2_service_respond_document(call, text);
		/* The answer was copied, so this is the consumer's to free. */
		free(text);
	}
	return NULL;
}

/* --- calling ----------------------------------------------------------- */

struct call_result {
	defw2_rc_t	rc;
	defw2_status_t	status;
	char		*answer;
};

static void call_doc(defw2_binding_t *b, const char *api, const char *method,
		     const char *request, struct call_result *r)
{
	defw2_call_opts_t opts = { .timeout_ms = 30000 };

	memset(r, 0, sizeof(*r));
	r->rc = defw2_doc_call(b, api, method, request, &opts, &r->answer,
			       &r->status);
}

static void result_free(struct call_result *r)
{
	free(r->answer);
	defw2_status_free(&r->status);
	memset(r, 0, sizeof(*r));
}

static bool answered(const struct call_result *r, const char *expect)
{
	return r->rc == DEFW2_OK && r->status.code == DEFW2_OK &&
	       r->answer != NULL && strcmp(r->answer, expect) == 0;
}

static bool failed(const struct call_result *r, uint32_t category,
		   const char *message)
{
	return r->rc == DEFW2_OK && r->status.category == category &&
	       r->status.message != NULL && r->answer == NULL &&
	       strstr(r->status.message, message) != NULL;
}

struct worker {
	defw2_binding_t	*binding;
	int		index;
	int		right;
};

/* Each call's answer must be its own, under concurrency. */
static void *call_many(void *arg)
{
	struct worker *w = arg;
	int i;

	for (i = 0; i < CALLS; i++) {
		struct call_result r;
		char request[64], expect[192];

		snprintf(request, sizeof(request), "{\"worker\": %d, "
			 "\"call\": %d}", w->index, i);
		snprintf(expect, sizeof(expect), "{\"api\": \"" API_QUEUED
			 "\", \"method\": \"whoami\", \"request\": %s}",
			 request);
		call_doc(w->binding, API_QUEUED, "whoami", request, &r);
		if (answered(&r, expect))
			w->right++;
		result_free(&r);
	}
	return NULL;
}

/* Can no call with these arguments get past the client? */
static bool refused_here(defw2_binding_t *b, const char *api,
			 const char *method, const char *request)
{
	struct call_result r;
	long before = asked();
	bool ok;

	call_doc(b, api, method, request, &r);
	ok = r.rc == DEFW2_ERR_INVALID && r.answer == NULL &&
	     asked() == before;
	result_free(&r);
	return ok;
}

int main(int argc, char **argv)
{
	defw2_doc_ops_t ops = { .call = answer_doc, .arg = NULL };
	defw2_service_t *doc_svc = NULL, *queued_svc = NULL, *none_svc = NULL;
	defw2_binding_t *doc = NULL, *queued = NULL, *none = NULL;
	defw2_rt_t *server = NULL, *client = NULL;
	defw2_config_t server_cfg, client_cfg;
	struct worker workers[THREADS];
	pthread_t threads[THREADS];
	pthread_t consumers[2];
	struct call_result r;
	char *big, *name;
	int i, right;

	if (argc > 1)
		setenv("DEFW2_TELEMETRY_DIR", argv[1], 1);

	defw2_config_from_env(&server_cfg);
	server_cfg.role = DEFW2_ROLE_SERVER;
	/* Each runtime's telemetry files are named after it. */
	server_cfg.node_name = "doc-server";
	server_cfg.profile = argc > 1;
	if (defw2_init(&server_cfg, &server) != DEFW2_OK) {
		printf("server init FAILED\n");
		return 1;
	}
	check("a C service binds documents",
	      defw2_service_create(server, "doc-c", "qfw.test", PROVIDER_DOC,
				   &doc_svc) == DEFW2_OK &&
	      defw2_doc_bind(doc_svc, API_DOC, &ops) == DEFW2_OK);
	check("a queued service binds documents beside echo",
	      defw2_service_create(server, "doc-queued", "qfw.test",
				   PROVIDER_QUEUED, &queued_svc) == DEFW2_OK &&
	      defw2_service_queue_open(queued_svc, 0) == DEFW2_OK &&
	      defw2_doc_bind(queued_svc, API_QUEUED, NULL) == DEFW2_OK &&
	      defw2_echo_bind(queued_svc, NULL) == DEFW2_OK);
	check("a service with neither binds them too",
	      defw2_service_create(server, "doc-none", "qfw.test",
				   PROVIDER_NONE, &none_svc) == DEFW2_OK &&
	      defw2_doc_bind(none_svc, API_NONE, NULL) == DEFW2_OK);
	check("an API with a space in its name is refused",
	      defw2_doc_bind(none_svc, "qfw test", NULL) ==
	      DEFW2_ERR_INVALID);
	for (i = 0; i < 2; i++)
		pthread_create(&consumers[i], NULL, consume, queued_svc);

	defw2_config_from_env(&client_cfg);
	client_cfg.role = DEFW2_ROLE_CLIENT;
	client_cfg.node_name = "doc-client";
	client_cfg.profile = argc > 1;
	if (defw2_init(&client_cfg, &client) != DEFW2_OK) {
		printf("client init FAILED\n");
		return 1;
	}
	defw2_binding_create(client, defw2_service_address(doc_svc),
			     PROVIDER_DOC, &doc);
	defw2_binding_create(client, defw2_service_address(queued_svc),
			     PROVIDER_QUEUED, &queued);
	defw2_binding_create(client, defw2_service_address(none_svc),
			     PROVIDER_NONE, &none);

	/* --- a C operations table */
	call_doc(doc, API_DOC, "echo", "{\"a\": [1, 2, 3]}", &r);
	check("a document reaches the C service and its answer comes back",
	      answered(&r, "{\"method\": \"echo\", "
			   "\"request\": {\"a\": [1, 2, 3]}}"));
	result_free(&r);
	call_doc(doc, API_DOC, "echo", NULL, &r);
	check("no request reaches the service as {}",
	      answered(&r, "{\"method\": \"echo\", \"request\": {}}"));
	result_free(&r);
	call_doc(doc, API_DOC, "nothing", "{}", &r);
	check("an answer of nothing is null", answered(&r, "null"));
	result_free(&r);
	call_doc(doc, API_DOC, "refuse", "{}", &r);
	check("a service's own failure is its status, with no answer",
	      failed(&r, DEFW2_CAT_INVALID_ARGUMENT, "the service refused"));
	result_free(&r);
	call_doc(doc, API_DOC, "nosuch", "{}", &r);
	check("so is a method it does not have",
	      failed(&r, DEFW2_CAT_NOT_FOUND, "no such method"));
	result_free(&r);

	/* --- the size limit, both ways */
	call_doc(doc, API_DOC, "biggest", "{}", &r);
	check("the largest answer a document carries arrives whole",
	      r.rc == DEFW2_OK && r.status.code == DEFW2_OK &&
	      r.answer != NULL && strlen(r.answer) == DEFW2_EAGER_MAX - 1);
	result_free(&r);
	call_doc(doc, API_DOC, "huge", "{}", &r);
	check("one byte more is the provider's failure, not a hang",
	      failed(&r, DEFW2_CAT_PROVIDER_FAILURE,
		     "larger than a document can carry"));
	result_free(&r);
	big = filled(DEFW2_EAGER_MAX - 1, 'x');
	big[0] = '"';
	big[DEFW2_EAGER_MAX - 2] = '"';
	call_doc(doc, API_DOC, "length", big, &r);
	check("the largest request a document carries arrives whole",
	      answered(&r, "{\"length\": 4194303}"));
	result_free(&r);
	free(big);
	big = filled(DEFW2_EAGER_MAX, 'x');
	check("one byte more is refused before it is sent",
	      refused_here(doc, API_DOC, "length", big));
	free(big);

	/* --- names the tier refuses */
	check("a method starting with an underscore is refused",
	      refused_here(doc, API_DOC, "_private", "{}"));
	check("so is one starting with a digit",
	      refused_here(doc, API_DOC, "9lives", "{}"));
	check("one with a dot", refused_here(doc, API_DOC, "a.b", "{}"));
	check("one with a space", refused_here(doc, API_DOC, "a b", "{}"));
	check("an empty one", refused_here(doc, API_DOC, "", "{}"));
	check("and none at all", refused_here(doc, API_DOC, NULL, "{}"));
	name = filled(DEFW2_DOC_METHOD_MAX + 1, 'm');
	check("a method one byte too long is refused",
	      refused_here(doc, API_DOC, name, "{}"));
	name[DEFW2_DOC_METHOD_MAX] = '\0';
	call_doc(doc, API_DOC, name, "{}", &r);
	check("and the longest one is not",
	      r.rc == DEFW2_OK && r.status.category == DEFW2_CAT_NOT_FOUND);
	result_free(&r);
	free(name);
	check("an API with a space in its name is refused",
	      refused_here(doc, "qfw test", "echo", "{}"));
	check("so is an empty API", refused_here(doc, "", "echo", "{}"));
	check("and no API at all", refused_here(doc, NULL, "echo", "{}"));
	check("a call with nowhere for the answer is refused",
	      defw2_doc_call(doc, API_DOC, "echo", "{}", NULL, NULL, NULL) ==
	      DEFW2_ERR_INVALID);

	/* --- what the provider does not serve */
	call_doc(doc, API_QUEUED, "whoami", "{}", &r);
	check("an API the provider does not serve is not found",
	      r.rc == DEFW2_ERR_NOT_FOUND && r.answer == NULL);
	result_free(&r);
	call_doc(none, API_NONE, "anything", "{}", &r);
	check("a service with no operations and no queue answers none",
	      failed(&r, DEFW2_CAT_NOT_FOUND,
		     "this service answers no documents"));
	result_free(&r);

	/* --- a consumer answering from the queue */
	call_doc(queued, API_QUEUED, "whoami", "{\"x\": 1}", &r);
	check("a consumer sees the API, the method and the request",
	      answered(&r, "{\"api\": \"" API_QUEUED "\", \"method\": "
			   "\"whoami\", \"request\": {\"x\": 1}}"));
	result_free(&r);
	call_doc(queued, API_QUEUED, "boom", "{}", &r);
	check("and its failure reaches the caller",
	      failed(&r, DEFW2_CAT_PROVIDER_FAILURE, "the consumer went boom"));
	result_free(&r);
	{
		defw2_call_opts_t opts = { .timeout_ms = 30000 };
		defw2_buffer_t reply = { 0 };
		defw2_status_t status = { 0 };
		defw2_binding_t *echo = NULL;

		defw2_binding_create(client, defw2_service_address(queued_svc),
				     PROVIDER_QUEUED, &echo);
		check("echo's bytes on the same queue still come back",
		      defw2_echo(echo, "abc", 3, &opts, &reply, &status) ==
		      DEFW2_OK && status.code == DEFW2_OK && reply.len == 3 &&
		      memcmp(reply.data, "abc", 3) == 0);
		check("and the consumer could not answer them as a document",
		      __atomic_load_n(&bytes_told_apart, __ATOMIC_RELAXED) ==
		      1);
		defw2_buffer_free(&reply);
		defw2_status_free(&status);
		defw2_binding_free(echo);
	}

	for (i = 0; i < THREADS; i++) {
		workers[i] = (struct worker){ queued, i, 0 };
		pthread_create(&threads[i], NULL, call_many, &workers[i]);
	}
	right = 0;
	for (i = 0; i < THREADS; i++) {
		pthread_join(threads[i], NULL);
		right += workers[i].right;
	}
	check("four threads at once each get their own answers",
	      right == THREADS * CALLS);

	defw2_binding_free(doc);
	defw2_binding_free(queued);
	defw2_binding_free(none);
	defw2_finalize(client);
	defw2_service_queue_close(queued_svc);
	for (i = 0; i < 2; i++)
		pthread_join(consumers[i], NULL);
	defw2_service_destroy(doc_svc);
	defw2_service_destroy(queued_svc);
	defw2_service_destroy(none_svc);
	defw2_finalize(server);

	printf("DOC SMOKE %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
