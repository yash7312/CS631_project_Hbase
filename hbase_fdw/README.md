# hbase_fdw — query HBase tables from PostgreSQL

A read-only foreign data wrapper that reads HBase tables through the HBase
**Thrift1 gateway** (`hbase thrift start`, port 9090), and uses HBase's
row-key order **like a PostgreSQL index**:
- key lookups, lists, ranges and prefixes become HBase gets and range
  scans;
- the planner knows those are cheap and that the output is sorted;
- joins on the row key do one lookup per outer row (a nested-loop "index
  scan").

Developed and tested on PostgreSQL 18.6 (REL_18_STABLE, built as in the
CS631 setup guide). The code has PG16/17 branches and a derived PG16
expected file; Week 1 was tested on 16, but the later work was not.

- Week 1 = full scans with column projection.
- Week 2 = index-style access, see [Row key as an index](#row-key-as-an-index).

```sql
CREATE EXTENSION hbase_fdw;
CREATE SERVER hb FOREIGN DATA WRAPPER hbase_fdw OPTIONS (host 'localhost', port '9090');
CREATE FOREIGN TABLE emp (
  id     text OPTIONS (rowkey 'true'),
  name   text OPTIONS (column 'info:name') NOT NULL,
  salary int  OPTIONS (column 'pay:salary')
) SERVER hb OPTIONS (table 'emp');
SELECT dept, sum(salary) FROM emp GROUP BY dept;
```

---

## 1. Build it into your PostgreSQL tree

```bash
source ~/cs631/<your-env-file>.sh          # whatever you created per pgsql_demo.txt
which pg_config                             # must point into your install/
cd postgresql/contrib/hbase_fdw
make && make install
```

It builds with your debug flags (`-O0 -g`), so gdb/VS Code breakpoints in
`hbase_fdw.c` work like they do in the core code. It needs no
`shared_preload_libraries` and no restart; `CREATE EXTENSION` loads it.

After any change to the C files: `make && make install`, then reconnect psql
(a new backend loads the new `.so`).

## 2. Install HBase (standalone) on WSL

```bash
sudo apt install -y openjdk-11-jdk python3-pip
cd ~/cs631
# get the current "stable" binary tarball from https://hbase.apache.org/downloads.html
wget https://dlcdn.apache.org/hbase/stable/hbase-<ver>-bin.tar.gz
tar xzf hbase-<ver>-bin.tar.gz && ln -s hbase-<ver> hbase
```

`hbase/conf/hbase-env.sh`: add
```bash
export JAVA_HOME=/usr/lib/jvm/java-11-openjdk-amd64
```

`hbase/conf/hbase-site.xml` (standalone, data under ~/cs631/hbase-data):
```xml
<configuration>
  <property><name>hbase.cluster.distributed</name><value>false</value></property>
  <property><name>hbase.rootdir</name><value>file:///home/yash7312/cs631/hbase-data/hbase</value></property>
  <property><name>hbase.zookeeper.property.dataDir</name><value>/home/yash7312/cs631/hbase-data/zk</value></property>
  <property><name>hbase.unsafe.stream.capability.enforce</name><value>false</value></property>
</configuration>
```

Start HBase, then the **Thrift1** gateway (not `thrift2`) with its defaults,
which are binary protocol and unframed transport, as the FDW expects:
```bash
~/cs631/hbase/bin/start-hbase.sh
~/cs631/hbase/bin/hbase-daemon.sh start thrift -p 9090
~/cs631/hbase/bin/hbase shell <<< "status"      # should say 1 active master
```
Stop: `hbase-daemon.sh stop thrift; stop-hbase.sh`.
Web UI: http://localhost:16010.

WSL gotcha: if the master can't start with "connection refused 127.0.1.1",
change the `127.0.1.1 <hostname>` line in `/etc/hosts` to `127.0.0.1`.

## 3. Load test data and run the tests

```bash
pip3 install happybase                       # thrift client used only by the loader
cd ~/cs631/postgresql/contrib/hbase_fdw
python3 test/load_data.py localhost 9090 10000   # creates emp (10k rows), odd, ikey, bkey, ukey; writes test/emp.csv
make installcheck PGPORT=<your port>             # server must be running
```

`make installcheck` creates a regular table `emp_local` from the same CSV and
checks that `emp EXCEPT emp_local` and `emp_local EXCEPT emp` are both empty,
and that aggregates and filtered counts agree. Every pushdown case is also
cross-checked against `emp_local`, and EXPLAIN ANALYZE pins the number of
HBase RPCs.

The loader drops and recreates its tables, so re-run it after pulling a
version that adds test tables. `test/emp.csv` is deterministic (seed 631).

**Expected files.** `expected/hbase_fdw.out` is PG18. `expected/hbase_fdw_1.out`
(PG16/17) is generated from it, since the only difference is `rows=3.00` vs
`rows=3` in EXPLAIN ANALYZE:
```bash
cp results/hbase_fdw.out expected/     # after checking a deliberate change
python3 test/pg16_expected.py
```

Quick manual try with the hbase shell:
```
create 't1', 'cf'
put 't1', 'k1', 'cf:a', '10'
put 't1', 'k2', 'cf:a', '20'
put 't1', 'k2', 'cf:b', 'hello'
```
```sql
CREATE FOREIGN TABLE t1 (k text OPTIONS (rowkey 'true'), a int, b text)
  SERVER hb OPTIONS (family 'cf');
SELECT * FROM t1;           -- k1 | 10 | NULL ;  k2 | 20 | hello
```

### Without HBase: the mock server

`test/mock_hbase.py` is a small in-memory HBase Thrift1 server. It serves the
real `Hbase.thrift` IDL (copied from happybase), so the bytes on the wire are
the same as real HBase. It's handy for debugging the FDW quickly, and it's
what I developed and tested against:
```bash
pip3 install thriftpy2 happybase
python3 test/mock_hbase.py 9090 &
python3 test/load_data.py localhost 9090 10000
make installcheck PGPORT=<your port>
```
The whole suite passes on the mock with the same expected output as on real
HBase (checked in Week 2 with HBase's thrift gateway stopped).

---

## Options

| where | option | default | meaning |
|---|---|---|---|
| server | `host`, `port` | localhost, 9090 | Thrift gateway |
| server | `timeout` | 30 | seconds to wait on the network (0 = forever); cancel always works |
| table | `table` | foreign table name | HBase table |
| table | `family` | — | columns without a `column` option map to `family:<colname>` |
| table | `batch_size` | 1000 | rows per `scannerGetList` RPC (also sent as the scan's `caching`), and keys per `getRowsWithColumns` RPC |
| table | `rows` | 1000 | **table size** for the planner. Key estimates are derived from it (see below), so set it to roughly the real row count |
| column | `rowkey` | false | this column is the HBase row key (at most one) |
| column | `column` | — | `'family:qualifier'` |

**Type mapping.** HBase stores bytes. We assume they are the *text form* of
the value, which is what `hbase shell` `put` stores, and we run them through
the column type's input function (so `int`, `date`, `numeric`, `jsonb` and so
on all work). `bytea` columns get the raw bytes. A missing cell is NULL. A
zero byte in a text column is an error that tells you to use `bytea`. Bad
values report the HBase row key and column in `CONTEXT:`.

---

## Row key as an index

HBase keeps every table sorted by row key, comparing **unsigned bytes**, and
can seek to any key. The row key is its only index. The FDW uses it the way
Postgres uses a btree index.

### What reaches HBase

| condition on the `rowkey` column | HBase does | key types |
|---|---|---|
| `k = v` | one get | text, varchar, bytea, int2/4/8 |
| `k IN (...)`, `k = ANY (array)` | one batched get (`batch_size` keys per RPC); NULLs, `''` and duplicates dropped | same |
| `k < v`, `<=`, `>`, `>=`, `BETWEEN` | range scan `[start, stop)` | bytea; text/varchar only under a **C collation**; also `~<~ ~<=~ ~>=~ ~>~` on any text |
| `k LIKE 'abc%'`, `starts_with(k, 'abc')`, `k ^@ 'abc'` | range scan `['abc', 'abd')` | text/varchar (deterministic collation), bytea |
| `k LIKE 'abc'` (no wildcard) | one get | same |
| `k = other_table.col` in a join | one get **per outer row** (parameterized nested loop) | as for `=` (and IN/ranges with outer values) |

- **Values.** `v` can be a constant, a `$1` parameter, a sub-select result,
  or, in a join, a column of another table. Values are evaluated when the
  scan starts, again on every rescan.
- **Combining conditions.** Several conditions are intersected: e.g.
  `k IN (...) AND k > 'x'` fetches only the listed keys above `'x'`.
- **No-RPC cases.** A NULL value or an empty range sends **no RPC at all**.
- **Recheck.** Every condition is still checked locally (it stays in
  `Filter`). Pushdown only saves work; it never changes the result.

**Why ranges need `COLLATE "C"`.** Postgres sorts text by the column's
collation, so under `en_US`, `'a' < 'B'`, while in bytes `'B' < 'a'`.
Pushing a range under such a collation would silently lose rows, so it is
only pushed when the orders agree. Declare the key column with
`COLLATE "C"`, or write the collation in the query:
```sql
CREATE FOREIGN TABLE emp_c (id text OPTIONS (rowkey 'true') COLLATE "C", ...) ...;
SELECT * FROM emp WHERE id < 'e000100' COLLATE "C";
```
Note: libc's `C.UTF-8` (the default database collation here) does not count
as C for Postgres. `=`, `IN` and prefixes are safe under any deterministic
collation.

**Integer keys** are stored as text, which doesn't sort numerically
(`'10' < '9'`), so only `=` and `IN` are pushed. They must hold the canonical
text (`"42"`, not `"042"`). A non-canonical key is missed by a lookup but
still found by a full scan.

### EXPLAIN

```
 Foreign Scan on emp_c
   Filter: ((id >= 'e000100'::text) AND (id <= 'e000199'::text))
   HBase Table: emp
   HBase Key Cond: (id >= 'e000100'::text) AND (id <= 'e000199'::text)
   HBase Start Row: e000100          -- ANALYZE: the range read
   HBase Stop Row: e000199\x00
   HBase RPCs: 2
```
| field | meaning |
|---|---|
| `HBase Key Cond` | the conditions sent to HBase (like `Index Cond`); operators are byte comparisons, e.g. `~<~` shows as `<` |
| `HBase Start Row` / `Stop Row` | (ANALYZE) the scan range; stop is exclusive, `k\x00` = "just after k" |
| `HBase Get Keys` | (ANALYZE) keys asked for by a get, after dedupe and clipping |
| `HBase RPCs` | (ANALYZE) `scannerGetList` + `getRowsWithColumns` calls, summed over rescans |
| `HBase Families` / `Columns` / `Filter`, `Batch Size` | (VERBOSE) what is fetched |

### What the planner knows

- **Row estimates.**
  - `=` → 1 row (row keys are unique).
  - `IN` → the number of keys (10 for an unknown array).
  - Ranges and LIKE → Postgres's default selectivities (there are no
    statistics), times the `rows` option.
  - Conditions on other columns then filter that estimate.
- **Costs.** A connect, then one round trip per batch or get, plus a small
  per-row cost. The constants were calibrated on this setup (see Timings), so
  the planner picks a nested loop of key gets for small outer sides and a
  hash join over one full scan for large ones, about where the measured
  switch is.
- **Order.** Rows always come back in key byte order. For a bytea key, or a
  text/varchar key with a C collation, the planner is told so: `ORDER BY k`
  needs no Sort, and merge joins can use it. `DESC` still sorts.

---

## Timings

Real HBase 3.0 standalone and PG18 (`-O0 -g`) on the same WSL machine,
`emp` = 10k rows, `batch_size` 1000. Median of 10–50 runs (planning
included); the second column is a second round.

"Week 1" is the original FDW, built side by side as extension
`hbase_fdw_w1`. Scripts: `test/bench_setup.sql`, `test/bench.sql`.

| query | Week 1 (ms) | now (ms) | speed-up |
|---|---|---|---|
| `SELECT count(*) FROM emp` (full scan) | 18.6 / 19.2 | 19.1 / 20.3 | – (same work) |
| full scan + `GROUP BY` | 35.6 / 36.4 | 35.1 / 35.3 | – |
| point lookup `id = 'e004242'` | 36.8 / 37.7 | **1.47 / 1.36** | ~26× |
| point lookup, missing key | 35.9 / 38.1 | **1.16 / 1.01** | ~33× |
| `id IN (100 keys)` | 20.6 / 21.7 | **7.5 / 7.1** | ~3× |
| `BETWEEN`, 100 rows (C collation) | 18.8 / 20.2 | **2.4 / 2.3** | ~8× |
| `LIKE 'e0042%'`, 100 rows | 19.8 / 20.8 | **2.1 / 2.0** | ~10× |
| `ORDER BY id LIMIT 10` (C collation) | 20.0 / 20.6 | **3.8 / 4.0** | ~5× (no Sort, stops after 1 batch) |
| join, 20 outer rows on `id` | 18.2 / 18.4 | **13.1 / 13.2** | 1.4× (20 gets vs 1 full scan) |
| join, 1000 outer rows on `id` | 18.5 / 19.2 | 21.5 / 17.8 | – (both: hash join over a full scan) |

- **Full scans are unchanged.** The new code adds no overhead there.
- **Point lookups.** A Week-1 point lookup fetched every column of all 10k
  rows, which is why it was slower than `count(*)`.
- **Joins.** One get costs ~0.65 ms. A nested loop of gets beats a full scan
  up to about **25 outer rows** (measured: 20 rows 14 ms vs 19 ms; 30 rows
  27 ms vs 21 ms), and the planner switches at 25–30.

---

## Design (what to say when someone asks "why")

**Layout.**
- `hbase_thrift.c` is a ~500-line Thrift client: TBinaryProtocol, strict
  headers, over a buffered TCP socket.
- `hbase_fdw.c` holds the FDW callbacks.

**Why hand-roll Thrift instead of linking libthrift?** Apache's C++ library
uses C++ exceptions. PostgreSQL's `ereport(ERROR)` uses `longjmp`, and mixing
the two inside a backend is undefined behaviour waiting to happen. The c_glib
binding drags in GLib. The binary protocol itself is tiny (typed fields,
big-endian ints, length-prefixed strings), so a direct implementation is
smaller than the glue code would be. It reports errors with `ereport`,
allocates with `palloc`, and stays cancellable.

**RPCs used.**
- Scans: `scannerOpenWithScan(table, TScan, attrs)` → `scannerGetList(id, n)`
  (repeated until it returns 0 rows) → `scannerClose(id)`.
  - `TScan` carries `startRow`/`stopRow` (the key range), `caching =
    batch_size` (one batch per RPC instead of one row) and a server-side
    filter.
- Key lookups: `getRowsWithColumns(table, keys, columns, attrs)`, one round
  trip for up to `batch_size` keys. A single `=` is just a one-key get.

**Plan.**
- `GetForeignRelSize` collects the attributes the query needs (target list +
  quals, `pull_varattnos`) and estimates the rows.
- `GetForeignPaths` offers:
  - a plain path;
  - a parameterized path for each join clause on the row key. Those are
    found in `joininfo` and in the equivalence classes, as postgres_fdw
    does.
- `GetForeignPlan` picks the row-key conditions and puts their *values* in
  `fdw_exprs`. There, setrefs keeps Params and init-plans right, and the
  outer columns of a join become nestloop Params.
  - The executor evaluates `fdw_exprs` at every scan start into either a
    sorted, deduplicated key set (get) or a `[start, stop)` range (scan).
  - Every qual also stays in the local filter.

**Which columns we ask HBase for.** This needed the most thought. HBase
*drops rows that have no cell in the requested columns*. If we fetched only
the qualifiers the query uses, `SELECT name FROM t` could return fewer rows
than `SELECT count(*) FROM t`. So we define the relation as *every row key
with at least one cell in a mapped family*, and:
- by default we request the **whole mapped families** and project in the
  FDW (EXPLAIN VERBOSE shows `HBase Families:`);
- if some column is declared `NOT NULL` (you promise every row has that
  cell), we request only the **needed qualifiers plus that one**
  (`HBase Columns:`), which saves bandwidth on wide families;
- when no non-key column is needed (`count(*)`, or `SELECT id`), we add
  `FirstKeyOnlyFilter() AND KeyOnlyFilter()`, so HBase returns one empty cell
  per row (`HBase Filter:`).

The `odd` / `odd_x` tests show the consequence: a row whose only cells are in
an unmapped family is not part of `odd`, but is part of `odd_x`, which maps
that family.

**Execution and memory.**
- The scanner (or the first get) is issued lazily on the first `Iterate`,
  so `ReScan` only has to close it and reset state. The next `Iterate`
  re-evaluates the key values, which is how each outer row of a nested
  loop gets its own lookup.
- Each batch is decoded into its own memory context, which is reset before the
  next RPC, so a full scan's memory use is bounded by `batch_size`.
- Datum conversion happens in the executor's per-tuple context.

**Resource safety.**
- The socket is tied to the query's memory context through a
  `MemoryContextCallback`, so it is closed even when the query errors out or
  is cancelled. There is no fd leak (I checked `/proc/<pid>/fd` after repeated
  failing queries).
- Network waits `poll()` in 1s slices and call `CHECK_FOR_INTERRUPTS()`, so
  `statement_timeout` and Ctrl-C work mid-RPC. The test showed a cancel at
  1.000s.

**EXPLAIN** shows the HBase table, the key conditions, families or columns,
and the filter. With `ANALYZE` it also shows the range or key count, and the
number of HBase RPCs. The rescan test checks that a nested loop on the key
costs 1 RPC per outer row (3), while one on a non-key column still costs
3 × (20 batches + 1 empty) = 63.

### Limitations
- Reads the latest version of each cell only.
- **Collations.**
  - Ranges are pushed only for bytea or C-collated text (see above).
  - Non-deterministic (ICU) collations are handled by the code but untested:
    this PG build has no ICU.
- **Estimates.**
  - No `ANALYZE` support. The `rows` option is the table size, and range or
    LIKE estimates are default guesses.
  - Join size estimates on the key are poor (Postgres assumes 200 distinct
    keys), though the path choice is still right.
- **Joins.** A parameterized lookup is 1 RPC per outer row. For an inner
  or LEFT join on the row key, the batched join (`hbase_fdw.batch_join`)
  fetches K outer keys per call instead (v1: the outer key must be a plain
  column, no subqueries in the join clauses).
- **LIMIT.** A plain scan under `LIMIT n` asks HBase for n rows per call
  instead of `batch_size` (EXPLAIN VERBOSE: `HBase Row Limit`), but only
  when nothing above it can drop or reorder rows: one table, no join, no
  sort on another column, and every WHERE condition a pushed row-key
  comparison. Otherwise the scan fetches full batches as before.
- **Parallel scans.** A plain scan (no `=`/`IN` key lookup, not the inner
  side of a lookup join) can run as a `Parallel Foreign Scan` under a
  `Gather`. At plan time the FDW asks HBase once per session how many
  regions the table has (`getTableRegions`). A table with one region is
  never parallel. Otherwise it picks the fewest workers that still give each
  process as few regions as possible: 4 regions and at most 2 workers means
  1 worker scanning 2 regions, and 3 workers means 1 region each. The cap is
  `hbase_fdw.parallel_workers` (default 8) and `max_parallel_workers_per_gather`.
  At run time the leader puts the region keys in shared memory. The leader
  and each worker take the next region with an atomic counter and scan only
  the part of it inside the key range, each over its own connection. The
  cost is the busiest process's share of the serial scan (round trips, rows
  and bytes), plus 3 calls per extra region. With measured costs it also
  credits back most of Gather's fixed charges: `parallel_setup_cost` (50 ms
  on our scale) and `parallel_tuple_cost` (5 us a row). Real HBase showed
  about 1 ms per worker and 2 us per row. It is only offered when the scan
  fetches at least two batches. EXPLAIN ANALYZE shows `HBase Regions` and the leader's own counts:
  workers keep theirs.
- **Writes.** INSERT, UPDATE and DELETE need a rowkey column.
  - INSERT is an upsert: an existing key is overwritten, as HBase puts are.
  - No rollback: each row is written to HBase at once, so `ROLLBACK` does
    not undo it, and a failing statement keeps the rows written before it.
  - UPDATE cannot change the row key (delete and insert instead); setting a
    column to NULL deletes its cell; a row with no cells left disappears.
  - `DELETE ... RETURNING` returns no column values.
  - INSERT sends `insert_batch` rows (table option, default 100) per
    `mutateRows` call, one round trip per batch; not with RETURNING or row
    triggers. UPDATE and DELETE are one call per row.
- **Not used for:** `DESC` order (an HBase reversed scan would do it), and
  `NOT IN` / `<>`.
- **Connections and scanners.**
  - One connection per scan node, with no connection caching.
  - If a query errors mid-scan, the server-side scanner isn't closed. HBase
    expires it after the scanner lease (60s by default).
  - The Thrift server drops connections that sit idle longer than
    `hbase.thrift.server.socket.read.timeout` (60s). That could only bite a
    huge nested loop that leaves the outer scan idle for over a minute.

### Why not a real index access method?
Option 2 of the assignment was a literal `IndexAmRoutine`. Postgres refuses
`CREATE INDEX` on a foreign table, and an index AM returns heap TIDs, so it
could only wrap a local stub table and call the same HBase lookups. Doing it
inside the FDW gives the same access paths (point, list, range, prefix,
ordered, parameterized for joins) with real planner integration and no stub
table.

## Files
```
hbase_fdw.c          FDW callbacks, option validation, column mapping
hbase_thrift.[ch]    Thrift1 binary-protocol client
hbase_fdw--1.0.sql   handler/validator + CREATE FOREIGN DATA WRAPPER
sql/, expected/      regression test (expected/hbase_fdw.out = PG18, _1 = PG16/17)
test/mock_hbase.py   in-memory HBase Thrift1 server for development
test/load_data.py    loads test tables via happybase, writes test/emp.csv
test/pg16_expected.py  derives expected/hbase_fdw_1.out from hbase_fdw.out
test/bench_setup.sql, test/bench.sql   the timing benchmark
test/Hbase.thrift    the Thrift1 IDL (from happybase), for reference
```
