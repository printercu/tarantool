#ifndef TARANTOOL_BOX_FUNC_KEY_H_INCLUDED
#define TARANTOOL_BOX_FUNC_KEY_H_INCLUDED
/*
 * Copyright 2010-2019, Tarantool AUTHORS, please see AUTHORS file.
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
#include <stdbool.h>
#include <inttypes.h>

#include "tuple_compare.h"

#ifdef __cplusplus
extern "C" {
#endif

struct func;
struct key_def;
struct tuple;

/**
 * Execute a given function (a functional index function) and
 * return an extracted key_data and key_data_sz and count of
 * extracted keys.
 *
 * Returns not NULL key_data pointer in case of success.
 * Routine allocates memory on fiber's region.
 */
const char *
func_key_extract(struct tuple *tuple, struct func *func,
		 const char **data_end, uint32_t *key_count);

/**
 * An iterator to iterate over the key_data returned by function
 * and validate it with given key definition (when required).
 */
struct func_key_iterator {
	/** The pointer to currently processed key. */
	const char *data;
	/** The pointer to the end of extracted key_data. */
	const char *data_end;
	/**
	 * The sequential functional index key definition that
	 * describes a format of functional index function keys.
	 */
	struct key_def *key_def;
	/** Whether iterator must validate processed keys. */
	bool validate;
};

/**
 * Initialize a new functional index function returned keys
 * iterator.
 */
static inline void
func_key_iterator_create(struct func_key_iterator *it, const char *data,
			 const char *data_end, struct key_def *key_def,
			 bool validate)
{
	it->data = data;
	it->data_end = data_end;
	it->key_def = key_def;
	it->validate = validate;
}

/**
 * Perform key iterator step and update iterator state.
 * Update key pointer with an actual key.
 *
 * Returns 0 on success. In case of error returns -1 and sets
 * the corresponding diag message.
 */
int
func_key_iterator_next(struct func_key_iterator *it, const char **key,
		       uint32_t *key_sz);

/**
 * Allocate a new func_key_hint for given tuple and return a
 * pointer to the hint value memory.
 */
hint_t
func_key_hint_new(struct tuple *tuple, const char *key, uint32_t key_sz);

/** Release a given func_key_hint memory chunk. */
void
func_key_hint_delete(struct tuple *tuple, hint_t func_key_hint);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TARANTOOL_BOX_FUNC_KEY_H_INCLUDED */
