/*
 * Do the QPM's fifteen methods survive the wire?
 *
 * One process, two runtimes over na+sm: a fake QPM serving all three APIs on
 * their default providers, and a client that calls every method. The fake
 * answers each request with a fingerprint of everything the request carried,
 * built by the same function the test uses on what it sent, so a field that
 * is dropped, truncated or mangled anywhere on the way shows up as a
 * mismatch rather than going unnoticed.
 *
 * It also holds the result path to its promises. A 20-qubit statevector,
 * 16 MiB, is pushed into the caller's buffer and checked element by element.
 * A buffer that is too small gets the size it needs and leaves the completion
 * queued. A service whose answer is malformed or too large to send is
 * reported as a provider failure instead of leaving the caller waiting.
 *
 * And it holds completion events to theirs. The client listens, as one that
 * wants events must, and registers a sink in its own runtime for one
 * reservation's tasks, twice under two tags. The fake keeps the
 * registrations as a QPM does and publishes each completion it queues. Every
 * completion of that reservation reaches the sink once per tag, as the
 * record read_cq answers with, its statevector described and then fetched
 * by read_cq into a buffer of the size the event gave.
 *
 * Given a directory, both runtimes profile into it, which is how the spans
 * the shared typed code records get checked: defw2_otlp_check.py reads them
 * back as a second test.
 *
 * The fake and the checks also run apart, which is how the same checks hold
 * a QPM in another language to the same answers:
 *
 *	defw2_qpm_smoke --serve		serve the fake, print its address,
 *					and stop when stdin closes
 *	defw2_qpm_smoke --remote ADDR	run the checks against ADDR
 *
 * tests/defw2_qpm_fake.py is the same fake in Python, and the Python
 * checks in tests/defw2_qpm_client_check.py run against either.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2_qpm.h>

#define SV_QUBITS	20
#define SV_COUNT	(1ull << SV_QUBITS)
#define SV_BYTES	(SV_COUNT * 16ull)
#define FP_LEN		4096
#define BIG_EXTRA	(200u * 1024u)
#define BIG_CIRCUIT	(1024u * 1024u)
#define QUEUED		64
#define REGISTRATIONS	8
#define EVENT_WAIT_MS	10000

/* The W3C example, so the event's trace can be told from any other. */
#define TRACE_ID	"0af7651916cd43dd8448eb211c80319c"
#define TRACEPARENT	"00-" TRACE_ID "-b7ad6b7169203331-01"

static int failures;

/* Where checks report. A server keeps stdout for its address alone. */
static FILE *report;

static void check(const char *what, bool ok)
{
	fprintf(report, "%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

static const char *s_or(const char *s)
{
	return s != NULL ? s : "(null)";
}

/* String equality that a missing answer fails rather than crashes. */
static bool eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static uint64_t fnv1a(const void *data, size_t len)
{
	const unsigned char *p = data;
	uint64_t h = 1469598103934665603ull;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

/* --- fingerprints, used by both sides -------------------------------- */

static void fp_ctx(char *buf, const defw2_qpm_ctx_t *c)
{
	snprintf(buf, FP_LEN, "rid=%" PRIu64 " token=%s", c->reservation_id,
		 s_or(c->token));
}

static void fp_reserve(char *buf, const defw2_qpm_reserve_req_t *r)
{
	const defw2_qpm_task_class_t *t = &r->task_class;

	snprintf(buf, FP_LEN,
		 "rid=%" PRIu64 " token=%s req=%" PRIu64 " user=%s job=%s "
		 "alloc=%s dev=%s scope=%s kind=%s nq=%u wall=%" PRIu64
		 " ttl=%" PRIu64 " tc=%d/%" PRIu64 "/%u/%u/%" PRIu64 "/%"
		 PRIu64 "/%" PRIu64 "/%" PRIu64 " extra=%s",
		 r->ctx.reservation_id, s_or(r->ctx.token), r->request_id,
		 s_or(r->user), s_or(r->job_id), s_or(r->allocation_id),
		 s_or(r->target_device_id), s_or(r->scope_id),
		 s_or(r->workload_kind), r->num_qubits, r->walltime_ns,
		 r->ttl_ns, r->has_task_class, t->count, t->qubit_count,
		 t->depth, t->one_q_gate_count, t->two_q_gate_count, t->shots,
		 t->measurement_count, s_or(r->extra));
}

/*
 * A timeout counts only when has_timeout says so, so one that is not
 * flagged is not part of what a run carries.
 */
static void fp_run(char *buf, const defw2_qpm_run_req_t *r)
{
	snprintf(buf, FP_LEN,
		 "rid=%" PRIu64 " token=%s fmt=%s len=%zu sum=%016" PRIx64
		 " nq=%u shots=%u comp=%s sv=%d to=%d/%" PRIu64 " cot=%d "
		 "extra=%s",
		 r->ctx.reservation_id, s_or(r->ctx.token),
		 s_or(r->circuit.format), r->circuit.len,
		 fnv1a(r->circuit.data, r->circuit.len), r->num_qubits,
		 r->num_shots, s_or(r->compiler), r->return_statevector,
		 r->has_timeout, r->has_timeout ? r->timeout_ms : 0,
		 r->cancel_on_timeout, s_or(r->extra));
}

static void fp_task(char *buf, const defw2_qpm_task_req_t *r)
{
	snprintf(buf, FP_LEN,
		 "rid=%" PRIu64 " token=%s cid=%s qtask=%" PRIu64 " reason=%s",
		 r->ctx.reservation_id, s_or(r->ctx.token), s_or(r->cid),
		 r->qtask_id, s_or(r->reason));
}

static void fp_notify(char *buf, const defw2_qpm_notify_req_t *r)
{
	snprintf(buf, FP_LEN,
		 "rid=%" PRIu64 " token=%s addr=%s provider=%u tag=%s type=%s "
		 "extra=%s",
		 r->ctx.reservation_id, s_or(r->ctx.token),
		 s_or(r->target.address), r->target.provider_id,
		 s_or(r->target.tag), s_or(r->type), s_or(r->extra));
}

/* The statevector the fake produces: amplitude k is (k, -k). */
static void sv_fill(double *amps, uint64_t count)
{
	uint64_t k;

	for (k = 0; k < count; k++) {
		amps[2 * k] = (double)k;
		amps[2 * k + 1] = -(double)k;
	}
}

static bool sv_check(const double *amps, uint64_t count)
{
	uint64_t k;

	for (k = 0; k < count; k++)
		if (amps[2 * k] != (double)k || amps[2 * k + 1] != -(double)k)
			return false;
	return true;
}

/* --- the fake QPM ---------------------------------------------------- */

/*
 * One completion, queued by async_run and collected by read_cq. The
 * handlers run on Margo's handler pool, so the queue has a lock.
 */
struct completion {
	char		cid[32];
	uint64_t	qtask_id;
	uint32_t	num_qubits;
	bool		statevector;
	bool		queued;
};

/*
 * A registration for completion events, kept as a QPM keeps one. The
 * strings are copies, because a request lasts only as long as its call.
 */
struct registration {
	char		*address;
	uint16_t	provider_id;
	char		*tag;
	char		*type;
	uint64_t	reservation_id;
	bool		used;
};

static struct {
	pthread_mutex_t		lock;
	struct completion	done[QUEUED];
	uint64_t		next_qtask;
	char			big_extra[BIG_EXTRA];
	struct registration	regs[REGISTRATIONS];
	defw2_event_publisher_t	*pub;
} fake = { .lock = PTHREAD_MUTEX_INITIALIZER, .next_qtask = 1 };

static char *fingerprint(defw2_call_t *call, void (*fp)(char *, const void *),
			 const void *req)
{
	char buf[FP_LEN];

	fp(buf, req);
	return defw2_call_strdup(call, buf);
}

#define FP(call, fn, req) \
	fingerprint((call), (void (*)(char *, const void *))(fn), (req))

static defw2_rc_t fake_is_ready(void *ctx, defw2_call_t *call,
				const defw2_qpm_ctx_t *req,
				defw2_qpm_service_status_t *out)
{
	(void)ctx;
	out->state = "running";
	out->ready = true;
	out->initialized = true;
	out->accepting_requests = true;
	out->provider_ready = false;
	out->active_task_count = 3;
	out->active_reservation_count = 2;
	out->extra = FP(call, fp_ctx, req);
	return DEFW2_OK;
}

/* Larger than any string may be, to prove text is its own type. */
static defw2_rc_t fake_service_status(void *ctx, defw2_call_t *call,
				      const defw2_qpm_ctx_t *req,
				      defw2_qpm_service_status_t *out)
{
	(void)ctx;
	(void)req;
	out->state = defw2_call_strdup(call, "running");
	out->extra = fake.big_extra;
	return DEFW2_OK;
}

static defw2_rc_t fake_reserve(void *ctx, defw2_call_t *call,
			       const defw2_qpm_reserve_req_t *req,
			       defw2_qpm_decision_t *out)
{
	(void)ctx;
	out->decision = DEFW2_QPM_DECISION_ACCEPTED;
	out->reservation_id = 7001;
	out->request_id = req->request_id;
	out->reason = "accepted";
	out->reason_code = 1;
	out->retry_after_ns = 42;
	out->extra = FP(call, fp_reserve, req);
	return DEFW2_OK;
}

static defw2_rc_t fake_release(void *ctx, defw2_call_t *call,
			       const defw2_qpm_close_req_t *req,
			       defw2_qpm_decision_t *out)
{
	(void)ctx;
	(void)call;
	(void)out;
	/* A failure with no status of its own. */
	return req->reason_code == 99 ? DEFW2_ERR_INTERNAL : DEFW2_OK;
}

static defw2_rc_t fake_cancel(void *ctx, defw2_call_t *call,
			      const defw2_qpm_close_req_t *req,
			      defw2_qpm_decision_t *out)
{
	(void)ctx;
	(void)out;
	defw2_call_set_status(call, DEFW2_ERR_NOT_FOUND,
			      DEFW2_CAT_INVALID_RESERVATION,
			      req->ctx.reservation_id == 0 ?
				      "no reservation named" :
				      "no such reservation");
	return DEFW2_ERR_NOT_FOUND;
}

static defw2_rc_t fake_get_reservation(void *ctx, defw2_call_t *call,
				       const defw2_qpm_ctx_t *req,
				       defw2_qpm_reservation_t *out)
{
	(void)ctx;
	out->reservation_id = req->reservation_id;
	if (req->reservation_id == 666) {
		/* An answer too long to send. */
		char *state = defw2_call_alloc(call, DEFW2_STR_MAX + 1);

		if (state == NULL)
			return DEFW2_ERR_NOMEM;
		memset(state, 'x', DEFW2_STR_MAX);
		out->state = state;
		return DEFW2_OK;
	}
	out->state = "active";
	out->created_at_ns = 1000;
	out->expires_at_ns = 2000;
	out->extra = FP(call, fp_ctx, req);
	return DEFW2_OK;
}

static uint64_t sv_bytes(uint32_t num_qubits)
{
	return (1ull << num_qubits) * 16ull;
}

/* The statevector into the call, when it fits what was lent. */
static defw2_rc_t give_statevector(defw2_call_t *call, uint32_t num_qubits,
				   defw2_qpm_task_t *out, bool lie)
{
	uint64_t count = 1ull << num_qubits;
	void *data;

	defw2_tensor_vector(&out->statevector, DEFW2_DTYPE_C128, count);
	if (sv_bytes(num_qubits) > defw2_call_result_capacity(call))
		return DEFW2_OK;
	data = defw2_call_bulk_reply(call, sv_bytes(num_qubits));
	if (data == NULL)
		return DEFW2_ERR_NOMEM;
	sv_fill(data, count);
	/* A description that does not match what was handed over. */
	if (lie)
		out->statevector.shape[0] = count / 2;
	return DEFW2_OK;
}

/* --- completion events, kept and sent as a QPM does ------------------ */

static void registration_drop(struct registration *r)
{
	free(r->address);
	free(r->tag);
	free(r->type);
	memset(r, 0, sizeof(*r));
}

static bool copy_into(char **dst, const char *src)
{
	*dst = src != NULL ? strdup(src) : NULL;
	return src == NULL || *dst != NULL;
}

static defw2_rc_t fake_register(void *ctx, defw2_call_t *call,
				const defw2_qpm_notify_req_t *req,
				defw2_qpm_decision_t *out)
{
	struct registration *r = NULL;
	bool copied = false;
	int i;

	(void)ctx;
	pthread_mutex_lock(&fake.lock);
	for (i = 0; i < REGISTRATIONS && r == NULL; i++)
		if (!fake.regs[i].used)
			r = &fake.regs[i];
	if (r != NULL) {
		copied = copy_into(&r->address, req->target.address) &&
			 copy_into(&r->tag, req->target.tag) &&
			 copy_into(&r->type, req->type);
		r->provider_id = req->target.provider_id;
		r->reservation_id = req->ctx.reservation_id;
		r->used = copied;
		if (!copied)
			registration_drop(r);
	}
	pthread_mutex_unlock(&fake.lock);
	if (r == NULL) {
		defw2_call_set_status(call, DEFW2_ERR_BUSY,
				      DEFW2_CAT_PENDING_CAPACITY,
				      "the fake keeps no more registrations");
		return DEFW2_ERR_BUSY;
	}
	if (!copied)
		return DEFW2_ERR_NOMEM;
	out->decision = DEFW2_QPM_DECISION_ACCEPTED;
	out->reservation_id = req->ctx.reservation_id;
	out->extra = FP(call, fp_notify, req);
	return DEFW2_OK;
}

/*
 * Send a queued completion to every registration that may hear of it: one
 * under the run's reservation, or under none. Publishing returns at once,
 * so the lock is held only while the event is copied. A target the
 * publisher reports gone loses its registration, as v1 dropped one whose
 * put failed.
 */
static void publish_completion(defw2_call_t *call,
			       const defw2_qpm_run_req_t *req, const char *cid,
			       uint64_t qtask)
{
	defw2_qpm_task_t task;
	defw2_event_target_t target;
	char extra[FP_LEN];
	defw2_rc_t rc;
	int i;

	if (fake.pub == NULL)
		return;
	memset(&task, 0, sizeof(task));
	task.outcome = DEFW2_QPM_COMPLETED;
	task.lifecycle_state = "completed";
	task.cid = cid;
	task.qtask_id = qtask;
	task.reservation_id = req->ctx.reservation_id;
	task.completion_ready = true;
	if (req->return_statevector)
		defw2_tensor_vector(&task.statevector, DEFW2_DTYPE_C128,
				    1ull << req->num_qubits);
	fp_run(extra, req);
	task.extra = extra;

	pthread_mutex_lock(&fake.lock);
	for (i = 0; i < REGISTRATIONS; i++) {
		struct registration *r = &fake.regs[i];

		if (!r->used || (r->reservation_id != 0 &&
				 r->reservation_id != req->ctx.reservation_id))
			continue;
		target.address = r->address;
		target.provider_id = r->provider_id;
		target.tag = r->tag;
		rc = defw2_qpm_publish_completion(fake.pub, &target, r->type,
						  &task,
						  defw2_call_traceparent(call));
		if (defw2_event_target_gone(rc))
			registration_drop(r);
	}
	pthread_mutex_unlock(&fake.lock);
}

static defw2_rc_t fake_async_run(void *ctx, defw2_call_t *call,
				 const defw2_qpm_run_req_t *req,
				 defw2_qpm_task_t *out)
{
	struct completion *c = NULL;
	char cid[32];
	uint64_t qtask;
	int i;

	(void)ctx;
	pthread_mutex_lock(&fake.lock);
	for (i = 0; i < QUEUED; i++) {
		if (!fake.done[i].queued) {
			c = &fake.done[i];
			break;
		}
	}
	qtask = fake.next_qtask++;
	if (c != NULL) {
		snprintf(c->cid, sizeof(c->cid), "cid-%" PRIu64, qtask);
		c->qtask_id = qtask;
		c->num_qubits = req->num_qubits;
		c->statevector = req->return_statevector;
		c->queued = true;
		snprintf(cid, sizeof(cid), "%s", c->cid);
	}
	pthread_mutex_unlock(&fake.lock);
	if (c == NULL) {
		defw2_call_set_status(call, DEFW2_ERR_BUSY,
				      DEFW2_CAT_PENDING_CAPACITY,
				      "the fake's queue is full");
		return DEFW2_ERR_BUSY;
	}
	/* The fake completes a task as it queues it. */
	publish_completion(call, req, cid, qtask);
	out->outcome = DEFW2_QPM_ACCEPTED;
	out->lifecycle_state = "queued";
	out->cid = defw2_call_strdup(call, cid);
	out->qtask_id = qtask;
	out->reservation_id = req->ctx.reservation_id;
	out->extra = FP(call, fp_run, req);
	return DEFW2_OK;
}

static defw2_rc_t fake_sync_run(void *ctx, defw2_call_t *call,
				const defw2_qpm_run_req_t *req,
				defw2_qpm_task_t *out)
{
	(void)ctx;
	out->outcome = DEFW2_QPM_COMPLETED;
	out->cid = "cid-sync";
	out->extra = FP(call, fp_run, req);
	if (!req->return_statevector)
		return DEFW2_OK;
	return give_statevector(call, req->num_qubits, out, false);
}

static struct completion *find(const defw2_qpm_task_req_t *req)
{
	int i;

	for (i = 0; i < QUEUED; i++) {
		struct completion *c = &fake.done[i];

		if (!c->queued)
			continue;
		if (req->cid != NULL && strcmp(req->cid, c->cid) != 0)
			continue;
		if (req->qtask_id != 0 && req->qtask_id != c->qtask_id)
			continue;
		return c;
	}
	return NULL;
}

/*
 * read_cq and peek_cq. A completion whose statevector does not fit what was
 * lent stays queued, which is the contract the header asks of a service.
 */
static defw2_rc_t collect(defw2_call_t *call, const defw2_qpm_task_req_t *req,
			  defw2_qpm_task_t *out, bool consume)
{
	struct completion copy, *c;
	bool fits;

	pthread_mutex_lock(&fake.lock);
	c = find(req);
	if (c != NULL)
		copy = *c;
	fits = c != NULL && (!c->statevector ||
			     sv_bytes(c->num_qubits) <=
				     defw2_call_result_capacity(call));
	if (c != NULL && consume && fits)
		c->queued = false;
	pthread_mutex_unlock(&fake.lock);

	if (c == NULL) {
		out->outcome = DEFW2_QPM_IN_PROGRESS;
		out->completion_ready = false;
		out->message = FP(call, fp_task, req);
		return DEFW2_OK;
	}
	out->outcome = DEFW2_QPM_COMPLETED;
	out->completion_ready = true;
	out->cid = defw2_call_strdup(call, copy.cid);
	out->qtask_id = copy.qtask_id;
	out->message = FP(call, fp_task, req);
	if (!copy.statevector)
		return DEFW2_OK;
	return give_statevector(call, copy.num_qubits, out,
				req->reason != NULL &&
				strcmp(req->reason, "lie") == 0);
}

static defw2_rc_t fake_read_cq(void *ctx, defw2_call_t *call,
			       const defw2_qpm_task_req_t *req,
			       defw2_qpm_task_t *out)
{
	(void)ctx;
	return collect(call, req, out, true);
}

static defw2_rc_t fake_peek_cq(void *ctx, defw2_call_t *call,
			       const defw2_qpm_task_req_t *req,
			       defw2_qpm_task_t *out)
{
	(void)ctx;
	return collect(call, req, out, false);
}

static defw2_rc_t fake_task_answer(void *ctx, defw2_call_t *call,
				   const defw2_qpm_task_req_t *req,
				   defw2_qpm_task_t *out)
{
	(void)ctx;
	out->outcome = req->reason != NULL ? DEFW2_QPM_CANCELLED
					   : DEFW2_QPM_ACCEPTED;
	out->cid = req->cid;
	out->qtask_id = req->qtask_id;
	out->message = FP(call, fp_task, req);
	return DEFW2_OK;
}

/* --- the client side ------------------------------------------------- */

static defw2_call_opts_t opts = { .timeout_ms = 20000 };

static void control_calls(defw2_binding_t *control)
{
	defw2_qpm_ctx_t req = { .reservation_id = 17, .token = "tok-17" };
	defw2_qpm_service_status_t out = { 0 };
	defw2_status_t status = { 0 };
	char expect[FP_LEN];

	fp_ctx(expect, &req);
	check("is_ready answers",
	      defw2_qpm_is_ready(control, &req, &opts, &out, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK);
	check("is_ready's typed fields cross the wire",
	      out.state != NULL && eq(out.state, "running") &&
	      out.ready && out.initialized && out.accepting_requests &&
	      !out.provider_ready && out.active_task_count == 3 &&
	      out.active_reservation_count == 2);
	check("is_ready's request arrived whole",
	      out.extra != NULL && eq(out.extra, expect));
	defw2_qpm_service_status_free(&out);
	check("a freed answer is empty", out.state == NULL &&
	      out.arena == NULL);

	check("get_service_status answers",
	      defw2_qpm_get_service_status(control, &req, &opts, &out,
					   &status) == DEFW2_OK &&
	      status.code == DEFW2_OK);
	check("a 200 KiB extra, longer than any string, arrives intact",
	      out.extra != NULL && strlen(out.extra) == BIG_EXTRA - 1 &&
	      memcmp(out.extra, fake.big_extra, BIG_EXTRA) == 0);
	defw2_qpm_service_status_free(&out);
	defw2_status_free(&status);
}

static void admission_calls(defw2_binding_t *admission)
{
	defw2_qpm_reserve_req_t reserve = {
		.ctx = { .reservation_id = 0, .token = "munge-cred" },
		.request_id = 0x1234567890abcdefull,
		.user = "doug",
		.job_id = "slurm-4242",
		.allocation_id = "alloc-1",
		.target_device_id = "fake-iqm-20q",
		.scope_id = NULL,
		.workload_kind = "quantum",
		.num_qubits = 20,
		.walltime_ns = 300000000000ull,
		.ttl_ns = 360000000000ull,
		.has_task_class = true,
		.task_class = { 4, 20, 10, 100, 50, 1024, 20 },
		.extra = "{\"workload\":{\"example\":\"w5\"},\"run_context\":{}}",
	};
	defw2_qpm_close_req_t close = { .ctx = { .reservation_id = 7001 } };
	defw2_qpm_renew_req_t renew = { .ctx = { .reservation_id = 7001 } };
	defw2_qpm_ctx_t get = { .reservation_id = 7001, .token = "t" };
	defw2_qpm_decision_t decision = { 0 };
	defw2_qpm_reservation_t reservation = { 0 };
	defw2_status_t status = { 0 };
	char expect[FP_LEN];

	fp_reserve(expect, &reserve);
	check("reserve answers",
	      defw2_qpm_reserve(admission, &reserve, &opts, &decision,
				&status) == DEFW2_OK &&
	      status.code == DEFW2_OK);
	check("reserve's decision crosses the wire",
	      decision.decision != NULL &&
	      eq(decision.decision, DEFW2_QPM_DECISION_ACCEPTED) &&
	      decision.reservation_id == 7001 &&
	      decision.request_id == reserve.request_id &&
	      decision.reason_code == 1 && decision.retry_after_ns == 42 &&
	      decision.message == NULL);
	check("every reserve field arrived, an absent one as absent",
	      decision.extra != NULL && eq(decision.extra, expect));
	defw2_qpm_decision_free(&decision);

	check("an unserved method is not found",
	      defw2_qpm_renew(admission, &renew, &opts, &decision,
			      &status) == DEFW2_OK &&
	      status.category == DEFW2_CAT_NOT_FOUND);
	defw2_qpm_decision_free(&decision);

	check("a service's own status category reaches the caller",
	      defw2_qpm_cancel(admission, &close, &opts, &decision,
			       &status) == DEFW2_OK &&
	      status.category == DEFW2_CAT_INVALID_RESERVATION &&
	      status.message != NULL &&
	      eq(status.message, "no such reservation"));
	check("and a failed answer carries no fields",
	      decision.decision == NULL && decision.reservation_id == 0);
	defw2_qpm_decision_free(&decision);

	close.reason_code = 99;
	check("a failure with no status is a provider failure",
	      defw2_qpm_release(admission, &close, &opts, &decision,
				&status) == DEFW2_OK &&
	      status.category == DEFW2_CAT_PROVIDER_FAILURE);
	defw2_qpm_decision_free(&decision);

	fp_ctx(expect, &get);
	check("get_reservation answers",
	      defw2_qpm_get_reservation(admission, &get, &opts, &reservation,
					&status) == DEFW2_OK &&
	      status.code == DEFW2_OK && reservation.reservation_id == 7001 &&
	      reservation.state != NULL &&
	      eq(reservation.state, "active") &&
	      reservation.created_at_ns == 1000 &&
	      reservation.expires_at_ns == 2000 && reservation.extra != NULL &&
	      eq(reservation.extra, expect));
	defw2_qpm_reservation_free(&reservation);

	get.reservation_id = 666;
	check("an answer too long to send fails rather than hangs",
	      defw2_qpm_get_reservation(admission, &get, &opts, &reservation,
					&status) == DEFW2_OK &&
	      status.category == DEFW2_CAT_PROVIDER_FAILURE &&
	      reservation.state == NULL);
	defw2_qpm_reservation_free(&reservation);
	defw2_status_free(&status);
}

static void execution_calls(defw2_binding_t *execution)
{
	static const char qasm[] = "OPENQASM 2.0;\ninclude \"qelib1.inc\";\n"
				   "qreg q[2];\nh q[0];\ncx q[0],q[1];\n";
	defw2_qpm_run_req_t run = {
		.ctx = { .reservation_id = 7001, .token = "tok" },
		.circuit = { DEFW2_QPM_FORMAT_OPENQASM2, qasm,
			     sizeof(qasm) - 1 },
		.num_qubits = 2,
		.num_shots = 1024,
		.compiler = "staq",
		.has_timeout = true,
		.timeout_ms = 0,
		.cancel_on_timeout = true,
		.extra = "{\"qubit_mapping\":{\"0\":\"QB1\",\"1\":\"QB2\"}}",
	};
	defw2_qpm_task_req_t ref = { .ctx = { .reservation_id = 7001 } };
	defw2_result_buffer_t small = { 0 }, whole = { 0 };
	defw2_qpm_task_t task = { 0 };
	defw2_status_t status = { 0 };
	unsigned char *binary;
	char expect[FP_LEN];
	char cid[32] = "";
	size_t i;

	fp_run(expect, &run);
	check("async_run answers",
	      defw2_qpm_async_run(execution, &run, &opts, &task, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK &&
	      eq(task.outcome, DEFW2_QPM_ACCEPTED) &&
	      task.cid != NULL && task.qtask_id != 0);
	check("every run field arrived, a zero timeout as a timeout",
	      task.extra != NULL && eq(task.extra, expect));
	defw2_qpm_task_free(&task);

	/*
	 * A binary circuit, large enough that Mercury moves it outside the
	 * eager message, with NUL bytes that a string would stop at.
	 */
	binary = malloc(BIG_CIRCUIT);
	for (i = 0; i < BIG_CIRCUIT; i++)
		binary[i] = (unsigned char)(i * 31u);
	run.circuit.format = DEFW2_QPM_FORMAT_QPY;
	run.circuit.data = binary;
	run.circuit.len = BIG_CIRCUIT;
	/* An unflagged timeout is not sent, whatever its value. */
	run.has_timeout = false;
	run.timeout_ms = 777;
	run.extra = NULL;
	fp_run(expect, &run);
	check("a 1 MiB binary circuit arrives byte for byte",
	      defw2_qpm_async_run(execution, &run, &opts, &task, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK && task.extra != NULL &&
	      eq(task.extra, expect));
	defw2_qpm_task_free(&task);
	free(binary);

	run.circuit.format = NULL;
	run.circuit.data = NULL;
	run.circuit.len = 0;
	check("a run with no circuit is an invalid argument",
	      defw2_qpm_async_run(execution, &run, &opts, &task, &status) ==
	      DEFW2_OK && status.category == DEFW2_CAT_INVALID_ARGUMENT);
	defw2_qpm_task_free(&task);

	/* --- the result path --- */
	whole.capacity = SV_BYTES;
	whole.data = malloc(SV_BYTES);
	small.capacity = 1024;
	small.data = malloc(small.capacity);

	run.circuit.format = DEFW2_QPM_FORMAT_OPENQASM2;
	run.circuit.data = qasm;
	run.circuit.len = sizeof(qasm) - 1;
	run.num_qubits = SV_QUBITS;
	run.return_statevector = true;
	memset(whole.data, 0, SV_BYTES);
	check("sync_run pushes a 20-qubit statevector",
	      defw2_qpm_sync_run(execution, &run, &whole, &opts, &task,
				 &status) == DEFW2_OK &&
	      status.code == DEFW2_OK && task.statevector_delivered &&
	      task.statevector.dtype == DEFW2_DTYPE_C128 &&
	      task.statevector.rank == 1 &&
	      task.statevector.shape[0] == SV_COUNT &&
	      task.statevector.nbytes == SV_BYTES);
	check("and all 16 MiB of it is right",
	      sv_check(whole.data, SV_COUNT));
	defw2_qpm_task_free(&task);

	check("sync_run with no buffer says how much it needed",
	      defw2_qpm_sync_run(execution, &run, NULL, &opts, &task,
				 &status) == DEFW2_OK &&
	      status.code == DEFW2_OK && !task.statevector_delivered &&
	      task.statevector.nbytes == SV_BYTES);
	defw2_qpm_task_free(&task);

	if (defw2_qpm_async_run(execution, &run, &opts, &task, &status) ==
	    DEFW2_OK && task.cid != NULL)
		snprintf(cid, sizeof(cid), "%s", task.cid);
	defw2_qpm_task_free(&task);
	ref.cid = cid;

	check("read_cq into a buffer that is too small",
	      defw2_qpm_read_cq(execution, &ref, &small, &opts, &task,
				&status) == DEFW2_OK &&
	      status.code == DEFW2_OK && task.completion_ready &&
	      !task.statevector_delivered &&
	      task.statevector.nbytes == SV_BYTES);
	defw2_qpm_task_free(&task);

	check("peek_cq still sees it, so nothing was consumed",
	      defw2_qpm_peek_cq(execution, &ref, NULL, &opts, &task,
				&status) == DEFW2_OK &&
	      task.completion_ready && eq(task.cid, cid));
	defw2_qpm_task_free(&task);

	ref.reason = "lie";
	check("a statevector that contradicts its description is refused",
	      defw2_qpm_peek_cq(execution, &ref, &whole, &opts, &task,
				&status) == DEFW2_OK &&
	      status.category == DEFW2_CAT_PROVIDER_FAILURE &&
	      !task.statevector_delivered);
	defw2_qpm_task_free(&task);
	ref.reason = NULL;

	memset(whole.data, 0, SV_BYTES);
	check("the retry with a big enough buffer gets it",
	      defw2_qpm_read_cq(execution, &ref, &whole, &opts, &task,
				&status) == DEFW2_OK &&
	      status.code == DEFW2_OK && task.statevector_delivered &&
	      task.statevector.nbytes == SV_BYTES &&
	      sv_check(whole.data, SV_COUNT));
	defw2_qpm_task_free(&task);

	check("and then it is gone",
	      defw2_qpm_read_cq(execution, &ref, &whole, &opts, &task,
				&status) == DEFW2_OK &&
	      !task.completion_ready && !task.statevector_delivered);
	defw2_qpm_task_free(&task);

	/* --- the rest of the task methods --- */
	ref.cid = "cid-x";
	ref.qtask_id = 9;
	fp_task(expect, &ref);
	check("task_status names its task both ways",
	      defw2_qpm_task_status(execution, &ref, &opts, &task,
				    &status) == DEFW2_OK &&
	      status.code == DEFW2_OK && task.qtask_id == 9 &&
	      eq(task.cid, "cid-x") &&
	      eq(task.message, expect));
	defw2_qpm_task_free(&task);

	ref.reason = "user-requested";
	fp_task(expect, &ref);
	check("cancel_task carries its reason",
	      defw2_qpm_cancel_task(execution, &ref, &opts, &task,
				    &status) == DEFW2_OK &&
	      eq(task.outcome, DEFW2_QPM_CANCELLED) &&
	      eq(task.message, expect));
	defw2_qpm_task_free(&task);

	ref.reason = NULL;
	ref.qtask_id = 0;
	fp_task(expect, &ref);
	check("delete_circuit answers",
	      defw2_qpm_delete_circuit(execution, &ref, &opts, &task,
				       &status) == DEFW2_OK &&
	      status.code == DEFW2_OK && eq(task.message, expect));
	defw2_qpm_task_free(&task);

	free(small.data);
	free(whole.data);
	defw2_status_free(&status);
}

/*
 * Completion events. Two registrations of one sink, told apart by their
 * tags, for reservation 7002. A run under 7001 goes first, so an event for
 * it would arrive ahead of the 7002 run's on each tag.
 */
static void event_calls(defw2_rt_t *rt, defw2_binding_t *execution)
{
	static const char qasm[] = "OPENQASM 2.0;\nqreg q[3];\nh q[0];\n";
	defw2_qpm_notify_req_t notify = {
		.ctx = { .reservation_id = 7002, .token = "tok-ev" },
		.target = { NULL, DEFW2_PROVIDER_EVENT, "c-tag" },
		.type = "circuit-result",
		.extra = "{\"filters\":{\"user\":\"doug\"}}",
	};
	defw2_qpm_run_req_t run = {
		.ctx = { .reservation_id = 7001 },
		.circuit = { DEFW2_QPM_FORMAT_OPENQASM2, qasm,
			     sizeof(qasm) - 1 },
		.num_qubits = 3,
		.num_shots = 64,
		.return_statevector = true,
	};
	defw2_call_opts_t traced = { .timeout_ms = 20000,
				     .traceparent = TRACEPARENT };
	defw2_qpm_task_req_t ref = { .ctx = { .reservation_id = 7002 } };
	const defw2_event_t *mine = NULL, *other = NULL;
	defw2_qpm_decision_t decision = { 0 };
	defw2_qpm_task_t task = { 0 }, done = { 0 };
	defw2_result_buffer_t sv = { 0 };
	defw2_event_sink_t *sink = NULL;
	defw2_status_t status = { 0 };
	const defw2_qpm_task_t *t;
	defw2_event_t ev[3];
	char expect[FP_LEN];
	char cid[32] = "";
	uint64_t qtask = 0;
	int i, got = 0;

	check("a listening client serves a sink for completions",
	      defw2_event_sink_create(rt, DEFW2_PROVIDER_EVENT, NULL,
				      &sink) == DEFW2_OK &&
	      defw2_qpm_event_accept(sink) == DEFW2_OK);
	if (sink == NULL)
		return;
	notify.target.address = defw2_event_sink_address(sink);

	fp_notify(expect, &notify);
	check("register_event_notification is accepted",
	      defw2_qpm_register_event_notification(execution, &notify, &opts,
						    &decision, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK &&
	      eq(decision.decision, DEFW2_QPM_DECISION_ACCEPTED) &&
	      decision.reservation_id == 7002);
	check("every registration field arrived",
	      decision.extra != NULL && eq(decision.extra, expect));
	defw2_qpm_decision_free(&decision);

	notify.target.tag = "c-other";
	notify.type = "other";
	notify.extra = NULL;
	check("and the same sink again under another tag",
	      defw2_qpm_register_event_notification(execution, &notify, &opts,
						    &decision, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK &&
	      eq(decision.decision, DEFW2_QPM_DECISION_ACCEPTED));
	defw2_qpm_decision_free(&decision);

	check("a run under another reservation is accepted",
	      defw2_qpm_async_run(execution, &run, &opts, &task, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK);
	defw2_qpm_task_free(&task);
	run.ctx.reservation_id = 7002;
	check("and so is one under the registered reservation",
	      defw2_qpm_async_run(execution, &run, &traced, &task, &status) ==
	      DEFW2_OK && status.code == DEFW2_OK && task.cid != NULL);
	if (task.cid != NULL)
		snprintf(cid, sizeof(cid), "%s", task.cid);
	qtask = task.qtask_id;
	defw2_qpm_task_free(&task);

	memset(ev, 0, sizeof(ev));
	for (i = 0; i < 2; i++)
		if (defw2_event_sink_next(sink, EVENT_WAIT_MS, &ev[got]) ==
		    DEFW2_OK)
			got++;
	for (i = 0; i < got; i++) {
		if (eq(ev[i].tag, "c-tag"))
			mine = &ev[i];
		else if (eq(ev[i].tag, "c-other"))
			other = &ev[i];
	}
	check("its completion reaches the sink once per registration",
	      mine != NULL && other != NULL);
	check("and the other reservation's never does",
	      mine != NULL && mine->seq == 1 && other != NULL &&
	      other->seq == 1 &&
	      defw2_event_sink_next(sink, 250, &ev[2]) == DEFW2_ERR_TIMEOUT);
	check("each event says what it is and whose it is",
	      mine != NULL && eq(mine->api, DEFW2_API_QPM_EXECUTION) &&
	      eq(mine->name, DEFW2_QPM_EVENT_COMPLETION) &&
	      eq(mine->type, "circuit-result") && mine->source != NULL &&
	      mine->source[0] != '\0' && other != NULL &&
	      eq(other->type, "other"));

	t = defw2_qpm_event_task(mine);
	fp_run(expect, &run);
	check("it carries the task read_cq answers with",
	      t != NULL && eq(t->outcome, DEFW2_QPM_COMPLETED) &&
	      eq(t->lifecycle_state, "completed") && eq(t->cid, cid) &&
	      t->qtask_id == qtask && t->reservation_id == 7002 &&
	      t->completion_ready && t->reason == NULL &&
	      t->message == NULL && eq(t->extra, expect));
	check("and describes its statevector without carrying it",
	      t != NULL && !t->statevector_delivered &&
	      t->statevector.dtype == DEFW2_DTYPE_C128 &&
	      t->statevector.rank == 1 && t->statevector.shape[0] == 8 &&
	      t->statevector.nbytes == 128);
	check("the event joins the run's trace",
	      mine != NULL && mine->traceparent != NULL &&
	      strlen(mine->traceparent) == strlen(TRACEPARENT) &&
	      strncmp(mine->traceparent + 3, TRACE_ID,
		      strlen(TRACE_ID)) == 0);

	/* What the event described is what a collecting read needs. */
	sv.capacity = t != NULL ? t->statevector.nbytes : 0;
	sv.data = calloc(1, sv.capacity + 1);
	ref.cid = cid;
	check("read_cq into a buffer of the size it gave collects it",
	      defw2_qpm_read_cq(execution, &ref, &sv, &opts, &done,
				&status) == DEFW2_OK &&
	      status.code == DEFW2_OK && done.statevector_delivered &&
	      sv.capacity == 128 && sv_check(sv.data, 8));
	defw2_qpm_task_free(&done);
	free(sv.data);
	for (i = 0; i < 3; i++)
		defw2_event_free(&ev[i]);

	notify.target.address = NULL;
	check("a registration that names no sink is refused",
	      defw2_qpm_register_event_notification(execution, &notify, &opts,
						    &decision, &status) ==
	      DEFW2_OK && status.category == DEFW2_CAT_INVALID_ARGUMENT);
	defw2_qpm_decision_free(&decision);
	notify.target.address = defw2_event_sink_address(sink);
	notify.target.provider_id = 0;
	check("and so is one on the directory's provider",
	      defw2_qpm_register_event_notification(execution, &notify, &opts,
						    &decision, &status) ==
	      DEFW2_OK && status.category == DEFW2_CAT_INVALID_ARGUMENT);
	defw2_qpm_decision_free(&decision);
	defw2_status_free(&status);

	/* The service hears it is gone on its next completion for 7002. */
	defw2_event_sink_destroy(sink);
}

/* Serve until whoever started us closes our stdin. */
static void serve_until_stdin_closes(defw2_rt_t *rt)
{
	char discard[256];

	printf("%s\n", defw2_address(rt));
	fflush(stdout);
	while (fread(discard, 1, sizeof(discard), stdin) > 0)
		;
}

int main(int argc, char **argv)
{
	const char *telemetry = NULL;
	const char *remote = NULL;
	bool serve = false;
	int i;
	static const defw2_qpm_control_ops_t control_ops = {
		.is_ready = fake_is_ready,
		.get_service_status = fake_service_status,
	};
	static const defw2_qpm_admission_ops_t admission_ops = {
		.reserve = fake_reserve,
		.release = fake_release,
		.cancel = fake_cancel,
		.get_reservation = fake_get_reservation,
	};
	static const defw2_qpm_execution_ops_t execution_ops = {
		.async_run = fake_async_run,
		.sync_run = fake_sync_run,
		.read_cq = fake_read_cq,
		.peek_cq = fake_peek_cq,
		.task_status = fake_task_answer,
		.cancel_task = fake_task_answer,
		.delete_circuit = fake_task_answer,
		.register_event_notification = fake_register,
	};
	defw2_config_t server_cfg, client_cfg;
	defw2_rt_t *server_rt = NULL, *client_rt = NULL;
	defw2_service_t *control_svc = NULL, *admission_svc = NULL;
	defw2_service_t *execution_svc = NULL;
	defw2_binding_t *control = NULL, *admission = NULL;
	defw2_binding_t *execution = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--serve") == 0) {
			serve = true;
		} else if (strcmp(argv[i], "--remote") == 0 && i + 1 < argc) {
			remote = argv[++i];
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: defw2_qpm_smoke [telemetry-dir] "
				"[--serve | --remote address]\n");
			return EXIT_FAILURE;
		} else {
			telemetry = argv[i];
		}
	}

	report = serve ? stderr : stdout;
	memset(fake.big_extra, 'e', BIG_EXTRA - 1);
	fake.big_extra[BIG_EXTRA - 1] = '\0';
	if (telemetry != NULL)
		setenv("DEFW2_TELEMETRY_DIR", telemetry, 1);
	if (remote != NULL)
		goto client;

	memset(&server_cfg, 0, sizeof(server_cfg));
	server_cfg.address = "na+sm://";
	server_cfg.node_name = "fake-qpm";
	server_cfg.role = DEFW2_ROLE_SERVER;
	server_cfg.log_level = DEFW2_LOG_ERROR;
	server_cfg.rpc_thread_count = 2;
	server_cfg.profile = telemetry != NULL;
	if (defw2_init(&server_cfg, &server_rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the QPM runtime\n");
		return EXIT_FAILURE;
	}

	/* One service identity, three providers, one per API. */
	check("the three QPM providers are created",
	      defw2_service_create(server_rt, "qpm:fake:fake-20q", "qfw.qpm",
				   DEFW2_PROVIDER_QPM_CONTROL,
				   &control_svc) == DEFW2_OK &&
	      defw2_service_create(server_rt, "qpm:fake:fake-20q", "qfw.qpm",
				   DEFW2_PROVIDER_QPM_ADMISSION,
				   &admission_svc) == DEFW2_OK &&
	      defw2_service_create(server_rt, "qpm:fake:fake-20q", "qfw.qpm",
				   DEFW2_PROVIDER_QPM_EXECUTION,
				   &execution_svc) == DEFW2_OK);
	check("and each binds its API",
	      defw2_qpm_control_bind(control_svc, &control_ops) == DEFW2_OK &&
	      defw2_qpm_admission_bind(admission_svc, &admission_ops) ==
	      DEFW2_OK &&
	      defw2_qpm_execution_bind(execution_svc, &execution_ops) ==
	      DEFW2_OK);
	check("the fake publishes completion events",
	      defw2_event_publisher_create(server_rt, NULL, &fake.pub) ==
	      DEFW2_OK);

	if (serve) {
		serve_until_stdin_closes(server_rt);
		goto server_down;
	}

client:
	/* A server, because the sink its completion events come to is a
	 * provider in its own runtime. */
	memset(&client_cfg, 0, sizeof(client_cfg));
	client_cfg.address = "na+sm://";
	client_cfg.node_name = "qpm-client";
	client_cfg.role = DEFW2_ROLE_SERVER;
	client_cfg.log_level = DEFW2_LOG_ERROR;
	client_cfg.profile = telemetry != NULL;
	if (defw2_init(&client_cfg, &client_rt) != DEFW2_OK) {
		fprintf(stderr, "cannot start the client runtime\n");
		return EXIT_FAILURE;
	}
	if (remote == NULL)
		remote = defw2_address(server_rt);
	check("the client binds all three",
	      defw2_binding_create(client_rt, remote,
				   DEFW2_PROVIDER_QPM_CONTROL, &control) ==
	      DEFW2_OK &&
	      defw2_binding_create(client_rt, remote,
				   DEFW2_PROVIDER_QPM_ADMISSION,
				   &admission) == DEFW2_OK &&
	      defw2_binding_create(client_rt, remote,
				   DEFW2_PROVIDER_QPM_EXECUTION,
				   &execution) == DEFW2_OK);

	if (failures == 0) {
		control_calls(control);
		admission_calls(admission);
		execution_calls(execution);
		event_calls(client_rt, execution);
	}

	/* A method on the wrong provider is answered, not lost. */
	{
		defw2_qpm_ctx_t req = { 0 };
		defw2_qpm_service_status_t out = { 0 };
		defw2_status_t status = { 0 };

		defw2_call_opts_t quick = { .timeout_ms = 5000 };
		defw2_rc_t rc;

		/*
		 * Nothing registered is_ready on the execution provider, so
		 * Mercury refuses it before any handler runs. That reaches
		 * the caller as a transport-level not found.
		 */
		rc = defw2_qpm_is_ready(execution, &req, &quick, &out,
					&status);
		check("a QPM method on another API's provider is not found",
		      rc == DEFW2_ERR_NOT_FOUND ||
		      (rc == DEFW2_OK &&
		       status.category == DEFW2_CAT_NOT_FOUND));
		defw2_qpm_service_status_free(&out);
		defw2_status_free(&status);
	}

	defw2_binding_free(control);
	defw2_binding_free(admission);
	defw2_binding_free(execution);
	defw2_finalize(client_rt);
	if (server_rt == NULL)
		goto done;
server_down:
	defw2_service_shutdown(control_svc);
	defw2_service_destroy(control_svc);
	defw2_service_destroy(admission_svc);
	defw2_service_destroy(execution_svc);
	/* No handler is left to publish, so the publisher can go. */
	defw2_event_publisher_destroy(fake.pub);
	fake.pub = NULL;
	for (i = 0; i < REGISTRATIONS; i++)
		registration_drop(&fake.regs[i]);
	defw2_finalize(server_rt);
	if (serve)
		return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
done:
	printf("\n%s\n", failures == 0 ? "qpm smoke passed"
				       : "qpm smoke FAILED");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
