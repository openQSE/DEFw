/*
 * Keeping a service registered.
 *
 * Every service needs the same three things and none of them are interesting:
 * register at startup, heartbeat until you stop, deregister on the way out.
 * Written once here, a service author supplies only the parts they alone know
 * and never sees a timer.
 *
 * The heartbeat runs on a Margo timer, which margo_timer_create submits to
 * the handler pool, so blocking on the forward is allowed for the same reason
 * a handler may block on a provider call. A timer is one-shot, so the
 * callback re-arms itself while a flag says to.
 *
 * The agent is also where the restart half of phase 1's exit criterion is
 * kept automatic. A directory that has forgotten this service answers the
 * heartbeat with NOT_FOUND, and the agent registers again rather than beating
 * against a record that is not there.
 */
#include <stdlib.h>
#include <string.h>

#include <margo-timer.h>

#include "defw2_dir_internal.h"

#include "../host/defw2_host.h"
#include "../telemetry/defw2_trace.h"

struct defw2_dir_agent {
	defw2_service_t		*svc;
	struct defw2_rt		*rt;
	defw2_dir_t		*dir;
	/*
	 * Our own copy of the record, because re-registering needs it long
	 * after the caller's has gone out of scope.
	 */
	defw2_dir_record_own_t	*record;
	margo_timer_t		timer;
	uint32_t		interval_ms;
	uint64_t		generation;
	bool			beating;
	pthread_mutex_t		lock;
};

/*
 * Send one registration and keep the generation it assigned. Returns whether
 * the directory accepted it.
 */
static bool register_once(defw2_dir_agent_t *agent)
{
	defw2_call_opts_t opts;
	defw2_dir_record_t view;
	defw2_status_t status;
	uint64_t generation = 0;
	bool accepted;
	defw2_rc_t rc;

	memset(&opts, 0, sizeof(opts));
	memset(&status, 0, sizeof(status));
	/*
	 * Bounded by the heartbeat interval: a registration that takes longer
	 * than the gap between beats is not worth waiting for, because the
	 * next beat will try again anyway.
	 */
	opts.timeout_ms = agent->interval_ms;

	defw2_dir_record_own_view(agent->record, &view);
	rc = defw2_dir_register(agent->dir, &view, &opts, &generation,
				&status);
	accepted = rc == DEFW2_OK && status.code == DEFW2_OK;
	if (accepted) {
		pthread_mutex_lock(&agent->lock);
		agent->generation = generation;
		pthread_mutex_unlock(&agent->lock);
		defw2_log(agent->rt, DEFW2_LOG_MESSAGE,
			  "directory: %s registered, generation %llu",
			  agent->record->service_id,
			  (unsigned long long)generation);
	} else {
		defw2_log(agent->rt, DEFW2_LOG_ERROR,
			  "directory: %s could not register: %s",
			  agent->record->service_id,
			  status.message ? status.message
					 : defw2_strerror(rc));
	}
	defw2_status_free(&status);
	return accepted;
}

static void beat_timer_cb(void *arg)
{
	defw2_dir_agent_t *agent = arg;
	defw2_call_opts_t opts;
	defw2_status_t status;
	uint64_t generation;
	defw2_rc_t rc;

	if (agent == NULL || !agent->beating)
		return;

	memset(&opts, 0, sizeof(opts));
	memset(&status, 0, sizeof(status));
	opts.timeout_ms = agent->interval_ms;

	pthread_mutex_lock(&agent->lock);
	generation = agent->generation;
	pthread_mutex_unlock(&agent->lock);

	if (generation == 0) {
		/* The first registration never landed, so keep trying. */
		register_once(agent);
		goto rearm;
	}

	rc = defw2_dir_heartbeat(agent->dir, agent->record->service_id,
				 agent->record->runtime_id, generation, &opts,
				 &status);
	if (rc != DEFW2_OK) {
		/*
		 * The directory is unreachable. Not an error to act on: it may
		 * be restarting, and the next beat either reaches it or the
		 * one after that does. Saying so once per beat would fill the
		 * log with something nobody can fix from here.
		 */
		defw2_log(agent->rt, DEFW2_LOG_DEBUG,
			  "directory: heartbeat for %s did not arrive: %s",
			  agent->record->service_id, defw2_strerror(rc));
	} else if (status.code == DEFW2_ERR_NOT_FOUND) {
		/*
		 * The registration is gone: either the directory restarted and
		 * lost it, or the liveness scan retired it while this process
		 * was stalled. Registering again is the only way back, and it
		 * is what makes a restart pick up a new generation without
		 * every service having to implement it.
		 */
		defw2_log(agent->rt, DEFW2_LOG_WARNING,
			  "directory: %s is no longer registered, registering again",
			  agent->record->service_id);
		pthread_mutex_lock(&agent->lock);
		agent->generation = 0;
		pthread_mutex_unlock(&agent->lock);
		register_once(agent);
	}
	defw2_status_free(&status);

rearm:
	if (agent->beating)
		margo_timer_start(agent->timer, (double)agent->interval_ms);
}

defw2_rc_t defw2_dir_agent_start(defw2_service_t *svc, const char *dir_address,
				 const defw2_dir_record_t *record,
				 uint32_t interval_ms,
				 defw2_dir_agent_t **out)
{
	defw2_dir_agent_t *agent;
	defw2_dir_record_t seeded;
	struct defw2_rt *rt;
	defw2_rc_t rc;

	if (svc == NULL || record == NULL || out == NULL)
		return DEFW2_ERR_INVALID;
	rt = defw2_service_runtime(svc);
	if (rt == NULL)
		return DEFW2_ERR_INVALID;

	agent = calloc(1, sizeof(*agent));
	if (agent == NULL)
		return DEFW2_ERR_NOMEM;
	if (pthread_mutex_init(&agent->lock, NULL) != 0) {
		free(agent);
		return DEFW2_ERR_INTERNAL;
	}
	agent->svc = svc;
	agent->rt = rt;
	agent->timer = MARGO_TIMER_NULL;
	agent->interval_ms = interval_ms > 0 ? interval_ms
					     : DEFW2_DIR_DEFAULT_HEARTBEAT_MS;

	/*
	 * The identity is the runtime's and the service's, not the caller's to
	 * get right. A record that named a different address than the one it
	 * is reachable at would resolve to nowhere, and the runtime already
	 * knows the answer.
	 */
	seeded = *record;
	seeded.runtime_id = defw2_runtime_id(rt);
	seeded.address = defw2_service_address(svc);
	seeded.endpoint.node_name = defw2_node_name(rt);
	seeded.endpoint.hostname = defw2_hostname(rt);
	seeded.endpoint.pid = defw2_pid(rt);
	if (seeded.service_id == NULL)
		seeded.service_id = defw2_service_id(svc);
	if (seeded.service_type == NULL)
		seeded.service_type = defw2_service_type(svc);

	rc = defw2_dir_record_own_from(&seeded, &agent->record);
	if (rc != DEFW2_OK)
		goto fail;
	rc = defw2_dir_open(rt, dir_address, &agent->dir);
	if (rc != DEFW2_OK)
		goto fail;

	/*
	 * Register before arming, so a caller that returns from start knows
	 * whether it is in the directory. A refusal is still fatal to the
	 * agent rather than something to retry in the background: a conflict
	 * with a live runtime will not resolve itself, and a service that
	 * silently serves while unregistered is worse than one that stops.
	 */
	if (!register_once(agent)) {
		rc = DEFW2_ERR_BUSY;
		goto fail;
	}

	if (margo_timer_create(rt->mid, beat_timer_cb, agent,
			       &agent->timer) != 0) {
		defw2_log(rt, DEFW2_LOG_ERROR,
			  "directory: no heartbeat timer for %s, it will time out",
			  agent->record->service_id);
		agent->timer = MARGO_TIMER_NULL;
	} else {
		agent->beating = true;
		margo_timer_start(agent->timer, (double)agent->interval_ms);
	}

	*out = agent;
	return DEFW2_OK;

fail:
	if (agent->dir != NULL)
		defw2_dir_close(agent->dir);
	defw2_dir_record_own_free(agent->record);
	pthread_mutex_destroy(&agent->lock);
	free(agent);
	return rc;
}

void defw2_dir_agent_stop(defw2_dir_agent_t *agent)
{
	defw2_call_opts_t opts;
	defw2_status_t status;
	uint64_t generation;

	if (agent == NULL)
		return;

	/*
	 * Stop beating first, and cancel before destroying: clearing the flag
	 * stops the callback re-arming, but a beat already queued has to be
	 * taken back before the agent it points at goes away.
	 */
	agent->beating = false;
	if (agent->timer != MARGO_TIMER_NULL) {
		margo_timer_cancel(agent->timer);
		margo_timer_destroy(agent->timer);
		agent->timer = MARGO_TIMER_NULL;
	}

	pthread_mutex_lock(&agent->lock);
	generation = agent->generation;
	pthread_mutex_unlock(&agent->lock);

	if (generation > 0) {
		memset(&opts, 0, sizeof(opts));
		memset(&status, 0, sizeof(status));
		opts.timeout_ms = agent->interval_ms;
		/*
		 * Best effort. A directory that has already gone will time
		 * this record out on its own, so failing to say goodbye is
		 * not worth reporting to a caller that is shutting down.
		 */
		defw2_dir_deregister(agent->dir, agent->record->service_id,
				     agent->record->runtime_id, generation,
				     &opts, &status);
		defw2_status_free(&status);
	}

	defw2_dir_close(agent->dir);
	defw2_dir_record_own_free(agent->record);
	pthread_mutex_destroy(&agent->lock);
	free(agent);
}

uint64_t defw2_dir_agent_generation(const defw2_dir_agent_t *agent)
{
	uint64_t generation;

	if (agent == NULL)
		return 0;
	pthread_mutex_lock((pthread_mutex_t *)&agent->lock);
	generation = agent->generation;
	pthread_mutex_unlock((pthread_mutex_t *)&agent->lock);
	return generation;
}

const char *defw2_dir_agent_runtime_id(const defw2_dir_agent_t *agent)
{
	return agent != NULL && agent->record != NULL ?
		agent->record->runtime_id : NULL;
}
