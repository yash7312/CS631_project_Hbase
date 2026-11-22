-- parallel scan by region: the leader and the workers each claim whole
-- regions; the mock splits a table every 2500 rows, real HBase may have one
-- region, the results must be the same either way
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS hbase_fdw CASCADE;
CREATE EXTENSION hbase_fdw;
RESET client_min_messages;
SET hbase_fdw.adaptive_costs = off;
SET hbase_fdw.batch_join = off;

CREATE SERVER hb FOREIGN DATA WRAPPER hbase_fdw
  OPTIONS (host 'localhost', port '9090');
CREATE FOREIGN TABLE emp (
  id     text OPTIONS (rowkey 'true'),
  name   text OPTIONS (column 'info:name') NOT NULL,
  dept   text OPTIONS (column 'info:dept'),
  joined date OPTIONS (column 'info:joined'),
  salary int  OPTIONS (column 'pay:salary'),
  bonus  numeric(10,2) OPTIONS (column 'pay:bonus')
) SERVER hb OPTIONS (table 'emp', rows '10000');
CREATE FOREIGN TABLE emp_c (
  id     text OPTIONS (rowkey 'true') COLLATE "C",
  salary int  OPTIONS (column 'pay:salary')
) SERVER hb OPTIONS (table 'emp', rows '1000000');
CREATE TABLE emp_local (id text PRIMARY KEY, name text, dept text,
                        joined date, salary int, bonus numeric(10,2));
\copy emp_local from 'test/emp.csv' with (format csv)

-- make Gather free so the parallel plan is chosen; up to 3 workers, so
-- 4 regions (the mock) take 3 workers, 1 region (real HBase emp) no Gather
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET max_parallel_workers_per_gather = 3;

EXPLAIN (COSTS OFF) SELECT count(*) FROM emp;
SELECT count(*), count(DISTINCT id), min(id), max(id), sum(salary) FROM emp;

-- every row exactly once, same as the local copy
SELECT count(*) AS diff FROM (
  (SELECT * FROM emp EXCEPT ALL SELECT * FROM emp_local)
  UNION ALL
  (SELECT * FROM emp_local EXCEPT ALL SELECT * FROM emp)) d;

-- a key range across region edges: only the regions it touches, clipped
-- (a range is guessed at 0.5% of the table: rows 1000000 makes that 5000)
EXPLAIN (COSTS OFF) SELECT count(*) FROM emp_c WHERE id >= 'e002000' AND id < 'e005600';
SELECT count(*), min(id), max(id) FROM emp_c WHERE id >= 'e002000' AND id < 'e005600';
SELECT count(*), min(id), max(id) FROM emp_c WHERE id > 'e007400';
SELECT count(*) FROM emp_c WHERE id >= 'e002501' AND id < 'e002501';

-- key lookups are never parallel
EXPLAIN (COSTS OFF) SELECT * FROM emp_c WHERE id = 'e000005';

-- the Gather is rescanned for every outer row: regions are handed out again
-- (a temp table cannot be read by workers, so it stays outside the Gather)
CREATE TEMP TABLE three (n int);
INSERT INTO three VALUES (1), (2), (3);
ANALYZE three;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
EXPLAIN (COSTS OFF)
SELECT t.n, count(*) FROM three t JOIN emp_c e ON e.salary > t.n * 100000 GROUP BY 1 ORDER BY 1;
SELECT t.n, count(*) FROM three t JOIN emp_c e ON e.salary > t.n * 100000 GROUP BY 1 ORDER BY 1;
SELECT t.n, count(*) FROM three t JOIN emp_local e ON e.salary > t.n * 100000 GROUP BY 1 ORDER BY 1;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- off
SET hbase_fdw.parallel_workers = 0;
EXPLAIN (COSTS OFF) SELECT count(*) FROM emp;
