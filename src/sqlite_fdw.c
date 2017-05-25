/*-------------------------------------------------------------------------
 *
 * sqlite Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2013-2016 Guillaume Lelarge
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Guillaume Lelarge <guillaume@lelarge.info>
 *
 * IDENTIFICATION
 *        sqlite_fdw/src/sqlite_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/reloptions.h>
#include <foreign/fdwapi.h>
#include <foreign/foreign.h>
#include <optimizer/pathnode.h>
#include <optimizer/planmain.h>
#include <optimizer/restrictinfo.h>
#include <optimizer/var.h>

#include <funcapi.h>
#include <catalog/pg_collation.h>
#include <catalog/pg_foreign_server.h>
#include <catalog/pg_foreign_table.h>
#include <catalog/pg_type.h>
#include <commands/defrem.h>
#include <commands/explain.h>
#include <utils/builtins.h>
#include <utils/formatting.h>
#include <utils/rel.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <nodes/nodeFuncs.h>

#include <sqlite3.h>
#include <sys/stat.h>


/** Place in a separate header file later **/
typedef struct SqliteOpt
{
    char   *database;
    char   *table;
} SqliteOpt;

extern bool is_foreign_expr(PlannerInfo *root,
        RelOptInfo *baserel, Expr *expr);

extern void sqlite_deparse_select(StringInfo buf,
        PlannerInfo *root, RelOptInfo *baserel,
        Bitmapset *attrs_used, char *svr_table, List **retrieved_attrs);
extern void sqlite_append_where_clause(StringInfo buf,
        PlannerInfo *root, RelOptInfo *baserel,
        List *exprs, bool is_first, List **params);
/****/

PG_MODULE_MAGIC;

/*
 * Default values
 */
/* ----
 * This value is taken from sqlite
   (without stats, sqlite defaults to 1 million tuples for a table)
 */
#define DEFAULT_ESTIMATED_LINES 1000000
#define DEFAULT_STARTUP_COST 10

/*
 * SQL functions
 */
extern Datum sqlite_fdw_handler(PG_FUNCTION_ARGS);
extern Datum sqlite_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(sqlite_fdw_handler);
PG_FUNCTION_INFO_V1(sqlite_fdw_validator);


/* callback functions */
static void sqliteGetForeignRelSize(PlannerInfo *root,
						   RelOptInfo *baserel,
						   Oid foreigntableid);

static void sqliteGetForeignPaths(PlannerInfo *root,
						 RelOptInfo *baserel,
						 Oid foreigntableid);

static ForeignScan *sqliteGetForeignPlan(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid,
						ForeignPath *best_path,
						List *tlist,
						List *scan_clauses,
						Plan *outer_plan);

static void sqliteBeginForeignScan(ForeignScanState *node,
						  int eflags);

static TupleTableSlot *sqliteIterateForeignScan(ForeignScanState *node);

static void sqliteReScanForeignScan(ForeignScanState *node);

static void sqliteEndForeignScan(ForeignScanState *node);

static void sqliteAddForeignUpdateTargets(Query *parsetree,
								 RangeTblEntry *target_rte,
								 Relation target_relation);

static List *sqlitePlanForeignModify(PlannerInfo *root,
						   ModifyTable *plan,
						   Index resultRelation,
						   int subplan_index);

static void sqliteBeginForeignModify(ModifyTableState *mtstate,
							ResultRelInfo *rinfo,
							List *fdw_private,
							int subplan_index,
							int eflags);

static TupleTableSlot *sqliteExecForeignInsert(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot);

static TupleTableSlot *sqliteExecForeignUpdate(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot);

static TupleTableSlot *sqliteExecForeignDelete(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot);

static void sqliteEndForeignModify(EState *estate,
						  ResultRelInfo *rinfo);

static void sqliteExplainForeignScan(ForeignScanState *node,
							struct ExplainState *es);

static void sqliteExplainForeignModify(ModifyTableState *mtstate,
							  ResultRelInfo *rinfo,
							  List *fdw_private,
							  int subplan_index,
							  struct ExplainState *es);

static bool sqliteAnalyzeForeignTable(Relation relation,
							 AcquireSampleRowsFunc *func,
							 BlockNumber *totalpages);

static List *sqliteImportForeignSchema(ImportForeignSchemaStmt *stmt,
							 Oid serverOid);

/*
 * Helper functions
 */
static void sqliteOpen(char const *filename, sqlite3 **db);
static sqlite3_stmt *sqlitePrepare(sqlite3 *db, char *query, const char **pzTail);
static bool sqliteIsValidOption(const char *option, Oid context);
static SqliteOpt *sqliteGetOptions(Oid foreigntableid);
static int GetEstimatedRows(char const * filename, char * sql);
static bool file_exists(const char *name);
static void sqliteTranslateType(StringInfo str, char *typname);


/*
 * structures used by the FDW
 *
 * These next two are not actually used by sqlite, but something like this
 * will be needed by anything more complicated that does actual work.
 *
 */

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct SQLiteFdwOption
{
	const char	*optname;
	Oid		optcontext;	/* Oid of catalog in which option may appear */
};

/*
 * Describes the valid options for objects that use this wrapper.
 */
static struct SQLiteFdwOption valid_options[] =
{

	/* Connection options */
	{ "database",  ForeignServerRelationId },

	/* Table options */
	{ "table",     ForeignTableRelationId },

	/* Sentinel */
	{ NULL,			InvalidOid }
};

/*
 * This is what will be set and stashed away in fdw_private and fetched
 * for subsequent routines.
 */
typedef struct
{
	char	   *foo;
	int			bar;
}	sqliteFdwPlanState;


typedef struct SqliteFdwRelationInfo
{
	/* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
	List	   *remote_conds;
	List	   *local_conds;

	/* Bitmap of attr numbers we need to fetch from the remote server. */
	Bitmapset  *attrs_used;

} SqliteFdwRelationInfo;



/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */

typedef struct SQLiteFdwExecutionState
{
	sqlite3       *db;
	sqlite3_stmt  *stmt;
	char          *query;
	List          *retrieved_attrs;   /* list of target attribute numbers */
	MemoryContext temp_cxt;           /* context for per-tuple temporary data */
} SQLiteFdwExecutionState;


static void sqlite_bind_param_values(SQLiteFdwExecutionState *festate,
        List *fdw_exprs, ForeignScanState * node);
static void sqlite_bind_param_value(SQLiteFdwExecutionState *festate,
        int index, Oid ptype, Datum pval, bool isNull);
static void cleanup_(SQLiteFdwExecutionState *);


Datum
sqlite_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	elog(DEBUG1,"entering function %s",__func__);

	/* assign the handlers for the FDW */

	/* these are required */
#if (PG_VERSION_NUM >= 90200)
	fdwroutine->GetForeignRelSize = sqliteGetForeignRelSize;
	fdwroutine->GetForeignPaths = sqliteGetForeignPaths;
	fdwroutine->GetForeignPlan = sqliteGetForeignPlan;
#else
	fdwroutine->PlanForeignScan = sqlitePlanForeignScan;
#endif
	fdwroutine->BeginForeignScan = sqliteBeginForeignScan;
	fdwroutine->IterateForeignScan = sqliteIterateForeignScan;
	fdwroutine->ReScanForeignScan = sqliteReScanForeignScan;
	fdwroutine->EndForeignScan = sqliteEndForeignScan;

	/* remainder are optional - use NULL if not required */
	/* support for insert / update / delete */
#if (PG_VERSION_NUM >= 90300)
	fdwroutine->AddForeignUpdateTargets = sqliteAddForeignUpdateTargets;
	fdwroutine->PlanForeignModify = sqlitePlanForeignModify;
	fdwroutine->BeginForeignModify = sqliteBeginForeignModify;
	fdwroutine->ExecForeignInsert = sqliteExecForeignInsert;
	fdwroutine->ExecForeignUpdate = sqliteExecForeignUpdate;
	fdwroutine->ExecForeignDelete = sqliteExecForeignDelete;
	fdwroutine->EndForeignModify = sqliteEndForeignModify;
#endif

	/* support for EXPLAIN */
	fdwroutine->ExplainForeignScan = sqliteExplainForeignScan;
#if (PG_VERSION_NUM >= 90300)
	fdwroutine->ExplainForeignModify = sqliteExplainForeignModify;
#endif

#if (PG_VERSION_NUM >= 90200)
	/* support for ANALYSE */
	fdwroutine->AnalyzeForeignTable = sqliteAnalyzeForeignTable;
#endif

#if (PG_VERSION_NUM >= 90500)
	/* support for IMPORT FOREIGN SCHEMA */
	fdwroutine->ImportForeignSchema = sqliteImportForeignSchema;
#endif

	PG_RETURN_POINTER(fdwroutine);
}

Datum
sqlite_fdw_validator(PG_FUNCTION_ARGS)
{
	List      *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid       catalog = PG_GETARG_OID(1);
	char      *svr_database = NULL;
	char      *svr_table = NULL;
	ListCell  *cell;

	elog(DEBUG1,"entering function %s",__func__);

	/*
	 * Check that only options supported by sqlite_fdw,
	 * and allowed for the current object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem	   *def = (DefElem *) lfirst(cell);

		if (!sqliteIsValidOption(def->defname, catalog))
		{
			struct SQLiteFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
							 opt->optname);
			}

			ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
				errmsg("invalid option \"%s\"", def->defname),
				errhint("Valid options in this context are: %s", buf.len ? buf.data : "<none>")
				));
		}

		if (strcmp(def->defname, "database") == 0)
		{
			if (svr_database)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("redundant options: database (%s)", defGetString(def))
					));
			if (!file_exists(defGetString(def)))
				ereport(ERROR,
					(errcode_for_file_access(),
					errmsg("could not access file \"%s\"", defGetString(def))
					));

			svr_database = defGetString(def);
		}
		else if (strcmp(def->defname, "table") == 0)
		{
			if (svr_table)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("redundant options: table (%s)", defGetString(def))
					));

			svr_table = defGetString(def);
		}
	}

	/* Check we have the options we need to proceed */
	if (catalog == ForeignServerRelationId && !svr_database)
		ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("The database name must be specified")
			));

	PG_RETURN_VOID();
}


/*
 * Open the given sqlite3 file, and throw an error if the file couldn't be
 * opened
 */
static void
sqliteOpen(char const *filename, sqlite3 **db)
{
	if (sqlite3_open(filename, db) != SQLITE_OK) 
    {
        char const * errmsg_from_sqlite3 = pstrdup(sqlite3_errmsg(db));
		ereport(ERROR,
			(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
			errmsg("Can't open sqlite database %s: %s", filename, errmsg_from_sqlite3)
			));
    }
}


/*
 * Prepare the given query. If case of error, close the db and throw an error
 */
static sqlite3_stmt *
sqlitePrepare(sqlite3 *db, char *query, const char **pzTail)
{
	int rc;
    sqlite3_stmt *stmt;
    
    elog(WARNING, "entering function sqlitePrepare with \n%s", query);

	/* Execute the query */
	rc = sqlite3_prepare_v2(db, query, -1, &stmt, pzTail);
	if (rc != SQLITE_OK)
	{
		sqlite3_close(db);
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			errmsg("SQL error during prepare: %s", sqlite3_errmsg(db))
			));
	}
    return stmt;
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
sqliteIsValidOption(const char *option, Oid context)
{
	struct SQLiteFdwOption *opt;

	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}

	return false;
}

/*
 * Fetch the options for a mysql_fdw foreign table.
 */
static SqliteOpt *
sqliteGetOptions(Oid foreigntableid)
{
	ForeignTable   *f_table;
	ForeignServer  *f_server;
	List           *options;
	ListCell       *lc;
    SqliteOpt      *opt;
	
    opt = (SqliteOpt*) palloc0(sizeof(SqliteOpt));

	/*
	 * Extract options from FDW objects.
	 */
	f_table = GetForeignTable(foreigntableid);
	f_server = GetForeignServer(f_table->serverid);

	options = NIL;
	options = list_concat(options, f_table->options);
	options = list_concat(options, f_server->options);

	/* Loop through the options */
	foreach(lc, options)
	{
		DefElem *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "database") == 0)
		{
			opt->database = defGetString(def);
		}

		if (strcmp(def->defname, "table") == 0)
		{
			opt->table = defGetString(def);
		}
	}

	if (!opt->table)
	{
		opt->table = get_rel_name(foreigntableid);
	}

	/* Check we have the options we need to proceed */
	if (!opt->database || !opt->table)
		ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("a database and a table must be specified")
			));
    return opt;
}


static void
sqliteGetForeignRelSize(PlannerInfo *root,
                        RelOptInfo *baserel,
                        Oid foreigntableid)
{
	SqliteFdwRelationInfo *fpinfo;
    SqliteOpt             *opt;
	ListCell              *lc;
	List                  *retrieved_attrs = NULL;
	StringInfoData        sql;
	List                  *params_list = NULL;
    
    elog(WARNING, "entering function sqliteGetForeignRelSize");

    // initialize the fields of baserel that we will set
	baserel->rows = 0;
	fpinfo = palloc0(sizeof(SqliteFdwRelationInfo));
	baserel->fdw_private = (void *) fpinfo;
    
    opt = sqliteGetOptions(foreigntableid);
    pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid, &fpinfo->attrs_used);
    
    //  classify the condition as local or remote
    foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (is_foreign_expr(root, baserel, ri->clause))
			fpinfo->remote_conds = lappend(fpinfo->remote_conds, ri);
		else
			fpinfo->local_conds = lappend(fpinfo->local_conds, ri);
	}
	
    // We will need to fetch the attributes that are needed locally by postgres
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		pull_varattnos((Node *) rinfo->clause, baserel->relid, &fpinfo->attrs_used);
	}
    
    // Form the query that will be sent to sqlite
    initStringInfo(&sql);
    sqlite_deparse_select(&sql, root, baserel, fpinfo->attrs_used, 
                          opt->table, &retrieved_attrs);
    if (fpinfo->remote_conds)
        sqlite_append_where_clause(&sql, root, baserel, fpinfo->remote_conds,
						           true, &params_list);

	baserel->rows = GetEstimatedRows(opt->database, sql.data);
    baserel->tuples = baserel->rows;
}


/*   Going to use sqlite_stmt_scanstatus to get an an estimate
 *   of the number of rows.
 *   The function sqlite_stmt_scanstatus is not a part of the standard
 *   sqlite3 distribution.
 */
static int
GetEstimatedRows(char const * filename, char * sql)
{
	sqlite3		   *db;
	sqlite3_stmt   *stmt;
	const char	   *pzTail;
    double          estimate = 0;
    
    elog(WARNING, "entering function GetEstimatedRows");

	/* Connect to the server */
	sqliteOpen(filename, &db);
	stmt = sqlitePrepare(db, sql, &pzTail);
    
    sqlite3_stmt_scanstatus_reset(stmt);
    if ( sqlite3_stmt_scanstatus(stmt, 0, SQLITE_SCANSTAT_EST, &estimate ) != SQLITE_OK ) 
    {
	    sqlite3_finalize(stmt);
	    sqlite3_close(db);
		ereport(ERROR,
			(errcode(ERRCODE_FDW_TABLE_NOT_FOUND),
			errmsg("Could not run sqlite_stmt_scanstatus")
			));
    }

	sqlite3_finalize(stmt);
	sqlite3_close(db);

	return (int) estimate;
}


static void
sqliteGetForeignPaths(PlannerInfo *root,
                      RelOptInfo *baserel,
                      Oid foreigntableid)
{
	Cost		startup_cost,
				total_cost;

	startup_cost = DEFAULT_STARTUP_COST;
	total_cost = startup_cost + baserel->rows;

	/* Create a ForeignPath node and add it as only possible path */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
									 NULL,		/* default pathtarget */
									 baserel->rows,
									 startup_cost,
									 total_cost,
									 NIL,		/* no pathkeys */
									 NULL,		/* no outer rel either */
									 NULL,		/* no extra plan */
									 NIL));		/* no fdw_private data */
}



static ForeignScan *
sqliteGetForeignPlan(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid,
						ForeignPath *best_path,
						List *tlist,
						List *scan_clauses,
						Plan *outer_plan)
{
	SqliteFdwRelationInfo *fpinfo = (SqliteFdwRelationInfo *) baserel->fdw_private;
	Index		scan_relid = baserel->relid;
	List        *fdw_private;
	List        *local_exprs = NULL;
	List        *remote_exprs = NULL;
	List        *params_list = NULL;
	List        *remote_conds = NIL;
	StringInfoData sql;
	SqliteOpt      *options;
	List           *retrieved_attrs;
	ListCell       *lc;

	/* Fetch options */
	options = sqliteGetOptions(foreigntableid);
	
    /* Build the query */
	initStringInfo(&sql);
	
    /*
	 * Separate the scan_clauses into those that can be executed remotely and
	 * those that can't.  baserestrictinfo clauses that were previously
	 * determined to be safe or unsafe by classifyConditions are shown in
	 * fpinfo->remote_conds and fpinfo->local_conds.  Anything else in the
	 * scan_clauses list will be a join clause, which we have to check for
	 * remote-safety.
	 *
	 * Note: the join clauses we see here should be the exact same ones
	 * previously examined by postgresGetForeignPaths.  Possibly it'd be worth
	 * passing forward the classification work done then, rather than
	 * repeating it here.
	 *
	 * This code must match "extract_actual_clauses(scan_clauses, false)"
	 * except for the additional decision about remote versus local execution.
	 * Note however that we only strip the RestrictInfo nodes from the
	 * local_exprs list, since appendWhereClause expects a list of
	 * RestrictInfos.
	 */
	foreach(lc, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(IsA(rinfo, RestrictInfo));

		/* Ignore any pseudoconstants, they're dealt with elsewhere */
		if (rinfo->pseudoconstant)
			continue;

		if (list_member_ptr(fpinfo->remote_conds, rinfo))
		{
			remote_conds = lappend(remote_conds, rinfo);
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		}
		else if (list_member_ptr(fpinfo->local_conds, rinfo))
			local_exprs = lappend(local_exprs, rinfo->clause);
		else if (is_foreign_expr(root, baserel, rinfo->clause))
		{
			remote_conds = lappend(remote_conds, rinfo);
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		}
		else
			local_exprs = lappend(local_exprs, rinfo->clause);
	}
	
    sqlite_deparse_select(&sql, root, baserel, fpinfo->attrs_used, 
                          options->table, &retrieved_attrs);

	if (remote_conds)
		sqlite_append_where_clause(&sql, root, baserel, remote_conds,
						           true, &params_list);
	
    if (baserel->relid == root->parse->resultRelation &&
		(root->parse->commandType == CMD_UPDATE ||
		root->parse->commandType == CMD_DELETE))
			/* Relation is UPDATE/DELETE target, so use FOR UPDATE */
			appendStringInfoString(&sql, " FOR UPDATE");
	
    /*   The sql query and the attributes are salted away
     *   Will be used later in BeginForeignScan
     */
	fdw_private = list_make2(makeString(sql.data), retrieved_attrs);

	/*
	 * Create the ForeignScan node from target list, local filtering
	 * expressions, remote parameter expressions, and FDW private information.
	 *
	 * Note that the remote parameter expressions are stored in the fdw_exprs
	 * field of the finished plan node; we can't keep them in private state
	 * because then they wouldn't be subject to later planner processing.
	 */
	return make_foreignscan(tlist,
	                        local_exprs,
	                        scan_relid,
	                        params_list,
	                        fdw_private,
	                        NIL,
	                        NIL,
	                        outer_plan
	                       );
}


static void
sqliteBeginForeignScan(ForeignScanState *node,
                       int eflags)
{
	/*
	 * Begin executing a foreign scan. This is called during executor startup.
	 * It should perform any initialization needed before the scan can start,
	 * but not start executing the actual scan (that should be done upon the
	 * first call to IterateForeignScan). The ForeignScanState node has
	 * already been created, but its fdw_state field is still NULL.
	 * Information about the table to scan is accessible through the
	 * ForeignScanState node (in particular, from the underlying ForeignScan
	 * plan node, which contains any FDW-private information provided by
	 * GetForeignPlan). eflags contains flag bits describing the executor's
	 * operating mode for this plan node.
	 *
	 * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
	 * should not perform any externally-visible actions; it should only do
	 * the minimum required to make the node state valid for
	 * ExplainForeignScan and EndForeignScan.
	 *
	 */
	SQLiteFdwExecutionState  *festate;
    SqliteOpt                *opt;
	
	ForeignScan       *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState            *estate = node->ss.ps.state;

	elog(WARNING, "entering function %s",__func__);
	
    /*
	 * We'll save private state in node->fdw_state.
	 */
	festate = (SQLiteFdwExecutionState *) palloc0(sizeof(SQLiteFdwExecutionState));
	node->fdw_state = (void *) festate;

	/* Fetch options and then connect  */
	opt = sqliteGetOptions(RelationGetRelid(node->ss.ss_currentRelation));
	sqliteOpen(opt->database, &festate->db);
	
    festate->query = strVal(list_nth(fsplan->fdw_private, 0));
	festate->retrieved_attrs = list_nth(fsplan->fdw_private, 1);
	
    festate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "sqlite_fdw temporary data",
											  ALLOCSET_SMALL_MINSIZE,
											  ALLOCSET_SMALL_INITSIZE,
											  ALLOCSET_SMALL_MAXSIZE);
    festate->stmt = sqlitePrepare(festate->db, festate->query, NULL);

    /* 
     * Bind the parameters to the prepared statement 
    */
    if ( list_length(fsplan->fdw_exprs) > 0 )
        sqlite_bind_param_values(festate, fsplan->fdw_exprs, node);
}


static TupleTableSlot *
sqliteIterateForeignScan(ForeignScanState *node)
{
	/*
	 * Fetch one row from the foreign source, returning it in a tuple table
	 * slot (the node's ScanTupleSlot should be used for this purpose). Return
	 * NULL if no more rows are available. The tuple table slot infrastructure
	 * allows either a physical or virtual tuple to be returned; in most cases
	 * the latter choice is preferable from a performance standpoint. Note
	 * that this is called in a short-lived memory context that will be reset
	 * between invocations. Create a memory context in BeginForeignScan if you
	 * need longer-lived storage, or use the es_query_cxt of the node's
	 * EState.
	 *
	 * The rows returned must match the column signature of the foreign table
	 * being scanned. If you choose to optimize away fetching columns that are
	 * not needed, you should insert nulls in those column positions.
	 *
	 * Note that PostgreSQL's executor doesn't care whether the rows returned
	 * violate any NOT NULL constraints that were defined on the foreign table
	 * columns — but the planner does care, and may optimize queries
	 * incorrectly if NULL values are present in a column declared not to
	 * contain them. If a NULL value is encountered when the user has declared
	 * that none should be present, it may be appropriate to raise an error
	 * (just as you would need to do in the case of a data type mismatch).
	 */

	char        **values;
	HeapTuple   tuple;
	int         x;

	SQLiteFdwExecutionState *festate = (SQLiteFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	elog(WARNING,"entering function %s",__func__);

	ExecClearTuple(slot);

	/* get the next record, if any, and fill in the slot */
	if (sqlite3_step(festate->stmt) == SQLITE_ROW)
	{
		/* Build the tuple */
		values = (char **) palloc(sizeof(char *) * sqlite3_column_count(festate->stmt));

		for (x = 0; x < sqlite3_column_count(festate->stmt); x++)
		{
			values[x] = (char *) sqlite3_column_text(festate->stmt, x);
		}

		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att), values);
		ExecStoreTuple(tuple, slot, InvalidBuffer, false);
	}

	/* then return the slot */
	return slot;
}


static void
sqliteReScanForeignScan(ForeignScanState *node)
{
	/*
	 * Restart the scan from the beginning. Note that any parameters the scan
	 * depends on may have changed value, so the new scan does not necessarily
	 * return exactly the same rows.
	 */

	elog(DEBUG1,"entering function %s",__func__);

}


static void
sqliteEndForeignScan(ForeignScanState *node)
{
	cleanup_((SQLiteFdwExecutionState *) node->fdw_state);
}


#if (PG_VERSION_NUM >= 90300)
static void
sqliteAddForeignUpdateTargets(Query *parsetree,
								 RangeTblEntry *target_rte,
								 Relation target_relation)
{
	/*
	 * UPDATE and DELETE operations are performed against rows previously
	 * fetched by the table-scanning functions. The FDW may need extra
	 * information, such as a row ID or the values of primary-key columns, to
	 * ensure that it can identify the exact row to update or delete. To
	 * support that, this function can add extra hidden, or "junk", target
	 * columns to the list of columns that are to be retrieved from the
	 * foreign table during an UPDATE or DELETE.
	 *
	 * To do that, add TargetEntry items to parsetree->targetList, containing
	 * expressions for the extra values to be fetched. Each such entry must be
	 * marked resjunk = true, and must have a distinct resname that will
	 * identify it at execution time. Avoid using names matching ctidN or
	 * wholerowN, as the core system can generate junk columns of these names.
	 *
	 * This function is called in the rewriter, not the planner, so the
	 * information available is a bit different from that available to the
	 * planning routines. parsetree is the parse tree for the UPDATE or DELETE
	 * command, while target_rte and target_relation describe the target
	 * foreign table.
	 *
	 * If the AddForeignUpdateTargets pointer is set to NULL, no extra target
	 * expressions are added. (This will make it impossible to implement
	 * DELETE operations, though UPDATE may still be feasible if the FDW
	 * relies on an unchanging primary key to identify rows.)
	 */

	elog(DEBUG1,"entering function %s",__func__);

}


static List *
sqlitePlanForeignModify(PlannerInfo *root,
						   ModifyTable *plan,
						   Index resultRelation,
						   int subplan_index)
{
	/*
	 * Perform any additional planning actions needed for an insert, update,
	 * or delete on a foreign table. This function generates the FDW-private
	 * information that will be attached to the ModifyTable plan node that
	 * performs the update action. This private information must have the form
	 * of a List, and will be delivered to BeginForeignModify during the
	 * execution stage.
	 *
	 * root is the planner's global information about the query. plan is the
	 * ModifyTable plan node, which is complete except for the fdwPrivLists
	 * field. resultRelation identifies the target foreign table by its
	 * rangetable index. subplan_index identifies which target of the
	 * ModifyTable plan node this is, counting from zero; use this if you want
	 * to index into plan->plans or other substructure of the plan node.
	 *
	 * If the PlanForeignModify pointer is set to NULL, no additional
	 * plan-time actions are taken, and the fdw_private list delivered to
	 * BeginForeignModify will be NIL.
	 */

	elog(DEBUG1,"entering function %s",__func__);


	return NULL;
}


static void
sqliteBeginForeignModify(ModifyTableState *mtstate,
							ResultRelInfo *rinfo,
							List *fdw_private,
							int subplan_index,
							int eflags)
{
	/*
	 * Begin executing a foreign table modification operation. This routine is
	 * called during executor startup. It should perform any initialization
	 * needed prior to the actual table modifications. Subsequently,
	 * ExecForeignInsert, ExecForeignUpdate or ExecForeignDelete will be
	 * called for each tuple to be inserted, updated, or deleted.
	 *
	 * mtstate is the overall state of the ModifyTable plan node being
	 * executed; global data about the plan and execution state is available
	 * via this structure. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. (The ri_FdwState field of ResultRelInfo is
	 * available for the FDW to store any private state it needs for this
	 * operation.) fdw_private contains the private data generated by
	 * PlanForeignModify, if any. subplan_index identifies which target of the
	 * ModifyTable plan node this is. eflags contains flag bits describing the
	 * executor's operating mode for this plan node.
	 *
	 * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
	 * should not perform any externally-visible actions; it should only do
	 * the minimum required to make the node state valid for
	 * ExplainForeignModify and EndForeignModify.
	 *
	 * If the BeginForeignModify pointer is set to NULL, no action is taken
	 * during executor startup.
	 */

	elog(DEBUG1,"entering function %s",__func__);

}


static TupleTableSlot *
sqliteExecForeignInsert(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot)
{
	/*
	 * Insert one tuple into the foreign table. estate is global execution
	 * state for the query. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. slot contains the tuple to be inserted; it will
	 * match the rowtype definition of the foreign table. planSlot contains
	 * the tuple that was generated by the ModifyTable plan node's subplan; it
	 * differs from slot in possibly containing additional "junk" columns.
	 * (The planSlot is typically of little interest for INSERT cases, but is
	 * provided for completeness.)
	 *
	 * The return value is either a slot containing the data that was actually
	 * inserted (this might differ from the data supplied, for example as a
	 * result of trigger actions), or NULL if no row was actually inserted
	 * (again, typically as a result of triggers). The passed-in slot can be
	 * re-used for this purpose.
	 *
	 * The data in the returned slot is used only if the INSERT query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignInsert pointer is set to NULL, attempts to insert
	 * into the foreign table will fail with an error message.
	 *
	 */

	elog(DEBUG1,"entering function %s",__func__);

	return slot;
}


static TupleTableSlot *
sqliteExecForeignUpdate(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot)
{
	/*
	 * Update one tuple in the foreign table. estate is global execution state
	 * for the query. rinfo is the ResultRelInfo struct describing the target
	 * foreign table. slot contains the new data for the tuple; it will match
	 * the rowtype definition of the foreign table. planSlot contains the
	 * tuple that was generated by the ModifyTable plan node's subplan; it
	 * differs from slot in possibly containing additional "junk" columns. In
	 * particular, any junk columns that were requested by
	 * AddForeignUpdateTargets will be available from this slot.
	 *
	 * The return value is either a slot containing the row as it was actually
	 * updated (this might differ from the data supplied, for example as a
	 * result of trigger actions), or NULL if no row was actually updated
	 * (again, typically as a result of triggers). The passed-in slot can be
	 * re-used for this purpose.
	 *
	 * The data in the returned slot is used only if the UPDATE query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignUpdate pointer is set to NULL, attempts to update the
	 * foreign table will fail with an error message.
	 *
	 */

	elog(DEBUG1,"entering function %s",__func__);

	return slot;
}


static TupleTableSlot *
sqliteExecForeignDelete(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot)
{
	/*
	 * Delete one tuple from the foreign table. estate is global execution
	 * state for the query. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. slot contains nothing useful upon call, but can
	 * be used to hold the returned tuple. planSlot contains the tuple that
	 * was generated by the ModifyTable plan node's subplan; in particular, it
	 * will carry any junk columns that were requested by
	 * AddForeignUpdateTargets. The junk column(s) must be used to identify
	 * the tuple to be deleted.
	 *
	 * The return value is either a slot containing the row that was deleted,
	 * or NULL if no row was deleted (typically as a result of triggers). The
	 * passed-in slot can be used to hold the tuple to be returned.
	 *
	 * The data in the returned slot is used only if the DELETE query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignDelete pointer is set to NULL, attempts to delete
	 * from the foreign table will fail with an error message.
	 */

	elog(DEBUG1,"entering function %s",__func__);

	return slot;
}


static void
sqliteEndForeignModify(EState *estate,
						  ResultRelInfo *rinfo)
{
	/*
	 * End the table update and release resources. It is normally not
	 * important to release palloc'd memory, but for example open files and
	 * connections to remote servers should be cleaned up.
	 *
	 * If the EndForeignModify pointer is set to NULL, no action is taken
	 * during executor shutdown.
	 */

	elog(DEBUG1,"entering function %s",__func__);

}
#endif


static void
sqliteExplainForeignScan(ForeignScanState *node,
							struct ExplainState *es)
{
	/*
	 * Print additional EXPLAIN output for a foreign table scan. This function
	 * can call ExplainPropertyText and related functions to add fields to the
	 * EXPLAIN output. The flag fields in es can be used to determine what to
	 * print, and the state of the ForeignScanState node can be inspected to
	 * provide run-time statistics in the EXPLAIN ANALYZE case.
	 *
	 * If the ExplainForeignScan pointer is set to NULL, no additional
	 * information is printed during EXPLAIN.
	 */
	sqlite3					   *db;
	sqlite3_stmt			   *stmt;
	char					   *query;
	size_t						len;
	const char				   *pzTail;
	SQLiteFdwExecutionState	   *festate = (SQLiteFdwExecutionState *) node->fdw_state;
    SqliteOpt                  *opt;

	elog(DEBUG1,"entering function %s",__func__);

	/* Show the query (only if VERBOSE) */
	if (es->verbose)
	{
		/* show query */
		ExplainPropertyText("sqlite query", festate->query, es);
	}

	/* Fetch options  */
	opt = sqliteGetOptions(RelationGetRelid(node->ss.ss_currentRelation));

	/* Connect to the server */
	sqliteOpen(opt->database, &db);

	/* Build the query */
	len = strlen(festate->query) + 20;
	query = (char *)palloc(len);
	snprintf(query, len, "EXPLAIN QUERY PLAN %s", festate->query);

    /* Execute the query */
	stmt= sqlitePrepare(db, query, &pzTail);

	/* get the next record, if any, and fill in the slot */
	while (sqlite3_step(stmt) == SQLITE_ROW)
	{
		/*
		 * I don't keep the three first columns;
		   it could be a good idea to add that later
		 */
		/*
		 * for (x = 0; x < sqlite3_column_count(festate->stmt); x++)
		 * {
		 */
			ExplainPropertyText("sqlite plan", (char*)sqlite3_column_text(stmt, 3), es);
		/* } */
	}

	/* Free the query stmts */
	sqlite3_finalize(stmt);

	/* Close temporary connection */
	sqlite3_close(db);

}


static void
sqliteExplainForeignModify(ModifyTableState *mtstate,
							  ResultRelInfo *rinfo,
							  List *fdw_private,
							  int subplan_index,
							  struct ExplainState *es)
{
	/*
	 * Print additional EXPLAIN output for a foreign table update. This
	 * function can call ExplainPropertyText and related functions to add
	 * fields to the EXPLAIN output. The flag fields in es can be used to
	 * determine what to print, and the state of the ModifyTableState node can
	 * be inspected to provide run-time statistics in the EXPLAIN ANALYZE
	 * case. The first four arguments are the same as for BeginForeignModify.
	 *
	 * If the ExplainForeignModify pointer is set to NULL, no additional
	 * information is printed during EXPLAIN.
	 */

	elog(DEBUG1,"entering function %s",__func__);

}


static bool
sqliteAnalyzeForeignTable(Relation relation,
							 AcquireSampleRowsFunc *func,
							 BlockNumber *totalpages)
{
	/* ----
	 * This function is called when ANALYZE is executed on a foreign table. If
	 * the FDW can collect statistics for this foreign table, it should return
	 * true, and provide a pointer to a function that will collect sample rows
	 * from the table in func, plus the estimated size of the table in pages
	 * in totalpages. Otherwise, return false.
	 *
	 * If the FDW does not support collecting statistics for any tables, the
	 * AnalyzeForeignTable pointer can be set to NULL.
	 *
	 * If provided, the sample collection function must have the signature:
	 *
	 *	  int
	 *	  AcquireSampleRowsFunc (Relation relation, int elevel,
	 *							 HeapTuple *rows, int targrows,
	 *							 double *totalrows,
	 *							 double *totaldeadrows);
	 *
	 * A random sample of up to targrows rows should be collected from the
	 * table and stored into the caller-provided rows array. The actual number
	 * of rows collected must be returned. In addition, store estimates of the
	 * total numbers of live and dead rows in the table into the output
	 * parameters totalrows and totaldeadrows. (Set totaldeadrows to zero if
	 * the FDW does not have any concept of dead rows.)
	 * ----
	 */

	elog(DEBUG1,"entering function %s",__func__);

	return false;
}

static List *
sqliteImportForeignSchema(ImportForeignSchemaStmt *stmt,
							 Oid serverOid)
{
	sqlite3		   *volatile db = NULL;
	sqlite3_stmt   *volatile tbls = NULL;
	ForeignServer  *f_server;
	ListCell	   *lc;
	char		   *svr_database = NULL;
	StringInfoData	query_tbl;
	const char	   *pzTail;
	List		   *commands = NIL;
	bool			import_default = false;
	bool			import_not_null = true;

	elog(DEBUG1,"entering function %s",__func__);

	/*
	 * The only legit sqlite schema are temp and main (or name of an attached
	 * database, which can't happen here).  Authorize only legit "main" schema,
	 * and "public" just in case.
	 */
	if (strcmp(stmt->remote_schema, "public") != 0 &&
		strcmp(stmt->remote_schema, "main") != 0)
	{
		ereport(ERROR,
			(errcode(ERRCODE_FDW_SCHEMA_NOT_FOUND),
			errmsg("Foreign schema \"%s\" is invalid", stmt->remote_schema)
			));
	}

	/* Parse statement options */
	foreach(lc, stmt->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "import_default") == 0)
			import_default = defGetBoolean(def);
		else if (strcmp(def->defname, "import_not_null") == 0)
			import_not_null = defGetBoolean(def);
		else
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					  errmsg("invalid option \"%s\"", def->defname)));
	}

	/* get the db filename */
	f_server = GetForeignServerByName(stmt->server_name, false);
	foreach(lc, f_server->options)
	{
		DefElem *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "database") == 0)
		{
			svr_database = defGetString(def);
			break;
		}
	}

	Assert(svr_database);

	/* Connect to the server */
	sqliteOpen(svr_database, (sqlite3 **) &db);

	PG_TRY();
	{
		/* You want all tables, except system tables */
		initStringInfo(&query_tbl);
		appendStringInfo(&query_tbl, "SELECT name FROM sqlite_master WHERE type = 'table'");
		appendStringInfo(&query_tbl, " AND name NOT LIKE 'sqlite_%%'");

		/* Handle LIMIT TO / EXCEPT clauses in IMPORT FOREIGN SCHEMA statement */
		if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO ||
			stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
		{
			bool		first_item = true;

			appendStringInfoString(&query_tbl, " AND name ");
			if (stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
				appendStringInfoString(&query_tbl, "NOT ");
			appendStringInfoString(&query_tbl, "IN (");

			foreach(lc, stmt->table_list)
			{
				RangeVar *rv = (RangeVar *) lfirst(lc);

				if (first_item)
					first_item = false;
				else
					appendStringInfoString(&query_tbl, ", ");

				appendStringInfoString(&query_tbl, quote_literal_cstr(rv->relname));
			}
			appendStringInfoChar(&query_tbl, ')');
		}

		/* Iterate to all matching tables, and get their definition */
		tbls = sqlitePrepare(db, query_tbl.data, &pzTail);
		while (sqlite3_step(tbls) == SQLITE_ROW)
		{
			sqlite3_stmt   *cols;
			char		   *tbl_name;
			char		   *query_cols;
			StringInfoData	cft_stmt;
			int				i = 0;

			tbl_name = (char *) sqlite3_column_text(tbls, 0);

			/*
			 * user-defined list of tables has been handled in main query, don't
			 * try to do the job here again
			 */

			/* start building the CFT stmt */
			initStringInfo(&cft_stmt);
			appendStringInfo(&cft_stmt, "CREATE FOREIGN TABLE %s.%s (\n",
					stmt->local_schema, quote_identifier(tbl_name));

			query_cols = palloc0(strlen(tbl_name) + 19 + 1);
			sprintf(query_cols, "PRAGMA table_info(%s)", tbl_name);

			cols = sqlitePrepare(db, query_cols, &pzTail);
			while (sqlite3_step(cols) == SQLITE_ROW)
			{
				char   *col_name;
				char   *typ_name;
				bool	not_null;
				char   *default_val;

				col_name = (char *) sqlite3_column_text(cols, 1);
				typ_name = (char *) sqlite3_column_text(cols, 2);
				not_null = (sqlite3_column_int(cols, 3) == 1);
				default_val = (char *) sqlite3_column_text(cols, 4);

				if (i != 0)
					appendStringInfo(&cft_stmt, ",\n");

				/* table name */
				appendStringInfo(&cft_stmt, "%s ",
						quote_identifier(col_name));

				/* translated datatype */
				sqliteTranslateType(&cft_stmt, typ_name);

				if (not_null && import_not_null)
					appendStringInfo(&cft_stmt, " NOT NULL");

				if (default_val && import_default)
					appendStringInfo(&cft_stmt, " DEFAULT %s", default_val);

				i++;
			}
			appendStringInfo(&cft_stmt, "\n) SERVER %s\n"
					"OPTIONS (table '%s')",
					quote_identifier(stmt->server_name),
					quote_identifier(tbl_name));

			commands = lappend(commands,
					pstrdup(cft_stmt.data));

			/* free per-table allocated data */
			pfree(query_cols);
			pfree(cft_stmt.data);
		}

		/* Free all needed data and close connection*/
		pfree(query_tbl.data);
	}
	PG_CATCH();
	{
		if (tbls)
			sqlite3_finalize(tbls);
		if (db)
			sqlite3_close(db);

		PG_RE_THROW();
	}
	PG_END_TRY();

	sqlite3_finalize(tbls);
	sqlite3_close(db);

	return commands;
}


static bool
file_exists(const char *name)
{
	struct stat st;

	AssertArg(name != NULL);

	if (stat(name, &st) == 0)
		return S_ISDIR(st.st_mode) ? false : true;
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
		ereport(ERROR,
                (errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", name)));

	return false;
}

/*
 * Translate sqlite type name to postgres compatible one and append it to
 * given StringInfo
 */
static void
sqliteTranslateType(StringInfo str, char *typname)
{
	char *type;

	/*
	 * get lowercase typname. We use C collation since the original type name
	 * should not contain exotic character.
	 */
	type = str_tolower(typname, strlen(typname), C_COLLATION_OID);

	/* try some easy conversion, from https://www.sqlite.org/datatype3.html */
	if (strcmp(type, "tinyint") == 0)
		appendStringInfoString(str, "smallint");

	else if (strcmp(type, "mediumint") == 0)
		appendStringInfoString(str, "integer");

	else if (strcmp(type, "unsigned big int") == 0)
		appendStringInfoString(str, "bigint");

	else if (strcmp(type, "double") == 0)
		appendStringInfoString(str, "double precision");

	else if (strcmp(type, "datetime") == 0)
		appendStringInfoString(str, "timestamp");

	else if (strcmp(type, "nvarchar text") == 0)
		appendStringInfoString(str, "text");

	else if (strcmp(type, "longvarchar") == 0)
	     appendStringInfoString(str, "text");

	else if (strncmp(type, "text", 4) == 0)
	     appendStringInfoString(str, "text");

	else if (strcmp(type, "blob") == 0)
	     appendStringInfoString(str, "bytea");

	else if (strcmp(type, "integer") == 0)
	     /* Type "integer" appears dynamically sized between 1 and 8
	      * bytes.  Need to assume worst case. */
	     appendStringInfoString(str, "bigint");

	/* XXX try harder handling sqlite datatype */

	/* if original type is compatible, return lowercase value */
	else
		appendStringInfoString(str, type);

	pfree(type);
}


void
sqlite_bind_param_values(SQLiteFdwExecutionState *festate,
                         List *fdw_exprs, 
                         ForeignScanState *node)
{
	ListCell   *lc;
    Oid  *param_types;
	List *param_exprs;
    int i;
    MemoryContext oldcontext;

    param_exprs = (List *) ExecInitExpr((Expr *) fdw_exprs, (PlanState *)node);
    param_types = (Oid *) palloc0(sizeof(Oid) * list_length(fdw_exprs));
    
    i = 0;
    foreach(lc, fdw_exprs)
		param_types[i++] = exprType((Node *) lfirst(lc));

    oldcontext = MemoryContextSwitchTo(node->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

    i = 0;
    foreach(lc, param_exprs)
	{
		ExprState  *expr_state = (ExprState *) lfirst(lc);
		Datum		expr_value;
		bool		isNull;

		/* Evaluate the parameter expression */
		expr_value = ExecEvalExpr(expr_state, node->ss.ps.ps_ExprContext, &isNull);
        sqlite_bind_param_value(festate, i+1, param_types[i], expr_value, isNull);
        i++;
    }
    oldcontext = MemoryContextSwitchTo(oldcontext);
}


void
sqlite_bind_param_value(SQLiteFdwExecutionState *festate,
                        int index, 
                        Oid ptype, 
                        Datum pval, 
                        bool isNull)
{
    int rc;
    if ( isNull ) 
        rc = sqlite3_bind_null(festate->stmt, index);
    else
        switch(ptype)
        {
            case INT2OID:
                rc = sqlite3_bind_int(festate->stmt, index, DatumGetInt16(pval));
                break;
            
            case INT4OID:
                rc = sqlite3_bind_int(festate->stmt, index, DatumGetInt32(pval));
                break;
            
            case INT8OID:
                rc = sqlite3_bind_int64(festate->stmt, index, DatumGetInt64(pval));
                break;

            default:
                rc = SQLITE_OK;
                break;
        }

    if ( rc != SQLITE_OK ) {
        char const * errmsg_from_sqlite3 = pstrdup(sqlite3_errmsg(festate->db));
        cleanup_(festate);
        ereport(ERROR,
            (errcode(ERRCODE_FDW_ERROR),
            errmsg("error while trying to bind param \"%s\"", errmsg_from_sqlite3)
            ));
    }
}


void
cleanup_(SQLiteFdwExecutionState *festate)
{
    if ( festate->stmt ) {
        sqlite3_finalize(festate->stmt);
        festate->stmt = NULL;
    }
    if ( festate->db ) {
        sqlite3_close(festate->db);
        festate->stmt = NULL;
    }
    if ( festate->query ) {
        pfree(festate->query);
        festate->query = NULL;
    }
}
