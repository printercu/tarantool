#ifndef TARANTOOL_BOX_TUPLE_UPDATE_FIELD_H
#define TARANTOOL_BOX_TUPLE_UPDATE_FIELD_H
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
#include <stdint.h>
#include "bit/int96.h"

struct rope;
struct update_field;
struct update_op;
struct tuple_dictionary;

/* {{{ update_op */

/** Argument of SET (and INSERT) operation. */
struct op_set_arg {
	uint32_t length;
	const char *value;
};

/** Argument of DELETE operation. */
struct op_del_arg {
	uint32_t count;
};

/**
 * MessagePack format code of an arithmetic argument or result.
 * MessagePack codes are not used to simplify type calculation.
 */
enum arith_type {
	AT_DOUBLE = 0, /* MP_DOUBLE */
	AT_FLOAT = 1,  /* MP_FLOAT */
	AT_INT = 2     /* MP_INT/MP_UINT */
};

/**
 * Argument (left and right) and result of ADD, SUBTRACT.
 *
 * To perform an arithmetic operation, update first loads left
 * and right arguments into corresponding value objects, then
 * performs arithmetics on types of arguments, thus calculating
 * the type of the result, and then performs the requested
 * operation according to the calculated type rules.
 *
 * The rules are as follows:
 * - when one of the argument types is double, the result is
 *   double;
 * - when one of the argument types is float, the result is float;
 * - for integer arguments, the result type code depends on the
 *   range in which falls the result of the operation. If the
 *   result is in negative range, it's MP_INT, otherwise it's
 *   MP_UINT. If the result is out of bounds of (-2^63, 2^64), an
 *   exception is raised for overflow.
 */
struct op_arith_arg {
	enum arith_type type;
	union {
		double dbl;
		float flt;
		struct int96_num int96;
	};
};

/** Argument of AND, XOR, OR operations. */
struct op_bit_arg {
	uint64_t val;
};

/** Argument of SPLICE. */
struct op_splice_arg {
	/** Splice position. */
	int32_t offset;
	/** Byte count to delete. */
	int32_t cut_length;
	/** New content. */
	const char *paste;
	/** New content length. */
	uint32_t paste_length;

	/** Offset of the tail in the old field. */
	int32_t tail_offset;
	/** Size of the tail. */
	int32_t tail_length;
};

/** Update operation argument. */
union update_op_arg {
	struct op_set_arg set;
	struct op_del_arg del;
	struct op_arith_arg arith;
	struct op_bit_arg bit;
	struct op_splice_arg splice;
};

/** Update procedure context. */
struct update_ctx {
	/**
	 * Index base for first level field numbers. Inside JSON
	 * path the base is always 1.
	 */
	int index_base;
	/** Allocator for update ops, fields, ropes etc. */
	struct region *region;
};

typedef int
(*update_op_do_f)(struct update_op *op, struct update_field *field,
		  struct update_ctx *ctx);

typedef int
(*update_op_read_arg_f)(struct update_op *op, const char **expr,
			int index_base);

typedef void
(*update_op_store_f)(struct update_op *op, const char *in, char *out);

/**
 * A set of functions and properties to initialize, do and store
 * an operation.
 */
struct update_op_meta {
	/**
	 * Virtual function to read the arguments of the
	 * operation.
	 */
	update_op_read_arg_f read_arg_f;
	/** Virtual function to execute the operation. */
	update_op_do_f do_f;
	/**
	 * Virtual function to store a result of the operation.
	 */
	update_op_store_f store_f;
	/** Argument count. */
	uint32_t arg_count;
};

/** A single UPDATE operation. */
struct update_op {
	/** Operation meta depending on the op type. */
	const struct update_op_meta *meta;
	/** Operation arguments. */
	union update_op_arg arg;
	/** First level subject field no. */
	int32_t field_no;
	/** Size of a new field after it is updated. */
	uint32_t new_field_len;
	/** Opcode symbol: = + - / ... */
	uint8_t opcode;
};

/**
 * Decode an update operation from MessagePack.
 * @param[out] op Update operation.
 * @param index_base Field numbers base: 0 or 1.
 * @param dict Dictionary to lookup field number by a name.
 * @param expr MessagePack.
 *
 * @retval 0 Success.
 * @retval -1 Client error.
 */
int
update_op_decode(struct update_op *op, int index_base,
		 struct tuple_dictionary *dict, const char **expr);

/* }}} update_op */

/* {{{ update_field */

/** Types of field update. */
enum update_type {
	/**
	 * Field is not updated. Just save it as is. It is used,
	 * for example, when a rope is split in two parts: not
	 * changed left range of fields, and a right range with
	 * its first field changed. In this case the left range is
	 * NOP.
	 */
	UPDATE_NOP,
	/**
	 * Field is a scalar value, updated via set, arith, bit,
	 * splice, or any other scalar-applicable operation.
	 */
	UPDATE_SCALAR,
	/**
	 * Field is an updated array. Check the rope for updates
	 * of individual fields.
	 */
	UPDATE_ARRAY,
};

/**
 * Generic structure describing update of a field. It can be
 * tuple field, field of a tuple field, or any another tuple
 * internal object: map, array, scalar, or unchanged field of any
 * type without op. This is a node of the whole update tree.
 *
 * If the field is array or map, it contains more children fields
 * inside corresponding sub-structures.
 */
struct update_field {
	/**
	 * Type of this field's update. Union below depends on it.
	 */
	enum update_type type;
	/** Field data to update. */
	const char *data;
	uint32_t size;
	union {
		/**
		 * This update is terminal. It does a scalar
		 * operation and has no children fields.
		 */
		struct {
			struct update_op *op;
		} scalar;
		/**
		 * This update changes an array. Children fields
		 * are stored in rope nodes.
		 */
		struct {
			struct rope *rope;
		} array;
	};
};

/**
 * Size of the updated field, including all children recursively.
 */
uint32_t
update_field_sizeof(struct update_field *field);

/** Save the updated field, including all children recursively. */
uint32_t
update_field_store(struct update_field *field, char *out, char *out_end);

/**
 * Generate declarations for a concrete field type: array, bar
 * etc. Each complex type has basic operations of the same
 * signature: insert, set, delete, arith, bit, splice.
 *
 * These operations can call each other going down a JSON path.
 */
#define OP_DECL_GENERIC(type)							\
int										\
do_op_##type##_insert(struct update_op *op, struct update_field *field,		\
		      struct update_ctx *ctx);					\
										\
int										\
do_op_##type##_set(struct update_op *op, struct update_field *field,		\
		   struct update_ctx *ctx);					\
										\
int										\
do_op_##type##_delete(struct update_op *op, struct update_field *field,		\
		      struct update_ctx *ctx);					\
										\
int										\
do_op_##type##_arith(struct update_op *op, struct update_field *field,		\
		     struct update_ctx *ctx);					\
										\
int										\
do_op_##type##_bit(struct update_op *op, struct update_field *field,		\
		   struct update_ctx *ctx);					\
										\
int										\
do_op_##type##_splice(struct update_op *op, struct update_field *field,		\
		      struct update_ctx *ctx);					\
										\
uint32_t									\
update_##type##_sizeof(struct update_field *field);				\
										\
uint32_t									\
update_##type##_store(struct update_field *field, char *out, char *out_end);

/* }}} update_field */

/* {{{ update_field.array */

/**
 * Initialize @a field as an array to update.
 * @param[out] field Field to initialize.
 * @param region Region to allocate rope, rope nodes, internal
 *        fields.
 * @param data MessagePack data of the array to update.
 * @param data_end End of @a data.
 * @param field_count Field count in @data.
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
int
update_array_create(struct update_field *field, struct region *region,
		    const char *data, const char *data_end,
		    uint32_t field_count);

OP_DECL_GENERIC(array)

/* }}} update_field.array */

/* {{{ scalar helpers */

/** Execute arith operation and store it in @a ret. */
int
make_arith_operation(struct op_arith_arg arg1, struct op_arith_arg arg2,
		     char opcode, uint32_t err_fieldno,
		     struct op_arith_arg *ret);

/** Store result of the arith operation. */
void
store_op_arith(struct update_op *op, const char *in, char *out);

/** Size of the arith updated field. */
uint32_t
update_arith_sizeof(struct op_arith_arg *arg);

/** Execute arith operation on the specified data. */
int
update_op_do_arith(struct update_op *op, const char *old, int index_base);

/** Execute bit operation on the specified data. */
int
update_op_do_bit(struct update_op *op, const char *old, int index_base);

/** Execute splice on the specified data. */
int
update_op_do_splice(struct update_op *op, const char *old, int index_base);

/* }}} scalar helpers */

#undef OP_DECL_GENERIC

#endif /* TARANTOOL_BOX_TUPLE_UPDATE_FIELD_H */
