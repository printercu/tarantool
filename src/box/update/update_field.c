#include "update_field.h"
#include "msgpuck.h"
#include "diag.h"
#include "box/error.h"
#include "box/tuple_format.h"
#include "json/json.h"

/* {{{ Error helpers. */

/**
 * Take a string identifier of a field being updated by @a op.
 * @param op Operation to convert field of.
 * @return String with field ID.
 */
static inline const char *
update_op_field_str(const struct update_op *op)
{
	if (op->path != NULL)
		return tt_sprintf("'%.*s'", op->path_len, op->path);
	else if (op->field_no >= 0)
		return tt_sprintf("%d", op->field_no + TUPLE_INDEX_BASE);
	else
		return tt_sprintf("%d", op->field_no);
}

static inline int
update_err_arg_type(const struct update_op *op, const char *needed_type)
{
	diag_set(ClientError, ER_UPDATE_ARG_TYPE, op->opcode,
		 update_op_field_str(op), needed_type);
	return -1;
}

static inline int
update_err_int_overflow(const struct update_op *op)
{
	diag_set(ClientError, ER_UPDATE_INTEGER_OVERFLOW, op->opcode,
		 update_op_field_str(op));
	return -1;
}

static inline int
update_err_splice_bound(const struct update_op *op)
{
	diag_set(ClientError, ER_UPDATE_SPLICE, update_op_field_str(op),
		 "offset is out of bound");
	return -1;
}

int
update_err_no_such_field(const struct update_op *op)
{
	if (op->path == NULL) {
		diag_set(ClientError, ER_NO_SUCH_FIELD_NO, op->field_no +
			 (op->field_no >= 0 ? TUPLE_INDEX_BASE : 0));
		return -1;
	}
	diag_set(ClientError, ER_NO_SUCH_FIELD_NAME, update_op_field_str(op));
	return -1;
}

int
update_err(const struct update_op *op, const char *reason)
{
	diag_set(ClientError, ER_UPDATE_FIELD, update_op_field_str(op),
		 reason);
	return -1;
}

/* }}} Error helpers. */

uint32_t
update_field_sizeof(struct update_field *field)
{
	switch (field->type) {
	case UPDATE_NOP:
		return field->size;
	case UPDATE_SCALAR:
		assert(field->scalar.op != NULL);
		return field->scalar.op->new_field_len;
	case UPDATE_ARRAY:
		return update_array_sizeof(field);
	case UPDATE_BAR:
		return update_bar_sizeof(field);
	default:
		unreachable();
	}
	return 0;
}

uint32_t
update_field_store(struct update_field *field, char *out, char *out_end)
{
	switch(field->type) {
	case UPDATE_NOP:
		assert(out_end - out >= field->size);
		memcpy(out, field->data, field->size);
		return field->size;
	case UPDATE_SCALAR:
		assert(field->scalar.op != NULL);
		assert(out_end - out >= field->scalar.op->new_field_len);
		field->scalar.op->meta->store_f(field->scalar.op, field->data,
						out);
		return field->scalar.op->new_field_len;
	case UPDATE_ARRAY:
		return update_array_store(field, out, out_end);
	case UPDATE_BAR:
		return update_bar_store(field, out, out_end);
	default:
		unreachable();
	}
	return 0;
}

/* {{{ read_arg helpers. */

/** Read a field index or any other integer field. */
static inline int
mp_read_i32(struct update_op *op, const char **expr, int32_t *ret)
{
	if (mp_read_int32(expr, ret) == 0)
		return 0;
	return update_err_arg_type(op, "an integer");
}

static inline int
mp_read_uint(struct update_op *op, const char **expr, uint64_t *ret)
{
	if (mp_typeof(**expr) == MP_UINT) {
		*ret = mp_decode_uint(expr);
		return 0;
	}
	return update_err_arg_type(op, "a positive integer");
}

static inline int
mp_read_arith_arg(struct update_op *op, const char **expr,
		  struct op_arith_arg *ret)
{
	switch(mp_typeof(**expr)) {
	case MP_UINT:
		ret->type = AT_INT;
		int96_set_unsigned(&ret->int96, mp_decode_uint(expr));
		return 0;
	case MP_INT:
		ret->type = AT_INT;
		int96_set_signed(&ret->int96, mp_decode_int(expr));
		return 0;
	case MP_DOUBLE:
		ret->type = AT_DOUBLE;
		ret->dbl = mp_decode_double(expr);
		return 0;
	case MP_FLOAT:
		ret->type = AT_FLOAT;
		ret->flt = mp_decode_float(expr);
		return 0;
	default:
		return update_err_arg_type(op, "a number");
	}
}

static inline int
mp_read_str(struct update_op *op, const char **expr, uint32_t *len,
	    const char **ret)
{
	if (mp_typeof(**expr) == MP_STR) {
		*ret = mp_decode_str(expr, len);
		return 0;
	}
	return update_err_arg_type(op, "a string");
}

/* }}} read_arg helpers. */

/* {{{ read_arg */

static int
read_arg_set(struct update_op *op, const char **expr, int index_base)
{
	(void) index_base;
	op->arg.set.value = *expr;
	mp_next(expr);
	op->arg.set.length = (uint32_t) (*expr - op->arg.set.value);
	return 0;
}

static int
read_arg_insert(struct update_op *op, const char **expr, int index_base)
{
	return read_arg_set(op, expr, index_base);
}

static int
read_arg_delete(struct update_op *op, const char **expr, int index_base)
{
	(void) index_base;
	if (mp_typeof(**expr) == MP_UINT) {
		op->arg.del.count = (uint32_t) mp_decode_uint(expr);
		return 0;
	}
	return update_err_arg_type(op, "a number of fields to delete");
}

static int
read_arg_arith(struct update_op *op, const char **expr, int index_base)
{
	(void) index_base;
	return mp_read_arith_arg(op, expr, &op->arg.arith);
}

static int
read_arg_bit(struct update_op *op, const char **expr, int index_base)
{
	(void) index_base;
	return mp_read_uint(op, expr, &op->arg.bit.val);
}

static int
read_arg_splice(struct update_op *op, const char **expr, int index_base)
{
	struct op_splice_arg *arg = &op->arg.splice;
	if (mp_read_i32(op, expr, &arg->offset))
		return -1;
	if (arg->offset >= 0) {
		if (arg->offset - index_base < 0)
			return update_err_splice_bound(op);
		arg->offset -= index_base;
	}
	if (mp_read_i32(op, expr, &arg->cut_length) == 0)
		return mp_read_str(op, expr, &arg->paste_length, &arg->paste);
	return -1;
}

/* }}} read_arg */

/* {{{ do_op helpers. */

static inline double
cast_arith_arg_to_double(struct op_arith_arg arg)
{
	if (arg.type == AT_DOUBLE) {
		return arg.dbl;
	} else if (arg.type == AT_FLOAT) {
		return arg.flt;
	} else {
		assert(arg.type == AT_INT);
		if (int96_is_uint64(&arg.int96)) {
			return int96_extract_uint64(&arg.int96);
		} else {
			assert(int96_is_neg_int64(&arg.int96));
			return int96_extract_neg_int64(&arg.int96);
		}
	}
}

/**
 * Return the MessagePack size of an arithmetic operation result.
 */
uint32_t
update_arith_sizeof(struct op_arith_arg *arg)
{
	if (arg->type == AT_INT) {
		if (int96_is_uint64(&arg->int96)) {
			uint64_t val = int96_extract_uint64(&arg->int96);
			return mp_sizeof_uint(val);
		} else {
			int64_t val = int96_extract_neg_int64(&arg->int96);
			return mp_sizeof_int(val);
		}
	} else if (arg->type == AT_DOUBLE) {
		return mp_sizeof_double(arg->dbl);
	} else {
		assert(arg->type == AT_FLOAT);
		return mp_sizeof_float(arg->flt);
	}
}

int
make_arith_operation(struct update_op *op, struct op_arith_arg arg,
		     struct op_arith_arg *ret)
{
	struct op_arith_arg value = op->arg.arith;
	enum arith_type lowest_type = arg.type;
	if (arg.type > value.type)
		lowest_type = value.type;

	if (lowest_type == AT_INT) {
		switch(op->opcode) {
		case '+':
			int96_add(&arg.int96, &value.int96);
			break;
		case '-':
			int96_invert(&value.int96);
			int96_add(&arg.int96, &value.int96);
			break;
		default:
			unreachable();
			break;
		}
		if (!int96_is_uint64(&arg.int96) &&
		    !int96_is_neg_int64(&arg.int96))
			return update_err_int_overflow(op);
		*ret = arg;
	} else {
		/* At least one of operands is double or float. */
		double a = cast_arith_arg_to_double(arg);
		double b = cast_arith_arg_to_double(value);
		double c;
		switch(op->opcode) {
		case '+':
			c = a + b;
			break;
		case '-':
			c = a - b;
			break;
		default:
			return update_err_arg_type(op, "a positive integer");
		}
		if (lowest_type == AT_DOUBLE) {
			ret->type = AT_DOUBLE;
			ret->dbl = c;
		} else {
			assert(lowest_type == AT_FLOAT);
			ret->type = AT_FLOAT;
			ret->flt = (float)c;
		}
	}
	return 0;
}

int
update_op_do_arith(struct update_op *op, const char *old)
{
	struct op_arith_arg left_arg;
	if (mp_read_arith_arg(op, &old, &left_arg) != 0)
		return -1;

	if (make_arith_operation(op, left_arg, &op->arg.arith) != 0)
		return -1;
	op->new_field_len = update_arith_sizeof(&op->arg.arith);
	return 0;
}

int
update_op_do_bit(struct update_op *op, const char *old)
{
	uint64_t val;
	if (mp_read_uint(op, &old, &val) != 0)
		return -1;
	struct op_bit_arg *arg = &op->arg.bit;
	switch (op->opcode) {
	case '&':
		arg->val &= val;
		break;
	case '^':
		arg->val ^= val;
		break;
	case '|':
		arg->val |= val;
		break;
	default:
		unreachable(); /* checked by update_read_ops */
	}
	op->new_field_len = mp_sizeof_uint(arg->val);
	return 0;
}

int
update_op_do_splice(struct update_op *op, const char *old)
{
	struct op_splice_arg *arg = &op->arg.splice;
	int32_t str_len;
	if (mp_read_str(op, &old, (uint32_t *) &str_len, &old) != 0)
		return -1;

	if (arg->offset < 0) {
		if (-arg->offset > str_len + 1)
			return update_err_splice_bound(op);
		arg->offset = arg->offset + str_len + 1;
	} else if (arg->offset > str_len) {
		arg->offset = str_len;
	}
	assert(arg->offset >= 0 && arg->offset <= str_len);
	if (arg->cut_length < 0) {
		if (-arg->cut_length > (str_len - arg->offset))
			arg->cut_length = 0;
		else
			arg->cut_length += str_len - arg->offset;
	} else if (arg->cut_length > str_len - arg->offset) {
		arg->cut_length = str_len - arg->offset;
	}
	assert(arg->offset <= str_len);

	arg->tail_offset = arg->offset + arg->cut_length;
	arg->tail_length = str_len - arg->tail_offset;
	op->new_field_len = mp_sizeof_str(arg->offset + arg->paste_length +
					  arg->tail_length);
	return 0;
}

/* }}} do_op helpers. */

/* {{{ store_op */

static void
store_op_set(struct update_op *op, const char *in, char *out)
{
	(void) in;
	memcpy(out, op->arg.set.value, op->arg.set.length);
}

void
store_op_arith(struct update_op *op, const char *in, char *out)
{
	(void) in;
	struct op_arith_arg *arg = &op->arg.arith;
	if (arg->type == AT_INT) {
		if (int96_is_uint64(&arg->int96)) {
			mp_encode_uint(out, int96_extract_uint64(&arg->int96));
		} else {
			assert(int96_is_neg_int64(&arg->int96));
			mp_encode_int(out, int96_extract_neg_int64(&arg->int96));
		}
	} else if (arg->type == AT_DOUBLE) {
		mp_encode_double(out, arg->dbl);
	} else {
		assert(arg->type == AT_FLOAT);
		mp_encode_float(out, arg->flt);
	}
}

static void
store_op_bit(struct update_op *op, const char *in, char *out)
{
	(void) in;
	mp_encode_uint(out, op->arg.bit.val);
}

static void
store_op_splice(struct update_op *op, const char *in, char *out)
{
	struct op_splice_arg *arg = &op->arg.splice;
	uint32_t new_str_len = arg->offset + arg->paste_length +
			       arg->tail_length;
	(void) mp_decode_strl(&in);
	out = mp_encode_strl(out, new_str_len);
	/* Copy field head. */
	memcpy(out, in, arg->offset);
	out = out + arg->offset;
	/* Copy the paste. */
	memcpy(out, arg->paste, arg->paste_length);
	out = out + arg->paste_length;
	/* Copy tail. */
	memcpy(out, in + arg->tail_offset, arg->tail_length);
}

/* }}} store_op */

static const struct update_op_meta op_set =
	{ read_arg_set, do_op_set, store_op_set, 3 };
static const struct update_op_meta op_insert =
	{ read_arg_insert, do_op_insert, store_op_set, 3 };
static const struct update_op_meta op_arith =
	{ read_arg_arith, do_op_arith, store_op_arith, 3 };
static const struct update_op_meta op_bit =
	{ read_arg_bit, do_op_bit, store_op_bit, 3 };
static const struct update_op_meta op_splice =
	{ read_arg_splice, do_op_splice, store_op_splice, 5 };
static const struct update_op_meta op_delete =
	{ read_arg_delete, do_op_delete, NULL, 3 };

static const struct update_op_meta *
update_op_by(char opcode)
{
	switch (opcode) {
	case '=':
		return &op_set;
	case '+':
	case '-':
		return &op_arith;
	case '&':
	case '|':
	case '^':
		return &op_bit;
	case ':':
		return &op_splice;
	case '#':
		return &op_delete;
	case '!':
		return &op_insert;
	default:
		diag_set(ClientError, ER_UNKNOWN_UPDATE_OP);
		return NULL;
	}
}

int
update_op_decode(struct update_op *op, int index_base,
		 struct tuple_dictionary *dict, const char **expr)
{
	if (mp_typeof(**expr) != MP_ARRAY) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS, "update operation "
			 "must be an array {op,..}");
		return -1;
	}
	/* Read operation */
	uint32_t len, arg_count = mp_decode_array(expr);
	if (arg_count < 1) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS, "update operation "\
			 "must be an array {op,..}, got empty array");
		return -1;
	}
	if (mp_typeof(**expr) != MP_STR) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS,
			 "update operation name must be a string");
		return -1;
	}
	op->opcode = *mp_decode_str(expr, &len);
	op->meta = update_op_by(op->opcode);
	if (op->meta == NULL)
		return -1;
	if (arg_count != op->meta->arg_count) {
		diag_set(ClientError, ER_UNKNOWN_UPDATE_OP);
		return -1;
	}
	int32_t field_no;
	switch(mp_typeof(**expr)) {
	case MP_INT:
	case MP_UINT: {
		op->path = NULL;
		op->path_offset = 0;
		op->path_len = 0;
		if (mp_read_i32(op, expr, &field_no) != 0)
			return -1;
		if (field_no - index_base >= 0) {
			op->field_no = field_no - index_base;
		} else if (field_no < 0) {
			op->field_no = field_no;
		} else {
			diag_set(ClientError, ER_NO_SUCH_FIELD_NO, field_no);
			return -1;
		}
		break;
	}
	case MP_STR: {
		const char *path = mp_decode_str(expr, &len);
		uint32_t field_no, hash = field_name_hash(path, len);
		op->path = path;
		op->path_len = len;
		op->path_offset = 0;
		if (tuple_fieldno_by_name(dict, path, len, hash,
					  &field_no) == 0) {
			op->field_no = (int32_t) field_no;
			op->path_offset = len;
			break;
		}
		struct json_lexer lexer;
		json_lexer_create(&lexer, path, len, 1);
		struct json_token token;
		int rc = json_lexer_next_token(&lexer, &token);
		if (rc != 0)
			return update_err_bad_json(op, rc);
		switch (token.type) {
		case JSON_TOKEN_NUM:
			op->field_no = token.num;
			break;
		case JSON_TOKEN_STR:
			hash = field_name_hash(token.str, token.len);
			if (tuple_fieldno_by_name(dict, token.str, token.len,
						  hash, &field_no) == 0) {
				op->field_no = (int32_t) field_no;
				break;
			}
			FALLTHROUGH;
		default:
			diag_set(ClientError, ER_NO_SUCH_FIELD_NAME,
				 tt_cstr(path, len));
			return -1;
		}
		op->path_offset = lexer.offset;
		break;
	}
	default:
		diag_set(ClientError, ER_ILLEGAL_PARAMS,
			 "field id must be a number or a string");
		return -1;
	}
	return op->meta->read_arg_f(op, expr, index_base);
}
