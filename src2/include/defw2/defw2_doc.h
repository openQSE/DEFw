/*
 * The document tier.
 *
 * A method with no typed structure travels as a document: a JSON object of
 * named arguments in, and any JSON value out. One RPC per API carries every
 * such method. It is registered as defw2.<api>.document on the provider
 * that serves the API's typed methods, so a document call shares that
 * provider's queue, header, version check, status and spans with them. A
 * span says which tier the call took and names the document's own method.
 *
 *	char *answer = NULL;
 *
 *	defw2_doc_call(telemetry, "qfw.qpm.telemetry", "get_backend_info",
 *		       "{\"lib\": \"qdmi\"}", &opts, &answer, &status);
 *	// answer is the service's JSON, when the call and the status are OK
 *	free(answer);
 *
 * This is the tier for methods that are still changing or that answer with
 * provider-shaped data. A method can be typed later without touching it.
 *
 * C never parses a document, so a document is only ever data to C. A method
 * name must be an identifier that does not start with an underscore,
 * because a service in another language looks the method up by name. The
 * client refuses any other name before it sends, and the provider refuses
 * it before the service sees it. A document travels inside the message, so
 * each one, request or answer, is at most DEFW2_EAGER_MAX bytes with its
 * terminator.
 */
#ifndef DEFW2_DOC_H
#define DEFW2_DOC_H

#include <stdbool.h>

#include <defw2/defw2_rpc.h>
#include <defw2/defw2_service.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The version the document RPC speaks, major in the high sixteen bits. It
 * is the tier's own, not an API's. What the documents of an API mean is the
 * API's business.
 */
#define DEFW2_DOC_VERSION_MAJOR		1
#define DEFW2_DOC_VERSION_MINOR		0
#define DEFW2_DOC_VERSION	(((uint32_t)DEFW2_DOC_VERSION_MAJOR << 16) | \
				 (uint32_t)DEFW2_DOC_VERSION_MINOR)

/* The longest method name, and the longest API name, without terminators. */
#define DEFW2_DOC_METHOD_MAX		128
#define DEFW2_DOC_API_MAX		128

/*
 * Call method of api over binding. request is the JSON text of an object
 * of named arguments, or NULL for none. When the call and the status are
 * both OK, *answer is the service's JSON text, which the caller frees with
 * free(). Otherwise it is NULL. The return code is the transport outcome
 * and status the service's own, as for every stub.
 */
defw2_rc_t defw2_doc_call(defw2_binding_t *binding, const char *api,
			  const char *method, const char *request,
			  const defw2_call_opts_t *opts, char **answer,
			  defw2_status_t *status);

/* Is this a name the document tier carries? */
bool defw2_doc_method_ok(const char *method);

/*
 * Serving from C. One function answers every method of an API. It answers
 * with JSON text from defw2_call_strdup or its relatives, which the provider
 * frees once the reply is on the wire. It returns DEFW2_OK, or fails the
 * call with defw2_call_set_status and some other code. An answer of NULL is
 * the JSON null.
 */
typedef defw2_rc_t (*defw2_doc_fn)(void *arg, defw2_call_t *call,
				   const char *method, const char *request,
				   const char **answer);

typedef struct {
	defw2_doc_fn	call;
	void		*arg;
} defw2_doc_ops_t;

/*
 * Serve api's documents on svc's provider. A service answering from its
 * queue answers them there, and needs no ops. A service with neither tells
 * the caller it answers no documents.
 */
defw2_rc_t defw2_doc_bind(defw2_service_t *svc, const char *api,
			  const defw2_doc_ops_t *ops);

/*
 * Serving from the queue. A document call's method is what
 * defw2_call_method returns, and its request is what defw2_call_document
 * returns, which is never NULL: a caller that sent no request sent "{}".
 * For any other call it is NULL, which is how a consumer tells them apart.
 * The request lasts until the call is answered.
 */
const char *defw2_call_document(const defw2_call_t *call);

/* Answer a document call with JSON text, which is copied. */
defw2_rc_t defw2_service_respond_document(defw2_call_t *call,
					  const char *answer);

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_DOC_H */
