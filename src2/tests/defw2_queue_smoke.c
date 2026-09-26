/*
 * Can a thread the runtime knows nothing about serve an RPC?
 *
 * That is the whole of the Python service design, so it is worth proving
 * without Python in the way. A plain pthread drains the call queue and
 * answers, while the Margo handler parks on an eventual and the client
 * waits for its reply. The consumer reverses the payload, so a reply that
 * came back unchanged would mean the built-in C echo answered instead.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2_echo.h>

#define CALLS		200
#define FAIL_AT		7
#define PAYLOAD_LEN	64

static int failures;
static long served;

static void check(const char *what, bool ok)
{
	printf("%-36s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
}

/* An ordinary thread. It never touches Margo, only the queue. */
static void *consume(void *arg)
{
	defw2_service_t *svc = arg;

	for (;;) {
		defw2_call_t *call = NULL;
		unsigned char reply[PAYLOAD_LEN];
		const unsigned char *request;
		size_t len, i;
		defw2_rc_t rc;

		rc = defw2_service_next_call(svc, 500, &call);
		if (rc == DEFW2_ERR_NOT_FOUND)
			break;		/* the queue closed */
		if (rc != DEFW2_OK)
			continue;	/* nothing arrived in time */

		request = defw2_call_request(call, &len);
		if (served == FAIL_AT) {
			/* A service saying no is part of the contract. */
			defw2_service_fail(call, DEFW2_ERR_INVALID,
					   DEFW2_CAT_INVALID_ARGUMENT,
					   "the consumer refused this one");
			served++;
			continue;
		}
		if (len > sizeof(reply))
			len = sizeof(reply);
		for (i = 0; i < len; i++)
			reply[i] = request[len - 1 - i];
		defw2_service_respond(call, reply, len);
		served++;
	}
	return NULL;
}

int main(void)
{
	defw2_call_opts_t opts = { .timeout_ms = 10000 };
	defw2_config_t server_cfg, client_cfg;
	defw2_rt_t *server = NULL, *client = NULL;
	defw2_service_t *svc = NULL;
	defw2_binding_t *echo = NULL;
	unsigned char payload[PAYLOAD_LEN];
	pthread_t consumer;
	long reversed = 0, refused = 0;
	size_t i;
	int call;

	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (unsigned char)(i * 7 + 1);

	defw2_config_from_env(&server_cfg);
	server_cfg.role = DEFW2_ROLE_SERVER;
	if (defw2_init(&server_cfg, &server) != DEFW2_OK) {
		printf("server init FAILED\n");
		return 1;
	}
	check("service", defw2_service_create(server, "queued-echo",
					      DEFW2_API_ECHO,
					      DEFW2_PROVIDER_ECHO,
					      &svc) == DEFW2_OK);
	check("queue opens",
	      defw2_service_queue_open(svc, 0) == DEFW2_OK);
	check("queue refuses a second open",
	      defw2_service_queue_open(svc, 0) == DEFW2_ERR_INVALID);
	check("the service says it is queued", defw2_service_queued(svc));
	check("echo bound", defw2_echo_bind(svc, NULL) == DEFW2_OK);
	check("consumer starts",
	      pthread_create(&consumer, NULL, consume, svc) == 0);

	defw2_config_from_env(&client_cfg);
	client_cfg.role = DEFW2_ROLE_CLIENT;
	if (defw2_init(&client_cfg, &client) != DEFW2_OK) {
		printf("client init FAILED\n");
		return 1;
	}
	check("bind", defw2_binding_create(client, defw2_service_address(svc),
					   DEFW2_PROVIDER_ECHO,
					   &echo) == DEFW2_OK);

	for (call = 0; call < CALLS; call++) {
		defw2_buffer_t reply = { 0 };
		defw2_status_t status = { 0 };
		bool ok = true;

		if (defw2_echo(echo, payload, sizeof(payload), &opts, &reply,
			       &status) != DEFW2_OK) {
			failures++;
			break;
		}
		if (status.category == DEFW2_CAT_INVALID_ARGUMENT) {
			refused++;
		} else if (reply.len == sizeof(payload)) {
			for (i = 0; i < sizeof(payload); i++) {
				if (((unsigned char *)reply.data)[i] !=
				    payload[sizeof(payload) - 1 - i]) {
					ok = false;
					break;
				}
			}
			if (ok)
				reversed++;
		}
		defw2_buffer_free(&reply);
		defw2_status_free(&status);
	}

	check("every call was answered by the consumer",
	      reversed == CALLS - 1 && refused == 1);
	check("the consumer saw them all", served == CALLS);

	/* Closing releases the consumer, which is how a serving loop ends. */
	defw2_service_queue_close(svc);
	check("the consumer stops", pthread_join(consumer, NULL) == 0);
	check("a closed queue is no longer queued", !defw2_service_queued(svc));

	defw2_binding_free(echo);
	defw2_finalize(client);
	defw2_service_destroy(svc);
	defw2_finalize(server);

	printf("%ld reversed, %ld refused\n", reversed, refused);
	printf("%s\n", failures ? "QUEUE SMOKE FAILED" : "QUEUE SMOKE PASSED");
	return failures ? 1 : 0;
}
