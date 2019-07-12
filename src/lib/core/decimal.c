/*
 * Copyright 2019, Tarantool AUTHORS, please see AUTHORS file.
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

#include "decimal.h"
#include "third_party/decNumber/decContext.h"
#include "third_party/decNumber/decPacked.h"
#include "lib/core/tt_static.h"
#include "lib/msgpuck/msgpuck.h"
#include <stddef.h>
#include <stdlib.h>
#include <float.h> /* DBL_DIG */
#include <math.h> /* isnan(), isinf(). */
#include "trivia/util.h"

#define DECIMAL_ROUNDING DEC_ROUND_HALF_UP

/** A single context for all the arithmetic operations. */
static __thread decContext decimal_context = {
	/* Maximum precision during operations. */
	DECIMAL_MAX_DIGITS,
	/*
	 * Maximum decimal lagarithm of the number.
	 * Allows for precision = DECIMAL_MAX_DIGITS
	 */
	DECIMAL_MAX_DIGITS - 1,
	/*
	 * Minimal adjusted exponent. The smallest absolute value will be
	 * exp((1 - DECIMAL_MAX_DIGITS) - 1) =
	 * exp(-DECIMAL_MAX_DIGITS) allowing for scale =
	 * DECIMAL_MAX_DIGITS
	 */
	-1,
	/* Rounding mode: .5 rounds away from 0. */
	DECIMAL_ROUNDING,
	/* Turn off signalling for failed operations. */
	0,
	/* Status holding occured events. Initially empty. */
	0,
	/* Turn off exponent clamping. */
	0
};

/**
 * A finalizer for all the operations.
 * Check the operation context status and empty it,
 * check that result isn't a NaN or infinity.
 *
 * @return NULL if finalization failed.
 *         result pointer otherwise.
 */
static decimal_t *
decimal_check_status(decimal_t *dec, decContext *context)
{
	uint32_t status = decContextGetStatus(context);
	decContextZeroStatus(context);
	/*
	 * Clear warnings. Every value less than 0.1 is
	 * subnormal, DEC_Inexact and DEC_Rounded result
	 * from rounding. DEC_Inexact with DEC_Subnormal
	 * together result in DEC_Underflow. DEC_Clamped
	 * happens after underflow if rounding to zero.
	 */
	status &= ~(uint32_t)(DEC_Inexact | DEC_Rounded | DEC_Underflow |
			      DEC_Subnormal | DEC_Clamped);
	return status || !decNumberIsFinite(dec) ? NULL : dec;
}

int decimal_precision(const decimal_t *dec) {
	return dec->exponent <= 0 ? MAX(dec->digits, -dec->exponent) :
				    dec->digits + dec->exponent;
}

int  decimal_scale(const decimal_t *dec) {
	return dec->exponent < 0 ? -dec->exponent : 0;
}

decimal_t *
decimal_zero(decimal_t *dec)
{
	decNumberZero(dec);
	return dec;
}

decimal_t *
decimal_from_string(decimal_t *dec, const char *str)
{
	decNumberFromString(dec, str, &decimal_context);
	return decimal_check_status(dec, &decimal_context);
}

decimal_t *
decimal_from_double(decimal_t *dec, double d)
{
	char buf[DECIMAL_MAX_DIGITS + 3];
	if (isinf(d) || isnan(d))
		return NULL;
	/*
	 * DBL_DIG is 15, it is the guaranteed amount of
	 * correct significant decimal digits in a double
	 * value.  There is no point in using higher precision,
	 * since every non-representable number has a long
	 * tail of erroneous digits:
	 * `23.42` -> `23.420000000000001705302565824240446091`
	 */
	snprintf(buf, sizeof(buf), "%.*g", DBL_DIG, d);
	return decimal_from_string(dec, buf);
}

decimal_t *
decimal_from_int64(decimal_t *dec, int64_t num)
{
	return decNumberFromInt64(dec, num);
}

decimal_t *
decimal_from_uint64(decimal_t *dec, uint64_t num)
{
	return decNumberFromUInt64(dec, num);
}

const char *
decimal_to_string(const decimal_t *dec)
{
	char *buf = tt_static_buf();
	/* No errors are possible. */
	char *tmp = decNumberToString(dec, buf);
	assert(buf == tmp);
	(void)tmp;
	return buf;
}

static decimal_t *
decimal_round_with_mode(decimal_t *dec, int scale, enum rounding mode);

decimal_t *
decimal_to_int64(decimal_t *dec, int64_t *num)
{
	decimal_t d, z;
	decNumberZero(&z);
	d = *dec;
	dec = &d;

	if (decimal_scale(dec) != 0) {
		/*
		 * Rounding mode is important here.
		 * We want to be consistent with double
		 * to int conversion so that comparison
		 * hints work correctly.
		 */
		dec = decimal_round_with_mode(dec, 0, DEC_ROUND_DOWN);
	}
	/*
	 * decNumberToInt64 works only with numbers with
	 * zero exponent.
	 */
	decNumberRescale(dec, dec, &z, &decimal_context);
	if (decimal_check_status(dec, &decimal_context) == NULL) {
		return NULL;
	}
	*num = decNumberToInt64(dec, &decimal_context);
	return decimal_check_status(dec, &decimal_context);
}

decimal_t *
decimal_to_uint64(decimal_t *dec, uint64_t *num)
{
	decimal_t d, z;
	decNumberZero(&z);
	d = *dec;
	dec = &d;

	if (decimal_scale(dec) != 0) {
		dec = decimal_round_with_mode(dec, 0, DEC_ROUND_DOWN);
	}
	decNumberRescale(dec, dec, &z, &decimal_context);
	if (decimal_check_status(dec, &decimal_context) == NULL) {
		return NULL;
	}
	*num = decNumberToUInt64(dec, &decimal_context);
	return decimal_check_status(dec, &decimal_context);
}

int
decimal_compare(const decimal_t *lhs, const decimal_t *rhs)
{
	decNumber res;
	decNumberCompare(&res, lhs, rhs, &decimal_context);
	int r = decNumberToInt32(&res, &decimal_context);
	assert(decimal_check_status(&res, &decimal_context) != NULL);
	return r;
}

static decimal_t *
decimal_round_with_mode(decimal_t *dec, int scale, enum rounding mode)
{
	if (scale < 0 || scale > DECIMAL_MAX_DIGITS)
		return NULL;

	if (scale >= decimal_scale(dec))
		return dec;

	int ndig = MAX(decimal_precision(dec) - decimal_scale(dec) + scale, 1);
	decContext context = {
		ndig, /* Precision */
		ndig, /* emax */
		scale != 0 ? -1 : 0, /* emin */
		mode, /* rounding */
		0, /* no traps */
		0, /* zero status */
		0 /* no clamping */
	};

	decNumberPlus(dec, dec, &context);
	assert(decimal_check_status(dec, &context) != NULL);
	return dec;
}

inline decimal_t *
decimal_round(decimal_t *dec, int scale)
{
	return decimal_round_with_mode(dec, scale, DECIMAL_ROUNDING);
}

decimal_t *
decimal_abs(decimal_t *res, const decimal_t *dec)
{
	decNumberAbs(res, dec, &decimal_context);
	assert(decimal_check_status(res, &decimal_context) != NULL);
	return res;
}

decimal_t *
decimal_minus(decimal_t *res, const decimal_t *dec)
{
	decNumberMinus(res, dec, &decimal_context);
	assert(decimal_check_status(res, &decimal_context) != NULL);
	return res;
}

decimal_t *
decimal_add(decimal_t *res, const decimal_t *lhs, const decimal_t *rhs)
{
	decNumberAdd(res, lhs, rhs, &decimal_context);
	return decimal_check_status(res, &decimal_context);
}

decimal_t *
decimal_sub(decimal_t *res, const decimal_t *lhs, const decimal_t *rhs)
{
	decNumberSubtract(res, lhs, rhs, &decimal_context);

	return decimal_check_status(res, &decimal_context);
}

decimal_t *
decimal_mul(decimal_t *res, const decimal_t *lhs, const decimal_t *rhs)
{
	decNumberMultiply(res, lhs, rhs, &decimal_context);

	return decimal_check_status(res, &decimal_context);
}

decimal_t *
decimal_div(decimal_t *res, const decimal_t *lhs, const decimal_t *rhs)
{
	decNumberDivide(res, lhs, rhs, &decimal_context);

	return decimal_check_status(res, &decimal_context);
}

decimal_t *
decimal_log10(decimal_t *res, const decimal_t *lhs)
{
	decNumberLog10(res, lhs, &decimal_context);

	return decimal_check_status(res, &decimal_context);
}

decimal_t *
decimal_ln(decimal_t *res, const decimal_t *lhs)
{
	/*
	 * ln hangs in an infinite loop when result is
	 * between -10 ^ emin and 10 ^ emin.
	 * For small x, ln(1 + x) = x. Say, we take the
	 * smallest allowed value for
	 * (1 + x) = 1 + 10 ^ -(DECIMAL_MAX_DIGITS - 1).
	 * For ln to work for this value we need to set emin to
	 * -DECIMAL_MAX_DIGITS.
	 */
	int32_t emin = decimal_context.emin;
	decimal_context.emin = -DECIMAL_MAX_DIGITS;

	decNumberLn(res, lhs, &decimal_context);

	decimal_context.emin = emin;
	if (decimal_check_status(res, &decimal_context) == NULL) {
		return NULL;
	} else {
		/*
		 * The increased EMIN allows for scale up to
		 * 2 * (DECIMAL_MAX_DIGITS - 1).
		 * Round back to DECIMAL_MAX_DIGITS - 1.
		 */
		decimal_round(res, DECIMAL_MAX_DIGITS - 1);
		return res;
	}
}

decimal_t *
decimal_pow(decimal_t *res, const decimal_t *lhs, const decimal_t *rhs)
{
	decNumberPower(res, lhs, rhs, &decimal_context);

	return decimal_check_status(res, &decimal_context);
}

decimal_t *
decimal_exp(decimal_t *res, const decimal_t *lhs)
{
	decNumberExp(res, lhs, &decimal_context);

	return decimal_check_status(res, &decimal_context);
}

decimal_t *
decimal_sqrt(decimal_t *res, const decimal_t *lhs)
{
	decNumberSquareRoot(res, lhs, &decimal_context);

	return decimal_check_status(res, &decimal_context);
}

uint32_t
decimal_len(const decimal_t *dec)
{
	/* 1  + ceil((digits + 1) / 2) */
	return 2 + dec->digits / 2;
}

char *
decimal_pack(char *data, const decimal_t *dec)
{
	uint32_t len = decimal_len(dec);
	/* reserve space for resulting scale */
	char *svp = data++;
	len--;
	int32_t scale;
	char *tmp = (char *)decPackedFromNumber((uint8_t *)data, len, &scale, dec);
	assert(tmp == data);
	/* scale may be negative, when exponent is > 0 */
	assert(scale == (int32_t)decimal_scale(dec) || scale < 0);
	mp_store_u8(svp, (int8_t)scale);
	(void)tmp;
	data += len;
	return data;
}

decimal_t *
decimal_unpack(const char **data, uint32_t len, decimal_t *dec)
{
	int32_t scale = (int8_t)mp_load_u8(data);
	len--;
	decimal_t *res = decPackedToNumber((uint8_t *)*data, len, &scale, dec);
	if (res)
		*data += len;
	else
		(*data)--;
	return res;
}
