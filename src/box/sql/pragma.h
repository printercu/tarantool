/* DO NOT EDIT!
 * This file is automatically generated by the script at
 * ../tool/mkpragmatab.tcl.  To update the set of pragmas, edit
 * that script and rerun it.
 */

/* The various pragma types */
#define PragTyp_COLLATION_LIST                 3
#define PragTyp_FLAG                           5
#define PragTyp_FOREIGN_KEY_LIST               9
#define PragTyp_INDEX_INFO                    10
#define PragTyp_INDEX_LIST                    11
#define PragTyp_STATS                         15
#define PragTyp_TABLE_INFO                    17
#define PragTyp_DEFAULT_ENGINE                25
#define PragTyp_COMPOUND_SELECT_LIMIT         26

/* Property flags associated with various pragma. */
#define PragFlg_NeedSchema 0x01	/* Force schema load before running */
#define PragFlg_NoColumns  0x02	/* OP_ResultRow called with zero columns */
#define PragFlg_NoColumns1 0x04	/* zero columns if RHS argument is present */
#define PragFlg_Result0    0x10	/* Acts as query when no argument */
#define PragFlg_Result1    0x20	/* Acts as query when has one argument */
#define PragFlg_SchemaOpt  0x40	/* Schema restricts name search if present */
#define PragFlg_SchemaReq  0x80	/* Schema required - "main" is default */

/**
 * Column names and types for pragmas. The type of the column is
 * the following value after its name.
 */
static const char *const pragCName[] = {
	/* Used by: table_info */
	/*   0 */ "cid",
	/*   1 */ "INTEGER",
	/*   2 */ "name",
	/*   3 */ "STRING",
	/*   4 */ "type",
	/*   3 */ "STRING",
	/*   6 */ "notnull",
	/*   1 */ "INTEGER",
	/*   8 */ "dflt_value",
	/*   9 */ "STRING",
	/*  10 */ "pk",
	/*  11 */ "INTEGER",
	/* Used by: stats */
	/*  12 */ "table",
	/*  13 */ "STRING",
	/*  14 */ "index",
	/*  15 */ "STRING",
	/*  16 */ "width",
	/*  17 */ "INTEGER",
	/*  18 */ "height",
	/*  19 */ "INTEGER",
	/* Used by: index_info */
	/*  20 */ "seqno",
	/*  21 */ "INTEGER",
	/*  22 */ "cid",
	/*  23 */ "INTEGER",
	/*  24 */ "name",
	/*  25 */ "STRING",
	/*  26 */ "desc",
	/*  27 */ "INTEGER",
	/*  28 */ "coll",
	/*  29 */ "STRING",
	/*  30 */ "type",
	/*  31 */ "STRING",
	/* Used by: index_list */
	/*  32 */ "seq",
	/*  33 */ "INTEGER",
	/*  34 */ "name",
	/*  35 */ "STRING",
	/*  36 */ "unique",
	/*  37 */ "INTEGER",
	/* Used by: collation_list */
	/*  38 */ "seq",
	/*  39 */ "INTEGER",
	/*  40 */ "name",
	/*  41 */ "STRING",
	/* Used by: foreign_key_list */
	/*  42 */ "id",
	/*  43 */ "INTEGER",
	/*  44 */ "seq",
	/*  45 */ "INTEGER",
	/*  46 */ "table",
	/*  47 */ "STRING",
	/*  48 */ "from",
	/*  49 */ "STRING",
	/*  50 */ "to",
	/*  51 */ "STRING",
	/*  52 */ "on_update",
	/*  53 */ "STRING",
	/*  54 */ "on_delete",
	/*  55 */ "STRING",
	/*  56 */ "match",
	/*  57 */ "STRING",
	/* Used by: case_sensitive_like */
	/*  58 */ "case_sensitive_like",
	/*  59 */ "INTEGER",
	/* Used by: count_changes */
	/*  60 */ "count_changes",
	/*  61 */ "INTEGER",
	/* Used by: defer_foreign_keys */
	/*  62 */ "defer_foreign_keys",
	/*  63 */ "INTEGER",
	/* Used by: full_column_names */
	/*  64 */ "full_column_names",
	/*  65 */ "INTEGER",
	/* Used by: parser_trace */
	/*  66 */ "parser_trace",
	/*  67 */ "INTEGER",
	/* Used by: recursive_triggers */
	/*  68 */ "recursive_triggers",
	/*  69 */ "INTEGER",
	/* Used by: reverse_unordered_selects */
	/*  70 */ "reverse_unordered_selects",
	/*  71 */ "INTEGER",
	/* Used by: select_trace */
	/*  72 */ "select_trace",
	/*  73 */ "INTEGER",
	/* Used by: short_column_names */
	/*  74 */ "short_column_names",
	/*  75 */ "INTEGER",
	/* Used by: sql_compound_select_limit */
	/*  76 */ "sql_compound_select_limit",
	/*  77 */ "INTEGER",
	/* Used by: sql_default_engine */
	/*  78 */ "sql_default_engine",
	/*  79 */ "STRING",
	/* Used by: sql_trace */
	/*  80 */ "sql_trace",
	/*  81 */ "INTEGER",
	/* Used by: vdbe_addoptrace */
	/*  82 */ "vdbe_addoptrace",
	/*  83 */ "INTEGER",
	/* Used by: vdbe_debug */
	/*  84 */ "vdbe_debug",
	/*  85 */ "INTEGER",
	/* Used by: vdbe_eqp */
	/*  86 */ "vdbe_eqp",
	/*  87 */ "INTEGER",
	/* Used by: vdbe_listing */
	/*  88 */ "vdbe_listing",
	/*  89 */ "INTEGER",
	/* Used by: vdbe_trace */
	/*  90 */ "vdbe_trace",
	/*  91 */ "INTEGER",
	/* Used by: where_trace */
	/*  92 */ "where_trace",
	/*  93 */ "INTEGER",
};

/* Definitions of all built-in pragmas */
typedef struct PragmaName {
	const char *const zName;	/* Name of pragma */
	u8 ePragTyp;		/* PragTyp_XXX value */
	u8 mPragFlg;		/* Zero or more PragFlg_XXX values */
	u8 iPragCName;		/* Start of column names in pragCName[] */
	u8 nPragCName;		/* Num of col names. */
	u32 iArg;		/* Extra argument */
} PragmaName;
/**
 * The order of pragmas in this array is important: it has
 * to be sorted. For more info see pragma_locate function.
 */
static const PragmaName aPragmaName[] = {
	{ /* zName:     */ "case_sensitive_like",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 58, 1,
	 /* iArg:      */ LIKE_CASE_SENS_FLAG},
	{ /* zName:     */ "collation_list",
	 /* ePragTyp:  */ PragTyp_COLLATION_LIST,
	 /* ePragFlg:  */ PragFlg_Result0,
	 /* ColNames:  */ 38, 2,
	 /* iArg:      */ 0},
	{ /* zName:     */ "count_changes",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 60, 1,
	 /* iArg:      */ SQL_CountRows},
	{ /* zName:     */ "defer_foreign_keys",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 62, 1,
	 /* iArg:      */ SQL_DeferFKs},
	{ /* zName:     */ "foreign_key_list",
	 /* ePragTyp:  */ PragTyp_FOREIGN_KEY_LIST,
	 /* ePragFlg:  */
	 PragFlg_NeedSchema | PragFlg_Result1 | PragFlg_SchemaOpt,
	 /* ColNames:  */ 42, 8,
	 /* iArg:      */ 0},
	{ /* zName:     */ "full_column_names",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 64, 1,
	 /* iArg:      */ SQL_FullColNames},
	{ /* zName:     */ "index_info",
	 /* ePragTyp:  */ PragTyp_INDEX_INFO,
	 /* ePragFlg:  */
	 PragFlg_NeedSchema | PragFlg_Result1 | PragFlg_SchemaOpt,
	 /* ColNames:  */ 20, 6,
	 /* iArg:      */ 1},
	{ /* zName:     */ "index_list",
	 /* ePragTyp:  */ PragTyp_INDEX_LIST,
	 /* ePragFlg:  */
	 PragFlg_NeedSchema | PragFlg_Result1 | PragFlg_SchemaOpt,
	 /* ColNames:  */ 32, 3,
	 /* iArg:      */ 0},
#if defined(SQL_DEBUG)
	{ /* zName:     */ "parser_trace",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 66, 1,
	 /* iArg:      */ PARSER_TRACE_FLAG},
#endif
	{ /* zName:     */ "recursive_triggers",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 68, 1,
	 /* iArg:      */ SQL_RecTriggers},
	{ /* zName:     */ "reverse_unordered_selects",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 70, 1,
	 /* iArg:      */ SQL_ReverseOrder},
#if defined(SQL_DEBUG)
	{ /* zName:     */ "select_trace",
	/* ePragTyp:  */ PragTyp_FLAG,
	/* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	/* ColNames:  */ 72, 1,
	/* iArg:      */ SQL_SelectTrace},
#endif
	{ /* zName:     */ "short_column_names",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 73, 1,
	 /* iArg:      */ SQL_ShortColNames},
	{ /* zName:     */ "sql_compound_select_limit",
	/* ePragTyp:  */ PragTyp_COMPOUND_SELECT_LIMIT,
	/* ePragFlg:  */ PragFlg_Result0,
	/* ColNames:  */ 76, 1,
	/* iArg:      */ 0},
	{ /* zName:     */ "sql_default_engine",
	 /* ePragTyp:  */ PragTyp_DEFAULT_ENGINE,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 78, 1,
	 /* iArg:      */ 0},
#if defined(SQL_DEBUG)
	{ /* zName:     */ "sql_trace",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 80, 1,
	 /* iArg:      */ SQL_SqlTrace},
#endif
	{ /* zName:     */ "stats",
	 /* ePragTyp:  */ PragTyp_STATS,
	 /* ePragFlg:  */
	 PragFlg_NeedSchema | PragFlg_Result0 | PragFlg_SchemaReq,
	 /* ColNames:  */ 12, 4,
	 /* iArg:      */ 0},
	{ /* zName:     */ "table_info",
	 /* ePragTyp:  */ PragTyp_TABLE_INFO,
	 /* ePragFlg:  */
	 PragFlg_NeedSchema | PragFlg_Result1 | PragFlg_SchemaOpt,
	 /* ColNames:  */ 0, 6,
	 /* iArg:      */ 0},
#if defined(SQL_DEBUG)
	{ /* zName:     */ "vdbe_addoptrace",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 82, 1,
	 /* iArg:      */ SQL_VdbeAddopTrace},
	{ /* zName:     */ "vdbe_debug",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 84, 1,
	 /* iArg:      */
	 SQL_SqlTrace | SQL_VdbeListing | SQL_VdbeTrace},
	{ /* zName:     */ "vdbe_eqp",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 86, 1,
	 /* iArg:      */ SQL_VdbeEQP},
	{ /* zName:     */ "vdbe_listing",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 88, 1,
	 /* iArg:      */ SQL_VdbeListing},
	{ /* zName:     */ "vdbe_trace",
	 /* ePragTyp:  */ PragTyp_FLAG,
	 /* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	 /* ColNames:  */ 90, 1,
	 /* iArg:      */ SQL_VdbeTrace},
	{ /* zName:     */ "where_trace",
	/* ePragTyp:  */ PragTyp_FLAG,
	/* ePragFlg:  */ PragFlg_Result0 | PragFlg_NoColumns1,
	/* ColNames:  */ 92, 1,
	/* iArg:      */ SQL_WhereTrace},
#endif
};
/* Number of pragmas: 36 on by default, 47 total. */
