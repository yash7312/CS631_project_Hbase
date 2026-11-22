/*
 * hbase_thrift.c
 *    Hand-rolled Thrift1 client for HBase: TBinaryProtocol over a plain
 *    buffered TCP socket. We avoid libthrift because it is C++/glib, and
 *    mixing C++ exceptions with PostgreSQL's longjmp-based error handling
 *    inside a backend is asking for trouble. The binary protocol is small:
 *
 *    message : i32 (0x80010000 | type)  string name  i32 seqid  <struct>
 *    struct  : { i8 type, i16 id, <value> }*  i8 0 (STOP)
 *    string  : i32 len, bytes             list/set : i8 elemtype, i32 n, ...
 *    map     : i8 ktype, i8 vtype, i32 n, ...    ints are big endian
 */
#include "postgres.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "utils/memutils.h"

#include "hbase_thrift.h"

/* thrift type ids */
#define T_STOP		0
#define T_BOOL		2
#define T_BYTE		3
#define T_DOUBLE	4
#define T_I16		6
#define T_I32		8
#define T_I64		10
#define T_STRING	11
#define T_STRUCT	12
#define T_MAP		13
#define T_SET		14
#define T_LIST		15

#define M_CALL		1
#define M_REPLY		2
#define M_EXC		3
#define VER1		0x80010000

#define IBUF		65536
#define MAXN		(64 * 1024 * 1024)	/* sanity cap on list/map sizes */

struct HbConn
{
	int			fd;
	int			tmo;			/* ms, per network wait */
	int32		seq;
	char	   *host;
	int			port;
	StringInfoData ob;
	char		ib[IBUF];
	int			il,
				ip;
	MemoryContextCallback cb;

	/* for measured costs: bytes received, and how long the last call took */
	long		nin;			/* bytes received since connect */
	double		t0;				/* when the current call started (ms) */
	long		n0;				/* nin when it started */
	double		lastms;			/* the last call: time, bytes received */
	long		lastin;
	double		connms;			/* time to connect */
};

static double
now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* a call's reply is fully read: note its time and size */
static void
done(HbConn *c)
{
	c->lastms = now_ms() - c->t0;
	c->lastin = c->nin - c->n0;
}

static void
hb_fail(HbConn *c, const char *what)
{
	int			e = errno;

	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	errno = e;
	ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
			 errmsg("hbase_fdw: %s (%s:%d): %m", what, c->host, c->port)));
}

static void
proto_err(const char *msg)
{
	ereport(ERROR,
			(errcode(ERRCODE_FDW_ERROR),
			 errmsg("hbase_fdw: thrift protocol error: %s", msg)));
}

/* closes the socket if the owning memory context goes away (incl. on abort) */
static void
conn_cb(void *arg)
{
	HbConn	   *c = arg;

	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
}

/* wait until fd is readable/writable, staying responsive to cancel */
static void
wait_fd(HbConn *c, short ev, const char *what)
{
	int			left = c->tmo;

	for (;;)
	{
		struct pollfd p;
		int			r,
					t = left > 1000 || left <= 0 ? 1000 : left;

		CHECK_FOR_INTERRUPTS();
		p.fd = c->fd;
		p.events = ev;
		p.revents = 0;
		r = poll(&p, 1, t);
		if (r > 0)
			return;
		if (r < 0 && errno != EINTR)
			hb_fail(c, what);
		if (c->tmo > 0 && r == 0 && (left -= t) <= 0)
		{
			errno = ETIMEDOUT;
			hb_fail(c, what);
		}
	}
}

HbConn *
hb_connect(const char *host, int port, int tmo)
{
	HbConn	   *c = palloc0(sizeof(HbConn));
	struct addrinfo h,
			   *res,
			   *a;
	char		ps[16];
	int			r,
				one = 1;
	double		t = now_ms();

	c->fd = -1;
	c->tmo = tmo;
	c->host = pstrdup(host);
	c->port = port;
	initStringInfo(&c->ob);
	c->cb.func = conn_cb;
	c->cb.arg = c;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &c->cb);

	memset(&h, 0, sizeof(h));
	h.ai_family = AF_UNSPEC;
	h.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof(ps), "%d", port);
	if ((r = getaddrinfo(host, ps, &h, &res)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
				 errmsg("hbase_fdw: could not resolve \"%s\": %s",
						host, gai_strerror(r))));

	for (a = res; a; a = a->ai_next)
	{
		int			err = 0;
		socklen_t	el = sizeof(err);

		c->fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (c->fd < 0)
			continue;
		fcntl(c->fd, F_SETFL, O_NONBLOCK);
		r = connect(c->fd, a->ai_addr, a->ai_addrlen);
		if (r < 0 && errno == EINPROGRESS)
		{
			PG_TRY();
			{
				wait_fd(c, POLLOUT, "connect timed out");
			}
			PG_CATCH();
			{
				freeaddrinfo(res);
				PG_RE_THROW();
			}
			PG_END_TRY();
			getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
			r = err ? -1 : 0;
			errno = err;
		}
		if (r == 0)
			break;
		close(c->fd);
		c->fd = -1;
	}
	freeaddrinfo(res);
	if (c->fd < 0)
		hb_fail(c, "could not connect to HBase thrift server");

	setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	c->connms = now_ms() - t;
	return c;
}

void
hb_disconnect(HbConn *c)
{
	if (c && c->fd >= 0)
		close(c->fd);
	if (c)
		c->fd = -1;
}

/* ---------- writing ---------- */

static void
w8(HbConn *c, int v)
{
	appendStringInfoChar(&c->ob, (char) v);
}

static void
w16(HbConn *c, int v)
{
	w8(c, v >> 8);
	w8(c, v);
}

static void
w32(HbConn *c, int32 v)
{
	uint32		u = pg_hton32((uint32) v);

	appendBinaryStringInfo(&c->ob, (char *) &u, 4);
}

static void
wstr(HbConn *c, const char *s, int n)
{
	w32(c, n);
	appendBinaryStringInfo(&c->ob, s, n);
}

static void
wfld(HbConn *c, int type, int id)
{
	w8(c, type);
	w16(c, id);
}

static void
wmsg(HbConn *c, const char *name)
{
	if (c->fd < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
				 errmsg("hbase_fdw: connection to %s:%d is closed",
						c->host, c->port)));
	resetStringInfo(&c->ob);
	c->t0 = now_ms();
	c->n0 = c->nin;
	w32(c, (int32) (VER1 | M_CALL));
	wstr(c, name, strlen(name));
	w32(c, ++c->seq);
}

static void
flush(HbConn *c)
{
	int			off = 0;

	while (off < c->ob.len)
	{
		ssize_t		n = send(c->fd, c->ob.data + off, c->ob.len - off, MSG_NOSIGNAL);

		if (n > 0)
			off += n;
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			wait_fd(c, POLLOUT, "send timed out");
		else
			hb_fail(c, "could not send to HBase thrift server");
	}
	resetStringInfo(&c->ob);
}

/* ---------- reading ---------- */

static void
rd(HbConn *c, void *dst, int n)
{
	char	   *d = dst;

	while (n > 0)
	{
		int			k;

		if (c->ip == c->il)
		{
			ssize_t		r;

			c->ip = c->il = 0;
			for (;;)
			{
				r = recv(c->fd, c->ib, IBUF, 0);
				if (r > 0)
					break;
				if (r == 0)
				{
					errno = ECONNRESET;
					hb_fail(c, "HBase thrift server closed the connection");
				}
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					wait_fd(c, POLLIN, "timed out waiting for HBase");
				else
					hb_fail(c, "could not read from HBase thrift server");
			}
			c->il = r;
			c->nin += r;
		}
		k = Min(n, c->il - c->ip);
		memcpy(d, c->ib + c->ip, k);
		c->ip += k;
		d += k;
		n -= k;
	}
}

static int
r8(HbConn *c)
{
	int8		v;

	rd(c, &v, 1);
	return v;
}

static int
r16(HbConn *c)
{
	uint16		v;

	rd(c, &v, 2);
	return (int16) pg_ntoh16(v);
}

static int32
r32(HbConn *c)
{
	uint32		v;

	rd(c, &v, 4);
	return (int32) pg_ntoh32(v);
}

static int64
r64(HbConn *c)
{
	uint64		v;

	rd(c, &v, 8);
	return (int64) pg_ntoh64(v);
}

static int32
rlen(HbConn *c)
{
	int32		n = r32(c);

	if (n < 0 || n > MAXN)
		proto_err("bad length");
	return n;
}

/* palloc'd and NUL terminated, so it is also usable as a C string */
static char *
rstr(HbConn *c, int *len)
{
	int32		n = r32(c);
	char	   *s;

	if (n < 0 || (Size) n >= MaxAllocSize)
		proto_err("bad string length");
	s = palloc(n + 1);
	rd(c, s, n);
	s[n] = '\0';
	if (len)
		*len = n;
	return s;
}

static void
skip(HbConn *c, int t, int depth)
{
	int32		n,
				i;
	int			kt,
				vt;
	char		tmp[256];

	if (depth > 32)
		proto_err("nesting too deep");
	switch (t)
	{
		case T_BOOL:
		case T_BYTE:
			r8(c);
			break;
		case T_I16:
			r16(c);
			break;
		case T_I32:
			r32(c);
			break;
		case T_DOUBLE:
		case T_I64:
			r64(c);
			break;
		case T_STRING:
			for (n = rlen(c); n > 0; n -= Min(n, 256))
				rd(c, tmp, Min(n, 256));
			break;
		case T_STRUCT:
			while ((kt = r8(c)) != T_STOP)
			{
				r16(c);
				skip(c, kt, depth + 1);
			}
			break;
		case T_MAP:
			kt = r8(c);
			vt = r8(c);
			for (n = rlen(c), i = 0; i < n; i++)
			{
				skip(c, kt, depth + 1);
				skip(c, vt, depth + 1);
			}
			break;
		case T_SET:
		case T_LIST:
			kt = r8(c);
			for (n = rlen(c), i = 0; i < n; i++)
				skip(c, kt, depth + 1);
			break;
		default:
			proto_err("unknown field type");
	}
}

/* read a reply header; raises on TApplicationException */
static void
rmsg(HbConn *c, const char *name)
{
	int32		v = r32(c),
				seq;
	int			typ;
	char	   *nm;

	if (v < 0)
	{
		if (((uint32) v & 0xffff0000) != VER1)
			proto_err("bad protocol version");
		typ = v & 0xff;
		nm = rstr(c, NULL);
	}
	else
	{
		/* old non-strict header: name length first */
		nm = palloc(v + 1);
		rd(c, nm, v);
		nm[v] = '\0';
		typ = r8(c);
	}
	seq = r32(c);

	if (typ == M_EXC)
	{
		char	   *msg = NULL;
		int			ft;

		while ((ft = r8(c)) != T_STOP)
		{
			int			id = r16(c);

			if (id == 1 && ft == T_STRING)
				msg = rstr(c, NULL);
			else
				skip(c, ft, 0);
		}
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("hbase_fdw: %s failed: %s", name, msg ? msg : "unknown error")));
	}
	if (typ != M_REPLY || strcmp(nm, name) != 0 || seq != c->seq)
		proto_err("unexpected reply");
}

/*
 * Real HBase puts a Java stack trace after the message and qualifies the
 * exception class ("org.apache.hadoop.hbase.TableNotFoundException: t").
 * Keep the first line and the bare class name. Returns a pointer into msg.
 */
static char *
short_msg(char *msg)
{
	char	   *nl = strpbrk(msg, "\r\n"),
			   *colon,
			   *p,
			   *dot = NULL;

	if (nl)
		*nl = '\0';
	if (!(colon = strchr(msg, ':')))
		return msg;
	for (p = msg; p < colon; p++)
	{
		if (*p == '.')
			dot = p;
		else if (!isalnum((unsigned char) *p) && *p != '_' && *p != '$')
			return msg;			/* not a "java.pkg.Class:" prefix */
	}
	return dot ? dot + 1 : msg;
}

/* field 1/2 of a result struct is IOError / IllegalArgument: message is field 1 */
static void
rexc(HbConn *c, const char *name, int id)
{
	char	   *msg = NULL;
	int			ft;

	while ((ft = r8(c)) != T_STOP)
	{
		if (r16(c) == 1 && ft == T_STRING)
			msg = rstr(c, NULL);
		else
			skip(c, ft, 0);
	}
	/*
	 * The RPC name stays out of the message: real HBase opens scanners
	 * lazily, so the same error can come from scannerOpenWithScan or from
	 * the first scannerGetList depending on the server.
	 */
	elog(DEBUG1, "hbase_fdw: %s raised: %s", name, msg ? msg : "(no message)");
	ereport(ERROR,
			(errcode(ERRCODE_FDW_ERROR),
			 errmsg("hbase_fdw: %s", msg ? short_msg(msg) : "error"),
			 errdetail("HBase raised %s.", id == 1 ? "IOError" : "IllegalArgument")));
}

/* ---------- RPCs ---------- */

int32
hb_scan_open(HbConn *c, const char *tab, const HbScanSpec *s)
{
	static const char *fn = "scannerOpenWithScan";
	static const char *ko = "FirstKeyOnlyFilter() AND KeyOnlyFilter()";
	int32		id = -1;
	bool		got = false;
	int			ft,
				i;

	wmsg(c, fn);
	wfld(c, T_STRING, 1);
	wstr(c, tab, strlen(tab));

	wfld(c, T_STRUCT, 2);		/* TScan */
	if (s->start && s->startlen > 0)
	{
		wfld(c, T_STRING, 1);
		wstr(c, s->start, s->startlen);
	}
	if (s->stop && s->stoplen > 0)
	{
		wfld(c, T_STRING, 2);
		wstr(c, s->stop, s->stoplen);
	}
	if (s->ncols > 0)
	{
		wfld(c, T_LIST, 4);
		w8(c, T_STRING);
		w32(c, s->ncols);
		for (i = 0; i < s->ncols; i++)
			wstr(c, s->cols[i], strlen(s->cols[i]));
	}
	if (s->caching > 0)
	{
		wfld(c, T_I32, 5);
		w32(c, s->caching);
	}
	if (s->keyonly)
	{
		wfld(c, T_STRING, 6);
		wstr(c, ko, strlen(ko));
	}
	w8(c, T_STOP);

	wfld(c, T_MAP, 3);			/* attributes: empty */
	w8(c, T_STRING);
	w8(c, T_STRING);
	w32(c, 0);
	w8(c, T_STOP);
	flush(c);

	rmsg(c, fn);
	while ((ft = r8(c)) != T_STOP)
	{
		int			fid = r16(c);

		if (fid == 0 && ft == T_I32)
		{
			id = r32(c);
			got = true;
		}
		else if (fid == 1 && ft == T_STRUCT)
			rexc(c, fn, 1);
		else
			skip(c, ft, 0);
	}
	if (!got)
		proto_err("scannerOpenWithScan returned no scanner id");
	return id;
}

static void
rcell(HbConn *c, HbCell *x)
{
	int			ft;

	while ((ft = r8(c)) != T_STOP)
	{
		int			id = r16(c);

		if (id == 1 && ft == T_STRING)
			x->val = rstr(c, &x->vallen);
		else if (id == 2 && ft == T_I64)
			x->ts = r64(c);
		else
			skip(c, ft, 0);
	}
}

static void
rrow(HbConn *c, HbRow *r)
{
	int			ft;

	memset(r, 0, sizeof(*r));
	while ((ft = r8(c)) != T_STOP)
	{
		int			id = r16(c);
		int32		n,
					i;

		if (id == 1 && ft == T_STRING)
			r->key = rstr(c, &r->keylen);
		else if (id == 2 && ft == T_MAP)
		{
			/* map<Text,TCell> columns */
			if (r8(c) != T_STRING || r8(c) != T_STRUCT)
				proto_err("bad TRowResult.columns");
			n = rlen(c);
			r->cells = n ? palloc0(sizeof(HbCell) * n) : NULL;
			r->ncells = n;
			for (i = 0; i < n; i++)
			{
				r->cells[i].col = rstr(c, &r->cells[i].collen);
				rcell(c, &r->cells[i]);
			}
		}
		else if (id == 3 && ft == T_LIST)
		{
			/* list<TColumn> sortedColumns (wins over the map if both sent) */
			if (r8(c) != T_STRUCT)
				proto_err("bad TRowResult.sortedColumns");
			n = rlen(c);
			r->cells = n ? palloc0(sizeof(HbCell) * n) : NULL;
			r->ncells = n;
			for (i = 0; i < n; i++)
			{
				int			t2;

				while ((t2 = r8(c)) != T_STOP)
				{
					int			id2 = r16(c);

					if (id2 == 1 && t2 == T_STRING)
						r->cells[i].col = rstr(c, &r->cells[i].collen);
					else if (id2 == 2 && t2 == T_STRUCT)
						rcell(c, &r->cells[i]);
					else
						skip(c, t2, 0);
				}
			}
		}
		else
			skip(c, ft, 0);
	}
	if (!r->key)
		proto_err("TRowResult without row key");
}

/* shared by the RPCs returning list<TRowResult> (field 0 of the result) */
static int
rrows(HbConn *c, const char *fn, HbRow **out)
{
	int			ft,
				cnt = 0;

	*out = NULL;
	while ((ft = r8(c)) != T_STOP)
	{
		int			fid = r16(c);

		if (fid == 0 && ft == T_LIST)
		{
			int			i;

			if (r8(c) != T_STRUCT)
				proto_err("bad list<TRowResult>");
			cnt = rlen(c);
			*out = cnt ? palloc(sizeof(HbRow) * cnt) : NULL;
			for (i = 0; i < cnt; i++)
				rrow(c, &(*out)[i]);
		}
		else if ((fid == 1 || fid == 2) && ft == T_STRUCT)
			rexc(c, fn, fid);
		else
			skip(c, ft, 0);
	}
	return cnt;
}

int
hb_scan_next(HbConn *c, int32 sid, int n, HbRow **out)
{
	static const char *fn = "scannerGetList";

	wmsg(c, fn);
	wfld(c, T_I32, 1);
	w32(c, sid);
	wfld(c, T_I32, 2);
	w32(c, n);
	w8(c, T_STOP);
	flush(c);

	rmsg(c, fn);
	n = rrows(c, fn, out);
	done(c);
	return n;
}

/*
 * One round trip for a set of row keys. HBase returns the rows in the order
 * asked for and leaves out keys with no cell in cols, just as a scan does.
 */
int
hb_get_rows(HbConn *c, const char *tab, const HbKey *keys, int nkeys,
			char **cols, int ncols, HbRow **out)
{
	static const char *fn = "getRowsWithColumns";
	int			i;

	wmsg(c, fn);
	wfld(c, T_STRING, 1);
	wstr(c, tab, strlen(tab));
	wfld(c, T_LIST, 2);
	w8(c, T_STRING);
	w32(c, nkeys);
	for (i = 0; i < nkeys; i++)
		wstr(c, keys[i].p, keys[i].len);
	wfld(c, T_LIST, 3);			/* empty = all columns */
	w8(c, T_STRING);
	w32(c, ncols);
	for (i = 0; i < ncols; i++)
		wstr(c, cols[i], strlen(cols[i]));
	wfld(c, T_MAP, 4);			/* attributes: empty */
	w8(c, T_STRING);
	w8(c, T_STRING);
	w32(c, 0);
	w8(c, T_STOP);
	flush(c);

	rmsg(c, fn);
	i = rrows(c, fn, out);
	done(c);
	return i;
}

void
hb_scan_close(HbConn *c, int32 sid)
{
	static const char *fn = "scannerClose";
	int			ft;

	wmsg(c, fn);
	wfld(c, T_I32, 1);
	w32(c, sid);
	w8(c, T_STOP);
	flush(c);

	rmsg(c, fn);
	while ((ft = r8(c)) != T_STOP)
	{
		int			fid = r16(c);

		if ((fid == 1 || fid == 2) && ft == T_STRUCT)
			rexc(c, fn, fid);
		else
			skip(c, ft, 0);
	}
}

/* ---------- writes (Part D) ---------- */

/* a void reply: field 1/2 = IOError / IllegalArgument, stop = done */
static void
void_reply(HbConn *c, const char *fn)
{
	int			ft;

	rmsg(c, fn);
	while ((ft = r8(c)) != T_STOP)
	{
		int			fid = r16(c);

		if ((fid == 1 || fid == 2) && ft == T_STRUCT)
			rexc(c, fn, fid);
		else
			skip(c, ft, 0);
	}
	done(c);
}

/* mutateRow: puts and cell deletes on one row, applied at once by HBase */
void
hb_mutate_row(HbConn *c, const char *tab, const char *key, int klen, HbMut *m, int n)
{
	static const char *fn = "mutateRow";
	int			i;

	wmsg(c, fn);
	wfld(c, T_STRING, 1);
	wstr(c, tab, strlen(tab));
	wfld(c, T_STRING, 2);
	wstr(c, key, klen);
	wfld(c, T_LIST, 3);
	w8(c, T_STRUCT);
	w32(c, n);
	for (i = 0; i < n; i++)
	{
		wfld(c, T_BOOL, 1);
		w8(c, m[i].del ? 1 : 0);
		wfld(c, T_STRING, 2);
		wstr(c, m[i].col, strlen(m[i].col));
		if (!m[i].del)
		{
			wfld(c, T_STRING, 3);
			wstr(c, m[i].val, m[i].vlen);
		}
		w8(c, T_STOP);			/* end of one Mutation */
	}
	wfld(c, T_MAP, 4);			/* attributes: empty */
	w8(c, T_STRING);
	w8(c, T_STRING);
	w32(c, 0);
	w8(c, T_STOP);
	flush(c);
	void_reply(c, fn);
}

/* mutateRows: n rows, ncol cell changes each (m is n * ncol), one round trip */
void
hb_mutate_rows(HbConn *c, const char *tab, const HbKey *keys, HbMut *m, int ncol, int n)
{
	static const char *fn = "mutateRows";
	int			i,
				j;

	wmsg(c, fn);
	wfld(c, T_STRING, 1);
	wstr(c, tab, strlen(tab));
	wfld(c, T_LIST, 2);
	w8(c, T_STRUCT);
	w32(c, n);
	for (i = 0; i < n; i++)
	{
		wfld(c, T_STRING, 1);	/* BatchMutation.row */
		wstr(c, keys[i].p, keys[i].len);
		wfld(c, T_LIST, 2);		/* BatchMutation.mutations */
		w8(c, T_STRUCT);
		w32(c, ncol);
		for (j = 0; j < ncol; j++)
		{
			HbMut	   *x = &m[i * ncol + j];

			wfld(c, T_BOOL, 1);
			w8(c, x->del ? 1 : 0);
			wfld(c, T_STRING, 2);
			wstr(c, x->col, strlen(x->col));
			if (!x->del)
			{
				wfld(c, T_STRING, 3);
				wstr(c, x->val, x->vlen);
			}
			w8(c, T_STOP);
		}
		w8(c, T_STOP);			/* end of one BatchMutation */
	}
	wfld(c, T_MAP, 3);			/* attributes: empty */
	w8(c, T_STRING);
	w8(c, T_STRING);
	w32(c, 0);
	w8(c, T_STOP);
	flush(c);
	void_reply(c, fn);
}

/* deleteAllRow: every cell of one row */
void
hb_delete_row(HbConn *c, const char *tab, const char *key, int klen)
{
	static const char *fn = "deleteAllRow";

	wmsg(c, fn);
	wfld(c, T_STRING, 1);
	wstr(c, tab, strlen(tab));
	wfld(c, T_STRING, 2);
	wstr(c, key, klen);
	wfld(c, T_MAP, 3);			/* attributes: empty */
	w8(c, T_STRING);
	w8(c, T_STRING);
	w32(c, 0);
	w8(c, T_STOP);
	flush(c);
	void_reply(c, fn);
}

/* isTableEnabled: a tiny reply whatever the table's size, to time one RTT */
void
hb_ping(HbConn *c, const char *tab)
{
	static const char *fn = "isTableEnabled";
	int			ft;

	wmsg(c, fn);
	wfld(c, T_STRING, 1);
	wstr(c, tab, strlen(tab));
	w8(c, T_STOP);
	flush(c);

	rmsg(c, fn);
	while ((ft = r8(c)) != T_STOP)
	{
		int			fid = r16(c);

		if (fid == 1 && ft == T_STRUCT)
			rexc(c, fn, 1);
		else
			skip(c, ft, 0);		/* field 0: the bool */
	}
	done(c);
}

/* getTableRegions: start and end key of every region, in key order */
int
hb_regions(HbConn *c, const char *tab, HbKey **st, HbKey **en)
{
	static const char *fn = "getTableRegions";
	int			ft,
				n = 0,
				i;

	*st = *en = NULL;
	wmsg(c, fn);
	wfld(c, T_STRING, 1);
	wstr(c, tab, strlen(tab));
	w8(c, T_STOP);
	flush(c);

	rmsg(c, fn);
	while ((ft = r8(c)) != T_STOP)
	{
		int			fid = r16(c);

		if (fid == 0 && ft == T_LIST)
		{
			if (r8(c) != T_STRUCT)
				proto_err("bad list<TRegionInfo>");
			n = rlen(c);
			*st = palloc0(sizeof(HbKey) * Max(n, 1));
			*en = palloc0(sizeof(HbKey) * Max(n, 1));
			for (i = 0; i < n; i++)
			{
				int			t;

				while ((t = r8(c)) != T_STOP)
				{
					int			id = r16(c);

					if (id == 1 && t == T_STRING)
						(*st)[i].p = rstr(c, &(*st)[i].len);
					else if (id == 2 && t == T_STRING)
						(*en)[i].p = rstr(c, &(*en)[i].len);
					else
						skip(c, t, 0);
				}
			}
		}
		else if (fid == 1 && ft == T_STRUCT)
			rexc(c, fn, 1);
		else
			skip(c, ft, 0);
	}
	done(c);
	return n;
}

double		hb_last_ms(HbConn *c) { return c->lastms; }
long		hb_last_in(HbConn *c) { return c->lastin; }
long		hb_bytes_in(HbConn *c) { return c->nin; }
double		hb_conn_ms(HbConn *c) { return c->connms; }
