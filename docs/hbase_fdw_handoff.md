# hbase_fdw — handoff

Everything someone needs to pick up this project cold: what it is, how to run
it, how the code fits together, every decision we made and why, and where each
new piece of code lives.

- Companion file: `~/cs631/hbase_fdw_logs.md` is the chronological story
  (problem → change → result per phase).
- This file is the **reference**: organised by topic, updated at the end of
  every phase.

Line numbers are as of the last update; they drift, so search by name.

Code dir (all paths below are relative to it unless absolute):
`~/cs631/postgresql/contrib/hbase_fdw`

---

## 1. Goal and current status

**Assignment.** Query HBase directly from PostgreSQL without loading the data,
and use HBase's index (its row-key order) the way Postgres uses an index:
"an index wrapper which acts like a PostgreSQL index but really uses HBase".

**Approach chosen (option 1).** Index-style access paths *inside the FDW*:
row-key conditions become HBase start/stop rows, the planner is told these are
cheap, and parameterized paths give nested-loop "index" lookups for joins.
A literal `IndexAmRoutine` (option 2) was deferred. Postgres refuses
`CREATE INDEX` on foreign tables, and an index AM must return heap TIDs, so it
could only be a thin wrapper on top of option 1, on a local stub table.

| Phase | What | Status |
|---|---|---|
| 0 | Green baseline (error text on real HBase) | done |
| 1 | `rowkey = value` → HBase point lookup | done |
| 2 | Ranges, `BETWEEN`, `LIKE 'prefix%'`, `starts_with` | done |
| 3 | `IN` / `= ANY` via `getRowsWithColumns`; single `=` in 1 RPC | done |
| 4 | Row estimates + cost model + pathkeys (key order) | done |
| 5 | Parameterized paths (joins use the "index") | done |
| 6 | README, mock run, timings, cost calibration | done |

The phase plan with its checkboxes is in the logs file.

---

## 2. Environment and everyday commands

| Thing | Value |
|---|---|
| PostgreSQL | 18 (REL_18_STABLE), built from `~/cs631/postgresql`, installed in `~/cs631/postgresql/install`, port **9432**, db `test`, `-O0 -g`, **no ICU** |
| HBase | 3.0.0 standalone, `~/cs631/hbase` → `hbase-3.0.0`, data in `~/cs631/hbase-data` |
| Thrift | **Thrift1** gateway (not thrift2), port 9090, binary protocol, unframed |
| Web UI | http://localhost:16010 |
| Python | `happybase` installed (used by the loader) |

```bash
# start
pg_ctl -D $PGDATA -l $PGDATA/logfile start
~/cs631/hbase/bin/start-hbase.sh
~/cs631/hbase/bin/hbase-daemon.sh start thrift -p 9090

# build + install (reconnect psql afterwards: new backend loads new .so)
cd ~/cs631/postgresql/contrib/hbase_fdw && make && make install

# load test data into HBase (emp 10k rows, odd, ikey) + writes test/emp.csv
python3 test/load_data.py localhost 9090 10000

# regression tests (needs PG + HBase + loaded data)
make installcheck PGPORT=9432       # diffs → regression.diffs
cp results/hbase_fdw.out expected/   # after reviewing a deliberate change
python3 test/pg16_expected.py        # then derive the PG16/17 expected file

# HBase shell
~/cs631/hbase/bin/hbase shell
```

**Debugging tips.**
- Server log: `$PGDATA/logfile`.
- Full HBase error text, including the Java stack trace and the RPC name:
  `SET client_min_messages = debug1;`
- gdb **cannot attach** to a backend here (`/proc/sys/kernel/yama/ptrace_scope`
  = 1). Use `sudo gdb -p <pid>`, or read the source.

**Test databases.**
- `make installcheck` uses its own db, `contrib_regression`.
- Your `test` db has server `hb` and foreign table `t1` (`k` rowkey, `a`,
  `b`, family `cf`).

---

## 3. Architecture

```
hbase_fdw.c       FDW callbacks: options, column mapping, planning, execution, EXPLAIN
hbase_thrift.c/.h hand-written Thrift1 client (TBinaryProtocol over TCP)
hbase_fdw--1.0.sql handler + validator + CREATE FOREIGN DATA WRAPPER
sql/hbase_fdw.sql  regression test;  expected/hbase_fdw.out (PG18), _1.out (PG16)
test/load_data.py  loads emp/odd/ikey into HBase, writes test/emp.csv
test/mock_hbase.py in-memory Thrift1 server (dev without HBase)
test/Hbase.thrift  Thrift1 IDL, reference for adding RPCs
```

### Query lifecycle

| Stage | Function | What it does |
|---|---|---|
| plan | `hbGetForeignRelSize` | builds `HbRelInfo` in `rel->fdw_private` (options, needed attnos, rowkey attno); `rel->rows` = `estimate(baserestrictinfo)` |
| plan | `hbGetForeignPaths` | the plain path (parameterized only by `lateral_relids`) plus one **parameterized path** per set of outer rels a rowkey join clause needs; each via `add_hb_path` (estimate + cost + `key_pathkeys`) |
| plan | `hbGetForeignPlan` | `key_conds()` on the path's clauses (`baserestrictinfo` + `ppi_clauses` if parameterized) → `fdw_exprs`; outer Vars there become nestloop Params (core's `replace_nestloop_params`); **all** quals stay local in `qual` |
| exec | `hbBeginForeignScan` | read options, `setup_cols()` decides what to fetch, init `KeyCond`s, connect |
| exec | `hbIterateForeignScan` | first call: `set_bounds()`. Then either **get mode** (key set: `hb_get_rows()` for `batch_size` keys per call, no scanner) or **scan mode** (`hb_scan_open()`, then batches via `hb_scan_next()`). `fill()` converts a row into a slot |
| exec | `hbReScanForeignScan` | close the scanner if open, clear `begun`; the next Iterate re-evaluates bounds |
| exec | `hbEndForeignScan` | close the scanner and the socket |
| explain | `hbExplainForeignScan` | HBase Table / Key Cond / Start+Stop Row / Families / RPCs |

### Plan ↔ executor contract (must stay in sync)

`ForeignScan.fdw_private` = `list_make3(atts, kops, makeString(kname))`

| idx | content | type | read at |
|---|---|---|---|
| 0 | attnos needed (0 = whole row) | `List` of int | `hbBeginForeignScan` → `setup_cols` |
| 1 | strategy per key cond, aligned with `fdw_exprs`: btree 1–5 (`<` `<=` `=` `>=` `>`), `KC_PREFIX` (6), `KC_LIKE` (7), `KC_IN` (8) | `List` of int | Begin, Explain |
| 2 | rowkey column name (`""` if none) | `String` | Begin → `st->kname` |

`ForeignScan.fdw_exprs` = the *value* side of each pushed key condition
(the column side is implicit: it is always the rowkey). The strategy is
already normalised to "rowkey op value": `'x' > k` is stored as `<`. For
`KC_LIKE` the value is the *pattern*; its prefix is extracted at scan open.
For `KC_IN` the value is the *array* (Const, Param, `ARRAY[...]`, init plan).

### Invariants (rely on these; break them knowingly)

1. **Every pushed qual is also in `plan->qual`.** That means:
   - Pushdown can only drop rows that the local filter would drop anyway,
     provided invariant 6 holds.
   - `hbExplainForeignScan` relies on EXPLAIN having just set
     `es->deparse_cxt` to our node (it does so while printing `Filter`).
2. **Bounds are computed at scan open, not at Begin.** Every rescan
   re-evaluates them. Prepared statements and Phase 5's nestloop Params
   (one key per outer row) depend on this.
3. **Key bytes = what HBase stores**: the raw bytes for text/varchar/bytea,
   and the output function's text otherwise.
4. **HBase stop row is exclusive.** A single key `k` is `[k, k\0)`; `k\0` is
   the smallest key after `k`, so `> k` → start `k\0`, `<= k` → stop `k\0`.
   `HbState.start/stop == NULL` means unbounded; we never store an empty
   start (HBase has no empty keys) or an empty stop (`< ''` makes
   `set_bounds` return "no row" instead).
5. **A row exists iff it has a cell in a mapped family** (Week-1
   definition). Key-only scans use `FirstKeyOnlyFilter() AND KeyOnlyFilter()`.
6. **Only push an operator whose Postgres meaning equals HBase byte order**
   (see `key_strategy`). Breaking this does not crash; it silently drops rows.
   The local recheck cannot save you, because the rows were never fetched.
7. **A get and a scan agree on which rows exist.** Both send the same
   column list (`hcols`). HBase leaves out a row with no cell in it, whether
   from a Get (`rowResultFromHBase` skips empty results) or a Scan. The mock
   does the same. Never call `getRowsWithColumns` with an empty column list:
   it would return rows that a scan hides.
8. **The key set is sorted and distinct.** Duplicate keys would give
   duplicate rows. Sorting makes get mode return rows in scan order.
9. **Every path's output is in rowkey byte order** (scan or get), and
   `key_pathkeys` advertises that. Anything that breaks the order (a
   reversed scan, parallel fetch, unsorted keys) must drop the pathkeys, or
   merge joins and `ORDER BY` give **wrong results**. A forced merge join
   relies on it.

---

## 4. Decisions register

Format: decision → why → alternatives rejected.

### Pre-existing (Week 1, inherited, not ours)
- **Hand-written Thrift client** instead of libthrift: C++ exceptions vs
  Postgres `longjmp`; c_glib pulls in GLib. Errors use `ereport` and memory
  uses `palloc`.
- **Fetch whole families by default.** HBase drops rows that have no cell in
  the requested columns. Qualifier projection is only used when some column
  is `NOT NULL`.
- **One connection per scan node**, closed via a memory-context callback.
  Cancellation works through `poll()` in 1s slices plus
  `CHECK_FOR_INTERRUPTS()`.

### Phase 0
- **Shorten HBase error messages** to the first line, and strip the Java
  package from the exception class.
  - *Why:* real HBase sends the stack trace (~80 lines).
  - *Rejected:* keeping the full text (unreadable, and the test can't match it).
- **Drop the RPC name from the user-visible error**; log it at DEBUG1 instead.
  - *Why:* HBase 3 opens scanners lazily, so the same error comes from
    `scannerOpenWithScan` (mock) or `scannerGetList` (real).
  - *Rejected:* making the mock lazy too (the message would still depend on
    the server version).

### Phase 1
- **Push down only `rowkey = value`** in this phase. Ranges are Phase 2
  because of the collation issue (see below).
- **"Value" = no Vars, no volatile functions, no SubPlans.** This admits
  Const, `$1` (PARAM_EXTERN) and init-plan results (PARAM_EXEC).
  - *Why:* all of these are fixed for the duration of one scan.
  - *Rejected:* Const only (it would lose prepared statements and sub-selects
    for no gain).
- **Operator whitelist by function OID** (`byte_eq_op`): `texteq` (only under
  a deterministic collation), `byteaeq`, and the int2/4/8 equality family
  including cross-type.
  - *Why:* Postgres equality must equal HBase byte equality.
  - *Rejected:* "any btree equality" (`numeric` `1.0 = 1.00`, `float`
    `1e1 = 10`, and `date` depends on DateStyle, so the bytes differ).
  - *Assumption:* integer keys are stored canonically (`"42"`, not `"042"` or
    `"+42"`). A non-canonical key is missed by a lookup but found by a full
    scan. This is documented and accepted.
- **Values go in `fdw_exprs`, not `fdw_private`.**
  - *Why:* setrefs fixes up `fdw_exprs` (Param numbering, init-plans,
    nestloop params in Phase 5) and never touches `fdw_private`.
- **Keep the pushed qual as a local filter too.**
  - *Why:* the recheck costs nothing and makes pushdown result-neutral.
  - *Rejected:* removing it from `qual` (risky for little gain).
- **Short-circuit with no RPC** when the value is NULL, the key is empty
  (HBase has no empty keys), or two `=` keys disagree.
- **Use the existing scan RPC with `[k, k\0)`** for point lookups, rather than
  a new `getRow` RPC.
  - *Why:* reuses tested code.
  - *Cost:* 2 `scannerGetList` calls per lookup (plus open/close). Phase 3
    fixes this.
- **Deparse the EXPLAIN text at EXPLAIN time, not plan time.**
  - *Why:* the plan-time `deparse_expression(val, NIL, …)` **segfaulted** on
    PARAM_EXEC, because `find_param_referent` does
    `linitial(context->namespaces)`.
  - EXPLAIN-time deparse with `es->deparse_cxt` also prints
    `(InitPlan 1).col1` correctly.
- **The test helper `hb_explain()` strips `(actual …)`.**
  - *Why:* PG18 prints `rows=1.00` and PG16 prints `rows=1`, so new tests give
    identical output on both.
  - `_1.out` was derived by re-applying the old PG16-only hunks (Phases 1–4); since Phase 5 it is generated by `test/pg16_expected.py`.

### Phase 2
- **Ranges are pushed only when Postgres order = unsigned byte order.**
  - That means text under a C collation (`coll_is_c`, i.e. Postgres's own
    `collate_is_c`), any bytea, and the `text_pattern_ops` operators
    `~<~ ~<=~ ~>=~ ~>~` (memcmp) under any collation.
  - *Why:* a wrongly pushed range silently loses rows.
  - *Rejected:* trusting libc `C.UTF-8`. glibc's C.UTF-8 does sort by code
    point (= UTF-8 byte order), but Postgres doesn't treat it as C, and
    copying that knowledge is fragile across platforms. Users write
    `COLLATE "C"` on the key column, or in the query.
  - *Rejected:* ranges on int keys (`'10' < '9'` in bytes). The only fix
    would be a fixed-width key encoding, which is out of scope.
- **`LIKE` and `starts_with`/`^@` → `[p, p+1)`** under any *deterministic*
  collation. A byte prefix doesn't depend on the collation.
  - `p+1` = drop trailing 0xFF bytes, then bump the last byte. An all-0xFF
    or empty prefix has no stop.
  - A pattern with no wildcard is treated as `=`.
  - *Rejected:* ILIKE (case folding is not a byte prefix).
- **The LIKE prefix is extracted at scan open, not at plan time.**
  - *Why:* `LIKE $1` and `LIKE outer.col || '%'` then work too. At plan time
    we only use a constant pattern to *skip* pushing one with no prefix
    (`'%x'`), so EXPLAIN doesn't show a useless Key Cond.
  - Escapes: backslash only. `LIKE ... ESCAPE '#'` arrives already rewritten
    to backslash form by `like_escape()` (a non-Var, stable expression → it
    is a valid "value").
  - Multibyte text: whole characters are copied with `pg_mblen`, so a
    backslash byte inside a multibyte character (non-UTF-8 encodings) is
    not taken as an escape.
- **Intersect all bounds at run time into one `[start, stop)`.** Take the max
  of the starts and the min of the stops; start ≥ stop → 0 RPCs.
  - *Rejected:* intersecting at plan time (it would lose `$1` and
    init-plans, which Phase 1 already supports).
- **Match `FuncExpr` as well as `OpExpr`.** All lookups are by function OID,
  so `starts_with(k, 'x')` (a function call) and `^@` (an operator) share one
  code path.
- **Store the strategy normalised to "key op value".** A commuted clause is
  flipped with `6 - s` (1↔5, 2↔4). EXPLAIN therefore prints `(id < 'x')` for
  `'x' > id`, and `<` for `~<~`. The original clause is still visible in
  `Filter`.

### Phase 3
- **`IN` / `= ANY` → Thrift1 `getRowsWithColumns`**, `batch_size` keys per
  call.
  - *Why:* one round trip for N keys. HBase groups the gets by region server
    itself.
  - *Rejected:* one `getRow` per key (N round trips); a min..max range scan
    (reads everything in between).
- **Route a single `=` through the same get path.** A point lookup is 1 RPC
  instead of open + 2×getList + close (4 round trips).
  - *Rejected:* a separate `getRow` code path (the same work, one more RPC to
    maintain).
- **`=` and `IN` build a key set; every other condition builds the range.**
  Key sets are intersected; the result is clipped to `[start, stop)`.
  - *Why:* `IN (...) AND k > 'x'` and `= 'a' AND IN ('a','b')` come out
    right with no special cases.
- **Sort + dedupe the keys at scan open** (byte order, `kcmp`).
  - *Why:* duplicates in a list must not give duplicate rows, and sorting
    makes the output order match a scan (byte order). For int keys that is
    `1, 12, 5`; Postgres adds a Sort if `ORDER BY` asks for it.
- **Drop NULL elements and `''`.** `k = NULL` is never true, and HBase has no
  empty keys. If nothing is left (including a NULL array or `'{}'`): 0 RPCs.
- **Get mode doesn't use the key-only filter.** `getRowsWithColumns` has no
  filter argument, so `count(*) … WHERE id IN (...)` fetches the mapped
  families of those rows. That is cheap: it is only the requested rows.
- **`NOT IN` / `<> ALL` stays local.** It excludes keys; HBase can't skip
  anything.
- **EXPLAIN ANALYZE shows `HBase Get Keys: N`** (the keys asked for after
  dedupe and clipping) in get mode, instead of Start/Stop Row.

### Phase 4
- **The `rows` option stays the table size;** we don't ask HBase.
  - *Why:* HBase has no cheap exact count (it would be a full key-only
    scan). Region-size based guesses aren't exposed by Thrift1.
  - *Rejected:* an ANALYZE callback (`AnalyzeForeignTable`). Possible later,
    but it would scan the whole table.
- **Key sets give exact upper bounds:** 1 row per `=`, the array length for
  `IN` (row keys are unique). `estimate_array_length` returns 10 for an
  unknown array (`$1`, init plan), which is Postgres's own convention.
- **Ranges use the stock selectivity functions** on the pushed quals. With no
  stats that means defaults (1/3, 0.5%). They are crude, but they keep
  "range < full scan".
  - *Rejected:* parsing start/stop bounds into a fraction of key space (it
    needs key distribution knowledge we don't have).
- **Rows = fetched × local-qual selectivity; cost is driven by `fetched`.**
  - *Why:* a non-key filter makes the result smaller but not the HBase work
    (the full table is still read).
- **Cost constants are `#define`s, not options:** `HB_CONNECT_COST` 10,
  `HB_RPC_COST` 5 → **15**, `HB_ROW_COST` 0.01 → **0.005** (recalibrated in
  Phase 6, see below).
  - *Why:* nothing needs tuning yet. postgres_fdw-style
    `fdw_startup_cost`/`fdw_tuple_cost` options are an easy add if Phase 6
    timings call for it.
- **Pathkeys only for bytea, or text/varchar with a C collation on the
  column;** `create_it = false` (only when the query has a matching
  EquivalenceClass).
  - *Why:* key order = byte order, and only those types sort that way in
    Postgres.
  - A query-level `COLLATE "C"` on a default-collation column doesn't match
    (it is a different expression).
  - *Rejected:* DESC via HBase reversed scans (`TScan.reversed`). Possible,
    but not needed now.

### Phase 5
- **Parameterized ForeignPaths, found as postgres_fdw does:** `joininfo` plus
  equivalence classes (`generate_implied_equalities_for_column` with a
  rowkey-matching callback), filtered by `join_clause_is_movable_to` and
  `key_cond`, one `ParamPathInfo` per distinct `required_outer`.
  - *Why:* a plain `a.k = b.x` lives only in an EC, so `joininfo` alone
    would miss the most common join.
- **"Value" now means "no Vars of this rel"** (`scan_const(n, relid)`), not
  "no Vars". Outer Vars become nestloop Params through core's
  `replace_nestloop_params` on `fdw_exprs`. No executor change was needed.
  - *Rejected:* building our own Params (core already does it, and does it
    correctly for `fdw_exprs`).
  - PlaceHolderVars are rejected outright (they might be computed at our
    level). This is simple and safe.
- **Any key condition can parameterize**, not just `=`: `IN`/`= ANY` with
  outer values, and ranges/LIKE under C collation. The cost decides.
- **A parameterized path charges no connect cost.** The connection is per
  plan node and survives rescans; core costs a ForeignScan rescan as a full
  re-run (`cost_rescan` default), so including it would charge every outer
  row.
- **The plain path takes `rel->lateral_relids` as `required_outer`.** A
  LATERAL reference in a key condition then becomes a nestloop Param.
- **Each path estimates for itself** (`estimate()` on its own clause list);
  `HbRelInfo` keeps only options, `used` and `katt`.
- **PG16 expected file generated by script** (`test/pg16_expected.py`):
  `rows=N.00` → `rows=N` in EXPLAIN ANALYZE blocks, header re-padded.
  - *Rejected:* re-applying the old diff (it breaks as soon as the
    PG16-specific block changes, which Phase 5 did).

### Phase 6
- **Calibrate costs against measured nested-loop/hash-join switch points**,
  not just the raw RPC:row time ratio.
  - Measured: RPC ≈ 0.65 ms, scanned row ≈ 1 µs (≈ 700:1); the real switch
    is at ~25 outer rows against 10k rows.
  - With RPC 10 (the raw ratio) the planner switched at 40–60. With RPC 15
    and row 0.005 it switches at 25–30.
  - *Why the extra:* core's hash-join CPU costs (`cpu_operator_cost` etc.)
    are not wall-clock calibrated, so matching the decision mattered more
    than matching a unit.
  - Machine-specific: a remote HBase would have a higher RPC cost. This is
    the case for server options (`fdw_startup_cost` / `fdw_tuple_cost`
    style), which aren't added.
- **Benchmark against the real Week-1 code**, built as a separate extension
  `hbase_fdw_w1`, in its own database.
  - *Rejected:* "disable pushdown" switches in the new code. That isn't the
    Week-1 behaviour (e.g. its cost model differs) and it adds a GUC nobody
    needs.
- **Medians of 10–50 runs, planning included, two rounds.** Full scans vary
  ±10% between runs, so only the big differences are claimed.

---

## 5. Symbol index

### `hbase_fdw.c`

| Symbol | Line | Phase | Purpose |
|---|---|---|---|
| `HB_CONNECT_COST`, `HB_RPC_COST`, `HB_ROW_COST` | 84 | 4 | cost constants (seq_page_cost units) |
| `HbRelInfo` (struct) | 119 | 4, 5 | `rel->fdw_private`: `o` (options), `used` (attnos), `katt` (rowkey attno) |
| `KC_PREFIX`, `KC_LIKE`, `KC_IN` | 139 | 2, 3 | strategies 6, 7, 8, after btree's 1–5 |
| `KeyCond` (struct) | 144 | 1–3 | one pushed key condition: `x`, `op`, `raw`, `text`, `out`; `elem`/`elen`/`ebyval`/`ealign` for `KC_IN` |
| `HbState` key fields | 158+ | 1–3 | `kc`/`nk`/`kname`/`kcxt`; `start`/`stop`; `get`, `keys`, `nkeys`, `kpos` (177); `begun` (184) |
| `rowkey_att()` | 360 | 1 | attno of the `rowkey 'true'` column (opens the relation; called once, in RelSize) |
| `is_key_var()` | 387 | 1 | is this node our rowkey Var (through `RelabelType`)? |
| `coll_is_c()` | 399 | 2 | does a collation sort like memcmp? |
| `key_strategy()` | 421 | 2 | function OID + collation + side → strategy or 0 (the operator whitelist) |
| `like_prefix()` | 495 | 2 | fixed prefix of a LIKE pattern, plus `*exact` |
| `scan_const()` | 524 | 2, 5 | value fixed for one scan of `relid`: no Vars *of relid* (outer Vars OK), no PHVs, nothing volatile, no SubPlans |
| `key_cond()` | 550 | 5 | one RestrictInfo → is it a pushable key condition? sets `*val`, `*op` (was the loop body of `key_conds`) |
| `key_conds()` | 619 | 1–5 | all pushable clauses → `*exprs`, `*ops`, `*ris`; takes `katt` (Phase 5) |
| `estimate()` | 650 | 4, 5 | rows for a clause list; `*fetched` (rows HBase sends), `*get` |
| `hbGetForeignRelSize()` | 693 | 4 | builds `HbRelInfo`; `rel->rows = estimate(baserestrictinfo)` |
| `key_pathkeys()` | 724 | 4 | rowkey ASC pathkeys for bytea / C-collated text or varchar |
| `add_hb_path()` | 753 | 5 | estimate + cost + `create_foreignscan_path` for a clause list and `required_outer`; no connect cost if parameterized |
| `ec_is_key()` | 791 | 5 | EC member callback: is it our rowkey Var? |
| `hbGetForeignPaths()` | 803 | 4, 5 | plain path (with `lateral_relids`) + parameterized paths from joininfo/EC rowkey clauses |
| `hbGetForeignPlan()` | 855 | 1, 5 | `key_conds(ri->katt, clauses)` → `fdw_exprs`; rowkey name via `get_attname` |
| `hbBeginForeignScan()` | 985 | 1–3 | builds `kc[]`, creates `kcxt` |
| `kcmp()` / `kcopy()` | 1114 / 1123 | 2 | unsigned byte compare / copy plus a `\0` byte |
| `raise_start()` / `lower_stop()` / `prefix_range()` | 1136 / 1150 / 1171 | 2 | intersect a bound; `[p, p+1)` with the 0xFF carry |
| `key_bytes()` | 1187 | 3 | value datum → key bytes |
| `hbkey_cmp()` / `sort_keys()` / `intersect_keys()` | 1204 / 1214 / 1229 | 3 | comparator; sort + dedupe; merge-intersect |
| `set_bounds()` | 1257 | 1–3 | `=`/`IN` → key set, others → range; clip; sets `get`; false = no row can match |
| `hbIterateForeignScan()` | 1387 | 1, 3 | get mode → `hb_get_rows`; scan mode → scanner |
| `hbReScanForeignScan()` | 1455 | 3 | closes the scanner, clears `begun` |
| `show_key()` | 1481 | 1 | key bytes → printable text |
| `hbExplainForeignScan()` | 1502 | 1–3 | `HBase Key Cond` (outer Params deparse as `l.col`); ANALYZE: `Get Keys` or `Start/Stop Row` |

New includes (Phase 1): `access/stratnum.h`, `nodes/nodeFuncs.h`,
`optimizer/clauses.h`, `utils/fmgroids.h`, `utils/ruleutils.h`.
Phase 2: `utils/pg_locale.h`. Phase 3: `utils/array.h`. Phase 4:
`<math.h>`, `optimizer/paths.h`, `utils/selfuncs.h`, `utils/typcache.h`.

### `hbase_thrift.c` / `.h`

| Symbol | Line | Phase | Purpose |
|---|---|---|---|
| `short_msg()` | 494 | 0 | first line only; strips the `java.pkg.` prefix if the text before `:` looks like a class name |
| `rexc()` | 517 | 0 | `DEBUG1` full message with the RPC name; user error = `hbase_fdw: <short msg>` |
| `hb_scan_open()` | 544 | – | sends `TScan.startRow` (1), `stopRow` (2), `columns` (4), `caching` (5), `filterString` (6) |
| `rrows()` | 695 | 3 | reads a `list<TRowResult>` reply (field 0, or IOError/IllegalArgument); factored out of `hb_scan_next` |
| `hb_get_rows()` | 746 | 3 | `getRowsWithColumns(table, keys, cols, {})` → rows in request order, missing keys left out |
| `HbKey` (struct) | `.h` 35 | 3 | `{p, len}`, one row key |

### Tests and data

| Item | Where | Phase | Notes |
|---|---|---|---|
| `hb_explain(q)` | `sql/hbase_fdw.sql` ~59 | 1 | EXPLAIN ANALYZE filtered to Scan/Filter/HBase/Result lines, version-neutral; dropped at the end of the test (it is not an extension member) |
| point-lookup cases | `sql/hbase_fdw.sql` after "row-key point lookups" | 1 | hit, miss, prefix≠match, `$1`, init plan, NULL/empty key, volatile/`<>` not pushed, cross-check vs `emp_local`, varchar/bytea/int keys |
| HBase table `ikey` | `test/load_data.py` | 1 | keys `'1'`..`'12'`, `d:sq` = key² (for int row keys) |
| foreign tables `emp_vc`, `odd_b`, `ikey` | `sql/hbase_fdw.sql` | 1 | varchar key on emp, bytea key on odd, int key on ikey |
| range/prefix cases | `sql/hbase_fdw.sql` from "row-key ranges" to "what gets fetched" | 2 | each operator, BETWEEN, commuted, empty range, `< ''`, not-pushed cases, `$1` bounds/patterns, escapes, multibyte, 0xFF carry, two correlated cross-checks vs `emp_local` |
| foreign table `emp_c` | `sql/hbase_fdw.sql` | 2 | emp with `id ... COLLATE "C"`: ranges pushed |
| key-list cases | `sql/hbase_fdw.sql` "key lists" (before "what gets fetched") | 3 | dups/NULL/`''`/missing, `= ANY` ∩ `IN`, `IN` clipped by a range, 0-RPC cases, 1200 keys = 3 RPCs, int/bigint lists, r4 invisible to a get, `NOT IN` not pushed, `$1` lists, correlated cross-check |
| foreign table `odd_d` | `sql/hbase_fdw.sql` | 3 | odd with only family `d` mapped (r4 must stay invisible) |
| estimate/order cases | `sql/hbase_fdw.sql` "row estimates" (after "key lists") | 4 | `hb_rows()` table (11 queries), 1-row lookup drives a Nested Loop, Sort elided for `emp_c`/get mode/`odd_b`, kept for `emp`/`ikey`/DESC |
| join cases | `sql/hbase_fdw.sql`, rescans block + "joins use the row key" | 5 | forced NL on key (3 RPCs) and on non-key (63, scan-mode rescans); `ord` (20 rows, 2 NULL, 4 missing): natural NL, INNER/LEFT, EXCEPT check, expression clause, `= ANY (ARRAY[o.x, …])`, semi join, LATERAL, range per outer row, int⋈bigint, 10k outer → hash join |
| `test/bench_setup.sql`, `test/bench.sql` | `test/` | 6 | benchmark: tables (`emp`, `emp_c`, `emp_local`, `ord` 20, `ord1k` 1000) + `bench(q, n)` median; `psql -v fdw=<name>`; output `name\|ms` |
| `test/pg16_expected.py` | `test/` | 5 | regenerates `expected/hbase_fdw_1.out` from `hbase_fdw.out`; run it after every expected-file update |
| `hb_rows(q)` | `sql/hbase_fdw.sql` | 4 | prints the Foreign Scan's estimated `rows=N` only; dropped at the end |
| mock `getRowsWithColumns` | `test/mock_hbase.py` | 3 | skips rows with no matching cells, counts as one call |
| HBase tables `bkey`, `ukey` | `test/load_data.py` | 2 | 10 rows each, `d:n` = position in byte order. `bkey`: binary keys around 0xFF. `ukey`: `e%`, `e_x`, `é1`, `ê`… (text, `COLLATE "C"` in the test) |

---

## 6. Known gaps and gotchas

- **Non-deterministic collation path untested.** This build has no ICU. The
  check exists in `key_strategy`.
- **The cluster here is `C.UTF-8`, which is not "C" to Postgres.** The test
  "not pushed: database collation" assumes that. On a cluster initdb'd with
  `--locale=C` that range *would* be pushed, and the expected output would
  differ (the result itself would still be correct).
- **Ranges need `COLLATE "C"`.** On a default-collation key only `=`, LIKE,
  `^@` and `~<~` reach HBase. Phase 6's README must say so.
- **EXPLAIN normalises the operator:** `~<~` shows as `<`, and `'x' > id` as
  `id < 'x'`. This is intended (it is the byte comparison HBase does).
- **Cost constants are calibrated for a local standalone HBase.** A remote
  or loaded cluster has costlier RPCs, so nested loops would then be
  favoured too long.
- **Join size estimates on the key are poor.** With no stats Postgres
  assumes 200 distinct keys: `ord(20) ⋈ emp` estimates 1000 rows, not 20.
  Path choice is still right, but it could mislead a bigger plan. An
  `AnalyzeForeignTable` (key-only full scan) would fix it.
- **The PG16 expected file is derived, not run.** No PG16 is available here.
  Regenerate it with `python3 test/pg16_expected.py`. It assumes the only
  PG16 difference is `rows=N.00` in EXPLAIN ANALYZE.
- **Get mode fetches whole mapped families even for `count(*)`.**
  `getRowsWithColumns` takes no filter. That is only the requested rows,
  so it's fine.
- **`HBase Get Keys` counts keys asked for, not rows found.** Missing keys
  just don't come back.
- **Get mode returns rows in byte order of the key** (`1, 12, 5` for int
  keys), the same as a scan.
- **The test suite was run against the mock only by hand** (port 9091, the
  Phase 3 queries). A full mock run is Phase 6.
- **`pkill -f mock_hbase` also kills the shell that runs it** (it matches its
  own command line). Stop the mock by PID.
- **Range and LIKE estimates are default guesses** (no stats): `>` = 1/3, a
  BETWEEN pair or a LIKE prefix = 0.5%. A wide BETWEEN is underestimated.
- **The `rows` option must be set by hand** to the table size. Its default is
  1000, so a huge table without it looks small.
- **Rescans are costed as full re-runs** (Postgres's default for a
  ForeignScan). Parameterized paths therefore leave the connect cost out,
  while the plain path keeps it.
- **`hb_explain` hides nodes without Scan/Filter/HBase/Result in the line**
  (e.g. `Nested Loop`, `Memoize`, `Hash`). The indentation shows they are
  there.
- **A parameterized path does 1 RPC per outer row, never batched across
  outer rows.** For many outer rows the planner rightly picks a hash join
  over a full scan instead. Batching outer keys (like postgres_fdw's
  batching or a "bulk nested loop") would need executor work outside the
  FDW API.
- **PlaceHolderVars in join values are never pushed** (e.g. some outer-join
  expressions). They stay local and stay correct.
- **A server-side scanner is left open if a query errors mid-scan.** HBase
  expires it after 60s (inherited).
- **The loader drops and recreates `emp`, `odd`, `ikey`, `bkey` and `ukey`.** `emp.csv` is
  deterministic (seed 631).

---

## 7. Benchmarks, leftovers, and what could come next

**Re-running the timings.**
```bash
cd ~/cs631/postgresql/contrib/hbase_fdw
psql -p 9432 bench_new -f test/bench.sql      # current build
psql -p 9432 bench_w1  -f test/bench.sql      # Week-1 baseline
```
Recreating them from scratch:
- *Week-1 baseline extension:* extract `~/cs631/hbase_fdw.tar.gz` somewhere
  else, then rename it in four places:
  - Makefile `MODULE_big`, `EXTENSION`, `DATA` → `hbase_fdw_w1`;
  - copy the SQL to `hbase_fdw_w1--1.0.sql` with
    `CREATE FOREIGN DATA WRAPPER hbase_fdw_w1`;
  - control file `module_pathname = '$libdir/hbase_fdw_w1'`;
  - then `make USE_PGXS=1 install`.
- *Databases:* `createdb bench_X`, `CREATE EXTENSION <ext>`, then
  `psql -v fdw=<ext> -f test/bench_setup.sql` from the contrib dir.

**Left installed on purpose (remove when done):**
```bash
psql -p 9432 postgres -c 'DROP DATABASE bench_w1' -c 'DROP DATABASE bench_new'
rm ~/cs631/postgresql/install/lib/hbase_fdw_w1.so \
   ~/cs631/postgresql/install/share/extension/hbase_fdw_w1*
```

**Packaging.** `~/cs631/hbase_fdw.tar.gz` and `~/cs631/hbase_fdw/` are still
the **Week-1** code (README §1 tells people to unpack the tarball). Re-create
the tarball from `postgresql/contrib/hbase_fdw` before handing in, without
`*.o`, `*.so`, `results/` and `regression.*`.

**Possible extensions** (none needed for the assignment):
- server options for the cost constants (remote HBase);
- `AnalyzeForeignTable` for real row counts and key statistics;
- `DESC` order via `TScan.reversed`;
- batching the outer keys of a nested loop into one get (needs work outside
  the FDW API, or an "outer batch" async pattern);
- the literal `IndexAmRoutine` wrapper on a stub table (option 2), calling
  `hb_get_rows` / range scans.

---

## 8. Maintaining this file

At the end of every phase:
1. Update the status table in §1.
2. Add the phase's decisions to §4.
3. Add new or changed symbols to §5, then refresh all their line numbers:
   `python3 ~/cs631/hbase_fdw_handoff_lines.py` (from `~/cs631`).
4. Revise §6 and §7.
