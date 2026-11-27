/*
 * Does a typed call go out, run in a handler and come back?
 *
 * One process, two runtimes: a server that hosts two echo services on two
 * provider identifiers, and a client that calls them. It uses na+sm, so it
 * needs no network and no port and says nothing about the machine it runs
 * on. The second service supplies its own operations, which is how the
 * Python echo service will arrive.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <defw2/defw2_echo.h>

#define REVERSE_PROVIDER	2
#define BULK_BYTES		(1024 * 1024)

static int failures;

static int check(const char *what, bool ok)
{
	printf("%-32s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failures++;
	return ok ? 0 : 1;
}

static void fill(unsigned char *buffer, size_t len, unsigned char seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buffer[i] = (unsigned char)(seed + i * 7u);
}

/* A service with opinions, to prove the operations table is consulted. */
static defw2_rc_t reverse_echo(void *ctx, const void *request, size_t len,
			       void **reply, size_t *reply_len)
{
	const unsigned char *in = request;
	unsigned char *out;
	size_t i;

	(void)ctx;
	out = malloc(len ? len : 1);
	if (out == NULL)
		return DEFW2_ERR_NOMEM;
	for (i = 0; i < len; i++)
		out[i] = in[len - 1 - i];
	*reply = out;
	*reply_len = len;
	return DEFW2_OK;
}

static defw2_rc_t invert(void *ctx, void *buffer, size_t len)
{
	unsigned char *bytes = buffer;
	size_t i;

	(void)ctx;
	for (i = 0; i < len; i++)
		bytes[i] = (unsigned char)~bytes[i];
	return DEFW2_OK;
}

static bool round_trip(defw2_binding_t *binding, size_t len)
{
	defw2_call_opts_t opts = { .timeout_ms = 10000 };
	unsigned char *payload = malloc(len ? len : 1);
	defw2_buffer_t reply = { 0 };
	defw2_status_t status = { 0 };
	bool ok;

	fill(payload, len, (unsigned char)len);
	ok = defw2_echo(binding, payload, len, &opts, &reply, &status) ==
	     DEFW2_OK;
	ok = ok && status.category == DEFW2_CAT_OK;
	ok = ok && reply.len == len;
	ok = ok && (len == 0 || memcmp(reply.data, payload, len) == 0);

	defw2_buffer_free(&reply);
	defw2_status_free(&status);
	free(payload);
	return ok;
}

static void *serve(void *arg)
{
	defw2_service_run((defw2_service_t *)arg);
	return NULL;
}

int main(void)
{
	defw2_config_t server_cfg, client_cfg;
	defw2_rt_t *server = NULL, *client = NULL;
	defw2_service_t *echo_svc = NULL, *reverse_svc = NULL;
	defw2_binding_t *echo = NULL, *reverse = NULL;
	defw2_echo_ops_t ops = {
		.echo = reverse_echo,
		.transform = invert,
	};
	defw2_call_opts_t opts = { .timeout_ms = 10000 };
	unsigned char *source = NULL, *sink = NULL;
	defw2_status_t status = { 0 };
	defw2_buffer_t reply = { 0 };
	uint64_t moved = 0;
	pthread_t runner;
	size_t i;

	defw2_config_from_env(&server_cfg);
	server_cfg.role = DEFW2_ROLE_SERVER;
	if (defw2_init(&server_cfg, &server) != DEFW2_OK) {
		printf("server init FAILED\n");
		return 1;
	}
	check("echo service", defw2_service_create(server, "echo-1",
						   DEFW2_API_ECHO,
						   DEFW2_PROVIDER_ECHO,
						   &echo_svc) == DEFW2_OK);
	check("built-in echo bound",
	      defw2_echo_bind(echo_svc, NULL) == DEFW2_OK);
	check("second echo service",
	      defw2_service_create(server, "echo-2", DEFW2_API_ECHO,
				   REVERSE_PROVIDER, &reverse_svc) == DEFW2_OK);
	check("supplied operations bound",
	      defw2_echo_bind(reverse_svc, &ops) == DEFW2_OK);
	printf("  serving at %s\n", defw2_service_address(echo_svc));

	defw2_config_from_env(&client_cfg);
	client_cfg.role = DEFW2_ROLE_CLIENT;
	if (defw2_init(&client_cfg, &client) != DEFW2_OK) {
		printf("client init FAILED\n");
		return 1;
	}
	check("bind to the service",
	      defw2_binding_create(client, defw2_service_address(echo_svc),
				   DEFW2_PROVIDER_ECHO, &echo) == DEFW2_OK);
	check("bind to the second",
	      defw2_binding_create(client, defw2_service_address(reverse_svc),
				   REVERSE_PROVIDER, &reverse) == DEFW2_OK);

	/* W1 and W2 payload sizes, and the two edges around them. */
	check("echo 64 bytes", round_trip(echo, 64));
	check("echo 4 KiB", round_trip(echo, 4096));
	check("echo nothing", round_trip(echo, 0));
	check("echo refuses oversize",
	      defw2_echo(echo, "x", (size_t)DEFW2_EAGER_MAX + 1, &opts, &reply,
			 &status) == DEFW2_ERR_INVALID);

	/* The supplied operation runs, so the payload comes back reversed. */
	check("supplied echo runs",
	      defw2_echo(reverse, "abcd", 4, &opts, &reply, &status) ==
		      DEFW2_OK &&
		      reply.len == 4 && memcmp(reply.data, "dcba", 4) == 0);
	defw2_buffer_free(&reply);
	defw2_status_free(&status);

	source = malloc(BULK_BYTES);
	sink = malloc(BULK_BYTES);
	fill(source, BULK_BYTES, 0x5a);
	memset(sink, 0, BULK_BYTES);
	check("bulk echo 1 MiB",
	      defw2_echo_bulk(echo, source, sink, BULK_BYTES, &opts, &moved,
			      &status) == DEFW2_OK &&
		      status.category == DEFW2_CAT_OK &&
		      memcmp(source, sink, BULK_BYTES) == 0);
	check("bulk moved both ways", moved == 2 * (uint64_t)BULK_BYTES);
	defw2_status_free(&status);

	/* One buffer, echoed into itself, which is the bandwidth case. */
	check("bulk echo in place",
	      defw2_echo_bulk(echo, source, source, BULK_BYTES, &opts, NULL,
			      &status) == DEFW2_OK &&
		      memcmp(source, sink, BULK_BYTES) == 0);
	defw2_status_free(&status);

	check("bulk refuses nothing",
	      defw2_echo_bulk(echo, source, sink, 0, &opts, NULL, &status) ==
		      DEFW2_ERR_INVALID);

	/* The transform runs on the way through, so every byte flips. */
	check("bulk transform runs",
	      defw2_echo_bulk(reverse, source, sink, BULK_BYTES, &opts, NULL,
			      &status) == DEFW2_OK);
	for (i = 0; i < BULK_BYTES; i++) {
		if (sink[i] != (unsigned char)~source[i]) {
			check("bulk transform applied", false);
			break;
		}
	}
	if (i == BULK_BYTES)
		check("bulk transform applied", true);
	defw2_status_free(&status);

	defw2_binding_free(echo);
	defw2_binding_free(reverse);
	defw2_finalize(client);

	/* Serving blocks until someone else stops the service. */
	check("service runs on its own thread",
	      pthread_create(&runner, NULL, serve, echo_svc) == 0);
	defw2_service_shutdown(echo_svc);
	check("service stops", pthread_join(runner, NULL) == 0);

	defw2_service_destroy(echo_svc);
	defw2_service_destroy(reverse_svc);
	defw2_finalize(server);
	free(source);
	free(sink);

	printf("%s\n", failures ? "ECHO SMOKE FAILED" : "ECHO SMOKE PASSED");
	return failures ? 1 : 0;
}
