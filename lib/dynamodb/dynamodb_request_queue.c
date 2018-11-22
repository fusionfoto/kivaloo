#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dynamodb_request.h"
#include "events.h"
#include "http.h"
#include "insecure_memzero.h"
#include "logging.h"
#include "monoclock.h"
#include "ptrheap.h"
#include "serverpool.h"
#include "sock.h"
#include "sock_util.h"

#include "dynamodb_request_queue.h"

/* Request. */
struct request {
	struct dynamodb_request_queue * Q;
	const char * op;
	const uint8_t * body;
	size_t bodylen;
	size_t maxrlen;
	char * logstr;
	int (* callback)(void *, struct http_response *);
	void * cookie;
	struct sock_addr * addrs[2];
	void * http_cookie;
	struct timeval t_start;
	int prio;
	uint64_t reqnum;
	size_t rc;
};

/* Queue of requests. */
struct dynamodb_request_queue {
	char * key_id;
	char * key_secret;
	char * region;
	struct serverpool * SP;
	int ratelimited;
	struct timeval ratedelay;
	void * timer_cookie;
	void * immediate_cookie;
	size_t inflight;
	size_t inflight_max;
	struct ptrheap * reqs;
	uint64_t reqnum;
	struct logging_file * logfile;
};

static int runqueue(struct dynamodb_request_queue *);

/* Callback from events_timer. */
static int
poke_timer(void * cookie)
{
	struct dynamodb_request_queue * Q = cookie;

	/* There is no timer callback pending any more. */
	Q->timer_cookie = NULL;

	/* Run the queue. */
	return (runqueue(Q));
}

/* Callback from events_immediate. */
static int
poke_immediate(void * cookie)
{
	struct dynamodb_request_queue * Q = cookie;

	/* There is no immediate callback pending any more. */
	Q->immediate_cookie = NULL;

	/* Run the queue. */
	return (runqueue(Q));
}

/* Schedule an immediate or timer callback as appropriate. */
static int
poke(struct dynamodb_request_queue * Q)
{

	/* If not already scheduled, schedule the appropriate callback. */
	if (Q->ratelimited) {
		if ((Q->timer_cookie == NULL) &&
		    ((Q->timer_cookie = events_timer_register(poke_timer,
			Q, &Q->ratedelay)) == NULL))
			goto err0;
	} else {
		if ((Q->immediate_cookie == NULL) &&
		    ((Q->immediate_cookie = events_immediate_register(
			poke_immediate, Q, 0)) == NULL))
			goto err0;
	}

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/* Is this a ProvisionedThroughputExceededException? */
static int
isthrottle(struct http_response * res)
{
	size_t i;

	/*
	 * Search the body for "#ProvisionedThroughputExceededException".
	 * The AWS SDKs extract the "__type" field, split this on '#'
	 * characters, and look at the last element; we're guaranteed to
	 * catch anything they catch, and if someone can trigger HTTP 400
	 * responses which yield false positives, we don't really care -- the
	 * worst they can do is to prevent us from bursting requests.
	 */
#define SS "#ProvisionedThroughputExceededException"
	for (i = 0; i + strlen(SS) <= res->bodylen; i++) {
		if (memcmp(&res->body[i], SS, strlen(SS)) == 0)
			return (1);
	}
#undef SS
	return (0);
}

/* Log the (attempted) request. */
static int
logreq(struct logging_file * F, struct request * R,
    struct http_response * res)
{
	struct timeval t_end;
	long t_micros;
	char * addr;
	int status;
	size_t bodylen;
	
	/* Compute how long the request took. */
	if (monoclock_get(&t_end))
		goto err0;
	t_micros = (long)(t_end.tv_sec - R->t_start.tv_sec) * 1000000 +
	    t_end.tv_usec - R->t_start.tv_usec;

	/* Prettyprint the address we selected. */
	if ((addr = sock_addr_prettyprint(R->addrs[0])) == NULL)
		goto err0;

	/* Extract parameters from HTTP response. */
	if (res != NULL) {
		status = res->status;
		bodylen = res->bodylen;
	} else {
		status = 0;
		bodylen = 0;
	}

	/* Write to the log file. */
	if (logging_printf(F, "|%s|%s|%d|%s|%ld|%zu",
	    R->op, R->logstr ? R->logstr : "",
	    status, addr, t_micros, bodylen))
		goto err1;

	/* Free string allocated by sock_addr_prettyprint. */
	free(addr);

	/* Success! */
	return (0);

err1:
	free(addr);
err0:
	/* Failure! */
	return (-1);
}

/* Callback from dynamodb_request (aka. from http_request). */
static int
callback_reqdone(void * cookie, struct http_response * res)
{
	struct request * R = cookie;
	struct dynamodb_request_queue * Q = R->Q;
	int rc = 0;

	/* Optionally log this request. */
	if (Q->logfile) {
		if (logreq(Q->logfile, R, res))
			rc = -1;
	}

	/* This request is no longer in progress. */
	R->http_cookie = NULL;
	Q->inflight--;

	/* Don't need the target address any more. */
	sock_addr_free(R->addrs[0]);
	R->addrs[0] = NULL;

	/* The priority of this request has changed. */
	ptrheap_decrease(Q->reqs, R->rc);

	/* What should we do with this response? */
	if ((res != NULL) && (res->status == 400) && isthrottle(res)) {
		/*
		 * We hit the throughput limits.  Turn on rate limiting for
		 * the queue, and leave the request on the queue; we'll be
		 * retrying it.
		 */
		Q->ratelimited = 1;
	} else if ((res != NULL) && (res->status < 500)) {
		/*
		 * Anything which isn't an internal DynamoDB error or a
		 * rate limiting response is something we should pass back
		 * to the upstream code.
		 */

		/* Dequeue the request. */
		ptrheap_delete(Q->reqs, R->rc);

		/* Invoke the upstream callback. */
		if (rc) {
			(void)(R->callback)(R->cookie, res);
		} else {
			rc = (R->callback)(R->cookie, res);
		}

		/* Free the request; we're done with it now. */
		free(R->logstr);
		free(R);
	}

	/*
	 * Poke the queue.  If the request failed, it may be possible to
	 * re-issue it; if the request succeeded, we may have ceased to be
	 * at our in-flight limit and might be able to issue a new request.
	 */
	if (poke(Q))
		rc = -1;

	/* Return status from callback, or our own success/failure. */
	return (rc);
}

/* Send a request. */
static int
sendreq(struct dynamodb_request_queue * Q, struct request * R)
{

	/* Get a target address. */
	R->addrs[0] = serverpool_pick(Q->SP);

	/* Record start time. */
	if (monoclock_get(&R->t_start))
		goto err1;

	/* Send the request. */
	Q->inflight++;
	if ((R->http_cookie = dynamodb_request(R->addrs, Q->key_id,
	    Q->key_secret, Q->region, R->op, R->body, R->bodylen, R->maxrlen,
	    callback_reqdone, R)) == NULL)
		goto err1;

	/* The priority of this request has changed. */
	ptrheap_increase(Q->reqs, R->rc);

	/* Success! */
	return (0);

err1:
	sock_addr_free(R->addrs[0]);
	R->addrs[0] = NULL;

	/* Failure! */
	return (-1);
}

/* Check if we need to do anything with the queue. */
static int
runqueue(struct dynamodb_request_queue * Q)
{
	struct request * R;

	/* If we're waiting for a timer, keep waiting. */
	if (Q->timer_cookie != NULL)
		goto done;

	/* Find the highest-priority request in the queue. */
	R = ptrheap_getmin(Q->reqs);

	/*
	 * Rate-limiting of requests ends as soon as we have no requests
	 * waiting to be sent when the timer has expired.
	 */
	if ((R == NULL) || (R->http_cookie != NULL)) {
		Q->ratelimited = 0;
		goto done;
	}

	/*
	 * If we're already at our maximum number of in-flight requests,
	 * return without doing anything.  Either our network is broken and
	 * requests are stuck on the wire somewhere, or we're handling a
	 * flood of requests before rate limiting has kicked in.
	 */
	if (Q->inflight == Q->inflight_max)
		goto done;

	/* Send the highest-priority request. */
	if (sendreq(Q, R))
		goto err0;

	/* Schedule a callback to send the next request (if any). */
	if (poke(Q))
		goto err0;

done:
	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/* Record-comparison callback from ptrheap. */
static int
compar(void * cookie, const void * x, const void * y)
{
	const struct request * _x = x;
	const struct request * _y = y;

	(void)cookie; /* UNUSED */

	/* Is one of the requests in progress? */
	if ((_x->http_cookie != NULL) && (_y->http_cookie == NULL))
		return (1);
	if ((_x->http_cookie == NULL) && (_y->http_cookie != NULL))
		return (-1);

	/* Is one a higher priority? */
	if (_x->prio > _y->prio)
		return (1);
	if (_x->prio < _y->prio)
		return (-1);

	/* Sort in order of arrival. */
	if (_x->reqnum > _y->reqnum)
		return (1);
	else
		return (-1);
}

/* Cookie-recording callback from ptrheap. */
static void
setreccookie(void * cookie, void * ptr, size_t rc)
{
	struct request * R = ptr;

	(void)cookie; /* UNUSED */

	R->rc = rc;
}

/**
 * dynamodb_request_queue_init(key_id, key_secret, region, SP, opps):
 * Create a DynamoDB request queue using AWS key id ${key_id} and secret key
 * ${key_secret} to make requests to DynamoDB in ${region}.  Obtain target
 * addresses from the pool ${SP}.  Upon encountering a "Throughput Exceeded"
 * exception, limit the request rate to ${opps} operations per second.
 */
struct dynamodb_request_queue *
dynamodb_request_queue_init(const char * key_id, const char * key_secret,
    const char * region, struct serverpool * SP, int opps)
{
	struct dynamodb_request_queue * Q;

	/* Allocate a request queue structure. */
	if ((Q = malloc(sizeof(struct dynamodb_request_queue))) == NULL)
		goto err0;

	/* Copy in strings. */
	if ((Q->key_id = strdup(key_id)) == NULL)
		goto err1;
	if ((Q->key_secret = strdup(key_secret)) == NULL)
		goto err2;
	if ((Q->region = strdup(region)) == NULL)
		goto err3;

	/* Record the server pool to draw IP addresses from. */
	Q->SP = SP;

	/* Initialize rate-limiting parameters. */
	Q->ratelimited = 0;
	if (opps == 1) {
		Q->ratedelay.tv_sec = 1;
		Q->ratedelay.tv_usec = 0;
	} else {
		Q->ratedelay.tv_sec = 0;
		Q->ratedelay.tv_usec = 1000000 / opps;
	}

	/* Allow a maximum of 5 seconds of quota in flight at once. */
	Q->inflight_max = opps * 5;

	/* We have no pending pokes. */
	Q->timer_cookie = NULL;
	Q->immediate_cookie = NULL;

	/* No requests yet. */
	if ((Q->reqs = ptrheap_init(compar, setreccookie, Q)) == NULL)
		goto err4;
	Q->reqnum = 0;
	Q->inflight = 0;

	/* No log file yet. */
	Q->logfile = NULL;

	/* Success! */
	return (Q);

err4:
	free(Q->region);
err3:
	insecure_memzero(Q->key_secret, strlen(Q->key_secret));
	free(Q->key_secret);
err2:
	free(Q->key_id);
err1:
	free(Q);
err0:
	/* Failure! */
	return (NULL);
}

/**
 * dynamodb_request_queue_log(Q, F):
 * Log all requests performed by the queue ${Q} to the log file ${F}.
 */
void
dynamodb_request_queue_log(struct dynamodb_request_queue * Q,
    struct logging_file * F)
{

	Q->logfile = F;
}

/**
 * dynamodb_request_queue(Q, prio, op, body, maxrlen, logstr, callback, cookie):
 * Using the DynamoDB request queue ${Q}, queue the DynamoDB request
 * contained in ${body} for the operation ${op}.  Read a response with a body
 * of up to ${maxrlen} bytes and invoke the callback as per dynamodb_request.
 * The strings ${op} and ${body} must remain valid until the callback is
 * invoked or the queue is flushed.
 * 
 * HTTP 5xx errors and HTTP 400 "Throughput Exceeded" errors will be
 * automatically retried; other errors are passed back.
 *
 * Requests will be served starting with the lowest ${prio}, breaking ties
 * according to the queue arrival time.
 * 
 * If dynamodb_request_queue_log has been called, ${logstr} will be included
 * when this request is logged.  (This could be used to identify the target
 * of the ${op} operation, for example.)
 */
int
dynamodb_request_queue(struct dynamodb_request_queue * Q, int prio,
    const char * op, const char * body, size_t maxrlen, const char * logstr,
    int (* callback)(void *, struct http_response *), void * cookie)
{
	struct request * R;

	/* Allocate and fill request structure. */
	if ((R = malloc(sizeof(struct request))) == NULL)
		goto err0;
	R->Q = Q;
	R->op = op;
	R->body = (const uint8_t *)body;
	R->bodylen = strlen(body);
	R->maxrlen = maxrlen;
	R->callback = callback;
	R->cookie = cookie;
	R->http_cookie = NULL;
	R->addrs[0] = NULL;
	R->addrs[1] = NULL;
	R->prio = prio;
	R->reqnum = Q->reqnum++;
	R->rc = 0;

	/* Duplicate the additional logging data. */
	if (logstr != NULL) {
		if ((R->logstr = strdup(logstr)) == NULL)
			goto err1;
	} else {
		R->logstr = NULL;
	}

	/* Add the request to the queue. */
	if (ptrheap_add(Q->reqs, R))
		goto err2;

	/* Poke the request queue if necessary. */
	if (poke(Q))
		goto err3;

	/* Success! */
	return (0);

err3:
	ptrheap_delete(Q->reqs, R->rc);
err2:
	free(R->logstr);
err1:
	free(R);
err0:
	/* Failure! */
	return (-1);
}

/**
 * dynamodb_request_queue_flush(Q):
 * Flush the DynamoDB request queue ${Q}.  Any queued requests will be
 * dropped; no callbacks will be performed.
 */
void
dynamodb_request_queue_flush(struct dynamodb_request_queue * Q)
{
	struct request * R;

	/* Pull requests off the queue until there are none left. */
	while ((R = ptrheap_getmin(Q->reqs)) != NULL) {
		/* Delete it from the queue. */
		ptrheap_deletemin(Q->reqs);

		/* Cancel any in-progress operation. */
		if (R->http_cookie != NULL) {
			http_request_cancel(R->http_cookie);
			sock_addr_free(R->addrs[0]);
			Q->inflight--;
		}

		/* Free the request. */
		free(R->logstr);
		free(R);
	}
}

/**
 * dynamodb_request_queue_free(Q):
 * Free the DynamoDB request queue ${Q}.  Any queued requests will be
 * dropped; no callbacks will be performed.
 */
void
dynamodb_request_queue_free(struct dynamodb_request_queue * Q)
{

	/* Flush the queue. */
	dynamodb_request_queue_flush(Q);

	/* Stop the rate-limiting timer (if any). */
	if (Q->timer_cookie != NULL)
		events_timer_cancel(Q->timer_cookie);

	/* Cancel the immediate callback (if any). */
	if (Q->immediate_cookie != NULL)
		events_immediate_cancel(Q->immediate_cookie);

	/* Free the (now empty) request queue. */
	ptrheap_free(Q->reqs);

	/* Free string allocated by strdup. */
	free(Q->region);

	/* Free the AWS keys. */
	insecure_memzero(Q->key_secret, strlen(Q->key_secret));
	free(Q->key_secret);
	free(Q->key_id);

	/* Free the request queue structure. */
	free(Q);
}
