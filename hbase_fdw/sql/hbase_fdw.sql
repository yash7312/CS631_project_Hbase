-- needs an HBase thrift server on localhost:9090 loaded by test/load_data.py
-- (10000 emp rows); test/emp.csv is the same data for the local copy
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS hbase_fdw CASCADE;
CREATE EXTENSION hbase_fdw;
RESET client_min_messages;
-- fixed costs: measured ones vary between runs and would change plans
SET hbase_fdw.adaptive_costs = off;
SET hbase_fdw.batch_join = off;  -- tested in hbase_batch.sql

CREATE SERVER hb FOREIGN DATA WRAPPER hbase_fdw
  OPTIONS (host 'localhost', port '9090');

-- option validation
CREATE SERVER bad FOREIGN DATA WRAPPER hbase_fdw OPTIONS (port '99999');
CREATE SERVER bad FOREIGN DATA WRAPPER hbase_fdw OPTIONS (hots 'x');
CREATE FOREIGN TABLE bad (a int OPTIONS (column 'nocolon')) SERVER hb;
CREATE FOREIGN TABLE bad (a int) SERVER hb OPTIONS (batch_size '0');

-- name is NOT NULL: every emp row has info:name, so the FDW may project
-- down to qualifiers
CREATE FOREIGN TABLE emp (
  id     text OPTIONS (rowkey 'true'),
  name   text OPTIONS (column 'info:name') NOT NULL,
  dept   text OPTIONS (column 'info:dept'),
  joined date OPTIONS (column 'info:joined'),
  salary int  OPTIONS (column 'pay:salary'),
  bonus  numeric(10,2) OPTIONS (column 'pay:bonus')
) SERVER hb OPTIONS (table 'emp', batch_size '500', rows '10000');

-- same table, no NOT NULL promise, unmapped columns default to family info
CREATE FOREIGN TABLE emp2 (
  id   text OPTIONS (rowkey 'true'),
  name text,
  dept text,
  salary int OPTIONS (column 'pay:salary')
) SERVER hb OPTIONS (table 'emp', family 'info');

CREATE TABLE emp_local (id text PRIMARY KEY, name text, dept text,
                        joined date, salary int, bonus numeric(10,2));
\copy emp_local from 'test/emp.csv' with (format csv)

-- full scans
SELECT count(*) FROM emp;
SELECT * FROM emp ORDER BY id LIMIT 8;
SELECT dept, count(*), sum(salary), count(bonus), round(avg(bonus), 2)
  FROM emp GROUP BY dept ORDER BY dept;

-- identical to the local copy, both directions
SELECT count(*) AS missing FROM (TABLE emp_local EXCEPT TABLE emp) s;
SELECT count(*) AS extra   FROM (TABLE emp EXCEPT TABLE emp_local) s;
SELECT count(*) AS dup FROM (SELECT id FROM emp GROUP BY id HAVING count(*) > 1) s;

-- quals on non-key columns stay local: HBase has no index on them
SELECT id, name, salary FROM emp WHERE id = 'e004242';
SELECT count(*) FROM emp WHERE salary > 250000 AND joined >= '2020-01-01';
SELECT count(*) FROM emp_local WHERE salary > 250000 AND joined >= '2020-01-01';

-- row-key point lookups: one getRowsWithColumns call instead of a scan.
-- hb_explain keeps the lines that matter and drops "actual rows", whose
-- format differs between PG versions.
CREATE FUNCTION hb_explain(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l ~ 'Scan|Filter|HBase|Result' THEN
      RETURN NEXT regexp_replace(l, ' \(actual .*\)$', '');
    END IF;
  END LOOP;
END $$;
-- non-key qual: full scan, 10000 rows / 500 per batch = 20 RPCs + 1 empty
SELECT hb_explain($$SELECT * FROM emp WHERE salary = 98000$$);
-- key qual: 1 get
SELECT hb_explain($$SELECT * FROM emp WHERE id = 'e004242'$$);
SELECT hb_explain($$SELECT * FROM emp WHERE 'e004242' = id AND salary > 0$$);
SELECT id, name, salary FROM emp WHERE 'e004242' = id;
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id = 'nope'$$);
SELECT count(*) FROM emp WHERE id = 'nope';
-- prefix of a real key is not a match
SELECT count(*) FROM emp WHERE id = 'e00424';
-- values known only at run time: prepared statement, init plan
SET plan_cache_mode = force_generic_plan;
PREPARE byid(text) AS SELECT id, name FROM emp WHERE id = $1;
EXPLAIN (COSTS OFF) EXECUTE byid('e000007');
EXECUTE byid('e000007');
EXECUTE byid('e000123');
EXECUTE byid(NULL);
EXECUTE byid('');
DEALLOCATE byid;
RESET plan_cache_mode;
SELECT hb_explain($$SELECT id FROM emp WHERE id = (SELECT 'e00000' || '9')$$);
-- NULL key: no row can match, HBase is not contacted (0 RPCs)
SELECT hb_explain($$SELECT id FROM emp WHERE id = (SELECT NULL::text)$$);
-- volatile values are not pushed down
EXPLAIN (COSTS OFF) SELECT id FROM emp WHERE id = 'e00000' || (random() * 0 + 5)::int;
-- <> on the key: HBase can't skip anything, stays local
EXPLAIN (COSTS OFF) SELECT id FROM emp WHERE id <> 'e000001';
-- every key we can look up agrees with the local copy
SELECT count(*) AS mismatches FROM emp_local l
  WHERE l.id IN ('e000001', 'e000500', 'e009999', 'e010000')
    AND (SELECT row(f.*) FROM emp f WHERE f.id = l.id) IS DISTINCT FROM row(l.*);
-- other key types: varchar, bytea, integer (canonical text "12")
CREATE FOREIGN TABLE emp_vc (id varchar(10) OPTIONS (rowkey 'true'),
  name text OPTIONS (column 'info:name')) SERVER hb OPTIONS (table 'emp');
SELECT hb_explain($$SELECT * FROM emp_vc WHERE id = 'e000010'$$);
SELECT * FROM emp_vc WHERE id = 'e000010';
CREATE FOREIGN TABLE odd_b (k bytea OPTIONS (rowkey 'true'),
  num int OPTIONS (column 'd:num')) SERVER hb OPTIONS (table 'odd');
SELECT hb_explain($$SELECT * FROM odd_b WHERE k = '\x7232'$$);
SELECT * FROM odd_b WHERE k = 'r2'::bytea;
CREATE FOREIGN TABLE ikey (k int OPTIONS (rowkey 'true'),
  sq int OPTIONS (column 'd:sq')) SERVER hb;
SELECT hb_explain($$SELECT * FROM ikey WHERE k = 12$$);
SELECT * FROM ikey WHERE k = 12;
SELECT * FROM ikey WHERE k = 12::bigint;
SELECT * FROM ikey WHERE k = 1::smallint;
SELECT count(*) FROM ikey WHERE k = 13;

-- row-key ranges: pushed only where Postgres order = HBase byte order, i.e.
-- a C collation, bytea, or the ~<~ family; bounds intersect to one range
CREATE FOREIGN TABLE emp_c (id text OPTIONS (rowkey 'true') COLLATE "C",
  salary int OPTIONS (column 'pay:salary')) SERVER hb OPTIONS (table 'emp');
SELECT hb_explain($$SELECT count(*) FROM emp_c WHERE id BETWEEN 'e000100' AND 'e000199'$$);
SELECT count(*), min(id), max(id) FROM emp_c WHERE id BETWEEN 'e000100' AND 'e000199';
SELECT hb_explain($$SELECT id FROM emp_c WHERE id > 'e009990' AND id >= 'e009000'$$);
SELECT id FROM emp_c WHERE id > 'e009995';
SELECT hb_explain($$SELECT count(*) FROM emp_c WHERE 'e000005' > id$$);
SELECT id FROM emp_c WHERE 'e000005' > id;
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id <= 'e000003' COLLATE "C"$$);
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id ~>=~ 'e009998'$$);
SELECT count(*) FROM emp WHERE id ~>=~ 'e009998';
-- empty intersection, and nothing sorts below '': 0 RPCs
SELECT hb_explain($$SELECT count(*) FROM emp_c WHERE id > 'e5' AND id < 'e1'$$);
SELECT hb_explain($$SELECT count(*) FROM emp_c WHERE id < ''$$);
SELECT count(*) FROM emp_c WHERE id >= '';
-- not pushed: database collation (not C here), integer keys ('10' < '9')
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id < 'e000100'$$);
EXPLAIN (COSTS OFF) SELECT * FROM ikey WHERE k > 9;
SELECT k FROM ikey WHERE k > 9 ORDER BY k;
-- run-time bounds
SET plan_cache_mode = force_generic_plan;
PREPARE rg(text, text) AS SELECT count(*) FROM emp_c WHERE id >= $1 AND id < $2;
EXPLAIN (COSTS OFF) EXECUTE rg('e000100', 'e000200');
EXECUTE rg('e000100', 'e000200');
EXECUTE rg('e000200', 'e000100');
EXECUTE rg(NULL, 'e000100');
DEALLOCATE rg;
RESET plan_cache_mode;

-- key prefixes: LIKE 'p%', starts_with, ^@ read [p, p+1)
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id LIKE 'e0001%'$$);
SELECT count(*) FROM emp WHERE id LIKE 'e0001%';
SELECT hb_explain($$SELECT count(*) FROM emp WHERE starts_with(id, 'e0099')$$);
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id ^@ 'e0099'$$);
SELECT count(*) FROM emp WHERE id ^@ 'e0099';
-- no wildcard: a point lookup; leading wildcard: nothing to push
SELECT hb_explain($$SELECT id FROM emp WHERE id LIKE 'e000123'$$);
EXPLAIN (COSTS OFF) SELECT id FROM emp WHERE id LIKE '%1';
-- not pushed: ILIKE, value LIKE key
EXPLAIN (COSTS OFF) SELECT id FROM emp WHERE id ILIKE 'e0001%';
EXPLAIN (COSTS OFF) SELECT id FROM emp WHERE 'e000123' LIKE id;
-- escapes and multibyte characters (keys sorted by bytes, n = position)
CREATE FOREIGN TABLE ukey (k text OPTIONS (rowkey 'true') COLLATE "C",
  n int OPTIONS (column 'd:n')) SERVER hb;
SELECT * FROM ukey;
SELECT * FROM ukey WHERE k LIKE 'e\%%';
SELECT hb_explain($$SELECT * FROM ukey WHERE k LIKE 'e#_x' ESCAPE '#'$$);
SELECT * FROM ukey WHERE k LIKE 'e#_x' ESCAPE '#';
SELECT * FROM ukey WHERE k LIKE 'e_';
SELECT hb_explain($$SELECT * FROM ukey WHERE k LIKE 'é%'$$);
SELECT * FROM ukey WHERE k LIKE 'é%';
SELECT * FROM ukey WHERE k BETWEEN 'e' AND 'é';
-- 0xff has no successor: a\xff% stops at b, \xff% has no stop row
CREATE FOREIGN TABLE bkey (k bytea OPTIONS (rowkey 'true'),
  n int OPTIONS (column 'd:n')) SERVER hb;
SELECT * FROM bkey;
SELECT hb_explain($$SELECT n FROM bkey WHERE k LIKE '\x61ff25'$$);
SELECT n FROM bkey WHERE k LIKE '\x61ff25';
SELECT hb_explain($$SELECT n FROM bkey WHERE k LIKE '\x61feff25'$$);
SELECT hb_explain($$SELECT n FROM bkey WHERE k LIKE '\xff25'$$);
SELECT n FROM bkey WHERE k LIKE '\xff25';
SELECT n FROM bkey WHERE k > '\x61ff' AND k <= '\xff';
-- run-time pattern
SET plan_cache_mode = force_generic_plan;
PREPARE lk(text) AS SELECT * FROM ukey WHERE k LIKE $1;
EXPLAIN (COSTS OFF) EXECUTE lk('ex%');
EXECUTE lk('ex%');
EXECUTE lk('%x');
EXECUTE lk(NULL);
DEALLOCATE lk;
RESET plan_cache_mode;
-- ranges and prefixes agree with the local copy (byte order)
SELECT count(*) AS mismatches FROM
  (VALUES ('e000990', 'e001010'), ('e0099', 'e01'), ('e', 'e000002'),
          ('e009999', 'f'), ('a', 'z')) v(lo, hi)
  WHERE (SELECT count(*) FROM emp_c WHERE id >= lo AND id < hi) <>
        (SELECT count(*) FROM emp_local WHERE id >= lo COLLATE "C" AND id < hi COLLATE "C");
SELECT count(*) AS mismatches FROM (VALUES ('e00999'), ('e0100'), ('e1'), ('e')) v(p)
  WHERE (SELECT count(*) FROM emp WHERE id LIKE p || '%') <>
        (SELECT count(*) FROM emp_local WHERE id LIKE p || '%');

-- key lists: IN / = ANY fetch the distinct non-NULL keys, batch_size per get,
-- in key order, clipped to any range on the key
SELECT hb_explain($$SELECT id, salary FROM emp WHERE id IN ('e000003', 'e000001', 'nope', 'e000003', NULL, '')$$);
SELECT id, salary FROM emp WHERE id IN ('e000003', 'e000001', 'nope', 'e000003', NULL, '');
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id = ANY (ARRAY['e000001', 'e000002']) AND id IN ('e000002', 'e000009')$$);
SELECT hb_explain($$SELECT id FROM emp_c WHERE id IN ('e000001', 'e000500', 'e009000') AND id > 'e000100'$$);
SELECT id FROM emp_c WHERE id IN ('e000001', 'e000500', 'e009000') AND id > 'e000100';
-- nothing left to fetch: 0 RPCs
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id IN ('a', 'b') AND id = 'c'$$);
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id = ANY (NULL::text[])$$);
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id = ANY ('{}')$$);
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id IN (NULL, '')$$);
-- 1200 keys / 500 per get = 3 RPCs
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id = ANY (ARRAY(SELECT 'e' || lpad(g::text, 6, '0') FROM generate_series(1, 2400, 2) g))$$);
SELECT count(*), min(id), max(id) FROM emp
  WHERE id = ANY (ARRAY(SELECT 'e' || lpad(g::text, 6, '0') FROM generate_series(1, 2400, 2) g));
-- integer keys, cross-type
SELECT * FROM ikey WHERE k IN (1, 5, 12, 99);
SELECT * FROM ikey WHERE k = ANY ('{3,4}'::bigint[]);
-- a get still needs a cell in a mapped family (r4 has none, as in a scan)
CREATE FOREIGN TABLE odd_d (k text OPTIONS (rowkey 'true'),
  num text OPTIONS (column 'd:num')) SERVER hb OPTIONS (table 'odd');
SELECT * FROM odd_d WHERE k IN ('r1', 'r4', 'r5');
SELECT * FROM odd_d WHERE k = 'r4';
-- not pushed: NOT IN
EXPLAIN (COSTS OFF) SELECT id FROM emp WHERE id NOT IN ('e000001', 'e000002');
-- run-time lists
SET plan_cache_mode = force_generic_plan;
PREPARE ia(text[]) AS SELECT id FROM emp WHERE id = ANY ($1);
EXPLAIN (COSTS OFF) EXECUTE ia('{e000007,e000008}');
EXECUTE ia('{e000008,e000007}');
EXECUTE ia(NULL);
PREPARE ib(text, text) AS SELECT id FROM emp WHERE id IN ($1, $2);
EXECUTE ib('e000009', NULL);
DEALLOCATE ia;
DEALLOCATE ib;
RESET plan_cache_mode;
-- lists agree with the local copy, also when rescanned per outer row
SELECT count(*) AS mismatches FROM emp_local l
  WHERE l.id IN ('e000001', 'e000500', 'e009999', 'e010000', 'e004242')
    AND (SELECT row(f.*) FROM emp f WHERE f.id IN (l.id, 'zz')) IS DISTINCT FROM row(l.*);

-- row estimates: the "rows" option is the table size; = / IN give one row
-- per key, ranges and prefixes default selectivities (no statistics), and
-- local quals filter what HBase sends
CREATE FUNCTION hb_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
    IF l ~ 'Foreign Scan' THEN RETURN substring(l from 'rows=[0-9]+'); END IF;
  END LOOP;
END $$;
SELECT hb_rows(q), q FROM (VALUES
  ('SELECT * FROM emp'),
  ('SELECT * FROM emp2'),
  ('SELECT * FROM emp WHERE salary > 100000'),
  ('SELECT * FROM emp WHERE id = ''e000001'''),
  ('SELECT * FROM emp WHERE id = ''e000001'' AND salary > 100000'),
  ('SELECT * FROM emp WHERE id IN (''a'', ''b'', ''c'')'),
  ('SELECT * FROM emp WHERE id IN (''a'', ''b'', ''c'') AND id = ''b'''),
  ('SELECT * FROM emp WHERE id = ANY (ARRAY(SELECT id FROM emp_local))'),
  ('SELECT * FROM emp_c WHERE id BETWEEN ''e000100'' AND ''e000199'''),
  ('SELECT * FROM emp_c WHERE id > ''e009000'''),
  ('SELECT * FROM emp WHERE id LIKE ''e0001%''')) v(q);
-- the planner now sees a key lookup as cheap: it drives the join
ANALYZE emp_local;
EXPLAIN (COSTS OFF) SELECT f.id, l.name FROM emp f JOIN emp_local l ON l.id = f.id
  WHERE f.id = 'e000042';
-- rows arrive in key byte order: no Sort where Postgres order is the same
-- (C collation, bytea), a Sort otherwise (default collation, integers, DESC)
EXPLAIN (COSTS OFF) SELECT id FROM emp_c ORDER BY id LIMIT 3;
SELECT id FROM emp_c ORDER BY id LIMIT 3;
EXPLAIN (COSTS OFF) SELECT id FROM emp_c WHERE id IN ('e000009', 'e000001') ORDER BY id;
SELECT id FROM emp_c WHERE id IN ('e000009', 'e000001') ORDER BY id;
EXPLAIN (COSTS OFF) SELECT k FROM odd_b ORDER BY k;
EXPLAIN (COSTS OFF) SELECT id FROM emp ORDER BY id LIMIT 3;
EXPLAIN (COSTS OFF) SELECT k FROM ikey ORDER BY k;
EXPLAIN (COSTS OFF) SELECT id FROM emp_c ORDER BY id DESC LIMIT 3;

-- what gets fetched
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM emp;
EXPLAIN (VERBOSE, COSTS OFF) SELECT id, salary FROM emp;
EXPLAIN (VERBOSE, COSTS OFF) SELECT name FROM emp2;
SELECT count(*), count(name), count(salary) FROM emp2;

-- rescans: foreign table on the inner side of a nested loop; the join is on
-- the row key, so each of the 3 outer rows is 1 get (no full scans)
ANALYZE emp_local;
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off; SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT l.id, f.name FROM (SELECT * FROM emp_local WHERE id < 'e000004') l
  JOIN emp f ON f.id = l.id;
SELECT l.id, f.name FROM (SELECT * FROM emp_local WHERE id < 'e000004') l
  JOIN emp f ON f.id = l.id ORDER BY 1;
-- same, joined on a non-key column: a full scan per outer row (scanner
-- reopened on every rescan: 3 x 21 RPCs)
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT l.id, f.id FROM (SELECT * FROM emp_local WHERE id < 'e000004') l
  JOIN emp f ON f.name = l.name AND f.salary = l.salary;
RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_material;
RESET enable_indexscan; RESET enable_indexonlyscan; RESET enable_bitmapscan;

-- joins use the row key like an index: a parameterized path looks up each
-- outer row's key (orders: 20 rows, 2 NULL keys = no RPC, 4 missing keys)
CREATE TABLE ord (oid int, emp_id text);
INSERT INTO ord SELECT g, CASE WHEN g % 7 = 0 THEN NULL
  WHEN g % 5 = 0 THEN 'zz' || g ELSE 'e' || lpad((g * 37 % 10000 + 1)::text, 6, '0') END
  FROM generate_series(1, 20) g;
ANALYZE ord;
SELECT hb_explain($$SELECT o.oid, e.name FROM ord o JOIN emp e ON e.id = o.emp_id$$);
SELECT count(*), count(e.name) FROM ord o JOIN emp e ON e.id = o.emp_id;
SELECT count(*), count(e.name) FROM ord o LEFT JOIN emp e ON e.id = o.emp_id;
SELECT count(*) AS mismatches FROM (
  (SELECT o.oid, e.* FROM ord o LEFT JOIN emp e ON e.id = o.emp_id EXCEPT
   SELECT o.oid, e.* FROM ord o LEFT JOIN emp_local e ON e.id = o.emp_id)
  UNION ALL
  (SELECT o.oid, e.* FROM ord o LEFT JOIN emp_local e ON e.id = o.emp_id EXCEPT
   SELECT o.oid, e.* FROM ord o LEFT JOIN emp e ON e.id = o.emp_id)) x;
-- join clauses that are not plain equivalences, IN, a semi join, LATERAL
SELECT hb_explain($$SELECT count(*) FROM ord o JOIN emp e ON e.id = 'e' || lpad(o.oid::text, 6, '0')$$);
SELECT hb_explain($$SELECT count(*) FROM ord o JOIN emp e ON e.id = ANY (ARRAY[o.emp_id, 'e000001'])$$);
SELECT hb_explain($$SELECT count(*) FROM emp WHERE id IN (SELECT emp_id FROM ord)$$);
SELECT count(*) FROM emp WHERE id IN (SELECT emp_id FROM ord);
SELECT hb_explain($$SELECT o.oid, x.salary FROM ord o,
  LATERAL (SELECT salary FROM emp WHERE id = o.emp_id OFFSET 0) x WHERE o.oid <= 4$$);
SELECT o.oid, x.salary FROM ord o,
  LATERAL (SELECT salary FROM emp WHERE id = o.emp_id OFFSET 0) x WHERE o.oid <= 4;
-- a range per outer row (C collation): one small scan each
SELECT hb_explain($$SELECT count(*) FROM ord o JOIN emp_c e
  ON e.id >= o.emp_id AND e.id < o.emp_id || '5' WHERE o.oid < 3$$);
SELECT count(*) FROM ord o JOIN emp_c e ON e.id >= o.emp_id AND e.id < o.emp_id || '5'
  WHERE o.oid < 3;
-- integer key, bigint outer column
SET enable_hashjoin = off; SET enable_mergejoin = off;
CREATE TABLE nums AS SELECT g::bigint AS b FROM generate_series(10, 14) g;
SELECT hb_explain($$SELECT b, sq FROM nums JOIN ikey ON k = b$$);
SELECT b, sq FROM nums JOIN ikey ON k = b;
RESET enable_hashjoin; RESET enable_mergejoin;
-- 10000 outer rows: one full scan into a hash join beats 10000 gets
EXPLAIN (COSTS OFF) SELECT count(*) FROM emp_local l JOIN emp e ON e.id = l.id;
DROP TABLE ord, nums;

-- self join over two concurrent scanners
SELECT count(*) FROM emp a JOIN emp b ON a.id = b.id AND a.salary = b.salary;

-- edge cases: unicode, empty string, binary, missing cells, bad data
CREATE FOREIGN TABLE odd (
  k   text OPTIONS (rowkey 'true'),
  txt text OPTIONS (column 'd:txt'),
  num int  OPTIONS (column 'd:num'),
  bin bytea OPTIONS (column 'd:bin')
) SERVER hb;
SELECT k, txt, txt = '' AS empty, bin FROM odd WHERE k <> 'r5' ORDER BY k;
SELECT * FROM odd;                -- r5 has num = 'not-a-number'
CREATE FOREIGN TABLE odd_txt (k text OPTIONS (rowkey 'true'),
  bin text OPTIONS (column 'd:bin')) SERVER hb OPTIONS (table 'odd');
SELECT * FROM odd_txt;            -- zero byte in a text column
-- r4 only has family x: in the relation only if x is mapped
CREATE FOREIGN TABLE odd_x (k text OPTIONS (rowkey 'true'),
  num text OPTIONS (column 'd:num'), o text OPTIONS (column 'x:other'))
  SERVER hb OPTIONS (table 'odd');
SELECT k, num, o FROM odd_x ORDER BY k;
SELECT count(*) FROM odd;
SELECT count(*) FROM odd_x;

-- mapping errors
CREATE FOREIGN TABLE nomap (k text OPTIONS (rowkey 'true'), v text) SERVER hb;
SELECT * FROM nomap;
CREATE FOREIGN TABLE twokeys (a text OPTIONS (rowkey 'true'),
  b text OPTIONS (rowkey 'true')) SERVER hb;
SELECT * FROM twokeys;
CREATE FOREIGN TABLE nosuch (k text OPTIONS (rowkey 'true')) SERVER hb;
SELECT * FROM nosuch;
CREATE SERVER dead FOREIGN DATA WRAPPER hbase_fdw OPTIONS (port '1');
CREATE FOREIGN TABLE deadt (k text OPTIONS (rowkey 'true')) SERVER dead;
SELECT * FROM deadt;

-- connection still fine after errors
SELECT count(*) FROM emp;

DROP TABLE emp_local;
DROP FUNCTION hb_explain(text);
DROP FUNCTION hb_rows(text);
DROP EXTENSION hbase_fdw CASCADE;
