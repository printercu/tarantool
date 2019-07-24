/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
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

/*
 *
 * This file contains code use to implement APIs that are part of the
 * VDBE.
 */
#include "sqlInt.h"
#include "vdbeInt.h"

/*
 * Invoke the profile callback.  This routine is only called if we already
 * know that the profile callback is defined and needs to be invoked.
 */
static SQL_NOINLINE void
invokeProfileCallback(sql * db, Vdbe * p)
{
	sql_int64 iNow;
	sql_int64 iElapse;
	assert(p->startTime > 0);
	assert(db->xProfile != 0 || (db->mTrace & SQL_TRACE_PROFILE) != 0);
	assert(db->init.busy == 0);
	assert(p->zSql != 0);
	sqlOsCurrentTimeInt64(db->pVfs, &iNow);
	iElapse = (iNow - p->startTime) * 1000000;
	if (db->xProfile) {
		db->xProfile(db->pProfileArg, p->zSql, iElapse);
	}
	if (db->mTrace & SQL_TRACE_PROFILE) {
		db->xTrace(SQL_TRACE_PROFILE, db->pTraceArg, p,
			   (void *)&iElapse);
	}
	p->startTime = 0;
}

/*
 * The checkProfileCallback(DB,P) macro checks to see if a profile callback
 * is needed, and it invokes the callback if it is needed.
 */
#define checkProfileCallback(DB,P) \
   if( ((P)->startTime)>0 ){ invokeProfileCallback(DB,P); }

/*
 * The following routine destroys a virtual machine that is created by
 * the sql_compile() routine. The integer returned is an SQL_
 * success/failure code that describes the result of executing the virtual
 * machine.
 */
int
sql_finalize(sql_stmt * pStmt)
{
	if (pStmt == NULL)
		return 0;
	Vdbe *v = (Vdbe *) pStmt;
	sql *db = v->db;
	assert(db != NULL);
	checkProfileCallback(db, v);
	return sqlVdbeFinalize(v);
}

int
sql_reset(sql_stmt * pStmt)
{
	assert(pStmt != NULL);
	struct Vdbe *v = (Vdbe *) pStmt;
	struct sql *db = v->db;
	checkProfileCallback(db, v);
	int rc = sqlVdbeReset(v);
	sqlVdbeRewind(v);
	return rc;
}

/*
 * Set all the parameters in the compiled SQL statement to NULL.
 */
int
sql_clear_bindings(sql_stmt * pStmt)
{
	int i;
	int rc = 0;
	Vdbe *p = (Vdbe *) pStmt;
	for (i = 0; i < p->nVar; i++) {
		sqlVdbeMemRelease(&p->aVar[i]);
		p->aVar[i].flags = MEM_Null;
	}
	if (p->isPrepareV2 && p->expmask) {
		p->expired = 1;
	}
	return rc;
}

/**************************** sql_value_  ******************************
 * The following routines extract information from a Mem or sql_value
 * structure.
 */
const void *
sql_value_blob(sql_value * pVal)
{
	Mem *p = (Mem *) pVal;
	if (p->flags & (MEM_Blob | MEM_Str)) {
		if (ExpandBlob(p) != 0) {
			assert(p->flags == MEM_Null && p->z == 0);
			return 0;
		}
		p->flags |= MEM_Blob;
		return p->n ? p->z : 0;
	} else {
		return sql_value_text(pVal);
	}
}

int
sql_value_bytes(sql_value * pVal)
{
	return sqlValueBytes(pVal);
}

double
sql_value_double(sql_value * pVal)
{
	double v = 0.0;
	sqlVdbeRealValue((Mem *) pVal, &v);
	return v;
}

bool
sql_value_boolean(sql_value *val)
{
	bool b = false;
	int rc = mem_value_bool((struct Mem *) val, &b);
	assert(rc == 0);
	(void) rc;
	return b;
}

int
sql_value_int(sql_value * pVal)
{
	int64_t i = 0;
	sqlVdbeIntValue((Mem *) pVal, &i);
	return (int)i;
}

sql_int64
sql_value_int64(sql_value * pVal)
{
	int64_t i = 0;
	sqlVdbeIntValue((Mem *) pVal, &i);
	return i;
}

enum sql_subtype
sql_value_subtype(sql_value * pVal)
{
	return (pVal->flags & MEM_Subtype) != 0 ? pVal->subtype : SQL_SUBTYPE_NO;
}

const unsigned char *
sql_value_text(sql_value * pVal)
{
	return (const unsigned char *)sqlValueText(pVal);
}

/* EVIDENCE-OF: R-12793-43283 Every value in sql has one of five
 * fundamental datatypes: 64-bit signed integer 64-bit IEEE floating
 * point number string BLOB NULL
 */
enum mp_type
sql_value_type(sql_value *pVal)
{
	switch (pVal->flags & MEM_PURE_TYPE_MASK) {
	case MEM_Int: return MP_INT;
	case MEM_Real: return MP_DOUBLE;
	case MEM_Str: return MP_STR;
	case MEM_Blob: return MP_BIN;
	case MEM_Bool: return MP_BOOL;
	case MEM_Null: return MP_NIL;
	default: unreachable();
	}
}

/* Make a copy of an sql_value object
 */
sql_value *
sql_value_dup(const sql_value * pOrig)
{
	sql_value *pNew;
	if (pOrig == 0)
		return 0;
	pNew = sql_malloc(sizeof(*pNew));
	if (pNew == 0)
		return 0;
	memset(pNew, 0, sizeof(*pNew));
	memcpy(pNew, pOrig, MEMCELLSIZE);
	pNew->flags &= ~MEM_Dyn;
	pNew->db = 0;
	if (pNew->flags & (MEM_Str | MEM_Blob)) {
		pNew->flags &= ~(MEM_Static | MEM_Dyn);
		pNew->flags |= MEM_Ephem;
		if (sqlVdbeMemMakeWriteable(pNew) != 0) {
			sqlValueFree(pNew);
			pNew = 0;
		}
	}
	return pNew;
}

/* Destroy an sql_value object previously obtained from
 * sql_value_dup().
 */
void
sql_value_free(sql_value * pOld)
{
	sqlValueFree(pOld);
}

/**************************** sql_result_  ******************************
 * The following routines are used by user-defined functions to specify
 * the function result.
 *
 * The setStrOrError() function calls sqlVdbeMemSetStr() to store the
 * result as a string or blob but if the string or blob is too large, it
 * then sets the error code.
 *
 * The invokeValueDestructor(P,X) routine invokes destructor function X()
 * on value P is not going to be used and need to be destroyed.
 */
static void
setResultStrOrError(sql_context * pCtx,	/* Function context */
		    const char *z,	/* String pointer */
		    int n,	/* Bytes in string, or negative */
		    void (*xDel) (void *)	/* Destructor function */
    )
{
	if (sqlVdbeMemSetStr(pCtx->pOut, z, n, 1, xDel) != 0)
		pCtx->is_aborted = true;
}

static int
invokeValueDestructor(const void *p,	/* Value to destroy */
		      void (*xDel) (void *),	/* The destructor */
		      sql_context *pCtx	/* Set an error if no NULL */
    )
{
	assert(xDel != SQL_DYNAMIC);
	if (xDel == 0) {
		/* noop */
	} else if (xDel == SQL_TRANSIENT) {
		/* noop */
	} else {
		xDel((void *)p);
	}
	if (pCtx) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob is too "\
			 "big");
		pCtx->is_aborted = true;
	}
	return -1;
}

void
sql_result_blob(sql_context * pCtx,
		    const void *z, int n, void (*xDel) (void *)
    )
{
	assert(n >= 0);
	if (sqlVdbeMemSetStr(pCtx->pOut, z, n, 0, xDel) != 0)
		pCtx->is_aborted = true;
}

void
sql_result_blob64(sql_context * pCtx,
		      const void *z, sql_uint64 n, void (*xDel) (void *)
    )
{
	assert(xDel != SQL_DYNAMIC);
	if (n > 0x7fffffff) {
		(void)invokeValueDestructor(z, xDel, pCtx);
	} else {
		setResultStrOrError(pCtx, z, (int)n, xDel);
	}
}

void
sql_result_double(sql_context * pCtx, double rVal)
{
	sqlVdbeMemSetDouble(pCtx->pOut, rVal);
}

void
sql_result_int(sql_context * pCtx, int iVal)
{
	sqlVdbeMemSetInt64(pCtx->pOut, (i64) iVal);
}

void
sql_result_bool(struct sql_context *ctx, bool value)
{
	mem_set_bool(ctx->pOut, value);
}

void
sql_result_int64(sql_context * pCtx, i64 iVal)
{
	sqlVdbeMemSetInt64(pCtx->pOut, iVal);
}

void
sql_result_null(sql_context * pCtx)
{
	sqlVdbeMemSetNull(pCtx->pOut);
}

void
sql_result_text(sql_context * pCtx,
		    const char *z, int n, void (*xDel) (void *)
    )
{
	setResultStrOrError(pCtx, z, n, xDel);
}

void
sql_result_text64(sql_context * pCtx,
		      const char *z,
		      sql_uint64 n,
		      void (*xDel) (void *))
{
	assert(xDel != SQL_DYNAMIC);
	if (n > 0x7fffffff) {
		(void)invokeValueDestructor(z, xDel, pCtx);
	} else {
		setResultStrOrError(pCtx, z, (int)n, xDel);
	}
}

void
sql_result_value(sql_context * pCtx, sql_value * pValue)
{
	sqlVdbeMemCopy(pCtx->pOut, pValue);
}

void
sql_result_zeroblob(sql_context * pCtx, int n)
{
	sqlVdbeMemSetZeroBlob(pCtx->pOut, n);
}

int
sql_result_zeroblob64(sql_context * pCtx, u64 n)
{
	Mem *pOut = pCtx->pOut;
	if (n > (u64) pOut->db->aLimit[SQL_LIMIT_LENGTH]) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob is too "\
			 "big");
		return -1;
	}
	sqlVdbeMemSetZeroBlob(pCtx->pOut, (int)n);
	return 0;
}

/*
 * Execute the statement pStmt, either until a row of data is ready, the
 * statement is completely executed or an error occurs.
 *
 * This routine implements the bulk of the logic behind the sql_step()
 * API.  The only thing omitted is the automatic recompile if a
 * schema change has occurred.  That detail is handled by the
 * outer sql_step() wrapper procedure.
 */
static int
sqlStep(Vdbe * p)
{
	sql *db;
	int rc;

	assert(p);
	if (p->magic != VDBE_MAGIC_RUN)
		sql_reset((sql_stmt *) p);

	/* Check that malloc() has not failed. If it has, return early. */
	db = p->db;
	if (db->mallocFailed) {
		p->is_aborted = true;
		return -1;
	}

	if (p->pc <= 0 && p->expired) {
		p->is_aborted = true;
		return -1;
	}
	if (p->pc < 0) {

		if ((db->xProfile || (db->mTrace & SQL_TRACE_PROFILE) != 0)
		    && !db->init.busy && p->zSql) {
			sqlOsCurrentTimeInt64(db->pVfs, &p->startTime);
		} else {
			assert(p->startTime == 0);
		}

		db->nVdbeActive++;
		p->pc = 0;
	}
	if (p->explain) {
		rc = sqlVdbeList(p);
	} else {
		db->nVdbeExec++;
		rc = sqlVdbeExec(p);
		db->nVdbeExec--;
	}

	/* If the statement completed successfully, invoke the profile callback */
	if (rc != SQL_ROW)
		checkProfileCallback(db, p);

	if (p->isPrepareV2 && rc != SQL_ROW && rc != SQL_DONE) {
		/* If this statement was prepared using sql_prepare_v2(), and an
		 * error has occurred, then return an error.
		 */
		if (p->is_aborted)
			rc = -1;
	}
	return rc;
}

/*
 * This is the top-level implementation of sql_step().  Call
 * sqlStep() to do most of the work.  If a schema error occurs,
 * call sqlReprepare() and try again.
 */
int
sql_step(sql_stmt * pStmt)
{
	Vdbe *v = (Vdbe *) pStmt;	/* the prepared statement */
	assert(v != NULL);
	return sqlStep(v);
}

/*
 * Extract the user data from a sql_context structure and return a
 * pointer to it.
 */
void *
sql_user_data(sql_context * p)
{
	assert(p && p->pFunc);
	return p->pFunc->pUserData;
}

/*
 * Extract the user data from a sql_context structure and return a
 * pointer to it.
 *
 * IMPLEMENTATION-OF: R-46798-50301 The sql_context_db_handle() interface
 * returns a copy of the pointer to the database connection (the 1st
 * parameter) of the sql_create_function() and
 * sql_create_function16() routines that originally registered the
 * application defined function.
 */
sql *
sql_context_db_handle(sql_context * p)
{
	assert(p && p->pOut);
	return p->pOut->db;
}

/*
 * Return the current time for a statement.  If the current time
 * is requested more than once within the same run of a single prepared
 * statement, the exact same time is returned for each invocation regardless
 * of the amount of time that elapses between invocations.  In other words,
 * the time returned is always the time of the first call.
 */
sql_int64
sqlStmtCurrentTime(sql_context * p)
{
	int rc;
#ifndef SQL_ENABLE_OR_STAT4
	sql_int64 *piTime = &p->pVdbe->iCurrentTime;
	assert(p->pVdbe != 0);
#else
	sql_int64 iTime = 0;
	sql_int64 *piTime =
	    p->pVdbe != 0 ? &p->pVdbe->iCurrentTime : &iTime;
#endif
	if (*piTime == 0) {
		rc = sqlOsCurrentTimeInt64(p->pOut->db->pVfs, piTime);
		if (rc)
			*piTime = 0;
	}
	return *piTime;
}

/*
 * The following is the implementation of an SQL function that always
 * fails with an error message stating that the function is used in the
 * wrong context.  The sql_overload_function() API might construct
 * SQL function that use this routine so that the functions will exist
 * for name resolution.
 */
void
sqlInvalidFunction(sql_context * context,	/* The function calling context */
		       int NotUsed,	/* Number of arguments to the function */
		       sql_value ** NotUsed2	/* Value of each argument */
    )
{
	const char *zName = context->pFunc->zName;
	UNUSED_PARAMETER2(NotUsed, NotUsed2);
	const char *err = "unable to use function %s in the requested context";
	diag_set(ClientError, ER_SQL_EXECUTE, tt_sprintf(err, zName));
	context->is_aborted = true;
}

/*
 * Create a new aggregate context for p and return a pointer to
 * its pMem->z element.
 */
static SQL_NOINLINE void *
createAggContext(sql_context * p, int nByte)
{
	Mem *pMem = p->pMem;
	assert((pMem->flags & MEM_Agg) == 0);
	if (nByte <= 0) {
		sqlVdbeMemSetNull(pMem);
		pMem->z = 0;
	} else {
		sqlVdbeMemClearAndResize(pMem, nByte);
		pMem->flags = MEM_Agg;
		pMem->u.pDef = p->pFunc;
		if (pMem->z) {
			memset(pMem->z, 0, nByte);
		}
	}
	return (void *)pMem->z;
}

/*
 * Allocate or return the aggregate context for a user function.  A new
 * context is allocated on the first call.  Subsequent calls return the
 * same context that was returned on prior calls.
 */
void *
sql_aggregate_context(sql_context * p, int nByte)
{
	assert(p && p->pFunc && p->pFunc->xFinalize);
	testcase(nByte < 0);
	if ((p->pMem->flags & MEM_Agg) == 0) {
		return createAggContext(p, nByte);
	} else {
		return (void *)p->pMem->z;
	}
}

/*
 * Return the number of columns in the result set for the statement pStmt.
 */
int
sql_column_count(sql_stmt * pStmt)
{
	Vdbe *pVm = (Vdbe *) pStmt;
	return pVm ? pVm->nResColumn : 0;
}

/*
 * Return the number of values available from the current row of the
 * currently executing statement pStmt.
 */
int
sql_data_count(sql_stmt * pStmt)
{
	Vdbe *pVm = (Vdbe *) pStmt;
	if (pVm == 0 || pVm->pResultSet == 0)
		return 0;
	return pVm->nResColumn;
}

/*
 * Return a pointer to static memory containing an SQL NULL value.
 */
static const Mem *
columnNullValue(void)
{
	/* Even though the Mem structure contains an element
	 * of type i64, on certain architectures (x86) with certain compiler
	 * switches (-Os), gcc may align this Mem object on a 4-byte boundary
	 * instead of an 8-byte one. This all works fine, except that when
	 * running with SQL_DEBUG defined the sql code sometimes assert()s
	 * that a Mem structure is located on an 8-byte boundary. To prevent
	 * these assert()s from failing, when building with SQL_DEBUG defined
	 * using gcc, we force nullMem to be 8-byte aligned using the magical
	 * __attribute__((aligned(8))) macro.
	 */
	static const Mem nullMem
#if defined(SQL_DEBUG) && defined(__GNUC__)
	    __attribute__ ((aligned(8)))
#endif
	    = {
		/* .u          = */  {
		0},
		    /* .flags      = */ (u16) MEM_Null,
		    /* .eSubtype   = */ (u8) 0,
		    /* .n          = */ (int)0,
		    /* .z          = */ (char *)0,
		    /* .zMalloc    = */ (char *)0,
		    /* .szMalloc   = */ (int)0,
		    /* .uTemp      = */ (u32) 0,
		    /* .db         = */ (sql *) 0,
		    /* .xDel       = */ (void (*)(void *))0,
#ifdef SQL_DEBUG
		    /* .pScopyFrom = */ (Mem *) 0,
		    /* .pFiller    = */ (void *)0,
#endif
	};
	return &nullMem;
}

/*
 * Check to see if column iCol of the given statement is valid.  If
 * it is, return a pointer to the Mem for the value of that column.
 * If iCol is not valid, return a pointer to a Mem which has a value
 * of NULL.
 */
static Mem *
columnMem(sql_stmt * pStmt, int i)
{
	Vdbe *pVm;
	Mem *pOut;

	pVm = (Vdbe *) pStmt;
	if (pVm == 0)
		return (Mem *) columnNullValue();
	assert(pVm->db);
	if (pVm->pResultSet != 0 && i < pVm->nResColumn && i >= 0) {
		pOut = &pVm->pResultSet[i];
	} else {
		pOut = (Mem *) columnNullValue();
	}
	return pOut;
}

/**************************** sql_column_  ******************************
 * The following routines are used to access elements of the current row
 * in the result set.
 */
const void *
sql_column_blob(sql_stmt * pStmt, int i)
{
	const void *val;
	val = sql_value_blob(columnMem(pStmt, i));
	return val;
}

int
sql_column_bytes(sql_stmt * pStmt, int i)
{
	return sql_value_bytes(columnMem(pStmt, i));
}

double
sql_column_double(sql_stmt * pStmt, int i)
{
	return sql_value_double(columnMem(pStmt, i));
}

int
sql_column_int(sql_stmt * pStmt, int i)
{
	return sql_value_int(columnMem(pStmt, i));
}

bool
sql_column_boolean(struct sql_stmt *stmt, int i)
{
	return sql_value_boolean(columnMem(stmt, i));
}

sql_int64
sql_column_int64(sql_stmt * pStmt, int i)
{
	return sql_value_int64(columnMem(pStmt, i));
}

const unsigned char *
sql_column_text(sql_stmt * pStmt, int i)
{
	return sql_value_text(columnMem(pStmt, i));
}

sql_value *
sql_column_value(sql_stmt * pStmt, int i)
{
	Mem *pOut = columnMem(pStmt, i);
	if (pOut->flags & MEM_Static) {
		pOut->flags &= ~MEM_Static;
		pOut->flags |= MEM_Ephem;
	}
	return (sql_value *) pOut;
}

enum mp_type
sql_column_type(sql_stmt * pStmt, int i)
{
	return sql_value_type(columnMem(pStmt, i));
}

enum sql_subtype
sql_column_subtype(struct sql_stmt *stmt, int i)
{
	return sql_value_subtype(columnMem(stmt, i));
}

/*
 * Convert the N-th element of pStmt->pColName[] into a string using
 * xFunc() then return that string.  If N is out of range, return 0.
 *
 * There are up to 5 names for each column.  useType determines which
 * name is returned.  Here are the names:
 *
 *    0      The column name as it should be displayed for output
 *    1      The datatype name for the column
 *    2      The name of the database that the column derives from
 *    3      The name of the table that the column derives from
 *    4      The name of the table column that the result column derives from
 *
 * If the result is not a simple column reference (if it is an expression
 * or a constant) then useTypes 2, 3, and 4 return NULL.
 */
static const void *
columnName(sql_stmt * pStmt,
	   int N, const void *(*xFunc) (Mem *), int useType)
{
	const void *ret;
	Vdbe *p;
	int n;
	sql *db;
	ret = 0;
	p = (Vdbe *) pStmt;
	db = p->db;
	assert(db != 0);
	n = sql_column_count(pStmt);
	if (N < n && N >= 0) {
		N += useType * n;
		assert(db->mallocFailed == 0);
		ret = xFunc(&p->aColName[N]);
		/* A malloc may have failed inside of the xFunc() call. If this
		 * is the case, clear the mallocFailed flag and return NULL.
		 */
		if (db->mallocFailed) {
			sqlOomClear(db);
			ret = 0;
		}
	}
	return ret;
}

/*
 * Return the name of the Nth column of the result set returned by SQL
 * statement pStmt.
 */
const char *
sql_column_name(sql_stmt * pStmt, int N)
{
	return columnName(pStmt, N, (const void *(*)(Mem *))sql_value_text,
			  COLNAME_NAME);
}

const char *
sql_column_datatype(sql_stmt *pStmt, int N)
{
	return columnName(pStmt, N, (const void *(*)(Mem *))sql_value_text,
			  COLNAME_DECLTYPE);
}

/*
 * Return the column declaration type (if applicable) of the 'i'th column
 * of the result set of SQL statement pStmt.
 */
const char *
sql_column_decltype(sql_stmt * pStmt, int N)
{
	return columnName(pStmt, N, (const void *(*)(Mem *))sql_value_text,
			  COLNAME_DECLTYPE);
}

/******************************* sql_bind_  **************************
 *
 * Routines used to attach values to wildcards in a compiled SQL statement.
 */
/*
 * Unbind the value bound to variable i in virtual machine p. This is the
 * the same as binding a NULL value to the column.
 */
static int
vdbeUnbind(Vdbe * p, int i)
{
	Mem *pVar;
	assert(p != NULL);
	assert(p->magic == VDBE_MAGIC_RUN && p->pc < 0);
	assert(i > 0);
	if(i > p->nVar) {
		diag_set(ClientError, ER_SQL_EXECUTE, "The number of "\
			 "parameters is too large");
		return -1;
	}
	i--;
	pVar = &p->aVar[i];
	sqlVdbeMemRelease(pVar);
	pVar->flags = MEM_Null;

	/* If the bit corresponding to this variable in Vdbe.expmask is set, then
	 * binding a new value to this variable invalidates the current query plan.
	 *
	 * IMPLEMENTATION-OF: R-48440-37595 If the specific value bound to host
	 * parameter in the WHERE clause might influence the choice of query plan
	 * for a statement, then the statement will be automatically recompiled,
	 * as if there had been a schema change, on the first sql_step() call
	 * following any change to the bindings of that parameter.
	 */
	if (p->isPrepareV2 &&
	    ((i < 32 && p->expmask & ((u32) 1 << i))
	     || p->expmask == 0xffffffff)
	    ) {
		p->expired = 1;
	}
	return 0;
}

/**
 * This function sets type for bound variable.
 * We should bind types only for variables which occur in
 * result set of SELECT query. For example:
 *
 * SELECT id, ?, ?, a WHERE id = ?;
 *
 * In this case we should set types only for two variables.
 * That one which is situated under WHERE condition - is out
 * of our interest.
 *
 * For named binding parameters we should propagate type
 * for all occurrences of this parameter - since binding
 * routine takes place only once for each DISTINCT parameter
 * from list.
 *
 * @param v Current VDBE.
 * @param position Ordinal position of binding parameter.
 * @param type String literal representing type of binding param.
 * @retval 0 on success.
 */
static int
sql_bind_type(struct Vdbe *v, uint32_t position, const char *type)
{
	if (v->res_var_count < position)
		return 0;
	int rc = 0;
	if (sqlVdbeSetColName(v, v->var_pos[position - 1], COLNAME_DECLTYPE,
			      type, SQL_TRANSIENT) != 0)
		rc = -1;
	const char *bind_name = v->aColName[position - 1].z;
	if (strcmp(bind_name, "?") == 0)
		return rc;
	for (uint32_t i = position; i < v->res_var_count; ++i) {
		if (strcmp(bind_name,  v->aColName[i].z) == 0) {
			if (sqlVdbeSetColName(v, v->var_pos[i],
					      COLNAME_DECLTYPE, type,
					      SQL_TRANSIENT) != 0)
				return -1;
		}
	}
	return 0;
}

/*
 * Bind a text or BLOB value.
 */
static int
bindText(sql_stmt * pStmt,	/* The statement to bind against */
	 int i,			/* Index of the parameter to bind */
	 const void *zData,	/* Pointer to the data to be bound */
	 int nData,		/* Number of bytes of data to be bound */
	 void (*xDel) (void *)	/* Destructor for the data */
    )
{
	Vdbe *p = (Vdbe *) pStmt;
	Mem *pVar;
	if (vdbeUnbind(p, i) != 0) {
		if (xDel != SQL_STATIC && xDel != SQL_TRANSIENT)
			xDel((void *)zData);
		return -1;
	}
	if (zData == NULL)
		return 0;
	pVar = &p->aVar[i - 1];
	if (sqlVdbeMemSetStr(pVar, zData, nData, 1, xDel) != 0)
		return -1;
	return sql_bind_type(p, i, "TEXT");
}

/*
 * Bind a blob value to an SQL statement variable.
 */
int
sql_bind_blob(sql_stmt * pStmt,
		  int i, const void *zData, int nData, void (*xDel) (void *)
    )
{
	struct Vdbe *p = (Vdbe *) pStmt;
	if (vdbeUnbind(p, i) != 0) {
		if (xDel != SQL_STATIC && xDel != SQL_TRANSIENT)
			xDel((void *)zData);
		return -1;
	}
	if (zData == NULL)
		return 0;
	struct Mem *var = &p->aVar[i - 1];
	if (sqlVdbeMemSetStr(var, zData, nData, 0, xDel) != 0)
		return -1;
	return sql_bind_type(p, i, "VARBINARY");
}

int
sql_bind_blob64(sql_stmt * pStmt,
		    int i,
		    const void *zData,
		    sql_uint64 nData, void (*xDel) (void *)
    )
{
	assert(xDel != SQL_DYNAMIC);
	if (nData > 0x7fffffff) {
		return invokeValueDestructor(zData, xDel, 0);
	} else {
		return sql_bind_blob(pStmt, i, zData, (int)nData, xDel);
	}
}

int
sql_bind_double(sql_stmt * pStmt, int i, double rValue)
{
	Vdbe *p = (Vdbe *) pStmt;
	if (vdbeUnbind(p, i) != 0)
		return -1;
	int rc = sql_bind_type(p, i, "NUMERIC");
	sqlVdbeMemSetDouble(&p->aVar[i - 1], rValue);
	return rc;
}

int
sql_bind_boolean(struct sql_stmt *stmt, int i, bool value)
{
	struct Vdbe *p = (struct Vdbe *) stmt;
	if (vdbeUnbind(p, i) != 0)
		return -1;
	int rc = sql_bind_type(p, i, "BOOLEAN");
	mem_set_bool(&p->aVar[i - 1], value);
	return rc;
}

int
sql_bind_int(sql_stmt * p, int i, int iValue)
{
	return sql_bind_int64(p, i, (i64) iValue);
}

int
sql_bind_int64(sql_stmt * pStmt, int i, sql_int64 iValue)
{
	Vdbe *p = (Vdbe *) pStmt;
	if (vdbeUnbind(p, i) != 0)
		return -1;
	int rc = sql_bind_type(p, i, "INTEGER");
	sqlVdbeMemSetInt64(&p->aVar[i - 1], iValue);
	return rc;
}

int
sql_bind_null(sql_stmt * pStmt, int i)
{
	Vdbe *p = (Vdbe *) pStmt;
	if (vdbeUnbind(p, i) != 0)
		return -1;
	return sql_bind_type(p, i, "BOOLEAN");
}

int
sql_bind_ptr(struct sql_stmt *stmt, int i, void *ptr)
{
	struct Vdbe *p = (struct Vdbe *) stmt;
	int rc = vdbeUnbind(p, i);
	if (rc == 0) {
		rc = sql_bind_type(p, i, "VARBINARY");
		mem_set_ptr(&p->aVar[i - 1], ptr);
	}
	return rc;
}

int
sql_bind_text(sql_stmt * pStmt,
		  int i, const char *zData, int nData, void (*xDel) (void *)
    )
{
	return bindText(pStmt, i, zData, nData, xDel);
}

int
sql_bind_text64(sql_stmt * pStmt,
		    int i,
		    const char *zData,
		    sql_uint64 nData,
		    void (*xDel) (void *))
{
	assert(xDel != SQL_DYNAMIC);
	if (nData > 0x7fffffff) {
		return invokeValueDestructor(zData, xDel, 0);
	} else {
		return bindText(pStmt, i, zData, (int)nData, xDel);
	}
}

int
sql_bind_zeroblob(sql_stmt * pStmt, int i, int n)
{
	Vdbe *p = (Vdbe *) pStmt;
	if (vdbeUnbind(p, i) != 0)
		return -1;
	sqlVdbeMemSetZeroBlob(&p->aVar[i - 1], n);
	return 0;
}

int
sql_bind_zeroblob64(sql_stmt * pStmt, int i, sql_uint64 n)
{
	Vdbe *p = (Vdbe *) pStmt;
	if (n > (u64) p->db->aLimit[SQL_LIMIT_LENGTH]) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob is too "\
			 "big");
		return -1;
	}
	assert((n & 0x7FFFFFFF) == n);
	return sql_bind_zeroblob(pStmt, i, n);
}

/*
 * Return the number of wildcards that can be potentially bound to.
 * This routine is added to support DBD::sql.
 */
int
sql_bind_parameter_count(sql_stmt * pStmt)
{
	Vdbe *p = (Vdbe *) pStmt;
	return p ? p->nVar : 0;
}

/*
 * Return the name of a wildcard parameter.  Return NULL if the index
 * is out of range or if the wildcard is unnamed.
 *
 * The result is always UTF-8.
 */
const char *
sql_bind_parameter_name(sql_stmt * pStmt, int i)
{
	Vdbe *p = (Vdbe *) pStmt;
	if (p == 0)
		return 0;
	return sqlVListNumToName(p->pVList, i);
}

/*
 * Given a wildcard parameter name, return the index of the variable
 * with that name.  If there is no variable with the given name,
 * return 0.
 */
int
sqlVdbeParameterIndex(Vdbe * p, const char *zName, int nName)
{
	if (p == 0 || zName == 0)
		return 0;
	return sqlVListNameToNum(p->pVList, zName, nName);
}

int
sql_bind_parameter_index(sql_stmt * pStmt, const char *zName)
{
	return sqlVdbeParameterIndex((Vdbe *) pStmt, zName,
					 sqlStrlen30(zName));
}

int
sql_bind_parameter_lindex(sql_stmt * pStmt, const char *zName,
			      int nName)
{
	return sqlVdbeParameterIndex((Vdbe *) pStmt, zName, nName);
}

/*
 * Transfer all bindings from the first statement over to the second.
 */
int
sqlTransferBindings(sql_stmt * pFromStmt, sql_stmt * pToStmt)
{
	Vdbe *pFrom = (Vdbe *) pFromStmt;
	Vdbe *pTo = (Vdbe *) pToStmt;
	int i;
	assert(pTo->db == pFrom->db);
	assert(pTo->nVar == pFrom->nVar);
	for (i = 0; i < pFrom->nVar; i++) {
		sqlVdbeMemMove(&pTo->aVar[i], &pFrom->aVar[i]);
	}
	return 0;
}

/*
 * Return the sql* database handle to which the prepared statement given
 * in the argument belongs.  This is the same database handle that was
 * the first argument to the sql_prepare() that was used to create
 * the statement in the first place.
 */
sql *
sql_db_handle(sql_stmt * pStmt)
{
	return pStmt ? ((Vdbe *) pStmt)->db : 0;
}

/*
 * Return true if the prepared statement is in need of being reset.
 */
int
sql_stmt_busy(sql_stmt * pStmt)
{
	Vdbe *v = (Vdbe *) pStmt;
	return v != 0 && v->magic == VDBE_MAGIC_RUN && v->pc >= 0;
}

/*
 * Return a pointer to the next prepared statement after pStmt associated
 * with database connection pDb.  If pStmt is NULL, return the first
 * prepared statement for the database connection.  Return NULL if there
 * are no more.
 */
sql_stmt *
sql_next_stmt(sql * pDb, sql_stmt * pStmt)
{
	sql_stmt *pNext;
	if (pStmt == 0) {
		pNext = (sql_stmt *) pDb->pVdbe;
	} else {
		pNext = (sql_stmt *) ((Vdbe *) pStmt)->pNext;
	}
	return pNext;
}

/*
 * Return the value of a status counter for a prepared statement
 */
int
sql_stmt_status(sql_stmt * pStmt, int op, int resetFlag)
{
	Vdbe *pVdbe = (Vdbe *) pStmt;
	u32 v;
	v = pVdbe->aCounter[op];
	if (resetFlag)
		pVdbe->aCounter[op] = 0;
	return (int)v;
}

/*
 * Return the SQL associated with a prepared statement
 */
const char *
sql_sql(sql_stmt * pStmt)
{
	Vdbe *p = (Vdbe *) pStmt;
	return p ? p->zSql : 0;
}

/*
 * Return the SQL associated with a prepared statement with
 * bound parameters expanded.  Space to hold the returned string is
 * obtained from sql_malloc().  The caller is responsible for
 * freeing the returned string by passing it to sql_free().
 */
char *
sql_expanded_sql(sql_stmt * pStmt)
{
	char *z = 0;
	const char *zSql = sql_sql(pStmt);
	if (zSql) {
		Vdbe *p = (Vdbe *) pStmt;
		z = sqlVdbeExpandSql(p, zSql);
	}
	return z;
}

