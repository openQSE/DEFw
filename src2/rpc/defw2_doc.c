/*
 * The document tier: a method's arguments and answer as JSON. See
 * defw2_doc.h.
 *
 * A document call is a typed call whose method happens to be generic, so
 * defw2_typed_call and defw2_typed_serve do everything here except name
 * the RPC and check the method. That is what makes the two tiers share a
 * header, a status, a queue and a span.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2_doc.h>

#include "defw2_typed.h"

#define DOC_PREFIX	"defw2."
#define DOC_SUFFIX	".document"
/* The longest RPC name, with its terminator. */
#define DOC_RPC_MAX	(sizeof(DOC_PREFIX) - 1 + DEFW2_DOC_API_MAX + \
			 sizeof(DOC_SUFFIX))

static bool letter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool digit(char c)
{
	return c >= '0' && c <= '9';
}

bool defw2_doc_method_ok(const char *method)
{
	size_t i;

	if (method == NULL || !letter(method[0]))
		return false;
	for (i = 1; method[i] != '\0'; i++) {
		if (i >= DEFW2_DOC_METHOD_MAX)
			return false;
		if (!letter(method[i]) && !digit(method[i]) &&
		    method[i] != '_')
			return false;
	}
	return true;
}

/* An API is named like qfw.qpm.admission-policy. */
static bool api_ok(const char *api)
{
	size_t i;

	if (api == NULL || api[0] == '\0')
		return false;
	for (i = 0; api[i] != '\0'; i++) {
		if (i >= DEFW2_DOC_API_MAX)
			return false;
		if (!letter(api[i]) && !digit(api[i]) && api[i] != '.' &&
		    api[i] != '_' && api[i] != '-')
			return false;
	}
	return true;
}

static void rpc_name(char *rpc, size_t len, const char *api)
{
	snprintf(rpc, len, DOC_PREFIX "%s" DOC_SUFFIX, api);
}

static void doc_method(struct defw2_method *m, const char *api,
		       const char *name, const char *rpc)
{
	m->api = api;
	m->name = name;
	m->rpc = rpc;
	m->version = DEFW2_DOC_VERSION;
	m->in_proc = hg_proc_defw2_doc_in_t;
	m->out_proc = hg_proc_defw2_doc_out_t;
	m->tier = DEFW2_TIER_DOCUMENT;
}

/* --- calling --------------------------------------------------------- */

static defw2_rc_t take_answer(struct defw2_typed_call *call)
{
	defw2_doc_out_t *out = call->out;
	char **answer = call->arg;

	/* A failed call says why in its status and carries no answer. */
	if (out->status.code != DEFW2_OK)
		return DEFW2_OK;
	*answer = strdup(out->document != NULL ? out->document : "null");
	return *answer != NULL ? DEFW2_OK : DEFW2_ERR_NOMEM;
}

defw2_rc_t defw2_doc_call(defw2_binding_t *binding, const char *api,
			  const char *method, const char *request,
			  const defw2_call_opts_t *opts, char **answer,
			  defw2_status_t *status)
{
	struct defw2_typed_call call;
	struct defw2_method m;
	char rpc[DOC_RPC_MAX];
	defw2_doc_out_t out;
	defw2_doc_in_t in;

	if (answer != NULL)
		*answer = NULL;
	if (binding == NULL || answer == NULL || !api_ok(api) ||
	    !defw2_doc_method_ok(method))
		return DEFW2_ERR_INVALID;
	/* Refuse to send what the receiver would refuse. */
	if (request != NULL &&
	    strnlen(request, DEFW2_EAGER_MAX) >= DEFW2_EAGER_MAX)
		return DEFW2_ERR_INVALID;

	rpc_name(rpc, sizeof(rpc), api);
	/* The span names the document's own method. */
	doc_method(&m, api, method, rpc);
	memset(&in, 0, sizeof(in));
	in.method = method;
	in.document = request;
	memset(&call, 0, sizeof(call));
	call.binding = binding;
	call.method = &m;
	call.opts = opts;
	call.in = &in;
	call.out = &out;
	call.out_size = sizeof(out);
	call.take = take_answer;
	call.arg = answer;
	return defw2_typed_call(&call, status);
}

/* --- serving --------------------------------------------------------- */

struct doc_bound {
	struct defw2_bound	base;
	struct defw2_method	method;
	defw2_doc_ops_t		ops;
	char			names[];	/* the API's, then the RPC's */
};

static void serve_document(struct defw2_served *served, void *in_,
			   void *out_)
{
	struct doc_bound *bound = (struct doc_bound *)served->bound;
	defw2_doc_out_t *out = out_;
	defw2_doc_in_t *in = in_;
	const char *answer = NULL;
	defw2_rc_t rc;

	if (!defw2_doc_method_ok(in->method)) {
		defw2_served_fail(served, DEFW2_ERR_INVALID,
				  DEFW2_CAT_INVALID_ARGUMENT,
				  "a document's method must be an identifier "
				  "that does not start with an underscore");
		return;
	}
	served->call.method = in->method;
	served->call.document = in->document != NULL ? in->document : "{}";
	served->call.response = &answer;

	if (defw2_served_queued(served)) {
		rc = defw2_served_queue(served);
	} else if (bound->ops.call != NULL) {
		rc = bound->ops.call(bound->ops.arg, &served->call,
				     in->method, served->call.document,
				     &answer);
	} else {
		defw2_served_fail(served, DEFW2_ERR_NOT_FOUND,
				  DEFW2_CAT_NOT_FOUND,
				  "this service answers no documents");
		return;
	}
	if (!defw2_served_finish(served, rc))
		return;
	if (answer == NULL)
		answer = "null";
	/*
	 * An answer the wire will not take would fail the respond, and a
	 * caller with no timeout would then wait for ever.
	 */
	if (strnlen(answer, DEFW2_EAGER_MAX) >= DEFW2_EAGER_MAX) {
		defw2_served_fail(served, DEFW2_ERR_INVALID,
				  DEFW2_CAT_PROVIDER_FAILURE,
				  "the service's answer is larger than a "
				  "document can carry");
		return;
	}
	out->document = answer;
}

/* What decodes a call that reaches a provider with nothing bound. */
static const struct defw2_method unbound = {
	"?", "document", DOC_PREFIX "?" DOC_SUFFIX, DEFW2_DOC_VERSION,
	hg_proc_defw2_doc_in_t, hg_proc_defw2_doc_out_t, DEFW2_TIER_DOCUMENT,
};

static void defw2_doc_ult(hg_handle_t handle)
{
	margo_instance_id mid = margo_hg_handle_get_instance(handle);
	const struct hg_info *info = margo_get_info(handle);
	struct doc_bound *bound = NULL;
	defw2_doc_out_t out;
	defw2_doc_in_t in;

	/* Each API's method lives in what it bound, since its name does. */
	if (info != NULL)
		bound = margo_registered_data(mid, info->id);
	defw2_typed_serve(handle, bound != NULL ? &bound->method : &unbound,
			  &in, sizeof(in), &out, sizeof(out), serve_document);
}
DEFINE_MARGO_RPC_HANDLER(defw2_doc_ult)

defw2_rc_t defw2_doc_bind(defw2_service_t *svc, const char *api,
			  const defw2_doc_ops_t *ops)
{
	struct defw2_typed_entry entry;
	struct doc_bound *bound;
	size_t api_len, rpc_len;
	char *rpc;

	if (svc == NULL || !api_ok(api))
		return DEFW2_ERR_INVALID;
	api_len = strlen(api) + 1;
	rpc_len = sizeof(DOC_PREFIX) - 1 + api_len - 1 + sizeof(DOC_SUFFIX);
	/*
	 * One allocation for the binding and the names its method points at,
	 * because Margo frees what a method registered with free() when the
	 * runtime finalizes.
	 */
	bound = calloc(1, sizeof(*bound) + api_len + rpc_len);
	if (bound == NULL)
		return DEFW2_ERR_NOMEM;
	memcpy(bound->names, api, api_len);
	rpc = bound->names + api_len;
	rpc_name(rpc, rpc_len, api);
	doc_method(&bound->method, bound->names, "document", rpc);
	if (ops != NULL)
		bound->ops = *ops;

	entry.method = &bound->method;
	entry.handler = _handler_for_defw2_doc_ult;
	return defw2_typed_bind(svc, &entry, 1, &bound->base);
}
