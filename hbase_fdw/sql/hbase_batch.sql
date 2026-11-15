-- batched index join: K outer keys per getRowsWithColumns call
-- needs the same HBase data as hbase_fdw.sql
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS hbase_fdw CASCADE;
CREATE EXTENSION hbase_fdw;
RESET client_min_messages;
SET hbase_fdw.adaptive_costs = off;

CREATE SERVER hb FOREIGN DATA WRAPPER hbase_fdw
  OPTIONS (host 'localhost', port '9090');
CREATE FOREIGN TABLE emp (
  id     text OPTIONS (rowkey 'true'),
  name   text OPTIONS (column 'info:name') NOT NULL,
  dept   text OPTIONS (column 'info:dept'),
  salary int  OPTIONS (column 'pay:salary')
) SERVER hb OPTIONS (table 'emp', rows '10000');
CREATE TABLE emp_local (id text PRIMARY KEY, name text, dept text,
                        joined date, salary int, bonus numeric(10,2));
\copy emp_local from 'test/emp.csv' with (format csv)

-- outer keys: duplicates, NULL, empty, missing
CREATE TABLE bo (k text, tag int);
INSERT INTO bo VALUES ('e000001', 1), ('e000001', 2), (NULL, 3), ('', 4),
  ('nope', 5), ('e000010', 6), ('e000003', 7), ('e000010', 8);
ANALYZE bo;

-- the plan: one Custom Scan over the outer side, no Foreign Scan
EXPLAIN (COSTS OFF) SELECT bo.tag, e.name FROM bo JOIN emp e ON e.id = bo.k;

-- K = 2 crosses batches; rows come back like the local join's
SET hbase_fdw.join_batch = 2;
SELECT bo.tag, e.id, e.name FROM bo JOIN emp e ON e.id = bo.k ORDER BY bo.tag;
SELECT count(*) AS diff FROM (
  (SELECT bo.tag, e.id, e.name FROM bo JOIN emp e ON e.id = bo.k
   EXCEPT ALL
   SELECT bo.tag, l.id, l.name FROM bo JOIN emp_local l ON l.id = bo.k)
  UNION ALL
  (SELECT bo.tag, l.id, l.name FROM bo JOIN emp_local l ON l.id = bo.k
   EXCEPT ALL
   SELECT bo.tag, e.id, e.name FROM bo JOIN emp e ON e.id = bo.k)) d;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT bo.tag, e.name FROM bo JOIN emp e ON e.id = bo.k;

-- a join qual on a non-key column is checked on each joined row
SELECT bo.tag, e.salary > 150000 AS big FROM bo JOIN emp e
  ON e.id = bo.k AND e.salary > 0 ORDER BY bo.tag;

-- v1 joins over the cheapest outer path, so ORDER BY sorts above the join
EXPLAIN (COSTS OFF) SELECT bo.tag, e.name FROM bo JOIN emp e ON e.id = bo.k
  ORDER BY bo.tag;

-- rescans: the join inside a correlated subquery
SELECT g, (SELECT count(*) FROM bo JOIN emp e ON e.id = bo.k WHERE bo.tag <= g) AS n
  FROM generate_series(1, 8) g;

-- 1000 outer rows at K = 1, 7 and 500: same result as the local join
CREATE TABLE bo1k AS SELECT g AS oid,
  'e' || lpad((g * 37 % 10000 + 1)::text, 6, '0') AS k FROM generate_series(1, 1000) g;
ANALYZE bo1k;
CREATE FUNCTION bj_diff() RETURNS bigint LANGUAGE sql AS $$
  SELECT count(*) FROM (
    (SELECT o.oid, e.name FROM bo1k o JOIN emp e ON e.id = o.k
     EXCEPT ALL SELECT o.oid, l.name FROM bo1k o JOIN emp_local l ON l.id = o.k)
    UNION ALL
    (SELECT o.oid, l.name FROM bo1k o JOIN emp_local l ON l.id = o.k
     EXCEPT ALL SELECT o.oid, e.name FROM bo1k o JOIN emp e ON e.id = o.k)) d $$;
SET hbase_fdw.join_batch = 1;   SELECT bj_diff();
SET hbase_fdw.join_batch = 7;   SELECT bj_diff();
SET hbase_fdw.join_batch = 500; SELECT bj_diff();
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT count(e.name) FROM bo1k o JOIN emp e ON e.id = o.k;

-- LEFT JOIN: an outer row with no match comes back once, with NULLs
SET hbase_fdw.join_batch = 3;
EXPLAIN (COSTS OFF) SELECT bo.tag, e.name FROM bo LEFT JOIN emp e ON e.id = bo.k;
SELECT bo.tag, e.id, e.name FROM bo LEFT JOIN emp e ON e.id = bo.k ORDER BY bo.tag;
-- ON on an HBase column decides the match; WHERE filters afterwards
EXPLAIN (COSTS OFF) SELECT bo.tag, e.id, e.salary FROM bo LEFT JOIN emp e
  ON e.id = bo.k AND e.salary > 150000;
SELECT bo.tag, e.id, e.salary FROM bo LEFT JOIN emp e
  ON e.id = bo.k AND e.salary > 150000 ORDER BY bo.tag;
SELECT bo.tag, e.id FROM bo LEFT JOIN emp e ON e.id = bo.k
  WHERE e.salary > 150000 OR bo.tag > 4 ORDER BY bo.tag;
-- 1000 outer rows, about half of them with no HBase row
CREATE TABLE bo2 AS SELECT g AS oid,
  'e' || lpad((g * 37 % 20000 + 1)::text, 6, '0') AS k FROM generate_series(1, 1000) g;
ANALYZE bo2;
-- force the batched join (K = 500) so these check it, not a hash join
SET hbase_fdw.join_batch = 500;
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_nestloop = off;
SELECT count(*) AS rows, count(e.id) AS matched FROM bo2 o LEFT JOIN emp e ON e.id = o.k;
SELECT count(*) AS diff FROM (
  (SELECT o.oid, e.name FROM bo2 o LEFT JOIN emp e ON e.id = o.k
   EXCEPT ALL SELECT o.oid, l.name FROM bo2 o LEFT JOIN emp_local l ON l.id = o.k)
  UNION ALL
  (SELECT o.oid, l.name FROM bo2 o LEFT JOIN emp_local l ON l.id = o.k
   EXCEPT ALL SELECT o.oid, e.name FROM bo2 o LEFT JOIN emp e ON e.id = o.k)) d;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT count(e.name) FROM bo2 o LEFT JOIN emp e ON e.id = o.k;
RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_nestloop;

-- switched off: back to the parameterized nested loop or a hash join
SET hbase_fdw.batch_join = off;
EXPLAIN (COSTS OFF) SELECT bo.tag, e.name FROM bo JOIN emp e ON e.id = bo.k;

DROP TABLE bo, bo1k, bo2, emp_local;
DROP FUNCTION bj_diff();
DROP EXTENSION hbase_fdw CASCADE;
