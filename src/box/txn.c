/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "txn.h"
#include "engine.h"
#include "tuple.h"
#include "journal.h"
#include <fiber.h>
#include "xrow.h"

double too_long_threshold;

/* Txn cache. */
static struct stailq txn_cache = {NULL, &txn_cache.first};

static void
txn_on_stop(struct trigger *trigger, void *event);

static void
txn_on_yield(struct trigger *trigger, void *event);

static void
txn_run_triggers(struct txn *txn, struct rlist *trigger);

static inline void
fiber_set_txn(struct fiber *fiber, struct txn *txn)
{
	fiber->storage.txn = txn;
}

static void
dummy_trigger_cb(struct trigger *trigger, void *event)
{
	(void)trigger;
	(void)event;
}

/**
 * A savepoint that points to the beginning of a transaction.
 * Use it to rollback all statements of any transaction.
 */
static struct txn_savepoint zero_svp = {
	/* .in_sub_stmt = */ 0,
	/* .has_triggers = */ false,
	/* .stmt = */ NULL,
	/* .fk_deferred_count = */ 0,
	/* .on_commit = */ {RLIST_LINK_INITIALIZER, 0, NULL, NULL},
	/* .on_rollback = */ {RLIST_LINK_INITIALIZER, 0, NULL, NULL},
};

/**
 * Create a savepoint that can be used to rollback to
 * the current transaction state.
 */
static void
txn_create_svp(struct txn *txn, struct txn_savepoint *svp)
{
	svp->in_sub_stmt = txn->in_sub_stmt;
	svp->stmt = stailq_last(&txn->stmts);
	svp->has_triggers = txn->has_triggers;
	if (svp->has_triggers) {
		trigger_create(&svp->on_commit, dummy_trigger_cb, NULL, NULL);
		trigger_add(&txn->on_commit, &svp->on_commit);
		trigger_create(&svp->on_rollback, dummy_trigger_cb, NULL, NULL);
		trigger_add(&txn->on_rollback, &svp->on_rollback);
	}
	if (txn->psql_txn != NULL)
		svp->fk_deferred_count = txn->psql_txn->fk_deferred_count;
}

/**
 * Destroy a savepoint that isn't going to be used anymore.
 * This function clears dummy commit and rollback triggers
 * installed by the savepoint.
 */
static void
txn_destroy_svp(struct txn_savepoint *svp)
{
	if (svp->has_triggers) {
		trigger_clear(&svp->on_commit);
		trigger_clear(&svp->on_rollback);
	}
}

static int
txn_add_redo(struct txn *txn, struct txn_stmt *stmt, struct request *request)
{
	/* Create a redo log row. */
	struct xrow_header *row;
	row = region_alloc_object(&txn->region, struct xrow_header);
	if (row == NULL) {
		diag_set(OutOfMemory, sizeof(*row),
			 "region", "struct xrow_header");
		return -1;
	}
	if (request->header != NULL) {
		*row = *request->header;
	} else {
		/* Initialize members explicitly to save time on memset() */
		row->type = request->type;
		row->replica_id = 0;
		row->group_id = stmt->space != NULL ?
				space_group_id(stmt->space) : 0;
		row->lsn = 0;
		row->sync = 0;
		row->tm = 0;
	}
	row->bodycnt = xrow_encode_dml(request, &txn->region, row->body);
	if (row->bodycnt < 0)
		return -1;
	stmt->row = row;
	return 0;
}

/** Initialize a new stmt object within txn. */
static struct txn_stmt *
txn_stmt_new(struct txn *txn)
{
	struct txn_stmt *stmt;
	stmt = region_alloc_object(&txn->region, struct txn_stmt);
	if (stmt == NULL) {
		diag_set(OutOfMemory, sizeof(*stmt),
			 "region", "struct txn_stmt");
		return NULL;
	}

	/* Initialize members explicitly to save time on memset() */
	stmt->space = NULL;
	stmt->old_tuple = NULL;
	stmt->new_tuple = NULL;
	stmt->engine_savepoint = NULL;
	stmt->row = NULL;

	/* Set the savepoint for statement rollback. */
	txn_create_svp(txn, &txn->sub_stmt_begin[txn->in_sub_stmt]);
	txn->in_sub_stmt++;

	stailq_add_tail_entry(&txn->stmts, stmt, next);
	return stmt;
}

static inline void
txn_stmt_unref_tuples(struct txn_stmt *stmt)
{
	if (stmt->old_tuple != NULL)
		tuple_unref(stmt->old_tuple);
	if (stmt->new_tuple != NULL)
		tuple_unref(stmt->new_tuple);
}

static void
txn_rollback_to_svp(struct txn *txn, struct txn_savepoint *svp)
{
	/*
	 * Undo changes done to the database by the rolled back
	 * statements. Don't release the tuples as rollback triggers
	 * might still need them.
	 */
	struct txn_stmt *stmt;
	struct stailq rollback;
	stailq_cut_tail(&txn->stmts, svp->stmt, &rollback);
	stailq_reverse(&rollback);
	stailq_foreach_entry(stmt, &rollback, next) {
		if (txn->engine != NULL && stmt->space != NULL)
			engine_rollback_statement(txn->engine, txn, stmt);
		if (stmt->row != NULL && stmt->row->replica_id == 0) {
			assert(txn->n_new_rows > 0);
			txn->n_new_rows--;
			if (stmt->row->group_id == GROUP_LOCAL)
				txn->n_local_rows--;
		}
		if (stmt->row != NULL && stmt->row->replica_id != 0) {
			assert(txn->n_applier_rows > 0);
			txn->n_applier_rows--;
		}
		stmt->space = NULL;
		stmt->row = NULL;
	}
	/*
	 * Remove commit and rollback triggers installed after
	 * the savepoint was set and run the rollback triggers.
	 */
	RLIST_HEAD(on_commit);
	RLIST_HEAD(on_rollback);
	if (svp->has_triggers) {
		rlist_cut_before(&on_commit, &txn->on_commit,
				 &svp->on_commit.link);
		rlist_cut_before(&on_rollback, &txn->on_rollback,
				 &svp->on_rollback.link);
	} else if (txn->has_triggers) {
		rlist_splice(&on_commit, &txn->on_commit);
		rlist_splice(&on_rollback, &txn->on_rollback);
		txn->has_triggers = false;
	}
	txn_run_triggers(txn, &on_rollback);

	/* Release the tuples. */
	stailq_foreach_entry(stmt, &rollback, next)
		txn_stmt_unref_tuples(stmt);

	if (txn->psql_txn != NULL)
		txn->psql_txn->fk_deferred_count = svp->fk_deferred_count;
}

/*
 * Return a txn from cache or create a new one if cache is empty.
 */
inline static struct txn *
txn_new()
{
	if (!stailq_empty(&txn_cache))
		return stailq_shift_entry(&txn_cache, struct txn, in_txn_cache);

	/* Create a region. */
	struct region region;
	region_create(&region, &cord()->slabc);

	/* Place txn structure on the region. */
	struct txn *txn = region_alloc_object(&region, struct txn);
	if (txn == NULL) {
		diag_set(OutOfMemory, sizeof(*txn), "region", "struct txn");
		return NULL;
	}
	assert(region_used(&region) == sizeof(*txn));
	txn->region = region;
	return txn;
}

/*
 * Free txn memory and return it to a cache.
 */
inline static void
txn_free(struct txn *txn)
{
	struct txn_stmt *stmt;
	stailq_foreach_entry(stmt, &txn->stmts, next)
		txn_stmt_unref_tuples(stmt);

	/* Truncate region up to struct txn size. */
	region_truncate(&txn->region, sizeof(struct txn));
	stailq_add(&txn_cache, &txn->in_txn_cache);
}

struct txn *
txn_begin()
{
	static int64_t tsn = 0;
	assert(! in_txn());
	struct txn *txn = txn_new();
	if (txn == NULL)
		return NULL;

	/* Initialize members explicitly to save time on memset() */
	stailq_create(&txn->stmts);
	txn->n_new_rows = 0;
	txn->n_local_rows = 0;
	txn->n_applier_rows = 0;
	txn->has_triggers  = false;
	txn->is_done = false;
	txn->can_yield = true;
	txn->is_aborted_by_yield = false;
	txn->in_sub_stmt = 0;
	txn->id = ++tsn;
	txn->signature = -1;
	txn->engine = NULL;
	txn->engine_tx = NULL;
	txn->psql_txn = NULL;
	txn->fiber = NULL;
	fiber_set_txn(fiber(), txn);
	/* fiber_on_yield is initialized by engine on demand */
	trigger_create(&txn->fiber_on_stop, txn_on_stop, NULL, NULL);
	trigger_add(&fiber()->on_stop, &txn->fiber_on_stop);
	return txn;
}

int
txn_begin_in_engine(struct engine *engine, struct txn *txn)
{
	if (engine->flags & ENGINE_BYPASS_TX)
		return 0;
	if (txn->engine == NULL) {
		txn->engine = engine;
		return engine_begin(engine, txn);
	} else if (txn->engine != engine) {
		/**
		 * Only one engine can be used in
		 * a multi-statement transaction currently.
		 */
		diag_set(ClientError, ER_CROSS_ENGINE_TRANSACTION);
		return -1;
	}
	return 0;
}

int
txn_begin_stmt(struct txn *txn, struct space *space)
{
	assert(txn == in_txn());
	assert(txn != NULL);
	if (txn->in_sub_stmt > TXN_SUB_STMT_MAX) {
		diag_set(ClientError, ER_SUB_STMT_MAX);
		return -1;
	}
	struct txn_stmt *stmt = txn_stmt_new(txn);
	if (stmt == NULL)
		return -1;
	if (space == NULL)
		return 0;

	struct engine *engine = space->engine;
	if (txn_begin_in_engine(engine, txn) != 0)
		goto fail;

	stmt->space = space;
	if (engine_begin_statement(engine, txn) != 0)
		goto fail;

	return 0;
fail:
	txn_rollback_stmt(txn);
	return -1;
}

bool
txn_is_distributed(struct txn *txn)
{
	assert(txn == in_txn());
	/**
	 * Transaction has both new and applier rows, and some of
	 * the new rows need to be replicated back to the
	 * server of transaction origin.
	 */
	return (txn->n_new_rows > 0 && txn->n_applier_rows > 0 &&
		txn->n_new_rows != txn->n_local_rows);
}

/**
 * End a statement.
 */
int
txn_commit_stmt(struct txn *txn, struct request *request)
{
	assert(txn->in_sub_stmt > 0);
	/*
	 * Run on_replace triggers. For now, disallow mutation
	 * of tuples in the trigger.
	 */
	struct txn_stmt *stmt = txn_current_stmt(txn);

	/* Create WAL record for the write requests in non-temporary spaces.
	 * stmt->space can be NULL for IRPOTO_NOP.
	 */
	if (stmt->space == NULL || !space_is_temporary(stmt->space)) {
		if (txn_add_redo(txn, stmt, request) != 0)
			goto fail;
		assert(stmt->row != NULL);
		if (stmt->row->replica_id == 0) {
			++txn->n_new_rows;
			if (stmt->row->group_id == GROUP_LOCAL)
				++txn->n_local_rows;

		} else {
			++txn->n_applier_rows;
		}
	}
	/*
	 * If there are triggers, and they are not disabled, and
	 * the statement found any rows, run triggers.
	 * XXX:
	 * - vinyl doesn't set old/new tuple, so triggers don't
	 *   work for it
	 * - perhaps we should run triggers even for deletes which
	 *   doesn't find any rows
	 */
	if (stmt->space != NULL && !rlist_empty(&stmt->space->on_replace) &&
	    stmt->space->run_triggers && (stmt->old_tuple || stmt->new_tuple)) {
		int rc = 0;
		if(!space_is_temporary(stmt->space)) {
			rc = trigger_run(&stmt->space->on_replace, txn);
		} else {
			/*
			 * There is no row attached to txn_stmt for
			 * temporary spaces, since DML operations on them
			 * are not written to WAL. Fake a row to pass operation
			 * type to lua on_replace triggers.
			 */
			assert(stmt->row == NULL);
			struct xrow_header temp_header;
			temp_header.type = request->type;
			stmt->row = &temp_header;
			rc = trigger_run(&stmt->space->on_replace, txn);
			stmt->row = NULL;
		}
		if (rc != 0)
			goto fail;
	}
	--txn->in_sub_stmt;
	txn_destroy_svp(&txn->sub_stmt_begin[txn->in_sub_stmt]);
	return 0;
fail:
	txn_rollback_stmt(txn);
	return -1;
}

/*
 * A helper function to process on_commit/on_rollback triggers.
 */
static void
txn_run_triggers(struct txn *txn, struct rlist *trigger)
{
	/* Rollback triggers must not throw. */
	if (trigger_run(trigger, txn) != 0) {
		/*
		 * As transaction couldn't handle a trigger error so
		 * there is no option except panic.
		 */
		diag_log();
		unreachable();
		panic("commit/rollback trigger failed");
	}
}

/**
 * Complete transaction processing.
 */
static void
txn_complete(struct txn *txn)
{
	/*
	 * Note, engine can be NULL if transaction contains
	 * IPROTO_NOP statements only.
	 */
	if (txn->signature < 0) {
		/* Undo the transaction. */
		if (txn->engine)
			engine_rollback(txn->engine, txn);
		if (txn->has_triggers)
			txn_run_triggers(txn, &txn->on_rollback);
	} else {
		/* Commit the transaction. */
		if (txn->engine != NULL)
			engine_commit(txn->engine, txn);
		if (txn->has_triggers) {
			/*
			 * Commit triggers must be run in the same
			 * order they were added. Since triggers
			 * are always added to the list head, we
			 * need to reverse the list.
			 */
			rlist_reverse(&txn->on_commit);
			txn_run_triggers(txn, &txn->on_commit);
		}

		double stop_tm = ev_monotonic_now(loop());
		if (stop_tm - txn->start_tm > too_long_threshold) {
			int n_rows = txn->n_new_rows + txn->n_applier_rows;
			say_warn_ratelimited("too long WAL write: %d rows at "
					     "LSN %lld: %.3f sec", n_rows,
					     txn->signature - n_rows + 1,
					     stop_tm - txn->start_tm);
		}
	}
	/*
	 * If there is no fiber waiting for the transaction then
	 * the transaction could be safely freed. In the opposite case
	 * the fiber is in duty to free this transaction.
	 */
	if (txn->fiber == NULL)
		txn_free(txn);
	else {
		txn->is_done = true;
		if (txn->fiber != fiber())
			/* Wake a waiting fiber up. */
			fiber_wakeup(txn->fiber);
	}
}

static void
txn_entry_done_cb(struct journal_entry *entry, void *data)
{
	struct txn *txn = data;
	txn->signature = entry->res;
	/*
	 * Some commit/rollback triggers require for in_txn fiber
	 * variable to be set so restore it for the time triggers
	 * are in progress.
	 */
	assert(in_txn() == NULL);
	fiber_set_txn(fiber(), txn);
	txn_complete(txn);
	fiber_set_txn(fiber(), NULL);
}

static int64_t
txn_write_to_wal(struct txn *txn)
{
	assert(txn->n_new_rows + txn->n_applier_rows > 0);

	/* Prepare a journal entry. */
	struct journal_entry *req = journal_entry_new(txn->n_new_rows +
						      txn->n_applier_rows,
						      &txn->region,
						      txn_entry_done_cb,
						      txn);
	if (req == NULL) {
		txn_rollback(txn);
		return -1;
	}

	struct txn_stmt *stmt;
	struct xrow_header **remote_row = req->rows;
	struct xrow_header **local_row = req->rows + txn->n_applier_rows;
	stailq_foreach_entry(stmt, &txn->stmts, next) {
		if (stmt->row == NULL)
			continue; /* A read (e.g. select) request */
		if (stmt->row->replica_id == 0)
			*local_row++ = stmt->row;
		else
			*remote_row++ = stmt->row;
		req->approx_len += xrow_approx_len(stmt->row);
	}
	assert(remote_row == req->rows + txn->n_applier_rows);
	assert(local_row == remote_row + txn->n_new_rows);

	/* Send the entry to the journal. */
	if (journal_write(req) < 0) {
		diag_set(ClientError, ER_WAL_IO);
		diag_log();
		return -1;
	}
	return 0;
}

/*
 * Prepare a transaction using engines.
 */
static int
txn_prepare(struct txn *txn)
{
	if (txn->is_aborted_by_yield) {
		assert(!txn->can_yield);
		diag_set(ClientError, ER_TRANSACTION_YIELD);
		diag_log();
		return -1;
	}
	/*
	 * If transaction has been started in SQL, deferred
	 * foreign key constraints must not be violated.
	 * If not so, just rollback transaction.
	 */
	if (txn->psql_txn != NULL) {
		struct sql_txn *sql_txn = txn->psql_txn;
		if (sql_txn->fk_deferred_count != 0) {
			diag_set(ClientError, ER_FOREIGN_KEY_CONSTRAINT);
			return -1;
		}
	}
	/*
	 * Perform transaction conflict resolution. Engine == NULL when
	 * we have a bunch of IPROTO_NOP statements.
	 */
	if (txn->engine != NULL) {
		if (engine_prepare(txn->engine, txn) != 0)
			return -1;
	}
	trigger_clear(&txn->fiber_on_stop);
	if (!txn->can_yield)
		trigger_clear(&txn->fiber_on_yield);
	return 0;
}

int
txn_write(struct txn *txn)
{
	if (txn_prepare(txn) != 0) {
		txn_rollback(txn);
		return -1;
	}

	/*
	 * After this point the transaction must not be used
	 * so reset the corresponding key in the fiber storage.
	 */
	txn->start_tm = ev_monotonic_now(loop());
	if (txn->n_new_rows + txn->n_applier_rows == 0) {
		/* Nothing to do. */
		txn->signature = 0;
		txn_complete(txn);
		fiber_set_txn(fiber(), NULL);
		return 0;
	}
	fiber_set_txn(fiber(), NULL);
	return txn_write_to_wal(txn);
}

int
txn_commit(struct txn *txn)
{
	txn->fiber = fiber();

	if (txn_write(txn) != 0)
		return -1;
	/*
	 * In case of non-yielding journal the transaction could already
	 * be done and there is nothing to wait in such cases.
	 */
	if (!txn->is_done) {
		bool cancellable = fiber_set_cancellable(false);
		fiber_yield();
		fiber_set_cancellable(cancellable);
	}
	int res = txn->signature >= 0 ? 0 : -1;
	if (res != 0)
		diag_set(ClientError, ER_WAL_IO);

	/* Synchronous transactions are freed by the calling fiber. */
	txn_free(txn);
	return res;
}

void
txn_rollback_stmt(struct txn *txn)
{
	if (txn == NULL || txn->in_sub_stmt == 0)
		return;
	struct txn_savepoint *svp = &txn->sub_stmt_begin[--txn->in_sub_stmt];
	txn_rollback_to_svp(txn, svp);
	txn_destroy_svp(svp);
}

void
txn_rollback(struct txn *txn)
{
	assert(txn == in_txn());
	trigger_clear(&txn->fiber_on_stop);
	if (!txn->can_yield)
		trigger_clear(&txn->fiber_on_yield);
	txn->signature = -1;
	txn_complete(txn);
	fiber_set_txn(fiber(), NULL);
}

int
txn_check_singlestatement(struct txn *txn, const char *where)
{
	if (!txn_is_first_statement(txn)) {
		diag_set(ClientError, ER_MULTISTATEMENT_TRANSACTION, where);
		return -1;
	}
	return 0;
}

void
txn_can_yield(struct txn *txn, bool set)
{
	assert(txn == in_txn());
	assert(txn->can_yield != set);
	txn->can_yield = set;
	if (set) {
		trigger_clear(&txn->fiber_on_yield);
	} else {
		trigger_create(&txn->fiber_on_yield, txn_on_yield, NULL, NULL);
		trigger_add(&fiber()->on_yield, &txn->fiber_on_yield);
	}
}

int64_t
box_txn_id(void)
{
	struct txn *txn = in_txn();
	if (txn != NULL)
		return txn->id;
	else
		return -1;
}

bool
box_txn()
{
	return in_txn() != NULL;
}

int
box_txn_begin()
{
	if (in_txn()) {
		diag_set(ClientError, ER_ACTIVE_TRANSACTION);
		return -1;
	}
	if (txn_begin() == NULL)
		return -1;
	return 0;
}

int
box_txn_commit()
{
	struct txn *txn = in_txn();
	/**
	 * COMMIT is like BEGIN or ROLLBACK
	 * a "transaction-initiating statement".
	 * Do nothing if transaction is not started,
	 * it's the same as BEGIN + COMMIT.
	*/
	if (! txn)
		return 0;
	if (txn->in_sub_stmt) {
		diag_set(ClientError, ER_COMMIT_IN_SUB_STMT);
		return -1;
	}
	int rc = txn_commit(txn);
	fiber_gc();
	return rc;
}

int
box_txn_rollback()
{
	struct txn *txn = in_txn();
	if (txn == NULL)
		return 0;
	if (txn && txn->in_sub_stmt) {
		diag_set(ClientError, ER_ROLLBACK_IN_SUB_STMT);
		return -1;
	}
	txn_rollback(txn); /* doesn't throw */
	fiber_gc();
	return 0;
}

void *
box_txn_alloc(size_t size)
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		/* There are no transaction yet - return an error. */
		diag_set(ClientError, ER_NO_TRANSACTION);
		return NULL;
	}
	union natural_align {
		void *p;
		double lf;
		long l;
	};
	return region_aligned_alloc(&txn->region, size,
	                            alignof(union natural_align));
}

box_txn_savepoint_t *
box_txn_savepoint()
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		diag_set(ClientError, ER_NO_TRANSACTION);
		return NULL;
	}
	struct txn_savepoint *svp =
		(struct txn_savepoint *) region_alloc_object(&txn->region,
							struct txn_savepoint);
	if (svp == NULL) {
		diag_set(OutOfMemory, sizeof(*svp),
			 "region", "struct txn_savepoint");
		return NULL;
	}
	txn_create_svp(txn, svp);
	return svp;
}

int
box_txn_rollback_to_savepoint(box_txn_savepoint_t *svp)
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		diag_set(ClientError, ER_NO_TRANSACTION);
		return -1;
	}
	struct txn_stmt *stmt = svp->stmt == NULL ? NULL :
			stailq_entry(svp->stmt, struct txn_stmt, next);
	if (stmt != NULL && stmt->space == NULL && stmt->row == NULL) {
		/*
		 * The statement at which this savepoint was
		 * created has been rolled back.
		 */
		diag_set(ClientError, ER_NO_SUCH_SAVEPOINT);
		return -1;
	}
	if (svp->in_sub_stmt != txn->in_sub_stmt) {
		diag_set(ClientError, ER_NO_SUCH_SAVEPOINT);
		return -1;
	}
	txn_rollback_to_svp(txn, svp);
	return 0;
}

static void
txn_on_stop(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	txn_rollback(in_txn());                 /* doesn't yield or fail */
}

/**
 * Memtx yield-in-transaction trigger callback.
 *
 * In case of a yield inside memtx multi-statement transaction
 * we must, first of all, roll back the effects of the transaction
 * so that concurrent transactions won't see dirty, uncommitted
 * data.
 *
 * Second, we must abort the transaction, since it has been rolled
 * back implicitly. The transaction can not be rolled back
 * completely from within a yield trigger, since a yield trigger
 * can't fail. Instead, we mark the transaction as aborted and
 * raise an error on attempt to commit it.
 *
 * So much hassle to be user-friendly until we have a true
 * interactive transaction support in memtx.
 */
static void
txn_on_yield(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	struct txn *txn = in_txn();
	assert(txn != NULL && !txn->can_yield);
	txn_rollback_to_svp(txn, &zero_svp);
	txn->is_aborted_by_yield = true;
}
