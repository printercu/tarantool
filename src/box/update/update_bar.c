/*
 * Copyright 2010-2018, Tarantool AUTHORS, please see AUTHORS file.
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
#include "update_field.h"
#include "trivia/util.h"
#include "json/json.h"
#include "box/tuple_format.h"
#include "box/tuple.h"

/**
 * Locate the field to update by JSON path in @a op->path. If
 * found, initialize @a field.
 * @param op Update operation.
 * @param field Field to locate in.
 *
 * @retval 0 Success.
 * @retval -1 Not found or invalid JSON.
 */
static inline int
update_bar_locate(struct update_op *op, struct update_field *field)
{
	const char *pos = field->data;
	const char *path = op->path + op->path_offset;
	int len = op->path_len - op->path_offset;
	int rc = tuple_go_to_path(&pos, path, len, MULTIKEY_NONE);
	if (rc != 0)
		return update_err_bad_json(op, rc);
	if (pos == NULL)
		return update_err_no_such_field(op);
	field->bar.point = pos;
	mp_next(&pos);
	field->bar.point_size = pos - field->bar.point;
	field->type = UPDATE_BAR;
	field->bar.op = op;
	return 0;
}

/**
 * Locate the optional field to set by JSON path in @a op->path.
 * If found or only a last path part is not found, initialize @a
 * field, including @a field->bar.parent.
 * @param op Update operation.
 * @param field Field to locate in.
 * @param[out] is_found Set if the field was found.
 * @param[out] mp_key When the target field is value in a map,
 *             here the pointer to MessagePack key is saved. Else
 *             NULL is saved.
 * @param[out] parent_type MessagePack type of the field parent.
 *
 * @retval 0 Success.
 * @retval -1 Not found non-last path part or invalid JSON.
 */
static inline int
update_bar_locate_opt(struct update_op *op, struct update_field *field,
		      bool *is_found, const char **mp_key,
		      enum mp_type *parent_type)
{
	int rc;
	const char *pos = field->data, *parent = NULL;
	struct json_lexer lexer;
	struct json_token token;
	json_lexer_create(&lexer, op->path + op->path_offset,
			  op->path_len - op->path_offset, TUPLE_INDEX_BASE);
	do {
		rc = json_lexer_next_token(&lexer, &token);
		if (rc != 0)
			return update_err_bad_json(op, rc);
		if (token.type == JSON_TOKEN_END) {
			assert(parent != NULL);
			*parent_type = mp_typeof(*parent);
			assert(*parent_type == MP_MAP ||
			       *parent_type == MP_ARRAY);
			*is_found = true;
			field->bar.point = pos;
			mp_next(&pos);
			field->bar.point_size = pos - field->bar.point;
			op->new_field_len = op->arg.set.length;
			goto success;
		}
		parent = pos;
		if (token.type == JSON_TOKEN_NUM) {
			rc = tuple_field_go_to_index(&pos, token.num);
		} else {
			rc = tuple_field_go_to_key(&pos, token.str, token.len);
			*mp_key = pos - mp_sizeof_str(token.len);
		}
	} while (rc == 0);
	assert(rc == -1);
	struct json_token tmp_token;
	rc = json_lexer_next_token(&lexer, &tmp_token);
	if (rc != 0)
		return update_err_bad_json(op, rc);
	if (tmp_token.type != JSON_TOKEN_END)
		return update_err_no_such_field(op);

	*is_found = false;
	if (token.type == JSON_TOKEN_NUM) {
		if (mp_typeof(*parent) != MP_ARRAY) {
			return update_err(op, "can not access by index a "\
					  "non-array field");
		}
		op->new_field_len = op->arg.set.length;
		const char *tmp = parent;
		uint32_t size = mp_decode_array(&tmp);
		if ((uint32_t) token.num > size)
			return update_err_no_such_field(op);
		if (parent == field->data) {
			field->bar.point = field->data + field->size;
		} else {
			field->bar.point = parent;
			mp_next(&field->bar.point);
		}
	} else {
		assert(token.type == JSON_TOKEN_STR);
		if (mp_typeof(*parent) != MP_MAP) {
			return update_err(op, "can not access by key a "\
					  "non-map field");
		}
		op->new_field_len = mp_sizeof_str(token.len) +
				    op->arg.set.length;
		field->bar.key = token.str;
		field->bar.key_len = token.len;
	}
success:
	field->type = UPDATE_BAR;
	field->bar.op = op;
	field->bar.parent = parent;
	return 0;
}

int
do_op_bar_insert(struct update_op *op, struct update_field *field)
{
	(void) op;
	(void) field;
	assert(op->opcode == '!');
	assert(field->type == UPDATE_BAR);
	diag_set(ClientError, ER_UNSUPPORTED, "update",
		 "intersected JSON paths");
	return -1;
}

int
do_op_nop_insert(struct update_op *op, struct update_field *field)
{
	assert(op->opcode == '!');
	assert(! update_op_is_term(op));
	assert(field->type == UPDATE_NOP);
	bool is_found;
	const char *unused_mp_key;
	enum mp_type parent_type;
	if (update_bar_locate_opt(op, field, &is_found, &unused_mp_key,
				  &parent_type) != 0)
		return -1;
	assert(field->bar.parent != NULL);
	if (is_found && parent_type == MP_MAP)
		return update_err_duplicate(op);
	return 0;
}

int
do_op_bar_set(struct update_op *op, struct update_field *field)
{
	(void) op;
	(void) field;
	assert(op->opcode == '=');
	assert(field->type == UPDATE_BAR);
	diag_set(ClientError, ER_UNSUPPORTED, "update",
		 "intersected JSON paths");
	return -1;
}

int
do_op_nop_set(struct update_op *op, struct update_field *field)
{
	assert(op->opcode == '=');
	assert(! update_op_is_term(op));
	assert(field->type == UPDATE_NOP);
	bool is_found;
	const char *unused_mp_key;
	enum mp_type parent_type;
	if (update_bar_locate_opt(op, field, &is_found, &unused_mp_key,
				  &parent_type) != 0)
		return -1;
	if (! is_found)
		op->opcode = '!';
	return 0;
}

int
do_op_bar_delete(struct update_op *op, struct update_field *field)
{
	(void) op;
	(void) field;
	assert(op->opcode == '#');
	assert(field->type == UPDATE_BAR);
	diag_set(ClientError, ER_UNSUPPORTED, "update",
		 "intersected JSON paths");
	return -1;
}

int
do_op_nop_delete(struct update_op *op, struct update_field *field)
{
	assert(op->opcode == '#');
	assert(! update_op_is_term(op));
	assert(field->type == UPDATE_NOP);
	bool is_found;
	const char *mp_key;
	enum mp_type parent_type;
	if (update_bar_locate_opt(op, field, &is_found, &mp_key,
				  &parent_type) != 0)
		return -1;
	if (! is_found)
		return update_err_no_such_field(op);
	assert(field->bar.parent != NULL);
	const char *value = field->bar.point;
	if (parent_type == MP_MAP) {
		if (op->arg.del.count != 1)
			return update_err_delete1(op);
		/* Delete the key together with value. */
		field->bar.point = mp_key;
		field->bar.point_size += value - mp_key;
	} else {
		assert(parent_type == MP_ARRAY);
		const char *tmp = field->bar.parent;
		uint32_t size = mp_decode_array(&tmp);
		if (size < op->arg.del.count)
			op->arg.del.count = size;
		value += field->bar.point_size;
		/* The first one is accounted in locate_opt(). */
		for (uint32_t i = 1; i < op->arg.del.count; ++i)
			mp_next(&value);
		field->bar.point_size = value - field->bar.point;
	}
	return 0;
}

#define DO_SCALAR_OP_GENERIC(op_type)						\
int										\
do_op_bar_##op_type(struct update_op *op, struct update_field *field)		\
{										\
	(void) op;								\
	(void) field;								\
	assert(field->type == UPDATE_BAR);					\
	diag_set(ClientError, ER_UNSUPPORTED, "update",				\
		 "intersected JSON paths");					\
	return -1;								\
}

DO_SCALAR_OP_GENERIC(arith)

DO_SCALAR_OP_GENERIC(bit)

DO_SCALAR_OP_GENERIC(splice)

#undef DO_SCALAR_OP_GENERIC

#define DO_SCALAR_OP_GENERIC(op_type)						\
int										\
do_op_nop_##op_type(struct update_op *op, struct update_field *field)		\
{										\
	assert(field->type == UPDATE_NOP);					\
	if (update_bar_locate(op, field) != 0)					\
		return -1;							\
	return update_op_do_##op_type(op, field->bar.point);			\
}

DO_SCALAR_OP_GENERIC(arith)

DO_SCALAR_OP_GENERIC(bit)

DO_SCALAR_OP_GENERIC(splice)

uint32_t
update_bar_sizeof(struct update_field *field)
{
	assert(field->type == UPDATE_BAR);
	switch(field->bar.op->opcode) {
	case '!': {
		assert(field->bar.parent != NULL);
		const char *tmp = field->bar.parent;
		enum mp_type type = mp_typeof(*tmp);
		uint32_t size;
		/* New header. */
		if (type == MP_MAP) {
			size = mp_decode_map(&tmp);
			size = mp_sizeof_map(size + 1);
		} else {
			assert(type == MP_ARRAY);
			size = mp_decode_array(&tmp);
			size = mp_sizeof_array(size + 1);
		}
		/* Before object. */
		size += field->bar.parent - field->data;
		/* Old objects and tail. */
		size += field->data + field->size - tmp;
		/* New [key] = value. */
		return size + field->bar.op->new_field_len;
	}
	case '#': {
		assert(field->bar.parent != NULL);
		const char *tmp = field->bar.parent;
		enum mp_type type = mp_typeof(*tmp);
		uint32_t delete_count = field->bar.op->arg.del.count;
		uint32_t size = field->size;
		/* New header. */
		if (type == MP_MAP) {
			assert(delete_count == 1);
			uint32_t old_count = mp_decode_map(&tmp);
			assert(old_count > 0);
			size = size - mp_sizeof_map(old_count) +
			       mp_sizeof_map(old_count - 1);
		} else {
			assert(type == MP_ARRAY);
			uint32_t old_count = mp_decode_array(&tmp);
			assert(old_count >= delete_count);
			size = size - mp_sizeof_array(old_count) +
			       mp_sizeof_array(old_count - delete_count);
		}
		return size - field->bar.point_size;
	}
	default: {
		uint32_t before_point = field->bar.point - field->data;
		const char *end = field->data + field->size;
		uint32_t after_point = end - (field->bar.point +
					      field->bar.point_size);
		return field->bar.op->new_field_len + before_point +
		       after_point;
	}
	}
}

uint32_t
update_bar_store(struct update_field *field, char *out, char *out_end)
{
	(void) out_end;
	struct update_op *op = field->bar.op;
	assert(field->type == UPDATE_BAR);
	switch(op->opcode) {
	case '!': {
		const char *pos = field->bar.parent;
		enum mp_type type = mp_typeof(*pos);
		uint32_t before_parent = pos - field->data;
		char *out_saved = out;
		/* Before parent. */
		memcpy(out, field->data, before_parent);
		out += before_parent;
		if (type == MP_MAP) {
			/* New map header. */
			uint32_t size = mp_decode_map(&pos);
			out = mp_encode_map(out, size + 1);
			/* New key. */
			out = mp_encode_str(out, field->bar.key,
					    field->bar.key_len);
		} else {
			assert(type == MP_ARRAY);
			/* New array header. */
			uint32_t size = mp_decode_array(&pos);
			out = mp_encode_array(out, size + 1);
			/* Before insertion point. */
			size = field->bar.point - pos;
			memcpy(out, pos, size);
			out += size;
			pos += size;
		}
		/* New value. */
		memcpy(out, op->arg.set.value, op->arg.set.length);
		out += op->arg.set.length;
		/* Old objects and tail. */
		uint32_t after_point = field->data + field->size - pos;
		memcpy(out, pos, after_point);
		out += after_point;
		return out - out_saved;
	}
	case '#': {
		const char *pos = field->bar.parent;
		enum mp_type type = mp_typeof(*pos);
		uint32_t size, before_parent = pos - field->data;
		char *out_saved = out;
		/* Before parent. */
		memcpy(out, field->data, before_parent);
		out += before_parent;
		if (type == MP_MAP) {
			/* New map header. */
			size = mp_decode_map(&pos);
			assert(size > 0);
			out = mp_encode_map(out, size - 1);
		} else {
			assert(type == MP_ARRAY);
			/* New array header. */
			size = mp_decode_array(&pos);
			assert(size >= op->arg.del.count);
			out = mp_encode_array(out, size - op->arg.del.count);
		}
		size = field->bar.point - pos;
		memcpy(out, pos, size);
		out += size;
		pos = field->bar.point + field->bar.point_size;

		size = field->data + field->size - pos;
		memcpy(out, pos, size);
		return out + size - out_saved;
	}
	default: {
		uint32_t before_point = field->bar.point - field->data;
		const char *end = field->data + field->size;
		uint32_t after_point = end - field->bar.point -
				       field->bar.point_size;

		memcpy(out, field->data, before_point);
		out += before_point;
		op->meta->store_f(op, field->bar.point, out);
		out += op->new_field_len;
		memcpy(out, field->bar.point + field->bar.point_size, after_point);
		return op->new_field_len + before_point + after_point;
	}
	}
}
