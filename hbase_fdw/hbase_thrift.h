/*
 * hbase_thrift.h
 *    Minimal client for the HBase Thrift1 gateway (Hbase.thrift), speaking
 *    TBinaryProtocol (strict) over a buffered (unframed) socket -- the
 *    default for `hbase thrift start`.
 *
 * Only the handful of calls the FDW needs are implemented. Everything is
 * palloc'd in the caller's memory context and errors are ereport(ERROR).
 */
#ifndef HBASE_THRIFT_H
#define HBASE_THRIFT_H

#include "postgres.h"

typedef struct HbCell
{
	char	   *col;			/* "family:qualifier" */
	int			collen;
	char	   *val;
	int			vallen;
	int64		ts;
} HbCell;

typedef struct HbRow
{
	char	   *key;
	int			keylen;
	int			ncells;
	HbCell	   *cells;
} HbRow;

typedef struct HbConn HbConn;

/* a row key, for hb_get_rows */
typedef struct HbKey
{
	char	   *p;
	int			len;
} HbKey;

/* scan description passed to hb_scan_open */
typedef struct HbScanSpec
{
	const char *start;			/* inclusive, NULL/0 = from first row */
	int			startlen;
	const char *stop;			/* exclusive, NULL/0 = to last row */
	int			stoplen;
	int			ncols;			/* 0 = all columns */
	char	  **cols;			/* "cf:q" or "cf" */
	int			caching;		/* rows per RPC on the region server side */
	bool		keyonly;		/* only fetch row keys (count(*) etc.) */
} HbScanSpec;

extern HbConn *hb_connect(const char *host, int port, int timeout_ms);
extern void hb_disconnect(HbConn *c);

extern int32 hb_scan_open(HbConn *c, const char *table, const HbScanSpec *s);
extern int	hb_scan_next(HbConn *c, int32 id, int n, HbRow **rows);
extern void hb_scan_close(HbConn *c, int32 id);

extern int	hb_get_rows(HbConn *c, const char *table, const HbKey *keys,
						int nkeys, char **cols, int ncols, HbRow **rows);

#endif
