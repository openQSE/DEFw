/*
 * Does the wire refuse what it should?
 *
 * Mercury's own string and bulk procs trust the length a message claims and
 * the terminator it promises. v2's do not, and this test holds them to it.
 * The first half hands the decoders the bytes a broken or hostile peer would
 * send, built by hand. The second half sends such requests to real
 * providers, echo, a QPM's control API and an event sink, to show that a
 * malformed request is answered rather than crashing the provider or
 * leaking what it half decoded.
 *
 * Whether a refused request leaks is measured rather than left to the leak
 * sanitizer. A provider decodes into a structure on its handler's stack, and
 * Argobots pools those stacks in reachable memory, so the sanitizer finds the
 * stale pointer and calls the string reachable: with the free removed, a
 * sanitizer run still passes. So the test floods the provider with requests
 * that each strand a large string, and checks that the heap did not grow by
 * what they would have leaked.
 */
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
/*
 * The sanitizer replaces malloc, so its own count is the one to read.
 * Declared here because the header that declares it is not always
 * installed, while the library that exports it always is.
 */
size_t __sanitizer_get_current_allocated_bytes(void);

static size_t heap_in_use(void)
{
	return __sanitizer_get_current_allocated_bytes();
}

static size_t heap_with_maps(void)
{
	return heap_in_use();
}
#else
#include <malloc.h>

static size_t heap_in_use(void)
{
	return mallinfo2().uordblks;
}

/* malloc maps a large allocation apart, and counts it apart. */
static size_t heap_with_maps(void)
{
	struct mallinfo2 info = mallinfo2();

	return info.uordblks + info.hblkhd;
}
#endif

#include <defw2/defw2_doc.h>
#include <defw2/defw2_echo.h>
#include <defw2/defw2_qpm.h>

#include "defw2_qpm_wire.h"
#include "defw2_dir_wire.h"
#include "defw2_event_internal.h"

static int failures;

static void check(const char *what, bool ok)
{
	printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

/* --- building bytes by hand ------------------------------------------ */

/*
 * Mercury without XDR encodes an integer as its own bytes, so a hostile
 * message is just these written in a row.
 */
struct msg {
	unsigned char	*data;
	size_t		len;
	size_t		cap;
};

static void put(struct msg *m, const void *p, size_t n)
{
	if (m->len + n > m->cap) {
		m->cap = (m->len + n) * 2;
		m->data = realloc(m->data, m->cap);
	}
	memcpy(m->data + m->len, p, n);
	m->len += n;
}

static void put_u64(struct msg *m, uint64_t v)
{
	put(m, &v, sizeof(v));
}

static void put_u32(struct msg *m, uint32_t v)
{
	put(m, &v, sizeof(v));
}

static void reset(struct msg *m)
{
	m->len = 0;
}

static hg_return_t decode(hg_proc_t proc, struct msg *m, hg_proc_cb_t cb,
			  void *obj)
{
	hg_proc_reset(proc, m->data, m->len, HG_DECODE);
	return cb(proc, obj);
}

/* What a handler does with whatever a decode left behind. */
static void release(hg_proc_t proc, hg_proc_cb_t cb, void *obj)
{
	hg_proc_reset(proc, NULL, 0, HG_FREE);
	cb(proc, obj);
}

static hg_return_t encode(hg_proc_t proc, struct msg *m, hg_proc_cb_t cb,
			  void *obj)
{
	hg_return_t ret;

	hg_proc_reset(proc, m->data, m->cap, HG_ENCODE);
	ret = cb(proc, obj);
	m->len = hg_proc_get_size_used(proc);
	return ret;
}

/* --- the procs, one by one ------------------------------------------- */

static void strings(hg_proc_t proc, struct msg *m)
{
	const char *s = NULL;
	char *big;

	reset(m);
	s = "qfw";
	check("a string survives a round trip",
	      encode(proc, m, hg_proc_defw2_str_t, &s) == HG_SUCCESS &&
	      decode(proc, m, hg_proc_defw2_str_t, &s) == HG_SUCCESS &&
	      s != NULL && strcmp(s, "qfw") == 0);
	release(proc, hg_proc_defw2_str_t, &s);
	check("and a free leaves it NULL", s == NULL);

	s = NULL;
	check("an absent string comes back absent",
	      encode(proc, m, hg_proc_defw2_str_t, &s) == HG_SUCCESS &&
	      decode(proc, m, hg_proc_defw2_str_t, &s) == HG_SUCCESS &&
	      s == NULL);

	s = "";
	check("an empty one comes back empty, not absent",
	      encode(proc, m, hg_proc_defw2_str_t, &s) == HG_SUCCESS &&
	      decode(proc, m, hg_proc_defw2_str_t, &s) == HG_SUCCESS &&
	      s != NULL && s[0] == '\0');
	release(proc, hg_proc_defw2_str_t, &s);

	reset(m);
	put_u64(m, 3);
	put(m, "abc", 3);
	check("a string with no terminator is refused",
	      decode(proc, m, hg_proc_defw2_str_t, &s) == HG_PROTOCOL_ERROR &&
	      s == NULL);

	reset(m);
	put_u64(m, 4);
	put(m, "a\0b", 4);
	check("a string with a NUL inside is refused",
	      decode(proc, m, hg_proc_defw2_str_t, &s) == HG_PROTOCOL_ERROR &&
	      s == NULL);

	/*
	 * Mercury's own proc would allocate the megabyte and fill it from
	 * memory it never received. This one refuses before allocating.
	 */
	reset(m);
	put_u64(m, 1u << 20);
	put(m, "abc", 4);
	check("a length longer than the message is refused",
	      decode(proc, m, hg_proc_defw2_str_t, &s) == HG_OVERFLOW &&
	      s == NULL);

	big = malloc(DEFW2_STR_MAX + 1);
	memset(big, 'a', DEFW2_STR_MAX);
	big[DEFW2_STR_MAX] = '\0';
	reset(m);
	put_u64(m, DEFW2_STR_MAX + 1);
	put(m, big, DEFW2_STR_MAX + 1);
	check("a string longer than DEFW2_STR_MAX is refused",
	      decode(proc, m, hg_proc_defw2_str_t, &s) == HG_OVERFLOW &&
	      s == NULL);
	check("the same bytes are fine as text",
	      decode(proc, m, hg_proc_defw2_text_t, &s) == HG_SUCCESS &&
	      s != NULL && strlen(s) == DEFW2_STR_MAX);
	release(proc, hg_proc_defw2_text_t, &s);

	s = big;
	check("and a sender refuses to encode it as a string",
	      encode(proc, m, hg_proc_defw2_str_t, &s) == HG_OVERFLOW);
	free(big);
}

static void counted(hg_proc_t proc, struct msg *m)
{
	defw2_bytes_t bytes = { 0 };

	reset(m);
	put_u64(m, 1u << 20);
	put(m, "0123456789", 10);
	check("counted bytes longer than the message are refused",
	      decode(proc, m, hg_proc_defw2_bytes_t, &bytes) == HG_OVERFLOW &&
	      bytes.data == NULL);

	reset(m);
	put_u64(m, (uint64_t)DEFW2_EAGER_MAX + 1);
	check("counted bytes past the eager limit are refused",
	      decode(proc, m, hg_proc_defw2_bytes_t, &bytes) == HG_OVERFLOW &&
	      bytes.data == NULL);
}

static void bulk(hg_proc_t proc, struct msg *m)
{
	defw2_wire_tensor_t tensor;
	defw2_wire_result_t result;

	memset(&tensor, 0, sizeof(tensor));
	reset(m);
	put_u32(m, DEFW2_DTYPE_C128);
	put_u32(m, DEFW2_TENSOR_RANK_MAX + 1);
	check("a tensor with too many dimensions is refused",
	      decode(proc, m, hg_proc_defw2_wire_tensor_t, &tensor) ==
	      HG_OVERFLOW);

	memset(&result, 0, sizeof(result));
	reset(m);
	put_u64(m, DEFW2_BULK_MAX + 1);
	put_u64(m, 0);
	check("a result buffer past the bulk limit is refused",
	      decode(proc, m, hg_proc_defw2_wire_result_t, &result) ==
	      HG_OVERFLOW && result.handle == HG_BULK_NULL);

	/*
	 * A bulk handle that claims a serialized size the message does not
	 * carry, which Mercury's own proc would reserve and parse from
	 * memory it never received.
	 */
	reset(m);
	put_u64(m, 4096);
	put_u64(m, 1u << 20);
	put(m, "handle", 6);
	check("a bulk handle longer than the message is refused",
	      decode(proc, m, hg_proc_defw2_wire_result_t, &result) ==
	      HG_OVERFLOW && result.handle == HG_BULK_NULL);

	reset(m);
	put_u64(m, 0);
	put_u64(m, 0);
	check("a request that lends nothing decodes as nothing",
	      decode(proc, m, hg_proc_defw2_wire_result_t, &result) ==
	      HG_SUCCESS && result.capacity == 0 &&
	      result.handle == HG_BULK_NULL);
	release(proc, hg_proc_defw2_wire_result_t, &result);
}

/*
 * A structure that fails part way. The runtime_id decoded before the
 * traceparent was refused, and a handler that frees the input after a
 * failed decode must release it. The sanitizer's leak check is what proves
 * it did.
 */
static void partial(hg_proc_t proc, struct msg *m)
{
	defw2_hdr_t hdr;

	memset(&hdr, 0, sizeof(hdr));
	reset(m);
	put_u32(m, DEFW2_API_VERSION);
	put_u64(m, 1);
	put_u64(m, 5);
	put(m, "rt-1", 5);
	put_u64(m, 3);
	put(m, "abc", 3);
	check("a header with a bad second string is refused",
	      decode(proc, m, hg_proc_defw2_hdr_t, &hdr) ==
	      HG_PROTOCOL_ERROR && hdr.runtime_id != NULL &&
	      hdr.traceparent == NULL);
	release(proc, hg_proc_defw2_hdr_t, &hdr);
	check("and the free releases the string that did decode",
	      hdr.runtime_id == NULL);
}

static void put_str(struct msg *m, const char *s)
{
	put_u64(m, strlen(s) + 1);
	put(m, s, strlen(s) + 1);
}

/* An event's header and envelope, up to where its payload starts. */
static void envelope(struct msg *m, const char *api, const char *name)
{
	reset(m);
	put_u32(m, DEFW2_QPM_VERSION);
	put_u64(m, 1);
	put_str(m, "rt-1");
	put_u64(m, 0);			/* no traceparent */
	put_u64(m, 0);			/* client_send_ns */
	put_str(m, "tag");
	put_u64(m, 0);			/* no type */
	put_u64(m, 7);			/* seq */
	put_str(m, api);
	put_str(m, name);
}

static void accepting(defw2_event_in_t *in)
{
	memset(in, 0, sizeof(*in));
	in->accepted[0] = &defw2_qpm_completion_kind;
	in->naccepted = 1;
}

/*
 * An event names its kind in the envelope, and its payload is decoded by
 * that kind's proc. A decoder that was not given the kind refuses it
 * before allocating anything for a payload it cannot check.
 */
static void events(hg_proc_t proc, struct msg *m)
{
	defw2_event_in_t in;
	long long grew;
	hg_return_t ret;
	size_t heap;
	unsigned i;

	memset(&in, 0, sizeof(in));
	envelope(m, DEFW2_API_QPM_EXECUTION, DEFW2_QPM_EVENT_COMPLETION);
	for (i = 0; i < 16; i++)
		put_u64(m, 0);		/* a payload of absent fields */
	check("an event of a kind the sink was not given is refused",
	      decode(proc, m, hg_proc_defw2_event_in_t, &in) == HG_NOENTRY &&
	      in.unknown && in.payload == NULL && in.tag != NULL);
	release(proc, hg_proc_defw2_event_in_t, &in);
	check("and the free releases the envelope that did decode",
	      in.tag == NULL && in.api == NULL && in.hdr.runtime_id == NULL);

	accepting(&in);
	check("the same bytes decode where the kind is taken",
	      decode(proc, m, hg_proc_defw2_event_in_t, &in) == HG_SUCCESS &&
	      in.kind == &defw2_qpm_completion_kind && in.payload != NULL &&
	      in.seq == 7);
	release(proc, hg_proc_defw2_event_in_t, &in);
	check("and the free releases the payload as well", in.payload == NULL);

	accepting(&in);
	envelope(m, DEFW2_API_QPM_EXECUTION, "another");
	for (i = 0; i < 16; i++)
		put_u64(m, 0);
	check("an event named for a kind nobody takes is refused",
	      decode(proc, m, hg_proc_defw2_event_in_t, &in) == HG_NOENTRY &&
	      in.unknown && in.payload == NULL);
	release(proc, hg_proc_defw2_event_in_t, &in);

	/* The payload breaks after a string of its own decoded. */
	accepting(&in);
	envelope(m, DEFW2_API_QPM_EXECUTION, DEFW2_QPM_EVENT_COMPLETION);
	put_str(m, DEFW2_QPM_COMPLETED);
	put_u64(m, 1u << 20);
	put(m, "abc", 4);
	check("a payload claiming more than the message carries is refused",
	      decode(proc, m, hg_proc_defw2_event_in_t, &in) == HG_OVERFLOW &&
	      in.payload != NULL);
	release(proc, hg_proc_defw2_event_in_t, &in);
	check("and the free releases what decoded before it",
	      in.payload == NULL && in.tag == NULL);

	memset(&in, 0, sizeof(in));
	check("a sender cannot encode an event without its kind",
	      encode(proc, m, hg_proc_defw2_event_in_t, &in) ==
	      HG_INVALID_ARG);
	release(proc, hg_proc_defw2_event_in_t, &in);

	/*
	 * A directory event carries a record, and a record carries counted
	 * lists. A count larger than the wire allows is refused before
	 * anything is allocated for it: a million aliases would be 8 MiB of
	 * pointers before the first one failed to decode.
	 */
	envelope(m, DEFW2_API_DIR, DEFW2_DIR_EVENT_SERVICE);
	put(m, "\1", 1);			/* connected */
	put_str(m, "registered");
	for (i = 0; i < 3; i++)
		put_str(m, "s");	/* service_id, type and runtime_id */
	put_u64(m, 1);			/* generation */
	put_u32(m, 0);			/* state */
	for (i = 0; i < 3; i++)
		put_str(m, "s");	/* address, node_name and hostname */
	put_u32(m, 7);			/* pid */
	put_str(m, "fake-20q");		/* selector_name */
	put_u32(m, 1u << 20);		/* aliases, a million of them */
	memset(&in, 0, sizeof(in));
	in.accepted[0] = &defw2_dir_change_kind;
	in.naccepted = 1;
	heap = heap_with_maps();
	ret = decode(proc, m, hg_proc_defw2_event_in_t, &in);
	grew = (long long)heap_with_maps() - (long long)heap;
	check("a directory event claiming a vast list is refused",
	      ret == HG_OVERFLOW && in.payload != NULL);
	check("before anything is allocated for the list",
	      grew < 64 * 1024);
	release(proc, hg_proc_defw2_event_in_t, &in);
	check("and the free releases the strings that did decode",
	      in.payload == NULL && in.tag == NULL);
}

/* --- a hostile peer against real providers -------------------------- */

static int served_calls;

static defw2_rc_t counting_echo(void *ctx, const void *request, size_t len,
				void **reply, size_t *reply_len)
{
	(void)ctx;
	(void)request;
	(void)len;
	__atomic_add_fetch(&served_calls, 1, __ATOMIC_RELAXED);
	*reply = NULL;
	*reply_len = 0;
	return DEFW2_OK;
}

static defw2_rc_t counting_document(void *arg, defw2_call_t *call,
				    const char *method, const char *request,
				    const char **answer)
{
	(void)arg;
	(void)call;
	(void)method;
	(void)request;
	__atomic_add_fetch(&served_calls, 1, __ATOMIC_RELAXED);
	*answer = "{}";
	return DEFW2_OK;
}

static defw2_rc_t counting_is_ready(void *ctx, defw2_call_t *call,
				    const defw2_qpm_ctx_t *req,
				    defw2_qpm_service_status_t *out)
{
	(void)ctx;
	(void)call;
	(void)req;
	__atomic_add_fetch(&served_calls, 1, __ATOMIC_RELAXED);
	out->state = "running";
	out->ready = true;
	return DEFW2_OK;
}

/*
 * One method to attack: its name, where it is served, the version its
 * header must carry, and how many zero integers make the rest of an empty
 * request after the header.
 */
struct target {
	const char	*what;
	const char	*rpc;
	uint16_t	provider;
	uint32_t	version;
	unsigned	trailer;
	hg_proc_cb_t	out_proc;
	bool		event;	/* a QPM completion's envelope follows */
};

enum evil {
	EVIL_UNTERMINATED,
	EVIL_LONG_LENGTH,
	EVIL_SECOND_STRING,
	EVIL_VERSION,
	EVIL_METHOD,	/* a well-formed document naming evil_method */
};

/*
 * What EVIL_SECOND_STRING sends ahead of the string it breaks: valid, and
 * large enough that a thousand of them stranded would stand out.
 */
#define STRANDED	2048
#define FLOOD		1000
static char stranded[STRANDED];

static const struct target *evil_target;
static enum evil evil_mode;
static const char *evil_method;

/*
 * A request as a hostile peer would encode it. Each mode breaks it in one
 * place, the header's first or second string, or not at all for the
 * version case, which is otherwise well formed. EVIL_SECOND_STRING breaks it
 * after a string that did decode, so the provider has something to free,
 * and the heap measured across a flood of them is what shows it did.
 */
static hg_return_t evil_in_proc(hg_proc_t proc, void *arg)
{
	hg_uint32_t version = evil_target->version;
	hg_uint64_t zero = 0, one = 1, len;
	unsigned i;

	(void)arg;
	if (hg_proc_get_op(proc) != HG_ENCODE)
		return HG_SUCCESS;
	if (evil_mode == EVIL_VERSION)
		version = 2u << 16;
	hg_proc_hg_uint32_t(proc, &version);
	hg_proc_hg_uint64_t(proc, &one);
	switch (evil_mode) {
	case EVIL_UNTERMINATED:
		len = 5;
		hg_proc_hg_uint64_t(proc, &len);
		hg_proc_bytes(proc, (void *)"abcde", 5);
		break;
	case EVIL_LONG_LENGTH:
		len = 1u << 20;
		hg_proc_hg_uint64_t(proc, &len);
		hg_proc_bytes(proc, (void *)"abc", 4);
		break;
	case EVIL_SECOND_STRING:
		len = STRANDED;
		hg_proc_hg_uint64_t(proc, &len);
		hg_proc_bytes(proc, stranded, STRANDED);
		len = 3;
		hg_proc_hg_uint64_t(proc, &len);	/* traceparent */
		hg_proc_bytes(proc, (void *)"abc", 3);
		return HG_SUCCESS;
	case EVIL_VERSION:
		len = 5;
		hg_proc_hg_uint64_t(proc, &len);
		hg_proc_bytes(proc, (void *)"evil", 5);
		break;
	case EVIL_METHOD:
		/*
		 * A request only a hostile peer sends, since the client
		 * refuses these names before it sends anything.
		 */
		len = 5;
		hg_proc_hg_uint64_t(proc, &len);
		hg_proc_bytes(proc, (void *)"evil", 5);
		hg_proc_hg_uint64_t(proc, &zero);	/* traceparent */
		hg_proc_hg_uint64_t(proc, &zero);	/* client_send_ns */
		len = evil_method != NULL ? strlen(evil_method) + 1 : 0;
		hg_proc_hg_uint64_t(proc, &len);
		if (len > 0)
			hg_proc_bytes(proc, (void *)evil_method, len);
		hg_proc_hg_uint64_t(proc, &zero);	/* no document */
		return HG_SUCCESS;
	}
	hg_proc_hg_uint64_t(proc, &zero);	/* traceparent, absent */
	hg_proc_hg_uint64_t(proc, &zero);	/* client_send_ns */
	if (evil_target->event) {
		hg_proc_hg_uint64_t(proc, &zero);	/* tag */
		hg_proc_hg_uint64_t(proc, &zero);	/* type */
		hg_proc_hg_uint64_t(proc, &zero);	/* seq */
		len = sizeof(DEFW2_API_QPM_EXECUTION);
		hg_proc_hg_uint64_t(proc, &len);
		hg_proc_bytes(proc, (void *)DEFW2_API_QPM_EXECUTION, len);
		len = sizeof(DEFW2_QPM_EVENT_COMPLETION);
		hg_proc_hg_uint64_t(proc, &len);
		hg_proc_bytes(proc, (void *)DEFW2_QPM_EVENT_COMPLETION, len);
	}
	for (i = 0; i < evil_target->trailer; i++)
		hg_proc_hg_uint64_t(proc, &zero);
	return HG_SUCCESS;
}

static uint32_t evil_call(margo_instance_id mid, hg_addr_t addr, hg_id_t id,
			  enum evil mode, char *message, size_t len)
{
	/* Room for any answer here; every answer leads with its status. */
	union {
		defw2_wire_status_t	status;
		unsigned char		room[512];
	} out;
	hg_handle_t handle;
	uint32_t category = DEFW2_CAT_TRANSPORT;
	int unused = 0;

	evil_mode = mode;
	message[0] = '\0';
	if (margo_create(mid, addr, id, &handle) != HG_SUCCESS)
		return category;
	/*
	 * The input is never read, but it must not be NULL: Mercury skips
	 * encoding altogether when it is, and nothing hostile would be sent.
	 */
	if (margo_provider_forward_timed(evil_target->provider, handle,
					 &unused, 10000.0) == HG_SUCCESS) {
		memset(&out, 0, sizeof(out));
		if (margo_get_output(handle, &out) == HG_SUCCESS) {
			category = out.status.category;
			if (out.status.message != NULL)
				snprintf(message, len, "%s",
					 out.status.message);
			margo_free_output(handle, &out);
		}
	}
	margo_destroy(handle);
	return category;
}

static void attack(margo_instance_id evil, const char *address,
		   const struct target *t)
{
	hg_addr_t addr = HG_ADDR_NULL;
	char what[128], message[256];
	size_t before, after, i;
	bool answered = true;
	hg_id_t id;

	evil_target = t;
	id = margo_register_name(evil, t->rpc, evil_in_proc, t->out_proc,
				 NULL);
	snprintf(what, sizeof(what), "the hostile peer reaches %s", t->what);
	check(what, id != 0 && margo_addr_lookup(evil, address, &addr) ==
	      HG_SUCCESS);

	check("  an unterminated string is answered, not run",
	      evil_call(evil, addr, id, EVIL_UNTERMINATED, message,
			sizeof(message)) == DEFW2_CAT_INVALID_ARGUMENT &&
	      strcmp(message, "cannot decode request") == 0);
	check("  so is a length the message does not carry",
	      evil_call(evil, addr, id, EVIL_LONG_LENGTH, message,
			sizeof(message)) == DEFW2_CAT_INVALID_ARGUMENT);
	check("  and a request broken after a string that decoded",
	      evil_call(evil, addr, id, EVIL_SECOND_STRING, message,
			sizeof(message)) == DEFW2_CAT_INVALID_ARGUMENT);
	check("  a different major version is refused as one",
	      evil_call(evil, addr, id, EVIL_VERSION, message,
			sizeof(message)) == DEFW2_CAT_VERSION_MISMATCH);

	/*
	 * The leak check proper. A warm-up first, so the pools Mercury and
	 * Margo grow on first use are not counted as a leak.
	 */
	for (i = 0; i < 50; i++)
		evil_call(evil, addr, id, EVIL_SECOND_STRING, message,
			  sizeof(message));
	before = heap_in_use();
	for (i = 0; i < FLOOD; i++)
		if (evil_call(evil, addr, id, EVIL_SECOND_STRING, message,
			      sizeof(message)) != DEFW2_CAT_INVALID_ARGUMENT)
			answered = false;
	after = heap_in_use();
	printf("    heap grew %zd bytes over %d refused requests that would "
	       "strand %d\n", (ssize_t)(after - before), FLOOD,
	       FLOOD * STRANDED);
	check("  a thousand of them were all answered", answered);
	check("  and freed what they decoded before they broke",
	      after < before + (size_t)FLOOD * STRANDED / 4);

	if (addr != HG_ADDR_NULL)
		margo_addr_free(evil, addr);
}

static void hostile(void)
{
	static const defw2_echo_ops_t echo_ops = { .echo = counting_echo };
	static const defw2_qpm_control_ops_t qpm_ops = {
		.is_ready = counting_is_ready,
	};
	const struct target echo_target = {
		"echo", DEFW2_RPC_ECHO, DEFW2_PROVIDER_ECHO, DEFW2_API_VERSION,
		1, hg_proc_defw2_echo_out_t,
	};
	const struct target qpm_target = {
		"a QPM", defw2_qpm_m_is_ready.rpc, DEFW2_PROVIDER_QPM_CONTROL,
		DEFW2_QPM_VERSION, 2, hg_proc_defw2_qpm_status_out_t,
	};
	/* A method and a document after the header, both absent. */
	const struct target doc_target = {
		"a document provider", "defw2.qfw.test.doc.document", 11,
		DEFW2_DOC_VERSION, 2, hg_proc_defw2_doc_out_t,
	};
	static const defw2_doc_ops_t doc_ops = { .call = counting_document };
	/* Enough zeros after the envelope for a payload of absent fields. */
	const struct target sink_target = {
		"an event sink", DEFW2_RPC_EVENT_DELIVER, DEFW2_PROVIDER_EVENT,
		DEFW2_QPM_VERSION, 16, hg_proc_defw2_event_out_t, true,
	};
	defw2_event_publisher_t *publisher = NULL;
	defw2_event_target_t target = { NULL, DEFW2_PROVIDER_EVENT, "honest" };
	defw2_qpm_task_t task = { .cid = "honest-1", .outcome = "COMPLETED" };
	defw2_event_sink_stats_t sink_stats;
	defw2_event_sink_t *sink = NULL;
	defw2_event_t event = { 0 };
	const defw2_qpm_task_t *got;
	defw2_config_t server_cfg, client_cfg;
	defw2_rt_t *server_rt = NULL, *client_rt = NULL;
	defw2_service_t *echo_svc = NULL, *qpm_svc = NULL, *doc_svc = NULL;
	defw2_binding_t *echo = NULL, *control = NULL, *doc = NULL;
	char message[256], *answer = NULL;
	hg_addr_t addr = HG_ADDR_NULL;
	hg_bool_t found = HG_FALSE;
	hg_id_t id;
	defw2_call_opts_t opts = { .timeout_ms = 10000 };
	defw2_qpm_service_status_t status_out = { 0 };
	defw2_buffer_t reply = { 0 };
	defw2_qpm_ctx_t req = { 0 };
	defw2_status_t status = { 0 };
	margo_instance_id evil;
	bool honest;

	memset(stranded, 's', STRANDED - 1);
	stranded[STRANDED - 1] = '\0';

	memset(&server_cfg, 0, sizeof(server_cfg));
	server_cfg.address = "na+sm://";
	server_cfg.node_name = "under-attack";
	server_cfg.role = DEFW2_ROLE_SERVER;
	server_cfg.log_level = DEFW2_LOG_ERROR;
	server_cfg.rpc_thread_count = 2;
	if (defw2_init(&server_cfg, &server_rt) != DEFW2_OK) {
		check("the providers start", false);
		return;
	}
	check("an echo provider and a QPM control provider bind",
	      defw2_service_create(server_rt, "victim-echo", DEFW2_API_ECHO,
				   DEFW2_PROVIDER_ECHO, &echo_svc) ==
	      DEFW2_OK && defw2_echo_bind(echo_svc, &echo_ops) == DEFW2_OK &&
	      defw2_service_create(server_rt, "victim-qpm", "qfw.qpm",
				   DEFW2_PROVIDER_QPM_CONTROL, &qpm_svc) ==
	      DEFW2_OK && defw2_qpm_control_bind(qpm_svc, &qpm_ops) ==
	      DEFW2_OK);
	check("and a document provider",
	      defw2_service_create(server_rt, "victim-doc", "qfw.test", 11,
				   &doc_svc) == DEFW2_OK &&
	      defw2_doc_bind(doc_svc, "qfw.test.doc", &doc_ops) == DEFW2_OK);
	check("and an event sink takes completions",
	      defw2_event_sink_create(server_rt, DEFW2_PROVIDER_EVENT, NULL,
				      &sink) == DEFW2_OK &&
	      defw2_qpm_event_accept(sink) == DEFW2_OK);

	/*
	 * A Margo instance of its own, so the hostile registrations of the
	 * methods' names cannot leak into the honest client's.
	 */
	evil = margo_init("na+sm://", MARGO_CLIENT_MODE, 0, 0);
	check("the hostile peer starts", evil != MARGO_INSTANCE_NULL);
	if (evil != MARGO_INSTANCE_NULL) {
		attack(evil, defw2_address(server_rt), &echo_target);
		attack(evil, defw2_address(server_rt), &qpm_target);
		attack(evil, defw2_address(server_rt), &sink_target);
		attack(evil, defw2_address(server_rt), &doc_target);

		/* The registration attack() made, rather than a second. */
		evil_target = &doc_target;
		id = 0;
		margo_provider_registered_name(evil, doc_target.rpc, 0, &id,
					       &found);
		margo_addr_lookup(evil, defw2_address(server_rt), &addr);
		evil_method = "_private";
		check("a document's method with an underscore first is refused",
		      evil_call(evil, addr, id, EVIL_METHOD, message,
				sizeof(message)) ==
		      DEFW2_CAT_INVALID_ARGUMENT &&
		      strstr(message, "identifier") != NULL);
		evil_method = NULL;
		check("so is a document with no method",
		      evil_call(evil, addr, id, EVIL_METHOD, message,
				sizeof(message)) ==
		      DEFW2_CAT_INVALID_ARGUMENT);
		if (addr != HG_ADDR_NULL)
			margo_addr_free(evil, addr);
	}
	check("no service ran for any of them",
	      __atomic_load_n(&served_calls, __ATOMIC_RELAXED) == 0);
	defw2_event_sink_stats(sink, &sink_stats);
	check("and the sink queued no event",
	      sink_stats.received == 0 && sink_stats.waiting == 0);

	memset(&client_cfg, 0, sizeof(client_cfg));
	client_cfg.address = "na+sm://";
	client_cfg.node_name = "honest-client";
	client_cfg.role = DEFW2_ROLE_CLIENT;
	client_cfg.log_level = DEFW2_LOG_ERROR;
	honest = defw2_init(&client_cfg, &client_rt) == DEFW2_OK &&
		 defw2_binding_create(client_rt, defw2_address(server_rt),
				      DEFW2_PROVIDER_ECHO, &echo) == DEFW2_OK &&
		 defw2_binding_create(client_rt, defw2_address(server_rt),
				      DEFW2_PROVIDER_QPM_CONTROL, &control) ==
		 DEFW2_OK &&
		 defw2_binding_create(client_rt, defw2_address(server_rt),
				      11, &doc) == DEFW2_OK;
	honest = honest &&
		 defw2_echo(echo, "hi", 2, &opts, &reply, &status) ==
		 DEFW2_OK && status.code == DEFW2_OK;
	honest = honest &&
		 defw2_qpm_is_ready(control, &req, &opts, &status_out,
				    &status) == DEFW2_OK &&
		 status.code == DEFW2_OK && status_out.ready;
	defw2_status_free(&status);
	honest = honest &&
		 defw2_doc_call(doc, "qfw.test.doc", "fine", "{}", &opts,
				&answer, &status) == DEFW2_OK &&
		 status.code == DEFW2_OK && answer != NULL &&
		 strcmp(answer, "{}") == 0;
	check("after all that an honest caller is served by all three",
	      honest && __atomic_load_n(&served_calls, __ATOMIC_RELAXED) == 3);
	free(answer);
	defw2_buffer_free(&reply);
	defw2_qpm_service_status_free(&status_out);
	defw2_status_free(&status);

	target.address = defw2_address(server_rt);
	honest = client_rt != NULL &&
		 defw2_event_publisher_create(client_rt, NULL, &publisher) ==
		 DEFW2_OK &&
		 defw2_qpm_publish_completion(publisher, &target, "completion",
					      &task, NULL) == DEFW2_OK &&
		 defw2_event_sink_next(sink, 10000, &event) == DEFW2_OK;
	got = defw2_qpm_event_task(&event);
	check("and an honest sender's event reaches the sink",
	      honest && got != NULL && got->cid != NULL &&
	      strcmp(got->cid, "honest-1") == 0);
	defw2_event_free(&event);
	defw2_event_publisher_destroy(publisher);
	defw2_event_sink_destroy(sink);

	defw2_binding_free(echo);
	defw2_binding_free(control);
	defw2_binding_free(doc);
	if (client_rt != NULL)
		defw2_finalize(client_rt);
	if (evil != MARGO_INSTANCE_NULL)
		margo_finalize(evil);
	defw2_service_shutdown(echo_svc);
	defw2_service_destroy(echo_svc);
	defw2_service_destroy(qpm_svc);
	defw2_service_destroy(doc_svc);
	defw2_finalize(server_rt);
}

int main(void)
{
	struct msg m = { 0 };
	margo_instance_id mid;
	hg_proc_t proc;

	mid = margo_init("na+sm://", MARGO_CLIENT_MODE, 0, 0);
	if (mid == MARGO_INSTANCE_NULL ||
	    hg_proc_create(margo_get_class(mid), HG_NOHASH, &proc) !=
	    HG_SUCCESS) {
		fprintf(stderr, "cannot set up a proc\n");
		return EXIT_FAILURE;
	}
	m.cap = 4096;
	m.data = malloc(m.cap);

	strings(proc, &m);
	counted(proc, &m);
	bulk(proc, &m);
	partial(proc, &m);
	events(proc, &m);

	hg_proc_free(proc);
	margo_finalize(mid);
	free(m.data);

	hostile();

	printf("\n%s\n", failures == 0 ? "wire smoke passed"
				       : "wire smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
