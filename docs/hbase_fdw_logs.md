# hbase_fdw — work log

Only *our* changes go here, newest phase at the bottom of each section.
Each phase ends with an **In short** line.

Code: `~/cs631/postgresql/contrib/hbase_fdw`
Setup: PG on port 9432 (db `test`), HBase 3.0.0 standalone, Thrift1 on 9090.

---

## Glossary

**Path / ForeignPath.** The planner does not build a plan right away. For each
table it first collects *paths*: candidate ways of producing that table's rows.
Each path records:
- its estimated row count;
- a startup cost and a total cost;
- its output sort order (`pathkeys`);
- the outer tables it needs values from (`param_info` / `required_outer`).

For a normal table the candidates are things like a SeqScan path or IndexScan
paths. Postgres knows nothing about a foreign table, so it asks the FDW: our
`GetForeignPaths` callback creates one or more **ForeignPath**s and offers them
with `add_path()`. The planner keeps the cheapest ones, chooses one, and our
`GetForeignPlan` turns it into a `ForeignScan` plan node. That is the hook for
making HBase "act like an index": we offer a cheap ForeignPath whenever a
row-key lookup is possible.

**Parameterized path.** A path that needs a value from another table in the
join, e.g. `emp.id = o.emp_id`. The planner can only use it as the inner side
of a nested loop. It is rescanned once per outer row, and each rescan receives
that row's `o.emp_id`. A plain index scan used inside a nested loop works
exactly this way.

**Qual / pushdown.** A qual is a WHERE or JOIN condition. "Pushing it down"
means HBase applies it, so fewer rows cross the network. Today every qual is
evaluated locally by Postgres, after the whole table has been fetched.

**Start/stop row.** An HBase scan can begin at `startRow` (inclusive) and end
at `stopRow` (exclusive). Because rows are stored sorted by key, HBase seeks
straight to `startRow`.

---

## What is cheap in HBase, and what is not

HBase stores each table **sorted by row key, comparing raw bytes**. The table
is split into regions, and each region's files carry a block index (plus bloom
filters). Finding a key therefore means a seek, not a scan. Row key is the
*only* indexed attribute.

| Access | HBase work | Cost grows with |
|---|---|---|
| Point lookup `k = 'x'` | locate region, then a block-index seek | ~constant (1 round trip via Thrift `getRow`) |
| Multi-point `k IN (a,b,c)` | one seek per key, batched | number of keys = output size |
| Range `k BETWEEN a AND b`, `k >= a`, `k < b` | seek to start, read sequentially to stop | rows in the range = **output size** |
| Prefix `k LIKE 'abc%'` | range `['abc','abd')` | rows with that prefix = **output size** |
| `ORDER BY k` | free, data is already sorted | nothing extra |
| Condition on a non-key column (`salary > 100`) | full scan; a server-side filter only saves network | **whole table size** |
| `count(*)`, no WHERE | full scan (KeyOnlyFilter saves network) | **whole table size** |
| `ORDER BY` on a non-key column | full scan + sort in Postgres | whole table size |

So the cheap cases are the row-key operators `=`, `IN`, ranges and prefix
`LIKE`: their cost tracks the size of the *result*. Everything else costs about
as much as reading the whole table.

**Byte-order caveat.** HBase compares unsigned bytes. Postgres `text` compares
using the database collation (e.g. `en_US`: `'a' < 'B'`, whereas in bytes
`'B' < 'a'`). Integer keys are stored as text, so in bytes `'10' < '9'`.
- `=`, `IN` and `LIKE 'prefix%'` are always safe to push down.
- Ranges are pushed only with `COLLATE "C"` or a `bytea` key; otherwise they
  stay local, so results remain correct.

---

## Plan (option 1: index-style access through the row key)

Status legend: [ ] pending, [~] in progress, [x] done

### Phase 0 — Green baseline
- [x] 0.1 On real HBase 3.0 a missing table fails at `scannerGetList` with a
      Java stack trace; the mock fails at `scannerOpenWithScan`. Keep only the
      first line of the HBase message, drop the `org.apache.hadoop.hbase.`
      prefix, and leave out the RPC name for this error.
- [x] 0.2 `make installcheck` passes against real HBase.

*Why:* every later phase relies on the regression test to catch breakage. A
test that already fails hides new failures.
**In short:** make the existing test suite pass on real HBase so it can guard
the later changes.

### Phase 1 — Point lookup on the row key (`k = const`)
- [x] 1.1 Planner: find the rowkey column, and in `baserestrictinfo` match
      `rowkey = Const` (either side, text/varchar/bytea/int types).
- [x] 1.2 Turn the match into the scan range `[k, k\0)` and store it in
      `fdw_private`. Keep the clause as a local filter too (cheap recheck).
- [x] 1.3 Executor: fill `HbScanSpec.start/stop` (the Thrift code already
      sends them as `TScan.startRow/stopRow`).
- [x] 1.4 EXPLAIN shows `HBase Start Row` / `HBase Stop Row`.
- [x] 1.5 Tests: results match `emp_local`; ANALYZE RPC count drops.

*Why:* today `WHERE id = 'x'` ships all 10k rows and throws 9,999 away. This is
the most common lookup and the clearest demonstration of "using HBase's index".
**In short:** `WHERE key = value` becomes a direct HBase seek, not a full scan.

### Phase 2 — Ranges and prefixes
- [x] 2.1 Match `<, <=, >, >=` and `BETWEEN` (which arrives as two quals).
- [x] 2.2 Match `LIKE 'abc%'` (and `^@`/`starts_with`) as `['abc','abd')`,
      handling the 0xFF carry case.
- [x] 2.3 Intersect all bounds; an empty range skips HBase entirely.
- [x] 2.4 Push ranges only under C collation or a bytea key (see the byte-order
      caveat); otherwise filter locally.
- [x] 2.5 Tests for each operator, mixed bounds, empty range, and a non-C
      collation that must *not* be pushed.

*Why:* a range read costs as much as its output, while a full scan costs as
much as the table. This is what an index range scan does in Postgres.
**In short:** key ranges and key prefixes read only the matching slice of the
HBase table.

### Phase 3 — Multi-key lookups (`IN`, `= ANY`)
- [x] 3.1 Add the Thrift1 `getRowsWithColumns` RPC to `hbase_thrift.c`
      (one round trip for a set of keys).
- [x] 3.2 Match `k IN (...)` / `k = ANY(array)`. Also route single-key `=` here,
      so a point lookup takes 1 RPC instead of open + getList×2 + close.
- [x] 3.3 Tests: duplicates, NULLs in the list, missing keys.

*Why:* an `IN` list is N independent seeks. A range from min to max would
read everything in between.
**In short:** a list of keys is fetched with one batched HBase get.

### Phase 4 — Costing and row estimates
- [x] 4.1 `rel->rows` = `rows` option × `clauselist_selectivity` (key `=` → 1
      row) instead of a fixed 1000.
- [x] 4.2 Cost model: startup ≈ one round trip; total grows with *estimated
      output* for key paths and with *table size* for full scans.
- [x] 4.3 (optional) Advertise `pathkeys` = rowkey order on full and range
      scans (C collation), so `ORDER BY k` needs no Sort.

*Why:* without correct estimates the planner cannot tell a 1-row lookup from a
10k-row scan, so it picks bad join orders and join methods.
**In short:** tell the planner that row-key lookups are cheap and small, so it
prefers them, just as it prefers an index.

### Phase 5 — Parameterized paths (joins use the "index")
- [x] 5.1 For join clauses `rowkey = other_table.col`, build a parameterized
      ForeignPath (`required_outer`) with point-lookup cost.
- [x] 5.2 Carry the key expression through `fdw_exprs`, init it with
      `ExecInitExprList`, and evaluate it on every (re)scan.
- [x] 5.3 Reuse the connection across rescans (already the case); open only a
      cheap get per outer row.
- [x] 5.4 Tests: `local JOIN emp ON emp.id = local.x` plans a Nested Loop with
      the foreign scan inside, and its results match `emp_local`.

*Why:* an index matters most inside joins. Without this, a nested loop re-reads
the whole HBase table for every outer row.
**In short:** each row of the other table looks up its match in HBase by key,
like a nested-loop index scan.

### Phase 6 — Wrap-up
- [x] 6.1 README: the new options, the byte-order assumptions, EXPLAIN fields.
- [x] 6.2 Full `make installcheck` on real HBase and on the mock; regenerate
      `expected/`.
- [x] 6.3 Before/after timings on `emp` (point, range, join) for the report.

**In short:** document it, prove it, measure it.

---

## Change log

### 2026-10-28 — Plan written
- Read the existing Week-1 FDW (full scans only, every qual evaluated locally).
- Confirmed it works on real HBase: `SELECT * FROM t1` is correct, but
  `EXPLAIN ... WHERE k = 'k2'` shows a full scan with a local `Filter` and the
  fixed estimate `rows=1000`.
- `make installcheck`: only the missing-table error text differs (real HBase
  vs the mock), all data checks pass → Phase 0.
- No code changed yet.

### 2026-10-28 — Phase 0 done: green baseline
**Problem.** `make installcheck` failed on real HBase only for the missing-table
test:
- expected (written against the mock):
  `hbase_fdw: scannerOpenWithScan: TableNotFoundException: nosuch`
- real HBase 3.0:
  `hbase_fdw: scannerGetList: org.apache.hadoop.hbase.TableNotFoundException: nosuch`
  followed by about 80 lines of Java stack trace.

**Why it differed.**
1. HBase 3's Thrift server opens scanners *lazily*: `scannerOpenWithScan`
   always succeeds, and the missing table is only noticed on the first
   `scannerGetList`. The mock checks for the table at open time.
2. Real HBase sends `Throwable.toString()` plus the stack trace as the IOError
   message, with the fully qualified Java class name.

**Why fix it now.** Every later phase relies on this test to catch
regressions. A suite that already fails (with a huge diff) would hide new
failures. The 80-line error is also unreadable for a user.

**Changes** (`contrib/hbase_fdw`):
- `hbase_thrift.c`: new `short_msg()` keeps only the first line of the HBase
  message and strips a `java.package.` prefix from the exception class
  (`org.apache.hadoop.hbase.TableNotFoundException` → `TableNotFoundException`).
  It only strips when the text before the first `:` looks like a Java class
  name, so ordinary messages are left alone.
- `hbase_thrift.c` `rexc()`: the user-facing message no longer includes the
  RPC name, because the failing RPC depends on the server (see 1). The full
  original message, including the RPC name and stack trace, is still logged
  at `DEBUG1` (`SET client_min_messages = debug1` shows it).
- Added `#include <ctype.h>` for `isalnum`.
- `expected/hbase_fdw.out` and `expected/hbase_fdw_1.out`: the line becomes
  `ERROR:  hbase_fdw: TableNotFoundException: nosuch`. The mock already sends
  exactly that text, so it produces the same output.

**Verified.**
- `make installcheck PGPORT=9432` on real HBase: `All 1 tests passed`.
- Manual check: the default error is 2 lines; at debug1 the full stack trace
  with `scannerGetList raised:` appears.

**In short:** HBase errors are now one clean line (full detail at debug1), and
the test suite passes on real HBase, so it can guard Phases 1–6.

### 2026-10-28 — Phase 1 done: point lookup on the row key
**Problem.** `SELECT * FROM emp WHERE id = 'e004242'` read all 10,000 rows
from HBase (21 RPCs at batch 500) and Postgres discarded 9,999 of them. HBase
keeps rows sorted by key and could have found that row with a single seek.

**What we did, and why each choice.**

1. *Planner* (`hbGetForeignPlan` → new `key_conds()`): looks at each WHERE
   clause of the form `rowkey = value` (either side).
   - **What counts as a value:** anything fixed for the whole scan. That means
     a constant, a `$1` from a prepared statement, or an init-plan
     `(SELECT ...)`. Excluded are expressions that reference columns, call
     volatile functions (`random()` could differ per row), or contain
     sub-plans.
   - **Which `=` operators:** only those where Postgres equality is the same
     as HBase byte equality (`byte_eq_op()`):
     - `text`/`varchar` under a *deterministic* collation (a case-insensitive
       ICU collation would call `'A' = 'a'`, but HBase would not);
     - `bytea`;
     - `int2`/`int4`/`int8` and their cross-type versions. This assumes keys
       hold the canonical text, i.e. `"42"`, not `"042"`.
   - **Where the values go:** into `fdw_exprs`, not `fdw_private`. Postgres
     fixes up expressions in `fdw_exprs` (setrefs: Params, init-plans, later
     the nested-loop params of Phase 5); it never touches `fdw_private`.
     `fdw_private` now holds `[needed attnos, strategy numbers, rowkey
     column name]`.
   - **The clause also stays in the local `Filter`.** Rechecking one row is
     nearly free, and it means pushdown can only save work, never change a
     result.
2. *Executor* (new `set_bounds()`, called every time the scanner opens, so a
   rescan with new parameter values works): evaluates each value and turns it
   into key bytes. text/bytea use the raw bytes; integers go through the
   type's output function. The scan range is `[key, key\0)`, which contains
   exactly that one key (stopRow is exclusive, and `key\0` is the smallest
   key after `key`). Three cases return no rows **without contacting HBase**:
   a NULL value, an empty key (HBase has no empty row keys), and two `=`
   conditions with different keys.
3. *EXPLAIN*: `HBase Key Cond` (like an index's `Index Cond`). Under ANALYZE
   it also shows `HBase Start Row` / `HBase Stop Row` for the last scan, with
   non-printable bytes as `\xNN`.

**Bug found and fixed during the phase.** The first version built the
`HBase Key Cond` text in the planner with `deparse_expression(val, NIL, ...)`.
For `WHERE id = (SELECT ...)` the value is a `PARAM_EXEC` Param, and ruleutils'
`find_param_referent()` does `linitial(context->namespaces)` → NULL dereference
→ **backend segfault** (Postgres auto-recovered). gdb couldn't attach because
`ptrace_scope=1`, so we found the cause by reading `ruleutils.c`. Fix: deparse
at EXPLAIN time from the plan's own `fdw_exprs` using `es->deparse_cxt`. That
is safe because our pushed clauses are always in `plan->qual` too, so EXPLAIN
has just pointed the context at our node. The init-plan now prints properly as
`(InitPlan 1).col1`.

**Results** (real HBase, `emp` 10k rows, batch 500):

| query | before | after |
|---|---|---|
| `WHERE salary = 98000` (non-key) | 21 RPCs | 21 RPCs (correct: no index on it) |
| `WHERE id = 'e004242'` | 21 RPCs, 9,999 rows discarded | **2 RPCs**, 1 row |
| `WHERE id = 'nope'` | 21 RPCs | 1 RPC |
| `WHERE id = (SELECT NULL)` | 21 RPCs | **0 RPCs** |

A point lookup takes 2 `scannerGetList` calls (1 row + 1 empty to detect the
end), plus open and close. Phase 3 makes this a single `getRow` call.

Bonus seen in testing: in `emp a JOIN emp b ON a.id = b.id WHERE a.id = 'x'`,
the planner's equivalence classes derive `b.id = 'x'`, so *both* sides become
point lookups.

**Files changed.**
- `hbase_fdw.c`:
  - new `KeyCond` struct, plus key state in `HbState`;
  - new `rowkey_att()`, `is_key_var()`, `byte_eq_op()`, `key_conds()`,
    `set_bounds()`, `show_key()`;
  - updated `hbGetForeignPlan`, `hbBeginForeignScan`,
    `hbIterateForeignScan` and `hbExplainForeignScan`.
- `test/load_data.py`: new HBase table `ikey` (integer keys 1–12), used to
  test int row keys. **Re-run the loader** after pulling this. emp.csv is
  byte-identical (same seed).
- `sql/hbase_fdw.sql`:
  - new helper `hb_explain()`: EXPLAIN ANALYZE without the "actual rows"
    text, whose format differs between PG16 and PG18, so both expected files
    get the same new output;
  - 20+ new cases: point hit and miss, key prefix ≠ match, prepared
    statement, init plan, NULL and empty keys, volatile not pushed, `<>` not
    pushed, cross-check against `emp_local`, varchar/bytea/int2/int4/int8
    keys.
- `expected/hbase_fdw.out` regenerated, and `expected/hbase_fdw_1.out` (PG16)
  derived from it by re-applying the old PG16-only differences.

**Verified.** `make installcheck` passes twice in a row on real HBase (re-run
safe). The old nested-loop rescan test still shows 63 RPCs: join clauses are
Phase 5.

**Not tested here:** non-deterministic collations. This PG build has no ICU
(`USE_ICU` undefined), and libc collations are always deterministic, so the
check is in place but can't be exercised.

**In short:** `WHERE key = value` (a constant, `$1` or a sub-select) now makes
HBase seek straight to that row instead of scanning the table: 21 RPCs → 2
for a lookup, and 0 when the key is NULL.

### 2026-10-28 — Phase 2 done: key ranges and prefixes
**Problem.** After Phase 1 only `key = value` reached HBase. `WHERE id
BETWEEN 'e000100' AND 'e000199'` or `WHERE id LIKE 'e0001%'` still shipped all
10k rows and threw away 9,900 of them, although HBase keeps rows sorted by key
and could seek straight to the slice.

**Why it was needed.** A range read costs as much as its *output*; a full scan
costs as much as the *table*. This is exactly what an index range scan buys a
normal Postgres table, and the second half of "use HBase's index".

**The catch: two different sort orders.** HBase compares keys as unsigned
bytes. Postgres compares `text` with the column's collation. They agree only
under a C collation. Our database is `C.UTF-8` (libc), which Postgres does
*not* treat as C, so a plain `id < 'x'` on `emp` must stay local. Integer keys
are stored as text, so their byte order is wrong too (`'10' < '9'`).

**Changes** (`hbase_fdw.c`):
1. *Planner.* `byte_eq_op()` became `key_strategy()`: it maps an operator's
   function OID to what HBase can do with it.
   - `=`: as before (text under a deterministic collation, bytea, ints).
   - `<`, `<=`, `>=`, `>` (so `BETWEEN` too): text only under a C collation
     (`coll_is_c()`), bytea always, and the `~<~` family (`text_pattern_ops`,
     which compares with memcmp) under any collation. Never for int keys.
   - `LIKE` (text and bytea) and `starts_with()` / `^@`: under any
     deterministic collation, since a byte prefix is a byte prefix.
   - Constant on the left (`'x' > id`) is flipped to `id < 'x'`.
   - `key_conds()` now also looks at `FuncExpr`, so `starts_with(id, 'x')`
     written as a function call is caught, not only the `^@` operator.
   - A constant LIKE pattern with no fixed prefix (`'%1'`) is not pushed, as
     it would narrow nothing.
2. *Executor.* `set_bounds()` now keeps a running `[start, stop)` and
   intersects every condition into it:
   - `>= k` / `> k`: raise start to `k` / `k\0`;
   - `< k` / `<= k`: lower stop to `k` / `k\0`;
   - `= k`: both;
   - prefix `p` (from `starts_with`, or the LIKE pattern up to its first
     unescaped `%` or `_`): `[p, p+1)`, where `p+1` drops trailing 0xFF bytes
     and bumps the last byte; an all-0xFF prefix has no stop row;
   - a LIKE pattern with no wildcard at all is treated as `=`.
   - If start ≥ stop, or `< ''`, or any value is NULL, the scan returns no
     rows **without contacting HBase**.
   - The LIKE prefix is computed at scan open, so `LIKE $1` works with a
     prepared statement and with values from an outer query.
3. *EXPLAIN.* `HBase Key Cond` prints the operator (`<`, `<=`, `=`, `>=`, `>`,
   `~~`, `^@`).

**Tests.**
- `test/load_data.py`: two small new HBase tables. **Re-run the loader.**
  `emp.csv` is byte-identical.
  - `bkey`: binary keys around 0xFF, to test the prefix carry.
  - `ukey`: multibyte keys and keys with literal `%` and `_`, to test
    escapes.
- `sql/hbase_fdw.sql`: about 60 new cases. They cover BETWEEN, mixed bounds,
  a commuted operator, `COLLATE "C"` in the query, `~>=~`, the empty
  intersection, `< ''`, and the not-pushed cases (database collation, int
  keys, ILIKE, `'x' LIKE id`, `LIKE '%1'`). They also cover prepared ranges
  and patterns (including NULL), LIKE with `\%` and `ESCAPE '#'`, `_`,
  multibyte prefixes, the 0xFF carry, and two cross-checks against
  `emp_local` that run per row through correlated sub-selects.
- `expected/hbase_fdw.out` regenerated; `_1.out` (PG16) derived by
  re-applying the PG16-only hunk.

**Verified.**
- `make installcheck` passes twice in a row on real HBase.
- A throwaway randomized check gave 0 mismatches against `emp_local`
  (compared with `COLLATE "C"`). It ran 300 random predicates mixing all
  range operators, BETWEEN, LIKE (`%`, `_`, exact, leading `%`),
  `starts_with`, and commuted forms.

**Results** (real HBase, `emp` 10k rows, batch 500):

| query | before | after |
|---|---|---|
| `id BETWEEN 'e000100' AND 'e000199'` (C collation) | 21 RPCs | **2 RPCs** |
| `id LIKE 'e0001%'` | 21 RPCs | **2 RPCs** |
| `id ^@ 'e0099'` | 21 RPCs | **2 RPCs** |
| `id > 'e5' AND id < 'e1'` | 21 RPCs | **0 RPCs** |
| `id < 'e000100'` (database collation `C.UTF-8`) | 21 RPCs | 21 RPCs (correct: orders differ) |

**Not tested here.** Non-deterministic collations (no ICU in this build, as in
Phase 1). LIKE with a non-UTF-8 server encoding (the prefix code steps over
multibyte characters with `pg_mblen`, so it should be fine).

**In short:** key ranges, `BETWEEN`, `LIKE 'p%'` and `starts_with` now read
only their slice of the HBase table (21 RPCs → 2), but only where Postgres
and HBase agree on the order.

### 2026-10-29 — Phase 3 done: key lists and one-RPC lookups
**Problem.**
- `WHERE id IN ('e000001', 'e005000', 'e009999')` was still a full scan
  (21 RPCs). Phase 2 couldn't help: the only range covering those keys runs
  from the smallest to the largest, which is nearly the whole table.
- A Phase 1 point lookup went through the scanner: `scannerOpenWithScan`,
  `scannerGetList` (the row), `scannerGetList` (empty, to detect the end) and
  `scannerClose`. That is 4 round trips; EXPLAIN only counted the 2
  getList calls.

**Why it was needed.** An `IN` list is N independent seeks. An index does
exactly that for `= ANY`, and HBase has a batched get for it. It is also the
building block Phase 5 needs: one lookup per outer row of a join.

**Changes.**
1. `hbase_thrift.c`:
   - new `hb_get_rows()`, which sends the Thrift1 `getRowsWithColumns`
     (table, list of keys, list of columns, empty attributes);
   - the `list<TRowResult>` reader was factored out of `hb_scan_next` into
     `rrows()`, now shared by both;
   - `hbase_thrift.h` has a new `HbKey {p, len}`.
2. Planner (`key_conds`): `ScalarArrayOpExpr` with `useOr` (that is,
   `k IN (...)` and `k = ANY (array)`) on the rowkey, with an equality operator
   `key_strategy` accepts, becomes the new strategy `KC_IN`. The array can be
   a Const, `$1`, `ARRAY[$1, $2]` or an init-plan result. `NOT IN`
   (`<> ALL`) is not pushed.
3. Executor:
   - `set_bounds` now builds a **key set** from `=` and `IN` (several are
     intersected). The rest still gives the `[start, stop)` range, and the
     key set is clipped to it.
     - The keys are deduplicated and sorted in byte order, so a key is
       fetched once and the rows arrive in the same order a scan returns them.
     - NULL elements and `''` are dropped. An empty key set means no rows
       and no RPC.
   - With a key set, `hbIterateForeignScan` opens no scanner. It calls
     `hb_get_rows` for `batch_size` keys at a time (1200 keys at batch 500 =
     3 RPCs). A new `begun` flag replaces "open" as the "bounds already set"
     test, since get mode has nothing open.
   - A plain `k = value` is a one-key set, so a point lookup is now **1 RPC**
     instead of 4 round trips. LIKE without a wildcard follows (it was
     already treated as `=`).
4. EXPLAIN:
   - `HBase Key Cond: (id = ANY ('{...}'::text[]))`;
   - under ANALYZE, a get prints `HBase Get Keys: N` (keys asked for) instead
     of Start/Stop Row.
5. `test/mock_hbase.py`: new `getRowsWithColumns` / `getRows`. Like real
   HBase, it leaves out rows with no cell in the requested columns. Checked
   against the mock on port 9091: same results and RPC counts.

**Semantics check.** A get must agree with a scan on which rows exist.
- HBase's Thrift handler builds each Get with `addFamily` / `addColumn`
  (the same column list we send to scans), and `rowResultFromHBase` skips
  empty results.
- So row `odd/r4` (only an unmapped family) is invisible both ways. There is
  a test for that.

**Tests** (`sql/hbase_fdw.sql`).
- New "key lists" section:
  - duplicates, NULL, `''` and missing keys in a list;
  - `= ANY` intersected with `IN`, and `IN` clipped by `> 'e000100'`;
  - 0-RPC cases (empty intersection, NULL array, `'{}'`, all NULL or empty);
  - 1200 keys from an init-plan array (3 RPCs);
  - int and bigint lists;
  - the r4 visibility test and `NOT IN` not pushed;
  - prepared `= ANY ($1)` and `IN ($1, $2)` with NULLs;
  - a correlated cross-check against `emp_local` (rescans in get mode).
- All Phase 1/2 point-lookup EXPLAINs changed from `Start/Stop Row … RPCs: 2`
  to `Get Keys: 1, RPCs: 1`.
- Expected files regenerated (PG16 re-derived).

**Verified.** `make installcheck` passes twice in a row on real HBase; the
loader and `emp.csv` are unchanged.

**Results** (real HBase, `emp` 10k rows):

| query | before | after |
|---|---|---|
| `id = 'e004242'` | open + 2 getList + close | **1 get** |
| `id IN (3 keys)` | 21 RPCs (full scan) | **1 get** |
| `id = ANY (1200 keys)` | 21 RPCs | 3 gets |
| `id IN ('a','b') AND id = 'c'` | 21 RPCs | 0 |

**In short:** `IN` / `= ANY` lists and single-key lookups now cost one batched
HBase get (per `batch_size` keys) instead of a scan.

### 2026-10-29 — Phase 4 done: row estimates, costs and key order
**Problem.** The planner still saw every HBase scan the same way: `rows` = the
`rows` option (default 1000) and a cost of `100 + rows × 0.02`, whatever the
WHERE clause. A 1-row key lookup and a 10k-row full scan looked identical. So
the planner couldn't prefer the lookup, chose join orders blind, and sorted
results that HBase already returns in key order.

**Why it was needed.** Phases 1–3 make key access cheap at run time. Unless
the planner knows that, it won't build plans around it: for example, driving
a join from a 1-row lookup, or skipping a Sort. Phase 5's parameterized paths
are also chosen purely on cost.

**Changes** (`hbase_fdw.c`).
1. *Planner state.* `rel->fdw_private` is now an `HbRelInfo`: options,
   needed attnos, the rowkey attno, the pushed RestrictInfos, get-vs-scan, and
   the expected number of rows HBase sends (`fetched`). `key_conds()` gained
   an output list of the RestrictInfos it pushed, so the planner can split
   key quals from local ones.
   `hbGetForeignRelSize` / `hbGetForeignPaths` moved below `key_conds`.
2. *Row estimate* (`hbGetForeignRelSize`). The `rows` option is the table
   size (`rel->tuples`).
   - With `=` / `IN`, row keys are unique, so `fetched` = the number of keys:
     1 per `=`, the array length for `IN` (`estimate_array_length`, which
     says 10 for an unknown array). The smallest set wins when there are
     several, capped at the table size.
   - Otherwise `fetched` = table × `clauselist_selectivity(pushed quals)`.
     With no statistics these are Postgres's defaults: 1/3 for `>`, 0.5%
     for a BETWEEN pair or LIKE prefix.
   - `rel->rows` = `fetched` × selectivity of the quals that stay local.
3. *Cost* (`hbGetForeignPaths`), in seq_page_cost units, with named
   constants `HB_CONNECT_COST` 10, `HB_RPC_COST` 5, `HB_ROW_COST` 0.01:
   - startup = connect + the first round trip;
   - plus one RPC per batch: a get needs ⌈keys/batch⌉; a scan needs
     ⌈rows/batch⌉ + 3 (open, the final empty batch, close);
   - plus `fetched × (cpu_tuple_cost + row cost + qual eval)`. All quals,
     pushed ones included, are rechecked locally.
   - Results: full scan of `emp` 15..325, point lookup 15..15.02, BETWEEN
     15..31.
4. *Key order* (`key_pathkeys`, 4.3). Both a scan and a get (keys sorted)
   return rows in key byte order. When Postgres's order is the same (a
   bytea key, or a text/varchar key with a C collation), the path advertises
   pathkeys on the rowkey, ascending, via
   `build_expression_pathkey(..., create_it = false)`. They are only
   attached if the query has a matching sort or merge clause. Int keys and
   default-collation keys get none; DESC still sorts.

**Tests** (`sql/hbase_fdw.sql`, new section after "key lists").
- A new `hb_rows(q)` helper prints just the Foreign Scan's estimated `rows=`,
  which is stable (no costs or widths). 11 estimate cases:
  - full scan, the `rows` option;
  - a non-key filter (3333 of 10000);
  - `=` (1), `IN` (3), `IN ∩ =` (1), an init-plan array (10);
  - BETWEEN, `>`, LIKE.
- Join plan: `emp f JOIN emp_local l ON l.id = f.id WHERE f.id = 'e000042'`
  is a Nested Loop driven by the 1-row foreign scan, with the local index
  inside.
- Sort elision:
  - no Sort for `emp_c ORDER BY id LIMIT 3` (C collation), a get-mode
    `IN … ORDER BY id`, or `odd_b ORDER BY k` (bytea);
  - still a Sort for `emp` (database collation), `ikey` (int) and `DESC`.
- Expected files regenerated (PG16 re-derived). None of the existing plans
  changed.

**Verified.**
- `make installcheck` passes twice in a row on real HBase.
- Extra hand check: a forced Merge Join `emp_c ⋈ loc_c ON id` reads the
  foreign side with no Sort (it relies on HBase's order) and matches all
  1000 local rows.

**Results.**

| query | rows before → after | cost before → after |
|---|---|---|
| `SELECT * FROM emp` | 10000 → 10000 | 100..300 → 15..325 |
| `WHERE id = 'e000001'` | 10000 → **1** | 100..300 → **15..15.02** |
| `WHERE id IN (3 keys)` | 10000 → **3** | same as scan → 15..15.07 |
| `emp_c ORDER BY id LIMIT 3` | Sort over full scan | **no Sort**, stops after the first batch |

**In short:** the planner now knows a key lookup returns a handful of rows
cheaply, and that HBase output is already sorted by key, so it plans around
both, as it would for an index.

### 2026-10-29 — Phase 5 done: joins use the row key (parameterized paths)
**Problem.** In a join like `emp_local l JOIN emp f ON f.id = l.id` the key
condition refers to another table, so Phases 1–4 couldn't push it. With
emp on the inner side of a nested loop, every outer row re-read the whole
HBase table: the regression test's 3 outer rows cost 3 × 21 = **63 RPCs**,
and 29,997 rows were thrown away by the Join Filter.

**Why it was needed.** This is where an index matters most. A nested loop
with an index on the inner side does one lookup per outer row. Without it,
the planner's only sane option was a hash join over a full HBase scan, even
for 3 outer rows.

**Changes** (`hbase_fdw.c`).
1. *Which values may be pushed.* `scan_const()` used to reject every Var. It
   now rejects only Vars of the foreign table itself (and PlaceHolderVars).
   Vars of other tables come from the outer side of a nested loop. Postgres
   turns them into nestloop Params in `fdw_exprs`
   (`create_foreignscan_plan` runs `replace_nestloop_params` on them), and
   `set_bounds` already re-evaluates `fdw_exprs` at every rescan. So the
   executor needed no change.
2. *Refactor.* The per-clause test in `key_conds()` became `key_cond()`.
   `key_conds()` now takes the rowkey attno instead of opening the relation
   each time. The Phase 4 estimate became `estimate()`, so it can run on any
   clause list. Path creation became `add_hb_path()`.
3. *Parameterized paths* (`hbGetForeignPaths`), the same way postgres_fdw
   finds them:
   - candidate join clauses come from `rel->joininfo`, and from the
     equivalence classes (`generate_implied_equalities_for_column` with
     `ec_is_key`). A plain `f.id = l.id` exists *only* in an EC;
   - a candidate must be movable to this rel (`join_clause_is_movable_to`)
     and accepted by `key_cond`. That covers `=`, `IN`/`= ANY` and, under
     C collation, ranges and LIKE, with an outer value;
   - one `ParamPathInfo` per distinct set of outer rels, via
     `get_baserel_parampathinfo`;
   - each path is estimated and costed on `baserestrictinfo + ppi_clauses`:
     1 row per outer row for `=`, and no connect cost, because a rescan
     keeps the connection;
   - the plain path is now created with `rel->lateral_relids` as
     `required_outer` (as postgres_fdw does). Now that a key condition can
     carry a LATERAL reference into `fdw_exprs`, the path must be
     parameterized by those rels so the reference becomes a nestloop Param.
4. Removed `HbRelInfo` fields that became dead (`kquals`, `get`, `fetched`):
   each path now estimates for itself.

**Tests** (`sql/hbase_fdw.sql`).
- The old rescan test: **63 → 3 RPCs**, `Foreign Scan … rows=1 loops=3`, and
  `HBase Key Cond: (id = emp_local.id)`. The Join Filter is gone.
- New: the same forced nested loop on a *non-key* column, so scan-mode
  rescans (scanner reopened every time) stay covered: still 63 RPCs, by
  design.
- New "joins" section with a 20-row `ord` table (2 NULL keys, 4 missing):
  - the planner picks the nested loop on its own: 18 RPCs (the NULL keys
    cost 0);
  - INNER and LEFT JOIN counts, and a two-way EXCEPT check against
    `emp_local` (0 mismatches);
  - an expression join clause (`id = 'e' || lpad(o.oid…)`, from joininfo,
    not an EC) and `id = ANY (ARRAY[o.emp_id, …])`;
  - a semi join (`id IN (SELECT …)`) and a `LATERAL (… OFFSET 0)` subquery;
  - a range per outer row on `emp_c` (scan mode inside a nested loop);
  - an int key joined to a bigint column (`int48eq`);
  - with 10,000 outer rows the planner still picks a hash join over one full
    scan, which is right: 10,000 gets would cost more.
- **New helper `test/pg16_expected.py`.** It derives the PG16 file from the
  PG18 one by rewriting `rows=N.00` → `rows=N` in EXPLAIN ANALYZE blocks and
  re-padding the psql header. The old "re-apply the diff" method broke
  because the PG16-specific block itself changed. The script was checked
  first: it rebuilds the Phase 4 PG16 file byte for byte.

**Verified.** `make installcheck` passes twice in a row on real HBase.

**Results** (real HBase):

| query | before | after |
|---|---|---|
| 3 outer rows `JOIN emp ON f.id = l.id` (forced NL) | 63 RPCs, 30,000 rows fetched | **3 RPCs, 3 rows** |
| 20 outer rows (planner's choice) | no key path existed (a nested loop meant 20 full scans) | nested loop, 18 gets |
| `WHERE id IN (SELECT emp_id FROM ord)` | full scan | 18 gets |
| 10,000 outer rows | hash join, full scan | same (correct) |

**In short:** in a join, each outer row now looks up its match in HBase by
key, like a nested-loop index scan: 63 RPCs → 3 for the test join.

### 2026-10-29 — Phase 6 done: mock run, timings, cost calibration, README
**Problem.**
- The work was only tested against real HBase.
- The cost constants from Phase 4 were guesses.
- The README still described Week 1 ("no qual pushdown yet").
- There were no numbers showing the gain.

**6.2 Full suite on the mock.**
- Stopped HBase's Thrift gateway, ran `test/mock_hbase.py` on 9090, loaded
  it with `test/load_data.py`, and ran `make installcheck`.
- **It passed with the same expected output as real HBase:** same rows,
  same RPC counts, same error texts.
- Then stopped the mock (by PID) and restarted the gateway
  (`hbase-daemon.sh start thrift -p 9090`), and the suite passed on real
  HBase again.

**6.3 Timings.**
- *Setup.* For a fair "before", the Week-1 tarball (`~/cs631/hbase_fdw.tar.gz`)
  was built side by side as a separate extension, **`hbase_fdw_w1`**:
  MODULE_big, EXTENSION, the control file and the FDW name were renamed,
  and it was built with PGXS into the same install, with the same
  `-O0 -g`.
- *Databases.* `bench_w1` and `bench_new` hold identical table definitions,
  and both read the same HBase tables.
- *Method.* `bench(q, n)` returns the median of n runs after one warm-up,
  with planning included. Scripts: `test/bench_setup.sql`, `test/bench.sql`.

| query | Week 1 (ms) | now (ms) |
|---|---|---|
| full `count(*)` | 18.6 / 19.2 | 19.1 / 20.3 |
| full scan + GROUP BY | 35.6 / 36.4 | 35.1 / 35.3 |
| point lookup | 36.8 / 37.7 | **1.47 / 1.36** |
| point lookup, miss | 35.9 / 38.1 | **1.16 / 1.01** |
| IN, 100 keys | 20.6 / 21.7 | **7.5 / 7.1** |
| BETWEEN, 100 rows (C) | 18.8 / 20.2 | **2.4 / 2.3** |
| LIKE prefix, 100 rows | 19.8 / 20.8 | **2.1 / 2.0** |
| ORDER BY id LIMIT 10 (C) | 20.0 / 20.6 | **3.8 / 4.0** |
| join, 20 outer rows | 18.2 / 18.4 | **13.1 / 13.2** |
| join, 1000 outer rows | 18.5 / 19.2 | 21.5 / 17.8 (both hash join) |

(Two back-to-back rounds; full scans vary ±10% between runs.)

**Cost calibration (found by the timings).**
- The first round showed the 20-row join barely faster than Week 1. The
  plan was right (a nested loop of 20 gets, 0.64 ms each), but the model
  had rated it 3× cheaper than the hash join, while in reality the two were
  close.
- *Measurements:*
  - a Thrift round trip ≈ 0.65 ms;
  - a scanned row ≈ 1 µs;
  - forced nested loop vs forced hash join by outer size:
    5 → 4.7 vs 18.0 ms, 20 → 14.2 vs 18.7, **30 → 26.5 vs 21.4**,
    60 → 40.6 vs 17.9.
  - So the real switch is at **~25 outer rows**.
- `HB_RPC_COST` 5 had rated the 20-row nested loop 3× cheaper than the
  hash join. Raised to 10 (the raw ratio), the planner still switched at
  40–60 (measured with the sweep above).
- **Set `HB_RPC_COST` 15, `HB_ROW_COST` 0.005.**
  - The planner now switches between 25 and 30, matching the measured
    switch.
  - This is a bit above the raw 700:1 RPC:row ratio, because core's own
    hash-join CPU costs are not wall-clock calibrated.
  - The comment at the constants records this.
- Regression output unchanged (the tests print rows, not costs); the suite
  passes twice in a row.

**6.1 README** (`README.md`, Week-1 text kept and extended):
- new intro;
- a "Row key as an index" section:
  - the table of what is pushed, per operator and key type;
  - why ranges need `COLLATE "C"` (and that `C.UTF-8` doesn't count);
  - integer keys;
  - EXPLAIN fields;
  - row estimates, costs and order;
- a Timings section;
- the RPCs and the plan, updated in Design;
- new Limitations;
- "Why not a real index access method?";
- the expected-file workflow (`test/pg16_expected.py`) and new files.
- It is honest that PG16 is only "has code paths + derived expected file",
  not run.

**Left in place (outside the contrib dir).**
- The `hbase_fdw_w1` extension files are in the PG install
  (`lib/hbase_fdw_w1.so`, `share/extension/hbase_fdw_w1*`).
- The `bench_w1` and `bench_new` databases.
- Both are kept so the timings can be re-run; how to remove them is in the
  handoff.

**Also.** `~/cs631/hbase_fdw_handoff_lines.py` refreshes the handoff's
symbol line numbers from the source (it found that the Phase 5 table had
drifted by one after a late edit).

**In short:** proven on both servers, measured (point lookups ~26× faster,
ranges and prefixes ~8–10×, small joins 1.4×, full scans unchanged), costs
calibrated to the measurements, and documented.
