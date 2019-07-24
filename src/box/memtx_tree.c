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
#include "memtx_tree.h"
#include "memtx_engine.h"
#include "space.h"
#include "schema.h" /* space_cache_find() */
#include "errinj.h"
#include "memory.h"
#include "fiber.h"
#include "key_list.h"
#include "tuple.h"
#include <third_party/qsort_arg.h>
#include <small/mempool.h>

/**
 * Struct that is used as a key in BPS tree definition.
 */
struct memtx_tree_key_data {
	/** Sequence of msgpacked search fields. */
	const char *key;
	/** Number of msgpacked search fields. */
	uint32_t part_count;
	/** Comparison hint, see tuple_hint(). */
	hint_t hint;
};

/**
 * Struct that is used as a elem in BPS tree definition.
 */
struct memtx_tree_data {
	/* Tuple that this node is represents. */
	struct tuple *tuple;
	/** Comparison hint, see key_hint(). */
	hint_t hint;
};

/**
 * Test whether BPS tree elements are identical i.e. represent
 * the same tuple at the same position in the tree.
 * @param a - First BPS tree element to compare.
 * @param b - Second BPS tree element to compare.
 * @retval true - When elements a and b are identical.
 * @retval false - Otherwise.
 */
static bool
memtx_tree_data_is_equal(const struct memtx_tree_data *a,
			 const struct memtx_tree_data *b)
{
	return a->tuple == b->tuple;
}

#define BPS_TREE_NAME memtx_tree
#define BPS_TREE_BLOCK_SIZE (512)
#define BPS_TREE_EXTENT_SIZE MEMTX_EXTENT_SIZE
#define BPS_TREE_COMPARE(a, b, arg)\
	tuple_compare((&a)->tuple, (&a)->hint, (&b)->tuple, (&b)->hint, arg)
#define BPS_TREE_COMPARE_KEY(a, b, arg)\
	tuple_compare_with_key((&a)->tuple, (&a)->hint, (b)->key,\
			       (b)->part_count, (b)->hint, arg)
#define BPS_TREE_IS_IDENTICAL(a, b) memtx_tree_data_is_equal(&a, &b)
#define BPS_TREE_NO_DEBUG 1
#define bps_tree_elem_t struct memtx_tree_data
#define bps_tree_key_t struct memtx_tree_key_data *
#define bps_tree_arg_t struct key_def *

#include "salad/bps_tree.h"

#undef BPS_TREE_NAME
#undef BPS_TREE_BLOCK_SIZE
#undef BPS_TREE_EXTENT_SIZE
#undef BPS_TREE_COMPARE
#undef BPS_TREE_COMPARE_KEY
#undef BPS_TREE_IS_IDENTICAL
#undef BPS_TREE_NO_DEBUG
#undef bps_tree_elem_t
#undef bps_tree_key_t
#undef bps_tree_arg_t

struct memtx_tree_index {
	struct index base;
	struct memtx_tree tree;
	struct memtx_tree_data *build_array;
	size_t build_array_size, build_array_alloc_size;
	struct memtx_gc_task gc_task;
	struct memtx_tree_iterator gc_iterator;
};

/* {{{ Utilities. *************************************************/

static inline struct key_def *
memtx_tree_cmp_def(struct memtx_tree *tree)
{
	return tree->arg;
}

static int
memtx_tree_qcompare(const void* a, const void *b, void *c)
{
	const struct memtx_tree_data *data_a = a;
	const struct memtx_tree_data *data_b = b;
	struct key_def *key_def = c;
	return tuple_compare(data_a->tuple, data_a->hint, data_b->tuple,
			     data_b->hint, key_def);
}

/* {{{ MemtxTree Iterators ****************************************/
struct tree_iterator {
	struct iterator base;
	const struct memtx_tree *tree;
	struct index_def *index_def;
	struct memtx_tree_iterator tree_iterator;
	enum iterator_type type;
	struct memtx_tree_key_data key_data;
	struct memtx_tree_data current;
	/** Memory pool the iterator was allocated from. */
	struct mempool *pool;
};

static_assert(sizeof(struct tree_iterator) <= MEMTX_ITERATOR_SIZE,
	      "sizeof(struct tree_iterator) must be less than or equal "
	      "to MEMTX_ITERATOR_SIZE");

static void
tree_iterator_free(struct iterator *iterator);

static inline struct tree_iterator *
tree_iterator(struct iterator *it)
{
	assert(it->free == tree_iterator_free);
	return (struct tree_iterator *) it;
}

static void
tree_iterator_free(struct iterator *iterator)
{
	struct tree_iterator *it = tree_iterator(iterator);
	struct tuple *tuple = it->current.tuple;
	if (tuple != NULL)
		tuple_unref(tuple);
	mempool_free(it->pool, it);
}

static int
tree_iterator_dummie(struct iterator *iterator, struct tuple **ret)
{
	(void)iterator;
	*ret = NULL;
	return 0;
}

static int
tree_iterator_next(struct iterator *iterator, struct tuple **ret)
{
	struct tree_iterator *it = tree_iterator(iterator);
	assert(it->current.tuple != NULL);
	struct memtx_tree_data *check =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	if (check == NULL || !memtx_tree_data_is_equal(check, &it->current)) {
		it->tree_iterator =
			memtx_tree_upper_bound_elem(it->tree, it->current,
						    NULL);
	} else {
		memtx_tree_iterator_next(it->tree, &it->tree_iterator);
	}
	tuple_unref(it->current.tuple);
	struct memtx_tree_data *res =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	if (res == NULL) {
		iterator->next = tree_iterator_dummie;
		it->current.tuple = NULL;
		*ret = NULL;
	} else {
		*ret = res->tuple;
		tuple_ref(*ret);
		it->current = *res;
	}
	return 0;
}

static int
tree_iterator_prev(struct iterator *iterator, struct tuple **ret)
{
	struct tree_iterator *it = tree_iterator(iterator);
	assert(it->current.tuple != NULL);
	struct memtx_tree_data *check =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	if (check == NULL || !memtx_tree_data_is_equal(check, &it->current)) {
		it->tree_iterator =
			memtx_tree_lower_bound_elem(it->tree, it->current, NULL);
	}
	memtx_tree_iterator_prev(it->tree, &it->tree_iterator);
	tuple_unref(it->current.tuple);
	struct memtx_tree_data *res =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	if (!res) {
		iterator->next = tree_iterator_dummie;
		it->current.tuple = NULL;
		*ret = NULL;
	} else {
		*ret = res->tuple;
		tuple_ref(*ret);
		it->current = *res;
	}
	return 0;
}

static int
tree_iterator_next_equal(struct iterator *iterator, struct tuple **ret)
{
	struct tree_iterator *it = tree_iterator(iterator);
	assert(it->current.tuple != NULL);
	struct memtx_tree_data *check =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	if (check == NULL || !memtx_tree_data_is_equal(check, &it->current)) {
		it->tree_iterator =
			memtx_tree_upper_bound_elem(it->tree, it->current, NULL);
	} else {
		memtx_tree_iterator_next(it->tree, &it->tree_iterator);
	}
	tuple_unref(it->current.tuple);
	struct memtx_tree_data *res =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	/* Use user key def to save a few loops. */
	if (res == NULL ||
	    tuple_compare_with_key(res->tuple, res->hint,
				   it->key_data.key,
				   it->key_data.part_count,
				   it->key_data.hint,
				   it->index_def->key_def) != 0) {
		iterator->next = tree_iterator_dummie;
		it->current.tuple = NULL;
		*ret = NULL;
	} else {
		*ret = res->tuple;
		tuple_ref(*ret);
		it->current = *res;
	}
	return 0;
}

static int
tree_iterator_prev_equal(struct iterator *iterator, struct tuple **ret)
{
	struct tree_iterator *it = tree_iterator(iterator);
	assert(it->current.tuple != NULL);
	struct memtx_tree_data *check =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	if (check == NULL || !memtx_tree_data_is_equal(check, &it->current)) {
		it->tree_iterator =
			memtx_tree_lower_bound_elem(it->tree, it->current, NULL);
	}
	memtx_tree_iterator_prev(it->tree, &it->tree_iterator);
	tuple_unref(it->current.tuple);
	struct memtx_tree_data *res =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	/* Use user key def to save a few loops. */
	if (res == NULL ||
	    tuple_compare_with_key(res->tuple, res->hint,
				   it->key_data.key,
				   it->key_data.part_count,
				   it->key_data.hint,
				   it->index_def->key_def) != 0) {
		iterator->next = tree_iterator_dummie;
		it->current.tuple = NULL;
		*ret = NULL;
	} else {
		*ret = res->tuple;
		tuple_ref(*ret);
		it->current = *res;
	}
	return 0;
}

static void
tree_iterator_set_next_method(struct tree_iterator *it)
{
	assert(it->current.tuple != NULL);
	switch (it->type) {
	case ITER_EQ:
		it->base.next = tree_iterator_next_equal;
		break;
	case ITER_REQ:
		it->base.next = tree_iterator_prev_equal;
		break;
	case ITER_ALL:
		it->base.next = tree_iterator_next;
		break;
	case ITER_LT:
	case ITER_LE:
		it->base.next = tree_iterator_prev;
		break;
	case ITER_GE:
	case ITER_GT:
		it->base.next = tree_iterator_next;
		break;
	default:
		/* The type was checked in initIterator */
		assert(false);
	}
}

static int
tree_iterator_start(struct iterator *iterator, struct tuple **ret)
{
	*ret = NULL;
	struct tree_iterator *it = tree_iterator(iterator);
	it->base.next = tree_iterator_dummie;
	const struct memtx_tree *tree = it->tree;
	enum iterator_type type = it->type;
	bool exact = false;
	assert(it->current.tuple == NULL);
	if (it->key_data.key == 0) {
		if (iterator_type_is_reverse(it->type))
			it->tree_iterator = memtx_tree_iterator_last(tree);
		else
			it->tree_iterator = memtx_tree_iterator_first(tree);
	} else {
		if (type == ITER_ALL || type == ITER_EQ ||
		    type == ITER_GE || type == ITER_LT) {
			it->tree_iterator =
				memtx_tree_lower_bound(tree, &it->key_data,
						       &exact);
			if (type == ITER_EQ && !exact)
				return 0;
		} else { // ITER_GT, ITER_REQ, ITER_LE
			it->tree_iterator =
				memtx_tree_upper_bound(tree, &it->key_data,
						       &exact);
			if (type == ITER_REQ && !exact)
				return 0;
		}
		if (iterator_type_is_reverse(type)) {
			/*
			 * Because of limitations of tree search API we use use
			 * lower_bound for LT search and upper_bound for LE
			 * and REQ searches. Thus we found position to the
			 * right of the target one. Let's make a step to the
			 * left to reach target position.
			 * If we found an invalid iterator all the elements in
			 * the tree are less (less or equal) to the key, and
			 * iterator_next call will convert the iterator to the
			 * last position in the tree, that's what we need.
			 */
			memtx_tree_iterator_prev(it->tree, &it->tree_iterator);
		}
	}

	struct memtx_tree_data *res =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	if (!res)
		return 0;
	*ret = res->tuple;
	tuple_ref(*ret);
	it->current = *res;
	tree_iterator_set_next_method(it);
	return 0;
}

/* }}} */

/* {{{ MemtxTree  **********************************************************/

static void
memtx_tree_index_free(struct memtx_tree_index *index)
{
	memtx_tree_destroy(&index->tree);
	free(index->build_array);
	free(index);
}

static void
memtx_tree_index_gc_run(struct memtx_gc_task *task, bool *done)
{
	/*
	 * Yield every 1K tuples to keep latency < 0.1 ms.
	 * Yield more often in debug mode.
	 */
#ifdef NDEBUG
	enum { YIELD_LOOPS = 1000 };
#else
	enum { YIELD_LOOPS = 10 };
#endif

	struct memtx_tree_index *index = container_of(task,
			struct memtx_tree_index, gc_task);
	struct memtx_tree *tree = &index->tree;
	struct memtx_tree_iterator *itr = &index->gc_iterator;

	unsigned int loops = 0;
	while (!memtx_tree_iterator_is_invalid(itr)) {
		struct memtx_tree_data *res =
			memtx_tree_iterator_get_elem(tree, itr);
		memtx_tree_iterator_next(tree, itr);
		tuple_unref(res->tuple);
		if (++loops >= YIELD_LOOPS) {
			*done = false;
			return;
		}
	}
	*done = true;
}

static void
memtx_tree_index_gc_free(struct memtx_gc_task *task)
{
	struct memtx_tree_index *index = container_of(task,
			struct memtx_tree_index, gc_task);
	memtx_tree_index_free(index);
}

static const struct memtx_gc_task_vtab memtx_tree_index_gc_vtab = {
	.run = memtx_tree_index_gc_run,
	.free = memtx_tree_index_gc_free,
};

static void
memtx_tree_index_destroy(struct index *base)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct memtx_engine *memtx = (struct memtx_engine *)base->engine;
	if (base->def->iid == 0) {
		/*
		 * Primary index. We need to free all tuples stored
		 * in the index, which may take a while. Schedule a
		 * background task in order not to block tx thread.
		 */
		index->gc_task.vtab = &memtx_tree_index_gc_vtab;
		index->gc_iterator = memtx_tree_iterator_first(&index->tree);
		memtx_engine_schedule_gc(memtx, &index->gc_task);
	} else {
		/*
		 * Secondary index. Destruction is fast, no need to
		 * hand over to background fiber.
		 */
		memtx_tree_index_free(index);
	}
}

static void
memtx_tree_index_update_def(struct index *base)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct index_def *def = base->def;
	/*
	 * We use extended key def for non-unique and nullable
	 * indexes. Unique but nullable index can store multiple
	 * NULLs. To correctly compare these NULLs extended key
	 * def must be used. For details @sa tuple_compare.cc.
	 */
	index->tree.arg = def->opts.is_unique && !def->key_def->is_nullable ?
						def->key_def : def->cmp_def;
}

static bool
memtx_tree_index_depends_on_pk(struct index *base)
{
	struct index_def *def = base->def;
	/* See comment to memtx_tree_index_update_def(). */
	return !def->opts.is_unique || def->key_def->is_nullable;
}

static ssize_t
memtx_tree_index_size(struct index *base)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	return memtx_tree_size(&index->tree);
}

static ssize_t
memtx_tree_index_bsize(struct index *base)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	return memtx_tree_mem_used(&index->tree);
}

static int
memtx_tree_index_random(struct index *base, uint32_t rnd, struct tuple **result)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct memtx_tree_data *res = memtx_tree_random(&index->tree, rnd);
	*result = res != NULL ? res->tuple : NULL;
	return 0;
}

static ssize_t
memtx_tree_index_count(struct index *base, enum iterator_type type,
		       const char *key, uint32_t part_count)
{
	if (type == ITER_ALL)
		return memtx_tree_index_size(base); /* optimization */
	return generic_index_count(base, type, key, part_count);
}

static int
memtx_tree_index_get(struct index *base, const char *key,
		     uint32_t part_count, struct tuple **result)
{
	assert(base->def->opts.is_unique &&
	       part_count == base->def->key_def->part_count);
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	struct memtx_tree_key_data key_data;
	key_data.key = key;
	key_data.part_count = part_count;
	key_data.hint = key_hint(key, part_count, cmp_def);
	struct memtx_tree_data *res = memtx_tree_find(&index->tree, &key_data);
	*result = res != NULL ? res->tuple : NULL;
	return 0;
}

static int
memtx_tree_index_replace(struct index *base, struct tuple *old_tuple,
			 struct tuple *new_tuple, enum dup_replace_mode mode,
			 struct tuple **result)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	if (new_tuple) {
		struct memtx_tree_data new_data;
		new_data.tuple = new_tuple;
		new_data.hint = tuple_hint(new_tuple, cmp_def);
		struct memtx_tree_data dup_data;
		dup_data.tuple = NULL;

		/* Try to optimistically replace the new_tuple. */
		int tree_res = memtx_tree_insert(&index->tree, new_data,
						 &dup_data);
		if (tree_res) {
			diag_set(OutOfMemory, MEMTX_EXTENT_SIZE,
				 "memtx_tree_index", "replace");
			return -1;
		}

		uint32_t errcode = replace_check_dup(old_tuple,
						     dup_data.tuple, mode);
		if (errcode) {
			memtx_tree_delete(&index->tree, new_data);
			if (dup_data.tuple != NULL)
				memtx_tree_insert(&index->tree, dup_data, NULL);
			struct space *sp = space_cache_find(base->def->space_id);
			if (sp != NULL)
				diag_set(ClientError, errcode, base->def->name,
					 space_name(sp));
			return -1;
		}
		if (dup_data.tuple != NULL) {
			*result = dup_data.tuple;
			return 0;
		}
	}
	if (old_tuple) {
		struct memtx_tree_data old_data;
		old_data.tuple = old_tuple;
		old_data.hint = tuple_hint(old_tuple, cmp_def);
		memtx_tree_delete(&index->tree, old_data);
	}
	*result = old_tuple;
	return 0;
}

/**
 * Perform tuple insertion by given multikey index.
 * In case of replacement, all old tuple entries are deleted
 * by all it's multikey indexes.
 */
static int
memtx_tree_index_replace_multikey_one(struct memtx_tree_index *index,
			struct tuple *old_tuple, struct tuple *new_tuple,
			enum dup_replace_mode mode, hint_t hint,
			struct memtx_tree_data *replaced_data,
			bool *is_multikey_conflict)
{
	struct memtx_tree_data new_data, dup_data;
	new_data.tuple = new_tuple;
	new_data.hint = hint;
	dup_data.tuple = NULL;
	*is_multikey_conflict = false;
	if (memtx_tree_insert(&index->tree, new_data, &dup_data) != 0) {
		diag_set(OutOfMemory, MEMTX_EXTENT_SIZE, "memtx_tree_index",
			 "replace");
		return -1;
	}
	int errcode = 0;
	if (dup_data.tuple == new_tuple) {
		/*
		 * When tuple contains the same key multiple
		 * times, the previous key occurrence is pushed
		 * out of the index.
		 */
		*is_multikey_conflict = true;
	} else if ((errcode = replace_check_dup(old_tuple, dup_data.tuple,
					        mode)) != 0) {
		/* Rollback replace. */
		memtx_tree_delete(&index->tree, new_data);
		if (dup_data.tuple != NULL)
			memtx_tree_insert(&index->tree, dup_data, NULL);
		struct space *sp = space_cache_find(index->base.def->space_id);
		if (sp != NULL) {
			diag_set(ClientError, errcode, index->base.def->name,
				 space_name(sp));
		}
		return -1;
	}
	*replaced_data = dup_data;
	return 0;
}

/**
 * Rollback the sequence of memtx_tree_index_replace_multikey_one
 * insertions with multikey indexes [0, err_multikey_idx - 1]
 * where the err_multikey_idx is the first multikey index where
 * error has been raised.
 *
 * This routine can't fail because all replaced_tuple (when
 * specified) nodes in tree are already allocated (they might be
 * overridden with new_tuple, but they definitely present) and
 * delete operation is fault-tolerant.
 */
static void
memtx_tree_index_replace_multikey_rollback(struct memtx_tree_index *index,
			struct tuple *new_tuple, struct tuple *replaced_tuple,
			int err_multikey_idx)
{
	struct memtx_tree_data data;
	if (replaced_tuple != NULL) {
		/* Restore replaced tuple index occurrences. */
		struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
		data.tuple = replaced_tuple;
		uint32_t multikey_count =
			tuple_multikey_count(replaced_tuple, cmp_def);
		for (int i = 0; (uint32_t) i < multikey_count; i++) {
			data.hint = i;
			memtx_tree_insert(&index->tree, data, NULL);
		}
	}
	/*
	 * Rollback new_tuple insertion by multikey index
	 * [0, multikey_idx).
	 */
	data.tuple = new_tuple;
	for (int i = 0; i < err_multikey_idx; i++) {
		data.hint = i;
		memtx_tree_delete_value(&index->tree, data, NULL);
	}
}

/**
 * :replace() function for a multikey index: replace old tuple
 * index entries with ones from the new tuple.
 *
 * In a multikey index a single tuple is associated with 0..N keys
 * of the b+*tree. Imagine old tuple key set is called "old_keys"
 * and a new tuple set is called "new_keys". This function must
 * 1) delete all removed keys: (new_keys - old_keys)
 * 2) update tuple pointer in all preserved keys: (old_keys ^ new_keys)
 * 3) insert data for all new keys (new_keys - old_keys).
 *
 * Compare with a standard unique or non-unique index, when a key
 * is present only once, so whenever we encounter a duplicate, it
 * is guaranteed to point at the old tuple (in non-unique indexes
 * we augment the secondary key parts with primary key parts, so
 * b+*tree still contains unique entries only).
 *
 * To reduce the number of insert and delete operations on the
 * tree, this function attempts to optimistically add all keys
 * from the new tuple to the tree first.
 *
 * When this step finds a duplicate, it's either of the following:
 * - for a unique multikey index, it may be the old tuple or
 *   some other tuple. Since unique index forbids duplicates,
 *   this branch ends with an error unless we found the old tuple.
 * - for a non-unique multikey index, both secondary and primary
 *   key parts must match, so it's guaranteed to be the old tuple.
 *
 * In other words, when an optimistic insert finds a duplicate,
 * it's either an error, in which case we roll back all the new
 * keys from the tree and abort the procedure, or the old tuple,
 * which we save to get back to, later.
 *
 * When adding new keys finishes, we have completed steps
 * 2) and 3):
 * - added set (new_keys - old_keys) to the index
 * - updated set (new_keys ^ old_keys) with a new tuple pointer.
 *
 * We now must perform 1), which is remove (old_keys - new_keys).
 *
 * This is done by using the old tuple pointer saved from the
 * previous step. To not accidentally delete the common key
 * set of the old and the new tuple, we don't using key parts alone
 * to compare - we also look at b+* tree value that has the tuple
 * pointer, and delete old tuple entries only.
 */
static int
memtx_tree_index_replace_multikey(struct index *base, struct tuple *old_tuple,
			struct tuple *new_tuple, enum dup_replace_mode mode,
			struct tuple **result)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	*result = NULL;
	if (new_tuple != NULL) {
		int multikey_idx = 0, err = 0;
		uint32_t multikey_count =
			tuple_multikey_count(new_tuple, cmp_def);
		for (; (uint32_t) multikey_idx < multikey_count;
		     multikey_idx++) {
			bool is_multikey_conflict;
			struct memtx_tree_data replaced_data;
			err = memtx_tree_index_replace_multikey_one(index,
						old_tuple, new_tuple, mode,
						multikey_idx, &replaced_data,
						&is_multikey_conflict);
			if (err != 0)
				break;
			if (replaced_data.tuple != NULL &&
			    !is_multikey_conflict) {
				assert(*result == NULL ||
				       *result == replaced_data.tuple);
				*result = replaced_data.tuple;
			}
		}
		if (err != 0) {
			memtx_tree_index_replace_multikey_rollback(index,
					new_tuple, *result, multikey_idx);
			return -1;
		}
		if (*result != NULL) {
			assert(old_tuple == NULL || old_tuple == *result);
			old_tuple = *result;
		}
	}
	if (old_tuple != NULL) {
		struct memtx_tree_data data;
		data.tuple = old_tuple;
		uint32_t multikey_count =
			tuple_multikey_count(old_tuple, cmp_def);
		for (int i = 0; (uint32_t) i < multikey_count; i++) {
			data.hint = i;
			memtx_tree_delete_value(&index->tree, data, NULL);
		}
	}
	return 0;
}


/** Release memory allocated for key_hint. */
static void
key_hint_destroy(struct memtx_tree_data *data)
{
	key_hint_delete(data->tuple, data->hint);
}

/**
 * The journal for multikey functional index replace operation
 * is required to rollback an incomplete action, restore the
 * original key_hint(s) hints both as to commit a completed
 * replace action and destruct useless key_hint(s).
*/
struct journal_entry {
	/** An inserted record copy. */
	struct memtx_tree_data inserted;
	/** A replaced record copy. */
	struct memtx_tree_data replaced;
};

/**
 * Rollback a sequence of memtx_tree_index_replace_multikey_one
 * insertions for functional index. Routine uses given journal
 * to return given index object in it's original state.
 */
static void
memtx_tree_index_replace_functional_rollback(struct memtx_tree_index *index,
						struct journal_entry *journal,
						int journal_sz)
{
	for (int i = 0; i < journal_sz; i++) {
		if (journal[i].replaced.tuple != NULL) {
			memtx_tree_insert(&index->tree, journal[i].replaced,
					  NULL);
		} else {
			memtx_tree_delete_value(&index->tree,
						journal[i].inserted, NULL);
		}
		key_hint_destroy(&journal[i].inserted);
	}
}

/**
 * Commit a sequence of memtx_tree_index_replace_multikey_one
 * insertions for functional index. Rotine uses given operations
 * journal to release unused memory.
 */
static void
memtx_tree_index_replace_functional_commit(struct memtx_tree_index *index,
						struct journal_entry *journal,
						int journal_sz)
{
	(void) index;
	for (int i = 0; i < journal_sz; i++) {
		if (journal[i].replaced.tuple == NULL)
			continue;
		key_hint_destroy(&journal[i].replaced);
	}
}

/**
 * @sa memtx_tree_index_replace_multikey().
 * Use functional index function from the key definition
 * to build a key list. Then each returned key is reallocated in
 * engine's memory as key_hint object and is used as comparison
 * hint.
 * To control key_hint(s) life cycle in case of functional
 * index we use a tiny journal object is allocated on region.
 * It allows to restore original nodes with their original
 * key_hint(s) pointers in case of failure and release
 * useless hints of replaced items in case of success.
 */
static int
memtx_tree_index_replace_functional(struct index *base, struct tuple *old_tuple,
			struct tuple *new_tuple, enum dup_replace_mode mode,
			struct tuple **result)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	assert(key_def_is_for_func_index(cmp_def));

	int rc = -1;
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	*result = NULL;
	const char *key_list_end;
	const char *key_list;
	uint32_t key_cnt;
	struct key_list_iterator it;
	if (new_tuple != NULL) {
		key_list = key_list_create(new_tuple, cmp_def->func_index_func,
					   &key_list_end, &key_cnt);
		if (key_list == NULL)
			goto end;

		int journal_idx = 0;
		struct journal_entry *journal =
			region_alloc(region, key_cnt * sizeof(*journal));
		if (journal == NULL) {
			diag_set(OutOfMemory, key_cnt * sizeof(*journal),
				 "region", "journal");
			goto end;
		}

		int err = 0;
		key_list_iterator_create(&it, key_list, key_list_end, cmp_def, true);
		const char *key;
		uint32_t key_sz;
		while (key_list_iterator_next(&it, &key, &key_sz) == 0 &&
		       key != NULL) {
			hint_t key_hint = key_hint_new(new_tuple, key, key_sz);
			if (key_hint == HINT_NONE) {
				err = -1;
				break;
			}

			/* Perform insertion, log it in journal. */
			bool is_multikey_conflict;
			journal[journal_idx].replaced.tuple = NULL;
			journal[journal_idx].inserted.tuple = new_tuple;
			journal[journal_idx].inserted.hint = key_hint;
			err = memtx_tree_index_replace_multikey_one(index,
						old_tuple, new_tuple, mode,
						key_hint,
						&journal[journal_idx].replaced,
						&is_multikey_conflict);
			if (err != 0)
				break;
			/**
			 * Modify a 'replace' record of journal
			 * because an original node shouldn't be
			 * restored in case of multikey conflict.
			 */
			if (is_multikey_conflict)
				journal[journal_idx].replaced.tuple = NULL;
			else if (journal[journal_idx].replaced.tuple != NULL)
				*result = journal[journal_idx].replaced.tuple;

			++journal_idx;
		}
		if (key != NULL || err != 0) {
			memtx_tree_index_replace_functional_rollback(index,
							journal, journal_idx);
			goto end;
		}
		if (*result != NULL) {
			assert(old_tuple == NULL || old_tuple == *result);
			old_tuple = *result;
		}
		memtx_tree_index_replace_functional_commit(index,
						journal, journal_idx);
	}
	if (old_tuple != NULL) {
		key_list = key_list_create(old_tuple, cmp_def->func_index_func,
					   &key_list_end, &key_cnt);
		if (key_list == NULL)
			goto end;
		struct memtx_tree_data data, deleted_data;
		data.tuple = old_tuple;
		key_list_iterator_create(&it, key_list, key_list_end, cmp_def, true);
		const char *key;
		uint32_t key_sz;
		while (key_list_iterator_next(&it, &key, &key_sz) == 0 &&
		       key != NULL) {
			data.hint = (hint_t) key; /* Raw data. */
			deleted_data.tuple = NULL;
			memtx_tree_delete_value(&index->tree, data,
						&deleted_data);
			if (deleted_data.tuple != NULL) {
				/*
				 * Release related hint on
				 * successfull node deletion.
				 */
				key_hint_destroy(&deleted_data);
			}
		}
		assert(key == NULL);
	}
	rc = 0;
end:
	region_truncate(region, region_svp);
	return rc;
}

static struct iterator *
memtx_tree_index_create_iterator(struct index *base, enum iterator_type type,
				 const char *key, uint32_t part_count)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct memtx_engine *memtx = (struct memtx_engine *)base->engine;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);

	assert(part_count == 0 || key != NULL);
	if (type > ITER_GT) {
		diag_set(UnsupportedIndexFeature, base->def,
			 "requested iterator type");
		return NULL;
	}

	if (part_count == 0) {
		/*
		 * If no key is specified, downgrade equality
		 * iterators to a full range.
		 */
		type = iterator_type_is_reverse(type) ? ITER_LE : ITER_GE;
		key = NULL;
	}

	struct tree_iterator *it = mempool_alloc(&memtx->iterator_pool);
	if (it == NULL) {
		diag_set(OutOfMemory, sizeof(struct tree_iterator),
			 "memtx_tree_index", "iterator");
		return NULL;
	}
	iterator_create(&it->base, base);
	it->pool = &memtx->iterator_pool;
	it->base.next = tree_iterator_start;
	it->base.free = tree_iterator_free;
	it->type = type;
	it->key_data.key = key;
	it->key_data.part_count = part_count;
	it->key_data.hint = key_hint(key, part_count, cmp_def);
	it->index_def = base->def;
	it->tree = &index->tree;
	it->tree_iterator = memtx_tree_invalid_iterator();
	it->current.tuple = NULL;
	return (struct iterator *)it;
}

static void
memtx_tree_index_begin_build(struct index *base)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	assert(memtx_tree_size(&index->tree) == 0);
	(void)index;
}

static int
memtx_tree_index_reserve(struct index *base, uint32_t size_hint)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	if (size_hint < index->build_array_alloc_size)
		return 0;
	struct memtx_tree_data *tmp =
		realloc(index->build_array, size_hint * sizeof(*tmp));
	if (tmp == NULL) {
		diag_set(OutOfMemory, size_hint * sizeof(*tmp),
			 "memtx_tree_index", "reserve");
		return -1;
	}
	index->build_array = tmp;
	index->build_array_alloc_size = size_hint;
	return 0;
}

/** Initialize the next element of the index build_array. */
static int
memtx_tree_index_build_array_append(struct memtx_tree_index *index,
				    struct tuple *tuple, hint_t hint)
{
	if (index->build_array == NULL) {
		index->build_array = malloc(MEMTX_EXTENT_SIZE);
		if (index->build_array == NULL) {
			diag_set(OutOfMemory, MEMTX_EXTENT_SIZE,
				 "memtx_tree_index", "build_next");
			return -1;
		}
		index->build_array_alloc_size =
			MEMTX_EXTENT_SIZE / sizeof(index->build_array[0]);
	}
	assert(index->build_array_size <= index->build_array_alloc_size);
	if (index->build_array_size == index->build_array_alloc_size) {
		index->build_array_alloc_size = index->build_array_alloc_size +
				DIV_ROUND_UP(index->build_array_alloc_size, 2);
		struct memtx_tree_data *tmp =
			realloc(index->build_array,
				index->build_array_alloc_size * sizeof(*tmp));
		if (tmp == NULL) {
			diag_set(OutOfMemory, index->build_array_alloc_size *
				 sizeof(*tmp), "memtx_tree_index", "build_next");
			return -1;
		}
		index->build_array = tmp;
	}
	struct memtx_tree_data *elem =
		&index->build_array[index->build_array_size++];
	elem->tuple = tuple;
	elem->hint = hint;
	return 0;
}

static int
memtx_tree_index_build_next(struct index *base, struct tuple *tuple)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	return memtx_tree_index_build_array_append(index, tuple,
						   tuple_hint(tuple, cmp_def));
}

static int
memtx_tree_index_build_next_multikey(struct index *base, struct tuple *tuple)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	uint32_t multikey_count = tuple_multikey_count(tuple, cmp_def);
	for (uint32_t multikey_idx = 0; multikey_idx < multikey_count;
	     multikey_idx++) {
		if (memtx_tree_index_build_array_append(index, tuple,
							multikey_idx) != 0)
			return -1;
	}
	return 0;
}

static int
memtx_tree_index_build_next_functional(struct index *base, struct tuple *tuple)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	assert(key_def_is_for_func_index(cmp_def));

	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	uint32_t key_cnt;
	const char *key_list;
	const char *key_list_end;
	key_list = key_list_create(tuple, cmp_def->func_index_func,
				   &key_list_end, &key_cnt);
	if (key_list == NULL)
		return -1;

	uint32_t insert_idx = index->build_array_size;
	struct key_list_iterator it;
	key_list_iterator_create(&it, key_list, key_list_end, cmp_def, true);
	const char *key;
	uint32_t key_sz;
	while (key_list_iterator_next(&it, &key, &key_sz) == 0 && key != NULL) {
		hint_t key_hint = key_hint_new(tuple, key, key_sz);
		if (key_hint == HINT_NONE)
			goto error;
		if (memtx_tree_index_build_array_append(index, tuple,
							key_hint) != 0)
			goto error;
	}
	assert(key == NULL);
	region_truncate(region, region_svp);
	return 0;
error:
	for (uint32_t i = insert_idx; i < index->build_array_size; i++)
		key_hint_destroy(&index->build_array[i]);
	region_truncate(region, region_svp);
	return -1;
}

/**
 * Process build_array of specified index and remove duplicates
 * of equal tuples (in terms of index's cmp_def and have same
 * tuple pointer). The build_array is expected to be sorted.
 */
static void
memtx_tree_index_build_array_deduplicate(struct memtx_tree_index *index,
			void (*data_destructor)(struct memtx_tree_data *data))
{
	if (index->build_array_size == 0)
		return;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	size_t w_idx = 0, r_idx = 1;
	while (r_idx < index->build_array_size) {
		if (index->build_array[w_idx].tuple !=
		    index->build_array[r_idx].tuple ||
		    tuple_compare(index->build_array[w_idx].tuple,
				  index->build_array[w_idx].hint,
				  index->build_array[r_idx].tuple,
				  index->build_array[r_idx].hint,
				  cmp_def) != 0) {
			/* Do not override the element itself. */
			if (++w_idx == r_idx)
				continue;
			index->build_array[w_idx] = index->build_array[r_idx];
			if (data_destructor == NULL)
				continue;
			for (size_t i = w_idx + 1; i < r_idx; i++)
				data_destructor(&index->build_array[i]);
		}
		r_idx++;
	}
	index->build_array_size = w_idx + 1;
	if (data_destructor != NULL) {
		for (size_t i = index->build_array_size + 1; i < r_idx; i++)
			data_destructor(&index->build_array[i]);
	}
}

static void
memtx_tree_index_end_build(struct index *base)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct key_def *cmp_def = memtx_tree_cmp_def(&index->tree);
	qsort_arg(index->build_array, index->build_array_size,
		  sizeof(index->build_array[0]), memtx_tree_qcompare, cmp_def);
	if (cmp_def->is_multikey) {
		/*
		 * Multikey index may have equal(in terms of
		 * cmp_def) keys inserted by different multikey
		 * offsets. We must deduplicate them because
		 * the following memtx_tree_build assumes that
		 * all keys are unique.
		 */
		memtx_tree_index_build_array_deduplicate(index, NULL);
	} else if (key_def_is_for_func_index(cmp_def)) {
		memtx_tree_index_build_array_deduplicate(index, key_hint_destroy);
	}
	memtx_tree_build(&index->tree, index->build_array,
			 index->build_array_size);

	free(index->build_array);
	index->build_array = NULL;
	index->build_array_size = 0;
	index->build_array_alloc_size = 0;
}

struct tree_snapshot_iterator {
	struct snapshot_iterator base;
	struct memtx_tree *tree;
	struct memtx_tree_iterator tree_iterator;
};

static void
tree_snapshot_iterator_free(struct snapshot_iterator *iterator)
{
	assert(iterator->free == tree_snapshot_iterator_free);
	struct tree_snapshot_iterator *it =
		(struct tree_snapshot_iterator *)iterator;
	struct memtx_tree *tree = (struct memtx_tree *)it->tree;
	memtx_tree_iterator_destroy(tree, &it->tree_iterator);
	free(iterator);
}

static const char *
tree_snapshot_iterator_next(struct snapshot_iterator *iterator, uint32_t *size)
{
	assert(iterator->free == tree_snapshot_iterator_free);
	struct tree_snapshot_iterator *it =
		(struct tree_snapshot_iterator *)iterator;
	struct memtx_tree_data *res =
		memtx_tree_iterator_get_elem(it->tree, &it->tree_iterator);
	if (res == NULL)
		return NULL;
	memtx_tree_iterator_next(it->tree, &it->tree_iterator);
	return tuple_data_range(res->tuple, size);
}

/**
 * Create an ALL iterator with personal read view so further
 * index modifications will not affect the iteration results.
 * Must be destroyed by iterator->free after usage.
 */
static struct snapshot_iterator *
memtx_tree_index_create_snapshot_iterator(struct index *base)
{
	struct memtx_tree_index *index = (struct memtx_tree_index *)base;
	struct tree_snapshot_iterator *it = (struct tree_snapshot_iterator *)
		calloc(1, sizeof(*it));
	if (it == NULL) {
		diag_set(OutOfMemory, sizeof(struct tree_snapshot_iterator),
			 "memtx_tree_index", "create_snapshot_iterator");
		return NULL;
	}

	it->base.free = tree_snapshot_iterator_free;
	it->base.next = tree_snapshot_iterator_next;
	it->tree = &index->tree;
	it->tree_iterator = memtx_tree_iterator_first(&index->tree);
	memtx_tree_iterator_freeze(&index->tree, &it->tree_iterator);
	return (struct snapshot_iterator *) it;
}

static const struct index_vtab memtx_tree_index_vtab = {
	/* .destroy = */ memtx_tree_index_destroy,
	/* .commit_create = */ generic_index_commit_create,
	/* .abort_create = */ generic_index_abort_create,
	/* .commit_modify = */ generic_index_commit_modify,
	/* .commit_drop = */ generic_index_commit_drop,
	/* .update_def = */ memtx_tree_index_update_def,
	/* .depends_on_pk = */ memtx_tree_index_depends_on_pk,
	/* .def_change_requires_rebuild = */
		memtx_index_def_change_requires_rebuild,
	/* .size = */ memtx_tree_index_size,
	/* .bsize = */ memtx_tree_index_bsize,
	/* .min = */ generic_index_min,
	/* .max = */ generic_index_max,
	/* .random = */ memtx_tree_index_random,
	/* .count = */ memtx_tree_index_count,
	/* .get = */ memtx_tree_index_get,
	/* .replace = */ memtx_tree_index_replace,
	/* .create_iterator = */ memtx_tree_index_create_iterator,
	/* .create_snapshot_iterator = */
		memtx_tree_index_create_snapshot_iterator,
	/* .stat = */ generic_index_stat,
	/* .compact = */ generic_index_compact,
	/* .reset_stat = */ generic_index_reset_stat,
	/* .begin_build = */ memtx_tree_index_begin_build,
	/* .reserve = */ memtx_tree_index_reserve,
	/* .build_next = */ memtx_tree_index_build_next,
	/* .end_build = */ memtx_tree_index_end_build,
};

static const struct index_vtab memtx_tree_index_multikey_vtab = {
	/* .destroy = */ memtx_tree_index_destroy,
	/* .commit_create = */ generic_index_commit_create,
	/* .abort_create = */ generic_index_abort_create,
	/* .commit_modify = */ generic_index_commit_modify,
	/* .commit_drop = */ generic_index_commit_drop,
	/* .update_def = */ memtx_tree_index_update_def,
	/* .depends_on_pk = */ memtx_tree_index_depends_on_pk,
	/* .def_change_requires_rebuild = */
		memtx_index_def_change_requires_rebuild,
	/* .size = */ memtx_tree_index_size,
	/* .bsize = */ memtx_tree_index_bsize,
	/* .min = */ generic_index_min,
	/* .max = */ generic_index_max,
	/* .random = */ memtx_tree_index_random,
	/* .count = */ memtx_tree_index_count,
	/* .get = */ memtx_tree_index_get,
	/* .replace = */ memtx_tree_index_replace_multikey,
	/* .create_iterator = */ memtx_tree_index_create_iterator,
	/* .create_snapshot_iterator = */
		memtx_tree_index_create_snapshot_iterator,
	/* .stat = */ generic_index_stat,
	/* .compact = */ generic_index_compact,
	/* .reset_stat = */ generic_index_reset_stat,
	/* .begin_build = */ memtx_tree_index_begin_build,
	/* .reserve = */ memtx_tree_index_reserve,
	/* .build_next = */ memtx_tree_index_build_next_multikey,
	/* .end_build = */ memtx_tree_index_end_build,
};

static const struct index_vtab memtx_tree_index_functional_vtab = {
	/* .destroy = */ memtx_tree_index_destroy,
	/* .commit_create = */ generic_index_commit_create,
	/* .abort_create = */ generic_index_abort_create,
	/* .commit_modify = */ generic_index_commit_modify,
	/* .commit_drop = */ generic_index_commit_drop,
	/* .update_def = */ memtx_tree_index_update_def,
	/* .depends_on_pk = */ memtx_tree_index_depends_on_pk,
	/* .def_change_requires_rebuild = */
		memtx_index_def_change_requires_rebuild,
	/* .size = */ memtx_tree_index_size,
	/* .bsize = */ memtx_tree_index_bsize,
	/* .min = */ generic_index_min,
	/* .max = */ generic_index_max,
	/* .random = */ memtx_tree_index_random,
	/* .count = */ memtx_tree_index_count,
	/* .get = */ memtx_tree_index_get,
	/* .replace = */ memtx_tree_index_replace_functional,
	/* .create_iterator = */ memtx_tree_index_create_iterator,
	/* .create_snapshot_iterator = */
		memtx_tree_index_create_snapshot_iterator,
	/* .stat = */ generic_index_stat,
	/* .compact = */ generic_index_compact,
	/* .reset_stat = */ generic_index_reset_stat,
	/* .begin_build = */ memtx_tree_index_begin_build,
	/* .reserve = */ memtx_tree_index_reserve,
	/* .build_next = */ memtx_tree_index_build_next_functional,
	/* .end_build = */ memtx_tree_index_end_build,
};

/**
 * A disabled index vtab provides safe dummy methods for
 * 'inactive' index. It is required to perform a fault-tolerant
 * recovery from snapshoot in case of functional index (because
 * key defintion is not completely initialized at that moment).
 */
static const struct index_vtab memtx_tree_index_disabled_vtab = {
	/* .destroy = */ memtx_tree_index_destroy,
	/* .commit_create = */ generic_index_commit_create,
	/* .abort_create = */ generic_index_abort_create,
	/* .commit_modify = */ generic_index_commit_modify,
	/* .commit_drop = */ generic_index_commit_drop,
	/* .update_def = */ generic_index_update_def,
	/* .depends_on_pk = */ generic_index_depends_on_pk,
	/* .def_change_requires_rebuild = */
		generic_index_def_change_requires_rebuild,
	/* .size = */ generic_index_size,
	/* .bsize = */ generic_index_bsize,
	/* .min = */ generic_index_min,
	/* .max = */ generic_index_max,
	/* .random = */ generic_index_random,
	/* .count = */ generic_index_count,
	/* .get = */ generic_index_get,
	/* .replace = */ disabled_index_replace,
	/* .create_iterator = */ generic_index_create_iterator,
	/* .create_snapshot_iterator = */
		generic_index_create_snapshot_iterator,
	/* .stat = */ generic_index_stat,
	/* .compact = */ generic_index_compact,
	/* .reset_stat = */ generic_index_reset_stat,
	/* .begin_build = */ generic_index_begin_build,
	/* .reserve = */ generic_index_reserve,
	/* .build_next = */ disabled_index_build_next,
	/* .end_build = */ generic_index_end_build,
};

struct index *
memtx_tree_index_new(struct memtx_engine *memtx, struct index_def *def)
{
	struct memtx_tree_index *index =
		(struct memtx_tree_index *)calloc(1, sizeof(*index));
	if (index == NULL) {
		diag_set(OutOfMemory, sizeof(*index),
			 "malloc", "struct memtx_tree_index");
		return NULL;
	}
	const struct index_vtab *vtab;
	if (key_def_is_for_func_index(def->key_def)) {
		if (def->key_def->func_index_func == NULL)
			vtab = &memtx_tree_index_disabled_vtab;
		else
			vtab = &memtx_tree_index_functional_vtab;
	} else if (def->key_def->is_multikey) {
		vtab = &memtx_tree_index_multikey_vtab;
	} else {
		vtab = &memtx_tree_index_vtab;
	}
	if (index_create(&index->base, (struct engine *)memtx,
			 vtab, def) != 0) {
		free(index);
		return NULL;
	}

	/* See comment to memtx_tree_index_update_def(). */
	struct key_def *cmp_def;
	cmp_def = def->opts.is_unique && !def->key_def->is_nullable ?
			index->base.def->key_def : index->base.def->cmp_def;

	memtx_tree_create(&index->tree, cmp_def, memtx_index_extent_alloc,
			  memtx_index_extent_free, memtx);
	return &index->base;
}
