#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "events.h"
#include "objmap.h"
#include "proto_dynamodb_kv.h"
#include "sysendian.h"
#include "warnp.h"

#include "deleteto.h"

struct deleteto {
	struct wire_requestqueue * Q;
	uint64_t N;	/* Delete objects below this number. */
	uint64_t M;	/* We've issued deletes up to this number. */
	size_t npending;	/* Operations in progress. */
	int inited;		/* We've finished reading M. */
	int updateDeletedTo;	/* M has changed since it was last stored. */
	int shuttingdown;	/* Stop issuing DELETEs. */
	int shutdown;		/* Everything is done. */
};

static int callback_done(void *, int);

/* Callback for reading DeletedTo. */
static int
callback_deletedto_get(void * cookie, int status,
    const uint8_t * buf, size_t len)
{
	struct deleteto * D = cookie;

	/* Failures are bad. */
	if (status == 1) {
		warn0("Error reading DeleteTo");
		goto err0;
	}

	/* Did the item exist? */
	if (status == 2) {
		/* That's fine; we haven't deleted anything yet. */
		D->M = 0;
		goto done;
	}

	/* We should have 8 bytes. */
	if (len != 8) {
		warn0("DeletedTo has incorrect size: %zu", len);
		goto err0;
	}

	/* Parse it. */
	D->M = be64dec(buf);

done:
	D->inited = 1;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/**
 * deleteto_init(Q_DDBKV):
 * Initialize the deleter to operate via the DynamoDB-KV daemon connected to
 * ${Q_DDBKV}.  This function may call events_run internally.
 */
struct deleteto *
deleteto_init(struct wire_requestqueue * Q_DDBKV)
{
	struct deleteto * D;

	/* Allocate and initialize a DeleteTo structure. */
	if ((D = malloc(sizeof(struct deleteto))) == NULL)
		goto err0;
	D->Q = Q_DDBKV;
	D->N = 0;
	D->npending = 0;
	D->inited = 0;
	D->updateDeletedTo = 0;
	D->shuttingdown = 0;
	D->shutdown = 0;

	/* Read "DeletedTo" into M.  Eventual consistency is fine. */
	if (proto_dynamodb_kv_request_get(D->Q, "DeletedTo",
	    callback_deletedto_get, D))
		goto err1;
	if (events_spin(&D->inited))
		goto err1;

	/* Success! */
	return (D);

err1:
	free(D);
err0:
	/* Failure! */
	return (NULL);
}

/* Do a round of deletes if appropriate. */
static int
poke(struct deleteto * D)
{
	uint8_t DeletedTo[8];

	/* If we're already busy, don't do anything. */
	if (D->npending)
		return (0);

	/*
	 * Store DeletedTo if we want to; since we have no requests in
	 * progress, we're guaranteed to have deleted everything below M.
	 */
	if (D->updateDeletedTo) {
		be64enc(DeletedTo, D->M);
		if (proto_dynamodb_kv_request_put(D->Q, "DeletedTo",
		    DeletedTo, 8, callback_done, D))
			goto err0;
		D->npending++;
		D->updateDeletedTo = 0;
		return (0);
	}

	/* Are we waiting to shut down? */
	if (D->shuttingdown) {
		D->shutdown = 1;
		return (0);
	}

	/* Can we issue more deletes? */
	while (D->M < D->N) {
		/* Issue a delete for M. */
		if (proto_dynamodb_kv_request_delete(D->Q, objmap(D->M),
		    callback_done, D))
			goto err0;
		D->npending++;

		/* We've issued deletes for everything under one more. */
		D->M++;

		/* If we've finished a "century", stop here for a moment. */
		if ((D->M % 256) == 0) {
			D->updateDeletedTo = 1;
			break;
		}
	}

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/* One of the DynamoDB-KV operations kicked off by poke() has completed. */
static int
callback_done(void * cookie, int status)
{
	struct deleteto * D = cookie;

	/* Sanity-check. */
	assert(D->npending > 0);

	/* Failures are bad, m'kay? */
	if (status) {
		warn0("DynamoDB-KV operation failed!");
		goto err0;
	}

	/* We've finished an operation. */
	D->npending -= 1;

	/* Check what we should do next. */
	if (poke(D))
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/**
 * deleteto_deleteto(D, N):
 * Pages with numbers less than ${N} are no longer needed by the B+Tree.
 * Inform the deleteto state ${D} which may opt to do something about them.
 */
int
deleteto_deleteto(struct deleteto * D, uint64_t N)
{

	/* Record the new DeleteTo value. */
	if (D->N < N)
		D->N = N;

	/* Start doing stuff if necessary. */
	return (poke(D));
}

/**
 * deleteto_stop(D):
 * Clean up, shut down, and free the deleteto state ${D}.  This function may
 * call events_run internally.
 */
int
deleteto_stop(struct deleteto * D)
{
	int rc;

	/* We don't want to do any more DELETEs, just shut down. */
	D->shuttingdown = 1;

	/* Store DeletedTo when all the DELETEs have finished. */
	D->updateDeletedTo = 1;

	/* Wait for all pending operations to finish. */
	rc = events_spin(&D->shutdown);

	/* Free the DeleteTo structure. */
	free(D);

	/* Return status from events_spin. */
	return (rc);
}
