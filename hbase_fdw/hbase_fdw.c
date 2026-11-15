/*
 * hbase_fdw.c
 *    Foreign data wrapper that reads HBase tables through the HBase Thrift1
 *    gateway. Week 1 scope: read-only full scans with column projection.
 *
 *  CREATE SERVER hb FOREIGN DATA WRAPPER hbase_fdw
 *      OPTIONS (host 'localhost', port '9090');
 *  CREATE FOREIGN TABLE emp (
 *      id     text OPTIONS (rowkey 'true'),
 *      name   text OPTIONS (column 'info:name'),
 *      salary int  OPTIONS (column 'pay:salary')
 *  ) SERVER hb OPTIONS (table 'emp');
 *
 * Assumptions (as with file_fdw): the user declares the schema; HBase values
 * are bytes holding the text form of the value (what `hbase shell` put
 * stores), converted with the column type's input function. bytea columns
 * get the raw bytes. A missing cell is NULL. We read the latest version only.
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"

#include "hbase_thrift.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(hbase_fdw_handler);
PG_FUNCTION_INFO_V1(hbase_fdw_validator);

#define DEF_PORT	9090
#define DEF_TMO		30			/* seconds */
#define DEF_BATCH	1000
#define DEF_ROWS	1000

/*
 * Planner costs, in seq_page_cost units, calibrated on a local standalone
 * HBase (Phase 6 timings): a Thrift round trip took ~0.65 ms, a scanned row
 * ~1 us, so a round trip is worth ~700 rows. HB_RPC_COST is set a bit above
 * that (15 vs 0.015 per row) so the planner switches from a nested loop of
 * gets to a hash join over a full scan where the measured switch is (~25
 * outer rows on 10k rows): core's own hash-join CPU costs are not wall-clock
 * calibrated.
 */
#define HB_CONNECT_COST	10.0
#define HB_RPC_COST		15.0
#define HB_ROW_COST		0.005

/*
 * Measured costs (hbase_fdw.adaptive_costs = on): a path is costed as the
 * time it should take on this connection, in ms, times cost_per_ms:
 *   connect + rpcs * rtt + bytes / bandwidth + rows * row_us
 * rtt and connect are timed once per session per server (best of 3 pings);
 * bandwidth and bytes per row are running totals from real scans.
 */
static bool adaptive = true;
static double force_rtt = -1;	/* ms, -1 = measure */
static double force_bw = -1;	/* Mbit/s, -1 = measure */
static double cpm = 20.0;		/* cost units per ms */
static double row_us = 1.0;		/* client time per row, microseconds */

/* batched join (Part C) */
static bool batch_join = true;
static int	join_batch = 500;	/* outer rows per batch, K */

typedef struct Opt
{
	const char *name;
	Oid			cat;
} Opt;

static const Opt opts[] = {
	{"host", ForeignServerRelationId},
	{"port", ForeignServerRelationId},
	{"timeout", ForeignServerRelationId},
	{"table", ForeignTableRelationId},
	{"family", ForeignTableRelationId},
	{"batch_size", ForeignTableRelationId},
	{"insert_batch", ForeignTableRelationId},
	{"rows", ForeignTableRelationId},
	{"rowkey", AttributeRelationId},
	{"column", AttributeRelationId},
	{NULL, InvalidOid}
};

typedef struct HbOpts
{
	char	   *host;
	int			port;
	int			tmo;
	char	   *table;
	char	   *family;			/* default family for unmapped columns */
	int			batch;
	int			ibatch;			/* rows per mutateRows call on INSERT */
	double		rows;
} HbOpts;

/* what the planner works out for one foreign rel, in rel->fdw_private */
typedef struct HbRelInfo
{
	HbOpts		o;
	Bitmapset  *used;			/* attnos needed: output + local quals */
	AttrNumber	katt;			/* rowkey column, InvalidAttrNumber if none */
} HbRelInfo;

/* one fetched attribute */
typedef struct Col
{
	AttrNumber	att;			/* 1-based */
	char	   *spec;			/* "cf:q", NULL for the row key */
	int			len;
	Oid			typ;
	FmgrInfo	in;
	Oid			ioparam;
	int32		typmod;
} Col;

/* key conditions besides the btree strategies <, <=, =, >=, > */
#define KC_PREFIX	(BTMaxStrategyNumber + 1)	/* starts_with(key, value) */
#define KC_LIKE		(BTMaxStrategyNumber + 2)	/* key LIKE value */
#define KC_IN		(BTMaxStrategyNumber + 3)	/* key = ANY (array value) */

/* a row-key condition pushed to HBase: rowkey <op> value */
typedef struct KeyCond
{
	ExprState  *x;				/* the value, evaluated at scan open */
	int			op;				/* btree strategy, KC_PREFIX or KC_LIKE */
	bool		raw;			/* text/bytea: key is the datum's bytes */
	bool		text;			/* the value is text (LIKE: multibyte chars) */
	FmgrInfo	out;			/* otherwise: its output function */
	/* KC_IN: the value is an array of these; raw/text/out refer to them */
	Oid			elem;
	int16		elen;
	bool		ebyval;
	char		ealign;
} KeyCond;

typedef struct HbState
{
	HbOpts		o;
	Col		   *cols;
	int			ncols;
	char	  **hcols;			/* distinct HBase columns we ask for */
	int			nh;
	bool		keyonly;
	bool		byqual;		/* qualifier-level projection */
	int			nq;			/* needed non-key columns */

	KeyCond    *kc;
	int			nk;
	char	   *kname;			/* rowkey column name, for EXPLAIN */
	MemoryContext kcxt;			/* holds the current bounds */
	char	   *start,			/* [start, stop) of the last scan, NULL = open */
			   *stop;
	int			startlen,
				stoplen;
	bool		get;			/* = or IN: fetch keys[] instead of scanning */
	HbKey	   *keys;			/* sorted, distinct, inside [start, stop) */
	int			nkeys,
				kpos;			/* next key to fetch */

	HbConn	   *conn;
	int32		sid;
	bool		begun;			/* bounds set for this (re)scan */
	bool		open;			/* HBase scanner open */
	bool		eof;

	MemoryContext bcxt;			/* holds the current batch */
	HbRow	   *rows;
	int			n,
				pos;
	long		nrpc;			/* scannerGetList + getRowsWithColumns calls */
	long		nrows;			/* rows fetched from HBase */
	double		ncell;			/* and their cells, the key counted as one */
	int			wi;				/* slot in the weight arrays, -1 = none yet */

	/* for error context */
	HbRow	   *cur;
	Col		   *ccol;
	Relation	rel;
} HbState;

/* ---------- measured weights (Part B) ---------- */

/* per server: fixed arrays, searched in order; there are only a few */
#define MAXSRV 16
static int	nsrv = 0;
static Oid	w_srv[MAXSRV];
static double w_rtt[MAXSRV];	/* ms: best ping, the bare network round trip */
static double w_call[MAXSRV];	/* ms: best 1-key get, round trip + HBase work */
static double w_key[MAXSRV];	/* ms: HBase work per extra key in a get */
static double w_conn[MAXSRV];	/* ms */
static double w_bytes[MAXSRV];	/* bytes in big replies ... */
static double w_ms[MAXSRV];		/* ... and their time beyond one call */

/* per foreign table: bytes per cell (the row key counts as a cell) */
#define MAXREL 64
static int	nrel = 0;
static Oid	r_rel[MAXREL];
static double r_bytes[MAXREL];
static double r_cells[MAXREL];

/* slot of a server already measured, or -1 */
static int
srv_slot(Oid srv)
{
	int			i;

	for (i = 0; i < nsrv; i++)
		if (w_srv[i] == srv)
			return i;
	return -1;
}

/* cells in a batch of rows, the key counted as one */
static double
ncells(HbRow *r, int n)
{
	double		t = 0;
	int			k;

	for (k = 0; k < n; k++)
		t += r[k].ncells + 1;
	return t;
}

static void rel_seen(Oid relid, double bytes, double cells);
static double srv_bw(int i);

/*
 * Slot of a server, measuring it the first time in this session:
 *  - best of 3 pings = rtt (reported; bare network)
 *  - best of 2 one-key gets = the cost of a call (what every RPC really pays)
 *  - a 100-key get: the extra time per key beyond the first
 *  - the 2nd batch of up to 1000 whole rows = bandwidth and bytes per cell
 *    (the 1st batch also pays for HBase warming up its scanner)
 * About 8 round trips, once per session.
 */
static int
find_srv(Oid srv, HbOpts *o, Oid relid)
{
	int			i = srv_slot(srv),
				k,
				n,
				m;
	HbConn	   *c;
	HbScanSpec	sp = {0};
	HbRow	   *r1,
			   *r2;
	HbKey		key,
			   *ks;
	int32		id;
	double		in,
				ms,
				cl;
	MemoryContext cx,
				old;

	if (i >= 0)
		return i;
	if (nsrv == MAXSRV)
		return 0;				/* out of slots: share the first */
	cx = AllocSetContextCreate(CurrentMemoryContext, "hbase_fdw probe",
							   ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cx);
	c = hb_connect(o->host, o->port, o->tmo * 1000);
	i = nsrv++;
	w_srv[i] = srv;
	w_conn[i] = hb_conn_ms(c);
	w_rtt[i] = w_call[i] = 1e9;
	w_key[i] = w_bytes[i] = w_ms[i] = 0;
	for (k = 0; k < 3; k++)
	{
		hb_ping(c, o->table);
		w_rtt[i] = Min(w_rtt[i], hb_last_ms(c));
	}

	sp.caching = 1000;
	id = hb_scan_open(c, o->table, &sp);
	n = hb_scan_next(c, id, 1000, &r1);
	in = hb_last_in(c);
	ms = hb_last_ms(c);
	cl = ncells(r1, n);
	m = n == 1000 ? hb_scan_next(c, id, 1000, &r2) : 0;
	if (m > 0)
	{
		in = hb_last_in(c);
		ms = hb_last_ms(c);
		cl = ncells(r2, m);
	}
	hb_scan_close(c, id);

	if (n > 0)
	{
		key.p = r1[0].key;
		key.len = r1[0].keylen;
		for (k = 0; k < 2; k++)
		{
			hb_get_rows(c, o->table, &key, 1, NULL, 0, &r2);
			w_call[i] = Min(w_call[i], hb_last_ms(c));
		}
		rel_seen(relid, in, cl);
	}
	else
		w_call[i] = w_rtt[i];
	if (in > 4096 && ms > w_call[i])
	{
		w_bytes[i] = in;
		w_ms[i] = ms - w_call[i];
	}
	if (n >= 100 && r1[0].ncells > 0)
	{
		double		t = 1e9;
		HbCell	   *x = &r1[0].cells[0];
		char	   *colon = memchr(x->col, ':', x->collen);
		char	   *none = psprintf("%.*s:hbase_fdw_none",
									(int) (colon ? colon - x->col : x->collen), x->col);

		ks = palloc(sizeof(HbKey) * 100);
		for (k = 0; k < 100; k++)
		{
			ks[k].p = r1[k].key;
			ks[k].len = r1[k].keylen;
		}
		/*
		 * Ask for a column no row has: HBase still looks up every key, but
		 * the reply is empty, so this times the per-key work alone (a big
		 * reply can stall on TCP delayed ACKs and say nothing about keys).
		 */
		for (k = 0; k < 2; k++)
		{
			hb_get_rows(c, o->table, ks, 100, &none, 1, &r2);
			t = Min(t, hb_last_ms(c) - w_call[i]);
		}
		w_key[i] = Max(0, t / 99);
	}
	hb_disconnect(c);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);	/* also closes the socket, if not yet */
	return i;
}

/* ms one RPC costs; with a forced rtt, that rtt plus HBase's own work */
static double
srv_call(int i)
{
	if (force_rtt >= 0)
		return force_rtt + Max(0, w_call[i] - w_rtt[i]);
	return w_call[i];
}

/* bytes per ms; until a big reply was seen, assume 1 Gbit/s */
static double
srv_bw(int i)
{
	if (force_bw > 0)
		return force_bw * 1000.0 / 8.0;
	if (w_ms[i] <= 0)
		return 125000.0;
	return w_bytes[i] / w_ms[i];
}

static int
rel_slot(Oid relid)
{
	int			i;

	for (i = 0; i < nrel; i++)
		if (r_rel[i] == relid)
			return i;
	return -1;
}

/* bytes per cell; 30 until a scan has been seen */
static double
rel_bpc(Oid relid)
{
	int			i = rel_slot(relid);

	if (i < 0 || r_cells[i] <= 0)
		return 30;
	return r_bytes[i] / r_cells[i];
}

static void
rel_seen(Oid relid, double bytes, double cells)
{
	int			i = rel_slot(relid);

	if (cells <= 0)
		return;
	if (i < 0)
	{
		if (nrel == MAXREL)
			return;
		i = nrel++;
		r_rel[i] = relid;
		r_bytes[i] = r_cells[i] = 0;
	}
	r_bytes[i] += bytes;
	r_cells[i] += cells;
}

/* ---------- options ---------- */

static int
int_opt(DefElem *d, int lo)
{
	char	   *s = defGetString(d),
			   *e;
	long		v;

	errno = 0;
	v = strtol(s, &e, 10);
	if (errno || *e || e == s || v < lo || v > INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
				 errmsg("invalid value for option \"%s\": \"%s\"", d->defname, s),
				 errhint("Expected an integer >= %d.", lo)));
	return (int) v;
}

static void
check_spec(const char *s)
{
	const char *c = strchr(s, ':');

	if (!c || c == s || c[1] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
				 errmsg("invalid HBase column \"%s\"", s),
				 errhint("Use \"family:qualifier\".")));
}

Datum
hbase_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *l = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			cat = PG_GETARG_OID(1);
	ListCell   *lc;

	foreach(lc, l)
	{
		DefElem    *d = lfirst(lc);
		const Opt  *o;

		for (o = opts; o->name; o++)
			if (o->cat == cat && strcmp(o->name, d->defname) == 0)
				break;
		if (!o->name)
		{
			StringInfoData b;

			initStringInfo(&b);
			for (o = opts; o->name; o++)
				if (o->cat == cat)
					appendStringInfo(&b, "%s%s", b.len ? ", " : "", o->name);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", d->defname),
					 b.len ? errhint("Valid options in this context are: %s", b.data)
					 : errhint("There are no valid options in this context.")));
		}

		if (strcmp(d->defname, "port") == 0)
		{
			if (int_opt(d, 1) > 65535)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
						 errmsg("port must be between 1 and 65535")));
		}
		else if (strcmp(d->defname, "timeout") == 0)
			int_opt(d, 0);
		else if (strcmp(d->defname, "batch_size") == 0 ||
				 strcmp(d->defname, "insert_batch") == 0 ||
				 strcmp(d->defname, "rows") == 0)
			int_opt(d, 1);
		else if (strcmp(d->defname, "rowkey") == 0)
			(void) defGetBoolean(d);
		else if (strcmp(d->defname, "column") == 0)
			check_spec(defGetString(d));
		else if (strcmp(d->defname, "family") == 0 &&
				 (strchr(defGetString(d), ':') || !*defGetString(d)))
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid HBase family \"%s\"", defGetString(d))));
	}
	PG_RETURN_VOID();
}

static void
get_opts(Oid relid, HbOpts *o)
{
	ForeignTable *ft = GetForeignTable(relid);
	ForeignServer *fs = GetForeignServer(ft->serverid);
	List	   *l = list_concat(list_copy(fs->options), ft->options);
	ListCell   *lc;

	memset(o, 0, sizeof(*o));
	o->host = "localhost";
	o->port = DEF_PORT;
	o->tmo = DEF_TMO;
	o->batch = DEF_BATCH;
	o->ibatch = 100;
	o->rows = DEF_ROWS;

	foreach(lc, l)
	{
		DefElem    *d = lfirst(lc);

		if (strcmp(d->defname, "host") == 0)
			o->host = defGetString(d);
		else if (strcmp(d->defname, "port") == 0)
			o->port = int_opt(d, 1);
		else if (strcmp(d->defname, "timeout") == 0)
			o->tmo = int_opt(d, 0);
		else if (strcmp(d->defname, "table") == 0)
			o->table = defGetString(d);
		else if (strcmp(d->defname, "family") == 0)
			o->family = defGetString(d);
		else if (strcmp(d->defname, "batch_size") == 0)
			o->batch = int_opt(d, 1);
		else if (strcmp(d->defname, "insert_batch") == 0)
			o->ibatch = int_opt(d, 1);
		else if (strcmp(d->defname, "rows") == 0)
			o->rows = int_opt(d, 1);
	}
	if (!o->table)
		o->table = get_rel_name(relid);
}

/*
 * HBase column for attribute att: NULL means it is the row key.
 * Unmapped columns fall back to "<family>:<column name>".
 */
static char *
att_spec(Oid relid, AttrNumber att, const char *colname, const HbOpts *o)
{
	ListCell   *lc;
	char	   *spec = NULL;

	foreach(lc, GetForeignColumnOptions(relid, att))
	{
		DefElem    *d = lfirst(lc);

		if (strcmp(d->defname, "rowkey") == 0 && defGetBoolean(d))
			return NULL;
		if (strcmp(d->defname, "column") == 0)
			spec = defGetString(d);
	}
	if (spec)
		return spec;
	if (o->family)
		return psprintf("%s:%s", o->family, colname);
	ereport(ERROR,
			(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
			 errmsg("column \"%s\" of foreign table \"%s\" has no HBase mapping",
					colname, get_rel_name(relid)),
			 errhint("Set OPTIONS (column 'family:qualifier') or (rowkey 'true') on it, "
					 "or a default \"family\" on the table.")));
	return NULL;				/* keep compiler quiet */
}

/* ---------- planning ---------- */

/* attno of the rowkey column, InvalidAttrNumber if there is none */
static AttrNumber
rowkey_att(Oid relid)
{
	Relation	r = table_open(relid, NoLock);
	TupleDesc	td = RelationGetDescr(r);
	AttrNumber	k = InvalidAttrNumber;
	int			i;

	for (i = 0; i < td->natts && k == InvalidAttrNumber; i++)
	{
		ListCell   *lc;

		if (TupleDescAttr(td, i)->attisdropped)
			continue;
		foreach(lc, GetForeignColumnOptions(relid, i + 1))
		{
			DefElem    *d = lfirst(lc);

			if (strcmp(d->defname, "rowkey") == 0 && defGetBoolean(d))
				k = i + 1;
		}
	}
	table_close(r, NoLock);
	return k;
}

/* is n (under binary-compatible casts like varchar -> text) the rowkey? */
static bool
is_key_var(Node *n, Index varno, AttrNumber katt)
{
	while (n && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n && IsA(n, Var) &&
		((Var *) n)->varno == varno &&
		((Var *) n)->varattno == katt &&
		((Var *) n)->varlevelsup == 0;
}

/* does collation coll sort like memcmp? */
static bool
coll_is_c(Oid coll)
{
	if (!OidIsValid(coll))
		return false;
#if PG_VERSION_NUM >= 180000
	return pg_newlocale_from_collation(coll)->collate_is_c;
#else
	return lc_collate_is_c(coll);
#endif
}

/*
 * How HBase can answer "rowkey fn value" (keyleft) or "value fn rowkey":
 * returns a btree strategy, KC_PREFIX, KC_LIKE, or 0 for "it can't". HBase
 * orders row keys as unsigned bytes. Equality, LIKE and prefixes agree with
 * that under any deterministic collation (a case-insensitive ICU collation
 * calls 'A' = 'a'). <, <=, >, >= agree only under a C collation, for bytea,
 * and for the text_pattern_ops operators (~<~ etc.), which use memcmp.
 * Integer keys get = only: their text form does not sort numerically
 * ("10" < "9"). For = we assume canonical text ("42", not "042" or "+42").
 */
static int
key_strategy(Oid fn, Oid coll, bool keyleft)
{
	int			s;

	switch (fn)
	{
		case F_TEXT_LT:
		case F_TEXT_LE:
		case F_TEXT_GE:
		case F_TEXT_GT:
			if (!coll_is_c(coll))
				return 0;
			break;
		case F_TEXTEQ:
		case F_TEXTLIKE:
		case F_STARTS_WITH:
			if (OidIsValid(coll) && !get_collation_isdeterministic(coll))
				return 0;
			break;
	}

	switch (fn)
	{
		case F_TEXTEQ:
		case F_BYTEAEQ:
		case F_INT2EQ:
		case F_INT4EQ:
		case F_INT8EQ:
		case F_INT24EQ:
		case F_INT42EQ:
		case F_INT28EQ:
		case F_INT82EQ:
		case F_INT48EQ:
		case F_INT84EQ:
			return BTEqualStrategyNumber;
		case F_TEXT_LT:
		case F_BYTEALT:
		case F_TEXT_PATTERN_LT:
			s = BTLessStrategyNumber;
			break;
		case F_TEXT_LE:
		case F_BYTEALE:
		case F_TEXT_PATTERN_LE:
			s = BTLessEqualStrategyNumber;
			break;
		case F_TEXT_GE:
		case F_BYTEAGE:
		case F_TEXT_PATTERN_GE:
			s = BTGreaterEqualStrategyNumber;
			break;
		case F_TEXT_GT:
		case F_BYTEAGT:
		case F_TEXT_PATTERN_GT:
			s = BTGreaterStrategyNumber;
			break;
		case F_TEXTLIKE:
		case F_BYTEALIKE:
			/* "value LIKE key" constrains nothing */
			return keyleft ? KC_LIKE : 0;
		case F_STARTS_WITH:
			return keyleft ? KC_PREFIX : 0;
		default:
			return 0;
	}
	/* "value < key" is "key > value" */
	return keyleft ? s : BTMaxStrategyNumber + 1 - s;
}

/*
 * The fixed prefix of LIKE pattern p: the bytes before the first unescaped
 * % or _ (backslash is the escape; LIKE ... ESCAPE arrives rewritten to it).
 * Copied to *out; *exact says the pattern has no wildcard at all.
 */
static int
like_prefix(const char *p, int len, bool text, char *out, bool *exact)
{
	int			n = 0,
				i = 0;

	while (i < len && p[i] != '%' && p[i] != '_')
	{
		int			l;

		if (p[i] == '\\' && ++i == len)
			break;				/* trailing escape: LIKE itself will complain */
		l = text ? pg_mblen(p + i) : 1;
		l = Min(l, len - i);
		memcpy(out + n, p + i, l);
		n += l;
		i += l;
	}
	*exact = (i == len);
	return n;
}

/*
 * Is n a value fixed for one scan of rel? No Vars of rel itself, nothing
 * volatile, no SubPlans. Vars of other rels are fine: they come from the
 * outer side of a nested loop (a join clause on a parameterized path, or a
 * LATERAL reference) and become nestloop Params in fdw_exprs, set before
 * every rescan.
 */
static bool
scan_const(Node *n, Index relid)
{
	List	   *vars;
	ListCell   *lc;

	if (contain_volatile_functions(n) || contain_subplans(n))
		return false;
	vars = pull_var_clause(n, PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = lfirst(lc);

		/* a PlaceHolderVar may be computed at our level: keep it simple */
		if (!IsA(v, Var) || (v->varno == relid && v->varlevelsup == 0))
			return false;
	}
	return true;
}

/*
 * Can HBase answer ri from its row-key order? rowkey <op> value for =, <,
 * <=, >, >= (so BETWEEN too), LIKE, starts_with/^@ and IN / = ANY (array),
 * where value is anything fixed for one scan (see scan_const). Sets *val
 * and the strategy *op.
 */
static bool
key_cond(RestrictInfo *ri, Index relid, AttrNumber katt, Node **val, int *op)
{
	Node	   *c = (Node *) ri->clause;
	List	   *args;
	Oid			fn,
				coll;
	bool		keyleft;

	if (ri->pseudoconstant)
		return false;
	if (IsA(c, ScalarArrayOpExpr))
	{
		/* k IN (...), k = ANY (array): a set of point lookups */
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) c;

		*val = lsecond(sa->args);
		*op = KC_IN;
		return sa->useOr && is_key_var(linitial(sa->args), relid, katt) &&
			key_strategy(get_opcode(sa->opno), sa->inputcollid, true) ==
			BTEqualStrategyNumber && scan_const(*val, relid);
	}
	if (IsA(c, OpExpr))
	{
		args = ((OpExpr *) c)->args;
		fn = get_opcode(((OpExpr *) c)->opno);
		coll = ((OpExpr *) c)->inputcollid;
	}
	else if (IsA(c, FuncExpr))
	{
		/* starts_with(k, 'x') and friends, written as functions */
		args = ((FuncExpr *) c)->args;
		fn = ((FuncExpr *) c)->funcid;
		coll = ((FuncExpr *) c)->inputcollid;
	}
	else
		return false;
	if (list_length(args) != 2)
		return false;
	if ((keyleft = is_key_var(linitial(args), relid, katt)))
		*val = lsecond(args);
	else if (is_key_var(lsecond(args), relid, katt))
		*val = linitial(args);
	else
		return false;
	if (!(*op = key_strategy(fn, coll, keyleft)) || !scan_const(*val, relid))
		return false;

	/* a constant pattern without a fixed prefix narrows nothing */
	if (*op == KC_LIKE && IsA(*val, Const) && !((Const *) *val)->constisnull)
	{
		struct varlena *v = PG_DETOAST_DATUM_PACKED(((Const *) *val)->constvalue);
		bool		exact;
		char	   *buf = palloc(VARSIZE_ANY_EXHDR(v) + 1);

		if (like_prefix(VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v),
						exprType(*val) != BYTEAOID, buf, &exact) == 0 && !exact)
			return false;
	}
	return true;
}

/*
 * Pick the quals HBase can answer (see key_cond). The values go to *exprs,
 * which setrefs keeps up to date for us (and turns outer Vars into nestloop
 * Params), and the matching strategies to *ops. Every qual is still
 * rechecked locally, so pushing one down can only save work, never change
 * the result. The RestrictInfos pushed go to *ris (if not NULL).
 */
static void
key_conds(RelOptInfo *rel, AttrNumber katt, List *clauses, List **exprs,
		  List **ops, List **ris)
{
	ListCell   *lc;

	if (katt == InvalidAttrNumber)
		return;
	foreach(lc, clauses)
	{
		Node	   *val;
		int			op;

		if (!key_cond(lfirst(lc), rel->relid, katt, &val, &op))
			continue;
		*exprs = lappend(*exprs, val);
		*ops = lappend_int(*ops, op);
		if (ris)
			*ris = lappend(*ris, lfirst(lc));
	}
}

/*
 * Estimate a scan with quals clauses. HBase sends what the pushed key
 * conditions select: for = / IN at most one row per key (row keys are
 * unique), for ranges and prefixes a share of the table, guessed by the
 * usual selectivity functions (there are no statistics, so these are
 * defaults). The quals that stay local then filter that. The "rows" option
 * is the table size. Returns the output rows; *fetched is what HBase sends,
 * *get whether it is fetched by key.
 */
static double
estimate(PlannerInfo *root, RelOptInfo *rel, HbRelInfo *ri, List *clauses,
		 double *fetched, bool *get)
{
	List	   *kexprs = NIL,
			   *kops = NIL,
			   *kris = NIL;
	ListCell   *lc,
			   *lo;
	double		nkeys = -1;		/* size of the key set, -1 = none */

	key_conds(rel, ri->katt, clauses, &kexprs, &kops, &kris);
	forboth(lc, kexprs, lo, kops)
	{
		double		n;

		if (lfirst_int(lo) == BTEqualStrategyNumber)
			n = 1;
		else if (lfirst_int(lo) == KC_IN)
#if PG_VERSION_NUM >= 170000
			n = estimate_array_length(root, lfirst(lc));
#else
			n = estimate_array_length(lfirst(lc));
#endif
		else
			continue;
		nkeys = nkeys < 0 ? n : Min(nkeys, n);	/* key sets intersect */
	}

	*get = (nkeys >= 0);
	if (*get)
		*fetched = Min(nkeys, ri->o.rows);
	else
		*fetched = ri->o.rows *
			clauselist_selectivity(root, kris, rel->relid, JOIN_INNER, NULL);
	*fetched = clamp_row_est(*fetched);

	return clamp_row_est(*fetched *
						 clauselist_selectivity(root,
												list_difference_ptr(clauses, kris),
												rel->relid, JOIN_INNER, NULL));
}

static void
hbGetForeignRelSize(PlannerInfo *root, RelOptInfo *rel, Oid relid)
{
	HbRelInfo  *ri = palloc0(sizeof(HbRelInfo));
	ListCell   *lc;
	double		fetched;
	bool		get;

	get_opts(relid, &ri->o);
	rel->fdw_private = ri;
	rel->tuples = ri->o.rows;

	/* columns the query needs: output + local quals */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &ri->used);
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *r = lfirst(lc);

		pull_varattnos((Node *) r->clause, rel->relid, &ri->used);
	}

	ri->katt = rowkey_att(relid);
	rel->rows = estimate(root, rel, ri, rel->baserestrictinfo, &fetched, &get);
}

/*
 * HBase returns rows in row-key byte order (a scan does; a get asks for its
 * keys sorted). Postgres can use that for ORDER BY or a merge join when its
 * own order is the same: bytea, or text/varchar under a C collation. Returns
 * NIL otherwise, or if no query clause cares about this order.
 */
static List *
key_pathkeys(PlannerInfo *root, RelOptInfo *rel, Oid relid, AttrNumber katt)
{
	Oid			typ,
				coll;
	int32		typmod;
	Expr	   *v;

	if (katt == InvalidAttrNumber)
		return NIL;
	get_atttypetypmodcoll(relid, katt, &typ, &typmod, &coll);
	if (typ != BYTEAOID &&
		!((typ == TEXTOID || typ == VARCHAROID) && coll_is_c(coll)))
		return NIL;
	v = (Expr *) makeVar(rel->relid, katt, typ, typmod, coll, 0);
	return build_expression_pathkey(root, v,
									lookup_type_cache(typ, TYPECACHE_LT_OPR)->lt_opr,
									rel->relids, false);
}

/*
 * Add a path with quals clauses, parameterized by required_outer (NULL: a
 * plain path). Cost: the first round trip comes before the first row (and
 * connecting, once per plan node: a rescan keeps the connection, so a
 * parameterized path, which is there to be rescanned, doesn't charge it);
 * then one round trip per batch (a scan also opens, ends on an empty batch
 * and closes; a get fetches batch_size keys per call), shipping each row,
 * and the local quals on each row.
 */
static void
add_hb_path(PlannerInfo *root, RelOptInfo *rel, Oid relid, List *clauses,
			Relids required_outer)
{
	HbRelInfo  *ri = rel->fdw_private;
	double		rows,
				fetched,
				rpcs;
	bool		get;
	QualCost	qc;
	Cost		su,
				tot;
	List	   *pk = key_pathkeys(root, rel, relid, ri->katt);
	ForeignPath *p;

	rows = estimate(root, rel, ri, clauses, &fetched, &get);
	rpcs = ceil(fetched / ri->o.batch) + (get ? 0 : 3);

	/* all quals are rechecked locally, the pushed ones too */
	cost_qual_eval(&qc, extract_actual_clauses(clauses, false), root);
	if (adaptive)
	{
		int			i = find_srv(GetForeignTable(relid)->serverid, &ri->o, relid);
		/* bytes per row = cells fetched (key + used columns) * bytes per cell */
		double		bpr = Max(1, bms_num_members(ri->used)) * rel_bpc(relid),
					msu,
					mtot;

		msu = (required_outer ? 0 : w_conn[i]) + srv_call(i);
		mtot = msu + (rpcs - 1) * srv_call(i) + fetched * bpr / srv_bw(i) +
			fetched * row_us / 1000.0;
		if (get)				/* keys beyond one per call */
			mtot += Max(0, fetched - rpcs) * w_key[i];
		su = msu * cpm + qc.startup;
		tot = mtot * cpm + qc.startup + fetched * (cpu_tuple_cost + qc.per_tuple);
	}
	else
	{
		su = (required_outer ? 0 : HB_CONNECT_COST) + HB_RPC_COST + qc.startup;
		tot = su + (rpcs - 1) * HB_RPC_COST +
			fetched * (cpu_tuple_cost + HB_ROW_COST + qc.per_tuple);
	}

#if PG_VERSION_NUM >= 180000
	p = create_foreignscan_path(root, rel, NULL, rows, 0, su, tot, pk,
								required_outer, NULL, NIL, NIL);
#elif PG_VERSION_NUM >= 170000
	p = create_foreignscan_path(root, rel, NULL, rows, su, tot, pk,
								required_outer, NULL, NIL, NIL);
#else
	p = create_foreignscan_path(root, rel, NULL, rows, su, tot, pk,
								required_outer, NULL, NIL);
#endif
	add_path(rel, (Path *) p);
}

/* generate_implied_equalities_for_column callback: our rowkey? */
static bool
ec_is_key(PlannerInfo *root, RelOptInfo *rel, EquivalenceClass *ec,
		  EquivalenceMember *em, void *arg)
{
	return is_key_var((Node *) em->em_expr, rel->relid, *(AttrNumber *) arg);
}

/*
 * The plain path, and, like an index on the row key, a parameterized path
 * for every set of outer rels a join clause "rowkey op outer value" needs:
 * as the inner side of a nested loop it looks up each outer row's key.
 */
static void
hbGetForeignPaths(PlannerInfo *root, RelOptInfo *rel, Oid relid)
{
	HbRelInfo  *ri = rel->fdw_private;
	List	   *cand = NIL,
			   *ppis = NIL;
	ListCell   *lc;

	/* a LATERAL reference makes even the plain path parameterized */
	add_hb_path(root, rel, relid, rel->baserestrictinfo, rel->lateral_relids);

	if (ri->katt == InvalidAttrNumber)
		return;

	/* join clauses: in joininfo, or (k = l.x) only in an EquivalenceClass */
	foreach(lc, rel->joininfo)
		cand = lappend(cand, lfirst(lc));
	if (rel->has_eclass_joins)
		cand = list_concat(cand,
						   generate_implied_equalities_for_column(root, rel,
																  ec_is_key, &ri->katt,
																  rel->lateral_referencers));
	foreach(lc, cand)
	{
		RestrictInfo *r = lfirst(lc);
		Relids		outer;
		Node	   *val;
		int			op;

		if (!join_clause_is_movable_to(r, rel) ||
			!key_cond(r, rel->relid, ri->katt, &val, &op))
			continue;
		outer = bms_del_member(bms_union(r->clause_relids, rel->lateral_relids),
							   rel->relid);
		if (bms_is_empty(outer))
			continue;
		/* get_baserel_parampathinfo makes no duplicates: pointers compare */
		ppis = list_append_unique_ptr(ppis,
									  get_baserel_parampathinfo(root, rel, outer));
	}

	/* the path's clauses: our own plus every join clause movable here */
	foreach(lc, ppis)
	{
		ParamPathInfo *ppi = lfirst(lc);

		add_hb_path(root, rel, relid,
					list_concat_copy(rel->baserestrictinfo, ppi->ppi_clauses),
					ppi->ppi_req_outer);
	}
}

static ForeignScan *
hbGetForeignPlan(PlannerInfo *root, RelOptInfo *rel, Oid relid,
				 ForeignPath *best, List *tlist, List *clauses, Plan *outer)
{
	HbRelInfo  *ri = rel->fdw_private;
	Bitmapset  *used = ri->used;
	List	   *atts = NIL,
			   *kexprs = NIL,
			   *kops = NIL;
	char	   *kname;
	int			k = -1;

	/* row-key conditions (with join clauses, if parameterized) go to HBase */
	key_conds(rel, ri->katt, clauses, &kexprs, &kops, NULL);
	kname = ri->katt == InvalidAttrNumber ? "" :
		get_attname(relid, ri->katt, false);

	/* every qual is still checked locally by the executor */
	clauses = extract_actual_clauses(clauses, false);

	/* attnos needed (0 = whole row, i.e. everything) */
	while ((k = bms_next_member(used, k)) >= 0)
	{
		AttrNumber	a = k + FirstLowInvalidHeapAttributeNumber;

		if (a >= 0)
			atts = lappend_int(atts, a);
	}

	return make_foreignscan(tlist, clauses, rel->relid, kexprs,
							list_make3(atts, kops, makeString(kname)), NIL, NIL, outer);
}

/* ---------- execution ---------- */

static void
add_h(HbState *st, char *s)
{
	int			j;

	for (j = 0; j < st->nh; j++)
		if (strcmp(st->hcols[j], s) == 0)
			return;
	st->hcols[st->nh++] = s;
}

/*
 * Decide what to fetch. HBase silently drops rows that have no cell in the
 * requested columns, so projecting to just the needed qualifiers would make
 * "SELECT name" return fewer rows than "SELECT count(*)". A row belongs to
 * the foreign table iff it has a cell in one of the mapped families, so by
 * default we ask for those whole families and project here. If the user
 * declared some column NOT NULL (i.e. promised every row has that cell), we
 * can ask for the needed qualifiers plus that one and still see every row.
 */
static void
setup_cols(HbState *st, Relation rel, List *atts)
{
	TupleDesc	td = RelationGetDescr(rel);
	Oid			relid = RelationGetRelid(rel);
	bool		all = list_member_int(atts, 0);
	char	  **spec = palloc0(sizeof(char *) * td->natts);
	char	   *nn = NULL;
	List	   *fams = NIL;
	ListCell   *lc;
	int			i,
				nkey = 0;

	for (i = 0; i < td->natts; i++)
	{
		Form_pg_attribute a = TupleDescAttr(td, i);
		char	   *f;

		if (a->attisdropped)
			continue;
		spec[i] = att_spec(relid, i + 1, NameStr(a->attname), &st->o);
		if (!spec[i])
		{
			nkey++;
			continue;
		}
		if (a->attnotnull && !nn)
			nn = spec[i];
		f = pnstrdup(spec[i], strchr(spec[i], ':') - spec[i]);
		if (!list_member(fams, makeString(f)))
			fams = lappend(fams, makeString(f));
	}
	if (nkey > 1)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
				 errmsg("foreign table \"%s\" has more than one rowkey column",
						RelationGetRelationName(rel))));

	st->cols = palloc0(sizeof(Col) * td->natts);
	st->hcols = palloc0(sizeof(char *) * (td->natts + 1));
	for (i = 0; i < td->natts; i++)
	{
		Form_pg_attribute a = TupleDescAttr(td, i);
		Col		   *c;
		Oid			fn;

		if (a->attisdropped || (!all && !list_member_int(atts, i + 1)))
			continue;
		c = &st->cols[st->ncols++];
		c->att = i + 1;
		c->spec = spec[i];
		c->len = c->spec ? strlen(c->spec) : 0;
		c->typ = a->atttypid;
		c->typmod = a->atttypmod;
		getTypeInputInfo(c->typ, &fn, &c->ioparam);
		fmgr_info(fn, &c->in);
		if (c->spec)
			st->nq++;
	}
	/* nothing but (maybe) the row key needed, e.g. count(*) */
	st->keyonly = (st->nq == 0);

	if (nn)
	{
		for (i = 0; i < st->ncols; i++)
			if (st->cols[i].spec)
				add_h(st, st->cols[i].spec);
		add_h(st, nn);
		st->byqual = true;
	}
	else
		foreach(lc, fams)
			add_h(st, strVal(lfirst(lc)));
}

static void
hbBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;
	Relation	rel = node->ss.ss_currentRelation;
	HbState  *st = palloc0(sizeof(HbState));

	List	   *kops = lsecond(fs->fdw_private);
	ListCell   *lc;
	int			i = 0;

	node->fdw_state = st;
	st->rel = rel;
	get_opts(RelationGetRelid(rel), &st->o);
	setup_cols(st, rel, (List *) linitial(fs->fdw_private));
	st->kname = strVal(lthird(fs->fdw_private));

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	st->nk = list_length(fs->fdw_exprs);
	st->kc = palloc0(sizeof(KeyCond) * Max(st->nk, 1));
	foreach(lc, fs->fdw_exprs)
	{
		KeyCond    *k = &st->kc[i];
		Oid			typ = exprType(lfirst(lc)),
					fn;
		bool		varlena;

		k->x = ExecInitExpr(lfirst(lc), (PlanState *) node);
		k->op = list_nth_int(kops, i++);
		if (k->op == KC_IN)
		{
			typ = k->elem = get_element_type(typ);
			get_typlenbyvalalign(typ, &k->elen, &k->ebyval, &k->ealign);
		}
		k->raw = (typ == TEXTOID || typ == VARCHAROID || typ == BYTEAOID);
		k->text = (typ != BYTEAOID);
		getTypeOutputInfo(typ, &fn, &varlena);
		fmgr_info(fn, &k->out);
	}
	st->kcxt = AllocSetContextCreate(node->ss.ps.state->es_query_cxt,
									 "hbase_fdw bounds", ALLOCSET_SMALL_SIZES);
	st->bcxt = AllocSetContextCreate(node->ss.ps.state->es_query_cxt,
									 "hbase_fdw batch", ALLOCSET_DEFAULT_SIZES);
	/* socket is closed by a reset callback on this (query) context */
	st->conn = hb_connect(st->o.host, st->o.port, st->o.tmo * 1000);
	st->wi = srv_slot(GetForeignTable(RelationGetRelid(rel))->serverid);
}

static void
conv_ctx(void *arg)
{
	HbState  *st = arg;
	TupleDesc	td = RelationGetDescr(st->rel);

	if (st->cur && st->ccol)
		errcontext("HBase table \"%s\", row key \"%.*s\", column \"%s\"",
				   st->o.table, Min(st->cur->keylen, 64), st->cur->key,
				   NameStr(TupleDescAttr(td, st->ccol->att - 1)->attname));
}

static Datum
conv(Col *c, char *p, int len)
{
	if (c->typ == BYTEAOID)
	{
		bytea	   *b = palloc(VARHDRSZ + len);

		SET_VARSIZE(b, VARHDRSZ + len);
		memcpy(VARDATA(b), p, len);
		return PointerGetDatum(b);
	}
	/* text input functions want a clean, NUL terminated string */
	if (memchr(p, '\0', len))
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
				 errmsg("HBase value contains a zero byte"),
				 errhint("Declare the column as bytea to read binary data.")));
	pg_verify_mbstr(GetDatabaseEncoding(), p, len, false);
	return InputFunctionCall(&c->in, p, c->ioparam, c->typmod);
}

static void
fill(HbState *st, TupleTableSlot *slot, HbRow *r)
{
	Datum	   *v = slot->tts_values;
	bool	   *nl = slot->tts_isnull;
	ErrorContextCallback cb;
	int			i,
				j;

	memset(nl, true, sizeof(bool) * slot->tts_tupleDescriptor->natts);

	cb.callback = conv_ctx;
	cb.arg = st;
	cb.previous = error_context_stack;
	error_context_stack = &cb;
	st->cur = r;

	for (i = 0; i < st->ncols; i++)
	{
		Col		   *c = &st->cols[i];
		int			a = c->att - 1;

		st->ccol = c;
		if (!c->spec)
		{
			v[a] = conv(c, r->key, r->keylen);
			nl[a] = false;
			continue;
		}
		for (j = 0; j < r->ncells; j++)
		{
			HbCell	   *x = &r->cells[j];

			if (x->collen == c->len && memcmp(x->col, c->spec, c->len) == 0)
			{
				v[a] = conv(c, x->val ? x->val : "", x->val ? x->vallen : 0);
				nl[a] = false;
				break;
			}
		}
	}
	error_context_stack = cb.previous;
	st->cur = NULL;
	st->ccol = NULL;
}

/* unsigned byte order, as HBase compares row keys */
static int
kcmp(const char *a, int alen, const char *b, int blen)
{
	int			c = Min(alen, blen) ? memcmp(a, b, Min(alen, blen)) : 0;

	return c ? c : (alen > blen) - (alen < blen);
}

/* copy of k[0..len) in cxt, plus a \0 byte so k\0 is at hand too */
static char *
kcopy(MemoryContext cxt, const char *k, int len)
{
	char	   *r = MemoryContextAllocZero(cxt, len + 1);

	memcpy(r, k, len);
	return r;
}

/*
 * Narrow the scan to keys >= k (> k if next: the smallest key after k is
 * k\0). An empty k bounds nothing, as HBase has no empty keys.
 */
static void
raise_start(HbState *st, const char *k, int len, bool next)
{
	int			n = len + (next ? 1 : 0);
	char	   *c = kcopy(st->kcxt, k, len);

	if (n > 0 && (!st->start || kcmp(c, n, st->start, st->startlen) > 0))
	{
		st->start = c;
		st->startlen = n;
	}
}

/* narrow the scan to keys < k (<= k if next); false if that leaves none */
static bool
lower_stop(HbState *st, const char *k, int len, bool next)
{
	int			n = len + (next ? 1 : 0);
	char	   *c = kcopy(st->kcxt, k, len);

	if (n == 0)
		return false;			/* key < '': there is no such key */
	if (!st->stop || kcmp(c, n, st->stop, st->stoplen) < 0)
	{
		st->stop = c;
		st->stoplen = n;
	}
	return true;
}

/*
 * Keys starting with p: [p, p+1) where p+1 is p with its last byte bumped,
 * after dropping trailing 0xff bytes (they have no successor). All 0xff (or
 * empty): no upper bound.
 */
static void
prefix_range(HbState *st, const char *p, int len)
{
	char	   *q = kcopy(st->kcxt, p, len);
	int			n = len;

	raise_start(st, p, len, false);
	while (n > 0 && (unsigned char) q[n - 1] == 0xff)
		n--;
	if (n == 0)
		return;
	q[n - 1]++;
	lower_stop(st, q, n, false);
}

/* the row-key bytes for value d of condition k (in the current context) */
static void
key_bytes(KeyCond *k, Datum d, char **p, int *len)
{
	if (k->raw)
	{
		struct varlena *v = PG_DETOAST_DATUM_PACKED(d);

		*p = VARDATA_ANY(v);
		*len = VARSIZE_ANY_EXHDR(v);
	}
	else
	{
		*p = OutputFunctionCall(&k->out, d);
		*len = strlen(*p);
	}
}

static int
hbkey_cmp(const void *a, const void *b)
{
	const HbKey *x = a,
			   *y = b;

	return kcmp(x->p, x->len, y->p, y->len);
}

/* sort and dedupe keys[0..n) in place; returns the new count */
static int
sort_keys(HbKey *keys, int n)
{
	int			i,
				m = 0;

	if (n > 1)
		qsort(keys, n, sizeof(HbKey), hbkey_cmp);
	for (i = 0; i < n; i++)
		if (m == 0 || hbkey_cmp(&keys[m - 1], &keys[i]) != 0)
			keys[m++] = keys[i];
	return m;
}

/* keep the keys of sorted a that are also in sorted b; returns the count */
static int
intersect_keys(HbKey *a, int na, const HbKey *b, int nb)
{
	int			i = 0,
				j = 0,
				m = 0;

	while (i < na && j < nb)
	{
		int			c = hbkey_cmp(&a[i], &b[j]);

		if (c == 0)
			a[m++] = a[i];
		i += (c <= 0);
		j += (c >= 0);
	}
	return m;
}

/*
 * Evaluate the key conditions. = and IN give a set of keys (intersected if
 * there are several), everything else the scan range [start, stop) (the
 * intersection of all of them). With a key set the scan fetches just those
 * keys that fall inside the range. Returns false if no row can match: a
 * NULL value (our operators are strict), no non-empty key left (HBase has
 * no empty keys) or an empty range. Done at every scan open, so a rescan
 * picks up new parameter values.
 */
static bool
set_bounds(ForeignScanState *node, HbState *st)
{
	ExprContext *ec = node->ss.ps.ps_ExprContext;
	HbKey	   *set = NULL;
	int			nset = -1,		/* -1 = no key set */
				i,
				j;

	MemoryContextReset(st->kcxt);
	st->start = st->stop = NULL;
	st->startlen = st->stoplen = 0;
	st->get = false;
	st->keys = NULL;
	st->nkeys = st->kpos = 0;

	for (i = 0; i < st->nk; i++)
	{
		KeyCond    *k = &st->kc[i];
		MemoryContext old = MemoryContextSwitchTo(ec->ecxt_per_tuple_memory);
		bool		isnull,
					exact = false;
		Datum		d = ExecEvalExpr(k->x, ec, &isnull);
		char	   *p = NULL;
		int			len = 0;
		HbKey	   *ks = NULL;
		int			nks = 0;

		if (!isnull && k->op == KC_IN)
		{
			Datum	   *ev;
			bool	   *en;
			int			ne;

			deconstruct_array(DatumGetArrayTypeP(d), k->elem, k->elen,
							  k->ebyval, k->ealign, &ev, &en, &ne);
			ks = MemoryContextAlloc(st->kcxt, sizeof(HbKey) * Max(ne, 1));
			for (j = 0; j < ne; j++)
			{
				if (en[j])
					continue;	/* k = NULL is never true */
				key_bytes(k, ev[j], &p, &len);
				if (len == 0)
					continue;	/* no such key */
				ks[nks].p = kcopy(st->kcxt, p, len);
				ks[nks++].len = len;
			}
		}
		else if (!isnull)
			key_bytes(k, d, &p, &len);
		if (!isnull && k->op == KC_LIKE)
		{
			char	   *pat = p;

			p = palloc(len + 1);
			len = like_prefix(pat, len, k->text, p, &exact);
		}
		MemoryContextSwitchTo(old);

		if (isnull)
			return false;
		switch (k->op)
		{
			case KC_LIKE:
				if (!exact)
				{
					prefix_range(st, p, len);
					break;
				}
				/* no wildcard: LIKE 'abc' is = 'abc' */
				/* FALLTHROUGH */
			case BTEqualStrategyNumber:
				if (len == 0)
					return false;
				ks = MemoryContextAlloc(st->kcxt, sizeof(HbKey));
				ks->p = kcopy(st->kcxt, p, len);
				ks->len = len;
				nks = 1;
				/* FALLTHROUGH */
			case KC_IN:
				nks = sort_keys(ks, nks);
				if (nset < 0)
				{
					set = ks;
					nset = nks;
				}
				else
					nset = intersect_keys(set, nset, ks, nks);
				if (nset == 0)
					return false;
				break;
			case KC_PREFIX:
				prefix_range(st, p, len);
				break;
			case BTGreaterEqualStrategyNumber:
				raise_start(st, p, len, false);
				break;
			case BTGreaterStrategyNumber:
				raise_start(st, p, len, true);
				break;
			case BTLessStrategyNumber:
				if (!lower_stop(st, p, len, false))
					return false;
				break;
			case BTLessEqualStrategyNumber:
				lower_stop(st, p, len, true);
				break;
			default:
				elog(ERROR, "hbase_fdw: unknown key condition %d", k->op);
		}
	}
	if (st->start && st->stop &&
		kcmp(st->start, st->startlen, st->stop, st->stoplen) >= 0)
		return false;
	if (nset < 0)
		return true;

	/* a key set: keep the keys inside the range */
	for (i = j = 0; i < nset; i++)
		if ((!st->start ||
			 kcmp(set[i].p, set[i].len, st->start, st->startlen) >= 0) &&
			(!st->stop ||
			 kcmp(set[i].p, set[i].len, st->stop, st->stoplen) < 0))
			set[j++] = set[i];
	st->get = true;
	st->keys = set;
	st->nkeys = j;
	return j > 0;
}

static TupleTableSlot *
hbIterateForeignScan(ForeignScanState *node)
{
	HbState  *st = node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	ExecClearTuple(slot);
	if (!st->begun)
	{
		st->begun = true;
		if (!set_bounds(node, st))
		{
			st->eof = true;		/* no row can match: skip HBase entirely */
			return slot;
		}
	}
	/* a key set is fetched with hb_get_rows below, a range needs a scanner */
	if (!st->get && !st->open && !st->eof)
	{
		HbScanSpec s = {0};

		s.start = st->start;
		s.startlen = st->startlen;
		s.stop = st->stop;
		s.stoplen = st->stoplen;
		s.ncols = st->nh;
		s.cols = st->hcols;
		s.caching = st->o.batch;
		s.keyonly = st->keyonly;
		st->sid = hb_scan_open(st->conn, st->o.table, &s);
		st->open = true;
	}

	while (st->pos >= st->n)
	{
		MemoryContext old;

		if (st->eof)
			return slot;
		MemoryContextReset(st->bcxt);
		old = MemoryContextSwitchTo(st->bcxt);
		if (st->get)
		{
			/* batch_size keys per round trip; missing keys return nothing */
			int			m = Min(st->o.batch, st->nkeys - st->kpos);

			st->n = hb_get_rows(st->conn, st->o.table, st->keys + st->kpos, m,
								st->hcols, st->nh, &st->rows);
			st->kpos += m;
			st->eof = (st->kpos >= st->nkeys);
		}
		else
			st->n = hb_scan_next(st->conn, st->sid, st->o.batch, &st->rows);
		MemoryContextSwitchTo(old);
		st->pos = 0;
		st->nrpc++;
		st->nrows += st->n;
		st->ncell += ncells(st->rows, st->n);

		/* a big reply of whole rows tells bandwidth apart from latency */
		if (st->wi >= 0 && !st->keyonly && hb_last_in(st->conn) > 4096 &&
			hb_last_ms(st->conn) > w_call[st->wi])
		{
			w_bytes[st->wi] += hb_last_in(st->conn);
			w_ms[st->wi] += hb_last_ms(st->conn) - w_call[st->wi];
		}
		if (st->n == 0 && st->open)
		{
			st->eof = true;
			hb_scan_close(st->conn, st->sid);
			st->open = false;
		}
	}

	fill(st, slot, &st->rows[st->pos++]);
	return ExecStoreVirtualTuple(slot);
}

static void
hbReScanForeignScan(ForeignScanState *node)
{
	HbState  *st = node->fdw_state;

	if (st->open)
		hb_scan_close(st->conn, st->sid);
	st->begun = st->open = st->eof = false;
	st->n = st->pos = 0;
	MemoryContextReset(st->bcxt);
}

static void
hbEndForeignScan(ForeignScanState *node)
{
	HbState  *st = node->fdw_state;

	if (!st || !st->conn)
		return;
	if (st->open)
		hb_scan_close(st->conn, st->sid);
	st->open = false;
	/* bytes per cell, from scans big enough to tell (key-only rows are tiny) */
	if (st->nrows >= 100 && !st->keyonly)
		rel_seen(RelationGetRelid(st->rel), hb_bytes_in(st->conn), st->ncell);
	hb_disconnect(st->conn);
}

/* row key for EXPLAIN: printable ASCII as is, other bytes as \xNN */
static char *
show_key(const char *k, int len)
{
	StringInfoData b;
	int			i;

	initStringInfo(&b);
	for (i = 0; i < len; i++)
	{
		unsigned char c = k[i];

		if (c == '\\')
			appendStringInfoString(&b, "\\\\");
		else if (c >= 0x20 && c < 0x7f)
			appendStringInfoChar(&b, c);
		else
			appendStringInfo(&b, "\\x%02x", c);
	}
	return b.data;
}

static void
hbExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	HbState  *st = node->fdw_state;
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;

	ExplainPropertyText("HBase Table", st->o.table, es);

	/*
	 * Deparse the values here, not in the planner: a Param needs the plan's
	 * namespaces to print (and to not crash). The pushed quals are always in
	 * plan->qual too, so EXPLAIN has just set es->deparse_cxt to this node
	 * while printing "Filter".
	 */
	if (fs->fdw_exprs)
	{
		static const char *const opname[] = {
			NULL, "<", "<=", "=", ">=", ">", "^@", "~~", "= ANY"
		};
		List	   *kops = lsecond(fs->fdw_private);
		StringInfoData b;
		ListCell   *lc;
		int			i = 0;

		initStringInfo(&b);
		foreach(lc, fs->fdw_exprs)
		{
			int			op = list_nth_int(kops, i);

			Assert(op >= BTLessStrategyNumber && op <= KC_IN);
			appendStringInfo(&b, op == KC_IN ? "%s(%s %s (%s))" : "%s(%s %s %s)",
							 i++ ? " AND " : "",
							 quote_identifier(st->kname), opname[op],
							 deparse_expression(lfirst(lc), es->deparse_cxt,
												false, false));
		}
		ExplainPropertyText("HBase Key Cond", b.data, es);
	}
	/* what the last scan read, known once it has run */
	if (es->analyze && st->get)
		ExplainPropertyInteger("HBase Get Keys", NULL, st->nkeys, es);
	if (es->analyze && !st->get && st->start)
		ExplainPropertyText("HBase Start Row", show_key(st->start, st->startlen), es);
	if (es->analyze && !st->get && st->stop)
		ExplainPropertyText("HBase Stop Row", show_key(st->stop, st->stoplen), es);
	if (es->verbose)
	{
		List	   *l = NIL;
		int			i;

		for (i = 0; i < st->nh; i++)
			l = lappend(l, st->hcols[i]);
		ExplainPropertyList(st->byqual ? "HBase Columns" : "HBase Families", l, es);
		if (st->keyonly)
			ExplainPropertyText("HBase Filter", "FirstKeyOnlyFilter() AND KeyOnlyFilter()", es);
		ExplainPropertyInteger("Batch Size", NULL, st->o.batch, es);
	}
	if (es->analyze)
		ExplainPropertyInteger("HBase RPCs", NULL, st->nrpc, es);
	/* the weights the planner costed with (only with measured costs) */
	if (adaptive && (es->verbose || es->analyze))
	{
		Oid			relid = RelationGetRelid(st->rel);
		int			i = srv_slot(GetForeignTable(relid)->serverid);

		if (i >= 0)
		{
			char		b[192];

			snprintf(b, sizeof(b), "rtt %.2f ms, call %.2f ms, +%.3f ms/key, bandwidth %.0f Mbit/s, %.0f bytes/cell",
					 force_rtt >= 0 ? force_rtt : w_rtt[i], srv_call(i), w_key[i],
					 srv_bw(i) * 8 / 1000.0, rel_bpc(relid));
			ExplainPropertyText("Network", b, es);
		}
		if (es->analyze && st->conn)
			ExplainPropertyInteger("Bytes Received", NULL, hb_bytes_in(st->conn), es);
	}
}

/* ---------- batched index join (Part C) ---------- */

/*
 * For  ... JOIN hbase_table h ON h.rowkey = outer.col  (inner join, plain
 * column), a join node that pulls K outer rows, fetches all their keys from
 * HBase in one getRowsWithColumns call, then emits the outer rows in their
 * own order joined with their HBase row. Round trips: outer rows / K
 * instead of one per outer row.
 *
 * Plan shape: CustomScan with scanrelid 0 (a join), the outer plan as its
 * only child, custom_scan_tlist = every Var the output and quals use. Its
 * scan slot is built in that order: inner Vars from HBase, outer Vars from
 * the outer tuple (opos = their position in the outer plan's output).
 */
static set_join_pathlist_hook_type prev_join_hook = NULL;
static CustomPathMethods bj_path_methods;
static CustomScanMethods bj_scan_methods;
static CustomExecMethods bj_exec_methods;

typedef struct BjState
{
	CustomScanState css;
	HbState		hb;				/* the HBase side, set up like a foreign scan */
	Relation	rel;			/* the HBase table */
	TupleTableSlot *inslot;		/* one HBase row, in the table's columns */
	int			nflat;
	int		   *opos;			/* per scan-slot column: outer position, 0 = HBase */
	int		   *iatt;			/* ... and its HBase table attno */
	int			kpos;			/* outer position of the key column */
	int			K;
	KeyCond		kk;				/* how to turn an outer key into bytes */

	TupleTableSlot **outs;		/* buffered outer rows */
	HbKey	   *okeys;			/* their keys, p = NULL for NULL / empty */
	int			nout,
				k;				/* next buffered row to emit */
	HbRow	   *rows;			/* HBase rows for this batch */
	int			nrows;
	bool		done;			/* outer side finished */
	MemoryContext bcxt;			/* keys and rows of one batch */
	long		nbatch,
				nrpc;
} BjState;

/* cost of the HBase side of a batched join with n outer rows */
static void
bj_cost(RelOptInfo *inner, Oid relid, double n, double out, Cost *su, Cost *tot)
{
	HbRelInfo  *ri = inner->fdw_private;
	double		b = ceil(n / join_batch);	/* batches */
	double		rpcs = b * ceil((double) join_batch / ri->o.batch);

	rpcs = Max(1, Min(rpcs, b * ceil(n / b / ri->o.batch)));
	out = Min(out, n);			/* a key lookup returns at most one row per key */
	if (adaptive)
	{
		int			i = find_srv(GetForeignTable(relid)->serverid, &ri->o, relid);
		double		bpr = Max(1, bms_num_members(ri->used)) * rel_bpc(relid);

		*su = (w_conn[i] + srv_call(i)) * cpm;
		*tot = (w_conn[i] + rpcs * srv_call(i) + Max(0, n - rpcs) * w_key[i] +
				out * bpr / srv_bw(i) + out * row_us / 1000.0) * cpm;
	}
	else
	{
		*su = HB_CONNECT_COST + HB_RPC_COST;
		*tot = HB_CONNECT_COST + rpcs * HB_RPC_COST + out * HB_ROW_COST;
	}
}

static void
bj_hook(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
		RelOptInfo *innerrel, JoinType jt, JoinPathExtraData *extra)
{
	HbRelInfo  *ri;
	Path	   *op = outerrel->cheapest_total_path;
	CustomPath *cp;
	Var		   *ov = NULL;
	ListCell   *lc;
	Oid			relid;
	Cost		su,
				tot;

	if (prev_join_hook)
		prev_join_hook(root, joinrel, outerrel, innerrel, jt, extra);
	if (!batch_join || jt != JOIN_INNER)
		return;
	if (innerrel->reloptkind != RELOPT_BASEREL || innerrel->fdwroutine == NULL ||
		innerrel->fdwroutine->GetForeignPaths != hbGetForeignPaths)
		return;					/* inner side is not one of our tables */
	ri = innerrel->fdw_private;
	if (ri == NULL || ri->katt == InvalidAttrNumber ||
		!bms_is_empty(innerrel->lateral_relids))
		return;
	if (op == NULL || op->param_info != NULL)
		return;

	/* v1 limits: the join outputs plain columns only */
	foreach(lc, joinrel->reltarget->exprs)
	{
		Var		   *v = lfirst(lc);

		if (!IsA(v, Var) || v->varattno <= 0)
			return;
	}

	/* find  inner.rowkey = outer.column  among the join clauses */
	foreach(lc, extra->restrictlist)
	{
		Node	   *val;
		int			kop;

		if (key_cond(lfirst(lc), innerrel->relid, ri->katt, &val, &kop) &&
			kop == BTEqualStrategyNumber)
		{
			while (IsA(val, RelabelType))
				val = (Node *) ((RelabelType *) val)->arg;
			if (IsA(val, Var) && ((Var *) val)->varattno > 0 &&
				bms_is_member(((Var *) val)->varno, outerrel->relids))
			{
				ov = (Var *) val;
				break;
			}
		}
	}
	if (ov == NULL)
		return;

	relid = planner_rt_fetch(innerrel->relid, root)->relid;
	bj_cost(innerrel, relid, op->rows, joinrel->rows, &su, &tot);

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = joinrel;
	cp->path.pathtarget = joinrel->reltarget;
	cp->path.rows = joinrel->rows;
	cp->path.startup_cost = op->startup_cost + su;
	cp->path.total_cost = op->total_cost + tot +
		op->rows * cpu_operator_cost + joinrel->rows * cpu_tuple_cost;
	cp->path.pathkeys = op->pathkeys;	/* outer order survives */
	cp->custom_paths = list_make1(op);
	cp->custom_private = list_make4(ov, extra->restrictlist,
									innerrel->baserestrictinfo,
									makeInteger(innerrel->relid));
	cp->methods = &bj_path_methods;
	add_path(joinrel, (Path *) cp);
}

static Plan *
bj_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *cp, List *tlist,
		List *clauses, List *plans)
{
	CustomScan *cs = makeNode(CustomScan);
	Var		   *ov = linitial(cp->custom_private);
	int			inner = intVal(lfourth(cp->custom_private));
	List	   *quals,
			   *flat,
			   *opos = NIL,
			   *iatt = NIL;
	Plan	   *outer = linitial(plans);
	int			kpos = 0;
	ListCell   *lc,
			   *lc2;

	/* join clauses are not in 'clauses' for a join: they came with the path */
	quals = list_concat(extract_actual_clauses(lsecond(cp->custom_private), false),
						extract_actual_clauses(lthird(cp->custom_private), false));

	/* every Var the output and the quals use, in one flat list */
	flat = add_to_flat_tlist(NIL, pull_var_clause((Node *) tlist, 0));
	flat = add_to_flat_tlist(flat, pull_var_clause((Node *) quals, 0));

	foreach(lc, flat)
	{
		Var		   *v = (Var *) ((TargetEntry *) lfirst(lc))->expr;
		int			pos = 0,
					i = 1;

		if (v->varno == inner)
		{
			opos = lappend_int(opos, 0);
			iatt = lappend_int(iatt, v->varattno);
			continue;
		}
		foreach(lc2, outer->targetlist)
		{
			Var		   *o = (Var *) ((TargetEntry *) lfirst(lc2))->expr;

			if (IsA(o, Var) && o->varno == v->varno && o->varattno == v->varattno)
				pos = i;
			i++;
		}
		if (pos == 0)
			elog(ERROR, "hbase_fdw: outer column not found in the outer plan");
		opos = lappend_int(opos, pos);
		iatt = lappend_int(iatt, 0);
	}
	/* where the outer key column sits in the outer plan's output */
	foreach(lc, outer->targetlist)
	{
		Var		   *o = (Var *) ((TargetEntry *) lfirst(lc))->expr;

		kpos++;
		if (IsA(o, Var) && o->varno == ov->varno && o->varattno == ov->varattno)
			break;
	}
	if (kpos > list_length(outer->targetlist))
		elog(ERROR, "hbase_fdw: join key not found in the outer plan");

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = quals;	/* ExecScan checks these on each joined row */
	cs->scan.scanrelid = 0;		/* a join: no single relation */
	cs->custom_scan_tlist = flat;
	cs->custom_plans = plans;
	/* the table's OID, not its range table index: setrefs renumbers those */
	cs->custom_private = list_make3(opos, iatt,
									list_make3_oid(planner_rt_fetch(inner, root)->relid,
												   join_batch, kpos));
	cs->methods = &bj_scan_methods;
	return (Plan *) cs;
}

static Node *
bj_create(CustomScan *cs)
{
	BjState    *s = palloc0(sizeof(BjState));

	NodeSetTag(s, T_CustomScanState);
	s->css.methods = &bj_exec_methods;
	return (Node *) s;
}

static void
bj_begin(CustomScanState *node, EState *estate, int eflags)
{
	BjState    *s = (BjState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *opos = linitial(cs->custom_private),
			   *iatt = lsecond(cs->custom_private),
			   *ints = lthird(cs->custom_private),
			   *atts = NIL;
	Oid			relid = linitial_oid(ints);
	TupleDesc	otd;
	Form_pg_attribute ka;
	Oid			fn;
	bool		varlena;
	int			i;

	node->custom_ps = list_make1(ExecInitNode(linitial(cs->custom_plans), estate, eflags));
	s->K = lsecond_oid(ints);
	s->kpos = lthird_oid(ints);
	s->nflat = list_length(opos);
	s->opos = palloc(sizeof(int) * Max(s->nflat, 1));
	s->iatt = palloc(sizeof(int) * Max(s->nflat, 1));
	for (i = 0; i < s->nflat; i++)
	{
		s->opos[i] = list_nth_int(opos, i);
		s->iatt[i] = list_nth_int(iatt, i);
		if (s->opos[i] == 0)
			atts = list_append_unique_int(atts, s->iatt[i]);
	}

	/* the HBase side: the same setup a foreign scan does */
	s->rel = table_open(relid, AccessShareLock);
	s->hb.rel = s->rel;
	get_opts(relid, &s->hb.o);
	setup_cols(&s->hb, s->rel, atts);
	s->inslot = MakeSingleTupleTableSlot(RelationGetDescr(s->rel), &TTSOpsVirtual);

	/* outer key -> bytes, as key_bytes() does for a key condition */
	otd = ExecGetResultType(linitial(node->custom_ps));
	ka = TupleDescAttr(otd, s->kpos - 1);
	s->kk.raw = (ka->atttypid == TEXTOID || ka->atttypid == VARCHAROID ||
				 ka->atttypid == BYTEAOID);
	getTypeOutputInfo(ka->atttypid, &fn, &varlena);
	fmgr_info(fn, &s->kk.out);

	s->outs = palloc(sizeof(TupleTableSlot *) * s->K);
	for (i = 0; i < s->K; i++)
		s->outs[i] = MakeSingleTupleTableSlot(otd, &TTSOpsMinimalTuple);
	s->okeys = palloc(sizeof(HbKey) * s->K);
	s->bcxt = AllocSetContextCreate(estate->es_query_cxt, "hbase_fdw batch join",
									ALLOCSET_DEFAULT_SIZES);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	s->hb.conn = hb_connect(s->hb.o.host, s->hb.o.port, s->hb.o.tmo * 1000);
	s->hb.wi = srv_slot(GetForeignTable(relid)->serverid);
}

/* pull up to K outer rows and fetch all their keys from HBase */
static void
bj_load(BjState *s)
{
	PlanState  *child = linitial(s->css.custom_ps);
	HbKey	   *keys;
	MemoryContext old;
	int			n = 0,
				i,
				nk = 0;

	MemoryContextReset(s->bcxt);
	s->nout = s->k = s->nrows = 0;
	while (s->nout < s->K)
	{
		TupleTableSlot *o = ExecProcNode(child);

		if (TupIsNull(o))
		{
			s->done = true;
			break;
		}
		ExecCopySlot(s->outs[s->nout++], o);
	}
	if (s->nout == 0)
		return;

	old = MemoryContextSwitchTo(s->bcxt);
	keys = palloc(sizeof(HbKey) * s->nout);
	for (i = 0; i < s->nout; i++)
	{
		bool		isnull;
		Datum		d = slot_getattr(s->outs[i], s->kpos, &isnull);
		HbKey	   *k = &s->okeys[i];

		k->p = NULL;
		k->len = 0;
		if (isnull)
			continue;			/* NULL never matches */
		key_bytes(&s->kk, d, &k->p, &k->len);
		if (k->len == 0)
		{
			k->p = NULL;		/* HBase has no empty row key */
			continue;
		}
		keys[nk++] = *k;
	}
	nk = sort_keys(keys, nk);

	s->rows = palloc(sizeof(HbRow) * Max(nk, 1));
	for (i = 0; i < nk; i += s->hb.o.batch)
	{
		HbRow	   *r;
		int			m = hb_get_rows(s->hb.conn, s->hb.o.table, keys + i,
									Min(s->hb.o.batch, nk - i),
									s->hb.hcols, s->hb.nh, &r);

		memcpy(s->rows + n, r, sizeof(HbRow) * m);
		n += m;
		s->nrpc++;
	}
	s->nrows = n;
	s->nbatch++;
	MemoryContextSwitchTo(old);
}

/* the HBase row with this key, or NULL: a plain loop, rows <= K */
static HbRow *
bj_find(BjState *s, HbKey *k)
{
	int			i;

	if (k->p == NULL)
		return NULL;
	for (i = 0; i < s->nrows; i++)
		if (kcmp(s->rows[i].key, s->rows[i].keylen, k->p, k->len) == 0)
			return &s->rows[i];
	return NULL;
}

/* fill the scan slot from outer row o and HBase row r */
static void
bj_row(BjState *s, TupleTableSlot *o, HbRow *r)
{
	TupleTableSlot *out = s->css.ss.ss_ScanTupleSlot;
	TupleTableSlot *in = s->inslot;
	int			i;

	ExecClearTuple(out);
	ExecClearTuple(in);
	fill(&s->hb, in, r);		/* fill() only writes the value arrays */
	ExecStoreVirtualTuple(in);
	slot_getallattrs(o);
	for (i = 0; i < s->nflat; i++)
	{
		if (s->opos[i] == 0)
		{
			out->tts_values[i] = in->tts_values[s->iatt[i] - 1];
			out->tts_isnull[i] = in->tts_isnull[s->iatt[i] - 1];
		}
		else
		{
			out->tts_values[i] = o->tts_values[s->opos[i] - 1];
			out->tts_isnull[i] = o->tts_isnull[s->opos[i] - 1];
		}
	}
	ExecStoreVirtualTuple(out);
}

static TupleTableSlot *
bj_next(ScanState *ss)
{
	BjState    *s = (BjState *) ss;

	for (;;)
	{
		while (s->k < s->nout)	/* the buffered outer rows, in order */
		{
			TupleTableSlot *o = s->outs[s->k];
			HbRow	   *r = bj_find(s, &s->okeys[s->k]);

			s->k++;
			if (r == NULL)
				continue;		/* inner join: no match, no row */
			/* converted values live until ExecScan's next per-tuple reset */
			{
				MemoryContext old =
					MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);

				bj_row(s, o, r);
				MemoryContextSwitchTo(old);
			}
			return ss->ss_ScanTupleSlot;
		}
		if (s->done)
			return ExecClearTuple(ss->ss_ScanTupleSlot);
		bj_load(s);
	}
}

static bool
bj_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
bj_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, bj_next, bj_recheck);
}

static void
bj_rescan(CustomScanState *node)
{
	BjState    *s = (BjState *) node;

	ExecReScan(linitial(node->custom_ps));
	s->nout = s->k = s->nrows = 0;
	s->done = false;
}

static void
bj_end(CustomScanState *node)
{
	BjState    *s = (BjState *) node;

	ExecEndNode(linitial(node->custom_ps));
	if (s->hb.conn)
		hb_disconnect(s->hb.conn);
	ExecDropSingleTupleTableSlot(s->inslot);
	for (int i = 0; i < s->K; i++)
		ExecDropSingleTupleTableSlot(s->outs[i]);
	table_close(s->rel, AccessShareLock);
}

static void
bj_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BjState    *s = (BjState *) node;

	ExplainPropertyText("HBase Table", s->hb.o.table, es);
	ExplainPropertyInteger("Batch Size", NULL, s->K, es);
	if (es->analyze)
	{
		ExplainPropertyInteger("Batches", NULL, s->nbatch, es);
		ExplainPropertyInteger("HBase RPCs", NULL, s->nrpc, es);
	}
}

static void
bj_init(void)
{
	bj_path_methods.CustomName = "HBase Batched Join";
	bj_path_methods.PlanCustomPath = bj_plan;

	bj_scan_methods.CustomName = "HBase Batched Join";
	bj_scan_methods.CreateCustomScanState = bj_create;
	RegisterCustomScanMethods(&bj_scan_methods);

	bj_exec_methods.CustomName = "HBase Batched Join";
	bj_exec_methods.BeginCustomScan = bj_begin;
	bj_exec_methods.ExecCustomScan = bj_exec;
	bj_exec_methods.EndCustomScan = bj_end;
	bj_exec_methods.ReScanCustomScan = bj_rescan;
	bj_exec_methods.ExplainCustomScan = bj_explain;

	prev_join_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = bj_hook;
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("hbase_fdw.adaptive_costs",
							 "Cost HBase scans from the measured RTT and bandwidth.",
							 "off: the fixed costs tuned on one machine.",
							 &adaptive, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("hbase_fdw.rtt_ms",
							 "Use this RTT in ms instead of measuring it (-1 = measure).",
							 NULL, &force_rtt, -1, -1, 100000, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("hbase_fdw.bw_mbps",
							 "Use this bandwidth in Mbit/s instead of measuring it (-1 = measure).",
							 NULL, &force_bw, -1, -1, 1e6, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("hbase_fdw.cost_per_ms",
							 "Planner cost units per millisecond of HBase time.",
							 NULL, &cpm, 20.0, 0.001, 1e6, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("hbase_fdw.row_us",
							 "Client time per fetched row, in microseconds.",
							 NULL, &row_us, 1.0, 0, 1e6, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("hbase_fdw.batch_join",
							 "Offer a batched join that fetches K outer keys per HBase call.",
							 NULL, &batch_join, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("hbase_fdw.join_batch",
							"Outer rows per batched join call (K).",
							NULL, &join_batch, 500, 1, 100000, PGC_USERSET, 0,
							NULL, NULL, NULL);
	MarkGUCPrefixReserved("hbase_fdw");
	bj_init();
}

/* ---------- INSERT, UPDATE, DELETE (Part D) ---------- */

/*
 * Writes go straight to HBase: INSERT and UPDATE become one mutateRow per
 * row (a value is a put, NULL deletes the cell), DELETE one deleteAllRow.
 * The row key is the row's identity: UPDATE and DELETE find it in a junk
 * column "hbase_key" that the scan below them carries up. HBase applies
 * each call at once, so there is no rollback, and an INSERT of a key that
 * exists overwrites it (an upsert).
 */
typedef struct HbMod
{
	HbOpts		o;
	HbConn	   *conn;
	AttrNumber	katt;			/* row key column */
	AttrNumber	junk;			/* "hbase_key" in the subplan's output */
	int			n;				/* columns written */
	AttrNumber *att;
	char	  **spec;			/* their "cf:q" */
	bool	   *raw;			/* bytea: write the bytes as they are */
	FmgrInfo   *out;			/* otherwise: the text form */
	bool		kraw;
	FmgrInfo	kout;
	HbMut	   *m;
	MemoryContext cx;			/* one insert batch */
	long		nrpc;			/* write calls sent */
} HbMod;

static int
hbIsForeignRelUpdatable(Relation rel)
{
	if (rowkey_att(RelationGetRelid(rel)) == InvalidAttrNumber)
		return 0;
	return (1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE);
}

static void
hbAddForeignUpdateTargets(PlannerInfo *root, Index rtindex,
						  RangeTblEntry *rte, Relation rel)
{
	AttrNumber	k = rowkey_att(RelationGetRelid(rel));
	Form_pg_attribute a;

	if (k == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("foreign table \"%s\" needs a rowkey column to be updated",
						RelationGetRelationName(rel))));
	a = TupleDescAttr(RelationGetDescr(rel), k - 1);
	add_row_identity_var(root, makeVar(rtindex, k, a->atttypid, a->atttypmod,
									   a->attcollation, 0),
						 rtindex, "hbase_key");
}

/* the columns to write: all of them for INSERT, the SET ones for UPDATE */
static List *
hbPlanForeignModify(PlannerInfo *root, ModifyTable *plan, Index rti, int i)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	Relation	rel = table_open(rte->relid, NoLock);
	TupleDesc	td = RelationGetDescr(rel);
	AttrNumber	k = rowkey_att(rte->relid);
	List	   *atts = NIL;
	int			a;

	if (plan->operation == CMD_INSERT)
	{
		for (a = 0; a < td->natts; a++)
			if (!TupleDescAttr(td, a)->attisdropped)
				atts = lappend_int(atts, a + 1);
	}
	else if (plan->operation == CMD_UPDATE)
	{
		Bitmapset  *upd = get_rel_all_updated_cols(root, find_base_rel(root, rti));
		int			c = -1;

		while ((c = bms_next_member(upd, c)) >= 0)
		{
			a = c + FirstLowInvalidHeapAttributeNumber;
			if (a <= 0)
				continue;
			if (a == k)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot update the row key of HBase table \"%s\"",
								RelationGetRelationName(rel)),
						 errhint("Delete the row and insert it with the new key.")));
			atts = lappend_int(atts, a);
		}
	}
	table_close(rel, NoLock);
	return list_make1(atts);
}

static void
hbBeginForeignModify(ModifyTableState *mt, ResultRelInfo *rinfo,
					 List *priv, int subplan, int eflags)
{
	Relation	rel = rinfo->ri_RelationDesc;
	Oid			relid = RelationGetRelid(rel);
	TupleDesc	td = RelationGetDescr(rel);
	List	   *atts = linitial(priv);
	HbMod	   *md = palloc0(sizeof(HbMod));
	Form_pg_attribute ka;
	ListCell   *lc;
	Oid			fn;
	bool		varlena;

	rinfo->ri_FdwState = md;
	get_opts(relid, &md->o);
	md->katt = rowkey_att(relid);
	if (md->katt == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("foreign table \"%s\" needs a rowkey column to be written",
						RelationGetRelationName(rel))));
	ka = TupleDescAttr(td, md->katt - 1);
	md->kraw = (ka->atttypid == BYTEAOID);
	getTypeOutputInfo(ka->atttypid, &fn, &varlena);
	fmgr_info(fn, &md->kout);

	/* the columns to write, minus the key, with their HBase names */
	md->att = palloc(sizeof(AttrNumber) * (list_length(atts) + 1));
	md->spec = palloc(sizeof(char *) * (list_length(atts) + 1));
	md->raw = palloc(sizeof(bool) * (list_length(atts) + 1));
	md->out = palloc(sizeof(FmgrInfo) * (list_length(atts) + 1));
	md->m = palloc(sizeof(HbMut) * (list_length(atts) + 1));
	foreach(lc, atts)
	{
		AttrNumber	a = lfirst_int(lc);
		Form_pg_attribute at = TupleDescAttr(td, a - 1);
		char	   *sp = att_spec(relid, a, NameStr(at->attname), &md->o);

		if (sp == NULL)
			continue;			/* the row key itself */
		md->att[md->n] = a;
		md->spec[md->n] = sp;
		md->raw[md->n] = (at->atttypid == BYTEAOID);
		getTypeOutputInfo(at->atttypid, &fn, &varlena);
		fmgr_info(fn, &md->out[md->n]);
		md->n++;
	}

	if (mt->operation != CMD_INSERT)
	{
		Plan	   *sub = outerPlanState(mt)->plan;

		md->junk = ExecFindJunkAttributeInTlist(sub->targetlist, "hbase_key");
		if (!AttributeNumberIsValid(md->junk))
			elog(ERROR, "hbase_fdw: could not find the hbase_key junk column");
	}
	md->cx = AllocSetContextCreate(mt->ps.state->es_query_cxt, "hbase_fdw insert batch",
								   ALLOCSET_DEFAULT_SIZES);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	md->conn = hb_connect(md->o.host, md->o.port, md->o.tmo * 1000);
}

/* bytes of a value as HBase stores it: raw for bytea, else its text form */
static void
val_bytes(bool raw, FmgrInfo *out, Datum d, char **p, int *len)
{
	if (raw)
	{
		struct varlena *v = PG_DETOAST_DATUM_PACKED(d);

		*p = VARDATA_ANY(v);
		*len = VARSIZE_ANY_EXHDR(v);
	}
	else
	{
		*p = OutputFunctionCall(out, d);
		*len = strlen(*p);
	}
}

/* the key bytes of d (never empty) and the cell changes for slot into m */
static void
row_muts(HbMod *md, Datum d, TupleTableSlot *slot, HbKey *key, HbMut *m)
{
	int			i;

	val_bytes(md->kraw, &md->kout, d, &key->p, &key->len);
	if (key->len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("HBase row key cannot be empty")));
	slot_getallattrs(slot);
	for (i = 0; i < md->n; i++)
	{
		int			a = md->att[i] - 1;

		m[i].col = md->spec[i];
		m[i].del = slot->tts_isnull[a];
		m[i].val = NULL;
		m[i].vlen = 0;
		if (!m[i].del)
			val_bytes(md->raw[i], &md->out[i], slot->tts_values[a], &m[i].val, &m[i].vlen);
	}
}

/* write the columns of slot to row key d */
static void
put_row(HbMod *md, Datum d, TupleTableSlot *slot)
{
	HbKey		k;

	row_muts(md, d, slot, &k, md->m);
	if (md->n > 0)				/* else nothing to write but the key */
	{
		hb_mutate_row(md->conn, md->o.table, k.p, k.len, md->m, md->n);
		md->nrpc++;
	}
}

/*
 * Batched INSERT: Postgres hands us up to insert_batch rows at once and we
 * send them in one mutateRows call, one round trip instead of one per row.
 * Not with RETURNING or row triggers, where Postgres needs each row back.
 */
static int
hbGetForeignModifyBatchSize(ResultRelInfo *rinfo)
{
	HbMod	   *md = rinfo->ri_FdwState;
	HbOpts		o;

	if (rinfo->ri_projectReturning != NULL ||
		(rinfo->ri_TrigDesc &&
		 (rinfo->ri_TrigDesc->trig_insert_before_row ||
		  rinfo->ri_TrigDesc->trig_insert_after_row)))
		return 1;
	if (md)
		return md->o.ibatch;
	get_opts(RelationGetRelid(rinfo->ri_RelationDesc), &o);
	return o.ibatch;
}

static TupleTableSlot **
hbExecForeignBatchInsert(EState *es, ResultRelInfo *rinfo, TupleTableSlot **slots,
						 TupleTableSlot **plans, int *n)
{
	HbMod	   *md = rinfo->ri_FdwState;
	MemoryContext old = MemoryContextSwitchTo(md->cx);
	HbKey	   *keys = palloc(sizeof(HbKey) * *n);
	HbMut	   *m = palloc(sizeof(HbMut) * Max(md->n, 1) * *n);
	int			i;

	for (i = 0; i < *n; i++)
	{
		bool		isnull;
		Datum		d = slot_getattr(slots[i], md->katt, &isnull);

		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NOT_NULL_VIOLATION),
					 errmsg("HBase row key cannot be NULL")));
		row_muts(md, d, slots[i], &keys[i], m + i * md->n);
	}
	if (md->n > 0)
	{
		hb_mutate_rows(md->conn, md->o.table, keys, m, md->n, *n);
		md->nrpc++;
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(md->cx);
	return slots;
}

static TupleTableSlot *
hbExecForeignInsert(EState *es, ResultRelInfo *rinfo, TupleTableSlot *slot,
					TupleTableSlot *plan)
{
	HbMod	   *md = rinfo->ri_FdwState;
	bool		isnull;
	Datum		d = slot_getattr(slot, md->katt, &isnull);

	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("HBase row key cannot be NULL")));
	put_row(md, d, slot);
	return slot;
}

static TupleTableSlot *
hbExecForeignUpdate(EState *es, ResultRelInfo *rinfo, TupleTableSlot *slot,
					TupleTableSlot *plan)
{
	HbMod	   *md = rinfo->ri_FdwState;
	bool		isnull;
	Datum		d = ExecGetJunkAttribute(plan, md->junk, &isnull);

	if (isnull)
		elog(ERROR, "hbase_fdw: hbase_key is NULL");
	put_row(md, d, slot);
	return slot;
}

static TupleTableSlot *
hbExecForeignDelete(EState *es, ResultRelInfo *rinfo, TupleTableSlot *slot,
					TupleTableSlot *plan)
{
	HbMod	   *md = rinfo->ri_FdwState;
	bool		isnull;
	Datum		d = ExecGetJunkAttribute(plan, md->junk, &isnull);
	char	   *k;
	int			klen;

	if (isnull)
		elog(ERROR, "hbase_fdw: hbase_key is NULL");
	val_bytes(md->kraw, &md->kout, d, &k, &klen);
	hb_delete_row(md->conn, md->o.table, k, klen);
	md->nrpc++;
	return slot;
}

static void
hbExplainForeignModify(ModifyTableState *mt, ResultRelInfo *rinfo, List *priv,
					   int subplan, ExplainState *es)
{
	HbMod	   *md = rinfo->ri_FdwState;

	if (rinfo->ri_BatchSize > 1)
		ExplainPropertyInteger("Batch Size", NULL, rinfo->ri_BatchSize, es);
	if (es->analyze && md)
		ExplainPropertyInteger("HBase RPCs", NULL, md->nrpc, es);
}

static void
hbEndForeignModify(EState *es, ResultRelInfo *rinfo)
{
	HbMod	   *md = rinfo->ri_FdwState;

	if (md && md->conn)
		hb_disconnect(md->conn);
}

Datum
hbase_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *r = makeNode(FdwRoutine);

	r->GetForeignRelSize = hbGetForeignRelSize;
	r->GetForeignPaths = hbGetForeignPaths;
	r->GetForeignPlan = hbGetForeignPlan;
	r->BeginForeignScan = hbBeginForeignScan;
	r->IterateForeignScan = hbIterateForeignScan;
	r->ReScanForeignScan = hbReScanForeignScan;
	r->EndForeignScan = hbEndForeignScan;
	r->ExplainForeignScan = hbExplainForeignScan;
	r->IsForeignRelUpdatable = hbIsForeignRelUpdatable;
	r->AddForeignUpdateTargets = hbAddForeignUpdateTargets;
	r->PlanForeignModify = hbPlanForeignModify;
	r->BeginForeignModify = hbBeginForeignModify;
	r->ExecForeignInsert = hbExecForeignInsert;
	r->ExecForeignUpdate = hbExecForeignUpdate;
	r->ExecForeignDelete = hbExecForeignDelete;
	r->EndForeignModify = hbEndForeignModify;
	r->GetForeignModifyBatchSize = hbGetForeignModifyBatchSize;
	r->ExplainForeignModify = hbExplainForeignModify;
	r->ExecForeignBatchInsert = hbExecForeignBatchInsert;

	PG_RETURN_POINTER(r);
}
