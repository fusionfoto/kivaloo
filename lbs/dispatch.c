#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "imalloc.h"
#include "netbuf.h"
#include "network.h"
#include "proto_lbs.h"
#include "wire.h"
#include "warnp.h"

#include "dispatch.h"
#include "dispatch_internal.h"

static int gotrequest(void *, struct proto_lbs_request *);
static int callback_accept(void *, int);

/* The ID of a thread with completed work has been read (or not). */
static int
workdone(void * cookie, ssize_t lenread)
{
	struct dispatch_state * D = cookie;

	/* If we failed to read a thread ID, something is seriously wrong. */
	if (lenread != sizeof(size_t)) {
		warnp("workdone failed to read thread ID");
		goto err0;
	}

	/* Sanity-check the thread ID. */
	assert(D->wakeupID <= D->nreaders + 2);

	/* Send a response for whatever work was finished. */
	if (dispatch_response_send(D, D->workers[D->wakeupID]))
		goto err0;

	/* Mark the thread as available for more work. */
	if (D->wakeupID == D->nreaders + 1) {
		D->deleter_busy = 0;
	} else if (D->wakeupID == D->nreaders) {
		D->writer_busy = 0;
	} else {
		D->readers_idle[D->nreaders_idle++] = D->wakeupID;
	}

	/*
	 * If this was a read thread, check if there is pending work which
	 * should now be scheduled for this thread.
	 */
	if ((D-> wakeupID < D->nreaders) && dispatch_request_pokereadq(D))
		goto err0;

	/* Read the ID of another thread with completed work. */
	if ((D->wakeup_cookie = network_read(D->spair[0],
	    (uint8_t *)&D->wakeupID, sizeof(size_t), sizeof(size_t),
	    workdone, D)) == NULL) {
		warnp("Error reading thread ID from socket");
		goto err0;
	}

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/* The connection is dying.  Help speed up the process. */
static int
dropconnection(struct dispatch_state * D)
{
	void * tmp;

	/* If we're reading a packet, stop it. */
	if (D->read_cookie != NULL) {
		proto_lbs_request_read_cancel(D->read_cookie);
		D->read_cookie = NULL;
	}

	/* Kill any queued read requests. */
	while (D->readq_head) {
		tmp = D->readq_head;
		D->readq_head = D->readq_head->next;
		D->npending -= 1;
		free(tmp);
	}

	/*
	 * Destroy the write queue.  Depending on how we reached this point,
	 * this may have already been done (many times, even).
	 */
	if (netbuf_write_destroy(D->writeq))
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/* We have read a request (or not). */
static int
gotrequest(void * cookie, struct proto_lbs_request * R)
{
	struct dispatch_state * D = cookie;

	/* This read is done. */
	D->read_cookie = NULL;

	/*
	 * If we failed to read a request, the connection is dead.  Don't try
	 * to read any more requests; cancel any queued read operations; and
	 * destroy the packet write queue.
	 */
	if (R == NULL) {
		/* If we can't read, kill off the connection. */
		goto drop;
	}

	/* We owe a response to the client. */
	D->npending += 1;

	/* Handle and free the packet. */
	switch (R->type) {
	case PROTO_LBS_PARAMS:
		if (dispatch_request_params(D, R))
			goto err0;
		break;
	case PROTO_LBS_GET:
		if (dispatch_request_get(D, R))
			goto err0;
		break;
	case PROTO_LBS_APPEND:
		/* Make sure the (implied) block length is correct. */
		if (R->r.append.blklen != D->blocklen) {
			free(R->r.append.buf);
			free(R);
			goto drop;
		}
		if (dispatch_request_append(D, R))
			goto err0;
		break;
	case PROTO_LBS_FREE:
		if (dispatch_request_free(D, R))
			goto err0;
		break;
	}

	/* Try to read another packet. */
	if ((D->read_cookie = proto_lbs_request_read(D->readq,
	    gotrequest, D)) == NULL) {
		warnp("Error reading request from connection");
		goto err0;
	}

	/* Success! */
	return (0);

drop:
	/* We didn't get a valid request.  Drop the connection. */
	if (dropconnection(D))
		goto err0;

	/* All is good. */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/**
 * dispatch_init(S, blocklen, nreaders):
 * Initialize a dispatcher to manage requests to storage state ${S} with
 * block size ${blocklen}, using ${nreaders} read threads.
 */
struct dispatch_state *
dispatch_init(struct storage_state * S, size_t blocklen, size_t nreaders)
{
	struct dispatch_state * D;
	size_t nworkers;
	size_t i;

	/* Bake a cookie. */
	if ((D = malloc(sizeof(struct dispatch_state))) == NULL)
		goto err0;
	D->nreaders = nreaders;
	D->writer_busy = D->deleter_busy = 0;
	D->blocklen = blocklen;
	D->sstate = S;

	/* All the readers will be idle when we create them. */
	if (IMALLOC(D->readers_idle, D->nreaders, size_t))
		goto err1;
	D->nreaders_idle = D->nreaders;
	for (i = 0; i < D->nreaders; i++)
		D->readers_idle[i] = i;

	/* Create a socket pair for sending work completion messages. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, D->spair)) {
		warnp("socketpair");
		goto err2;
	}

	/* Mark the read end of the socket pair as non-blocking. */
	if (fcntl(D->spair[0], F_SETFL, O_NONBLOCK) == -1) {
		warnp("Cannot make wakeup socket non-blocking");
		goto err3;
	}

	/* Read work completion messages from the socket. */
	if ((D->wakeup_cookie = network_read(D->spair[0],
	    (uint8_t *)&D->wakeupID, sizeof(size_t), sizeof(size_t),
	    workdone, D)) == NULL) {
		warnp("Error reading thread ID from socket");
		goto err3;
	}

	/* Create worker threads. */
	nworkers = D->nreaders + 2;
	if (IMALLOC(D->workers, nworkers, struct workctl *)) {
		warnp("malloc");
		goto err4;
	}
	for (i = 0; i < nworkers; i++)
		D->workers[i] = NULL;
	for (i = 0; i < nworkers; i++) {
		if ((D->workers[i] =
		    worker_create(i, S, D->spair[1])) == NULL) {
			warnp("Cannot create worker thread");
			goto err5;
		}
	}

	/* Succcess! */
	return (D);

err5:
	for (i = 0; i < nworkers; i++) {
		if (D->workers[i] == NULL)
			continue;
		worker_kill(D->workers[i]);
	}
	free(D->workers);
err4:
	network_read_cancel(D->wakeup_cookie);
err3:
	close(D->spair[1]);
	close(D->spair[0]);
err2:
	free(D->readers_idle);
err1:
	free(D);
err0:
	/* Failure! */
	return (NULL);
}

/**
 * dispatch_accept(D, s):
 * Accept a connection from the listening socket ${s} and perform all
 * associated initialization in the dispatcher ${D}.
 */
int
dispatch_accept(struct dispatch_state * D, int s)
{

	/* Accept a connection. */
	D->accepting = 1;
	if (network_accept(s, callback_accept, D) == NULL)
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/* A connection has arrived. */
static int
callback_accept(void * cookie, int s)
{
	struct dispatch_state * D = cookie;

	/* We have a socket. */
	if ((D->sconn = s) == -1) {
		warnp("Error accepting connection");
		goto err0;
	}

	/* We have no pending requests and no queued reads. */
	D->npending = 0;
	D->readq_head = NULL;

	/* Make the accepted connection non-blocking. */
	if (fcntl(D->sconn, F_SETFL, O_NONBLOCK) == -1) {
		warnp("Cannot make connection non-blocking");
		goto err1;
	}

	/* Create a buffered writer for the connection. */
	if ((D->writeq = netbuf_write_init(D->sconn)) == NULL) {
		warnp("Cannot create packet write queue");
		goto err1;
	}

	/* Create a buffered reader for the connection. */
	if ((D->readq = netbuf_read_init(D->sconn)) == NULL) {
		warnp("Cannot create packet read queue");
		goto err2;
	}

	/* Start reading requests. */
	if ((D->read_cookie = proto_lbs_request_read(D->readq,
	    gotrequest, D)) == NULL) {
		warnp("Error reading request from connection");
		goto err3;
	}

	/* We are no longer waiting for a connection. */
	D->accepting = 0;

	/* Success! */
	return (0);

err3:
	netbuf_read_free(D->readq);
err2:
	netbuf_write_destroy(D->writeq);
	netbuf_write_free(D->writeq);
err1:
	close(D->sconn);
err0:
	/* Failure! */
	return (-1);
}

/**
 * dispatch_alive(D):
 * Return non-zero iff the current connection being handled by the dispatcher
 * ${D} is still alive (if it is reading requests, has requests queued, is
 * processing requests, has responses queued up to be sent back, et cetera).
 */
int
dispatch_alive(struct dispatch_state * D)
{

	return ((D->accepting != 0) ||
	    (D->read_cookie != NULL) ||
	    (D->npending > 0));
}

/**
 * dispatch_close(D):
 * Clean up and close the current connection being handled by the dispatcher.
 * ${D}.  The function dispatch_alive must have previously returned a
 * non-zero value.
 */
int
dispatch_close(struct dispatch_state * D)
{

	/* Sanity check. */
	assert(D->read_cookie == NULL);
	assert(D->npending == 0);

	/* Free the buffered reader for the connection. */
	netbuf_read_free(D->readq);

	/* Free the buffered writer for the connection. */
	netbuf_write_free(D->writeq);

	/* Close the connection. */
	while (close(D->sconn)) {
		if (errno == EINTR)
			continue;
		warnp("close");
		goto err0;
	}

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/**
 * dispatch_done(D):
 * Clean up and free the dispatcher ${D}.
 */
int
dispatch_done(struct dispatch_state * D)
{
	size_t i;
	int rc = 0;	/* No errors yet. */

	/* Shut down the worker threads. */
	for (i = 0; i < D->nreaders + 2; i++) {
		if (worker_kill(D->workers[i])) {
			warnp("Cannot destroy worker thread");
			rc = -1;
		}
	}
	free(D->workers);

	/* Stop reading work completion messages. */
	network_read_cancel(D->wakeup_cookie);

	/* Close the work completion message conduit. */
	while (close(D->spair[1])) {
		if (errno == EINTR)
			continue;
		warnp("close");
		rc = -1;
	}
	while (close(D->spair[0])) {
		if (errno == EINTR)
			continue;
		warnp("close");
		rc = -1;
	}

	/* Free allocated memory. */
	free(D->readers_idle);
	free(D);

	/* Return success, or failure if anything went wrong. */
	return (rc);
}

/**
 * dispatch_writresponse(cookie, status):
 * Callback for packet writes: kill the connection if a packet write failed.
 */
int
dispatch_writresponse(void * cookie, int status)
{
	struct dispatch_state * D = cookie;

	/* We owe one less response to the client. */
	D->npending -= 1;

	/* If we failed to send the response, kill the connection. */
	if (status) {
		if (dropconnection(D))
			goto err0;
	}

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}
