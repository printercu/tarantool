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
#include "key_list.h"

#include "errcode.h"
#include "diag.h"
#include "func.h"
#include "key_def.h"
#include "port.h"
#include "tt_static.h"
#include "tuple.h"
#include "tuple_compare.h"

const char *
func_key_build(struct tuple *tuple, struct func *func,
		 const char **data_end, uint32_t *key_count)
{
	struct port out_port, in_port;
	port_tuple_create(&in_port);
	port_tuple_add(&in_port, tuple);
	int rc = func_call(func, &in_port, &out_port);
	port_destroy(&in_port);
	if (rc != 0)
		goto error;
	uint32_t key_data_sz;
	const char *key_data = port_get_msgpack(&out_port, &key_data_sz);
	port_destroy(&out_port);
	if (key_data == NULL)
		goto error;

	assert(mp_typeof(*key_data) == MP_ARRAY);
	*data_end = key_data + key_data_sz;
	*key_count = mp_decode_array(&key_data);
	return key_data;
error:
	diag_set(ClientError, ER_FUNCTIONAL_INDEX_FUNC_ERROR, func->def->name,
		 diag_last_error(diag_get())->errmsg);
	return NULL;
}

int
func_key_iterator_next(struct func_key_iterator *it, const char **key,
		       uint32_t *key_sz)
{
	assert(it->data <= it->data_end);
	if (it->data == it->data_end) {
		*key = NULL;
		*key_sz = 0;
		return 0;
	}
	*key = it->data;
	if (!it->validate) {
		mp_next(&it->data);
		assert(it->data <= it->data_end);
		*key_sz = it->data - *key;
		return 0;
	}

	if (mp_typeof(*it->data) != MP_ARRAY) {
		diag_set(ClientError, ER_FUNCTIONAL_INDEX_FUNC_ERROR,
			 it->key_def->func_index_func->def->name,
			 "returned key type is invalid");
		return -1;
	}
	const char *rptr = *key;
	uint32_t part_count = mp_decode_array(&rptr);
	uint32_t functional_part_count = it->key_def->functional_part_count;
	if (part_count != functional_part_count) {
		const char *error_msg =
			tt_sprintf(tnt_errcode_desc(ER_EXACT_MATCH),
				   functional_part_count, part_count);
		diag_set(ClientError, ER_FUNCTIONAL_INDEX_FUNC_ERROR,
			 it->key_def->func_index_func->def->name, error_msg);
		return -1;
	}
	const char *key_end;
	if (key_validate_parts(it->key_def, rptr, functional_part_count, true,
			       &key_end) != 0) {
		diag_set(ClientError, ER_FUNCTIONAL_INDEX_FUNC_ERROR,
			 it->key_def->func_index_func->def->name,
			 diag_last_error(diag_get())->errmsg);
		return -1;
	}

	*key_sz = key_end - *key;
	it->data = key_end;
	return 0;
}

hint_t
key_list.hint_new(struct tuple *tuple, const char *key, uint32_t key_sz)
{
	struct tuple_chunk *chunk = tuple_chunk_new(tuple, key_sz);
	if (chunk == NULL)
		return HINT_NONE;
	memcpy(chunk->data, key, key_sz);
	return (hint_t)chunk->data;
}

void
key_list.hint_delete(struct tuple *tuple, hint_t key_list.hint)
{
	struct tuple_chunk *chunk =
		container_of((typeof(chunk->data) *)key_list.hint,
			     struct tuple_chunk, data);
	tuple_chunk_delete(tuple, chunk);
}
