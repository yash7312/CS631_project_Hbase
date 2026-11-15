-- INSERT, UPDATE, DELETE into HBase. Uses only keys starting with 'w' and
-- removes them again, so the other tests' data is untouched.
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS hbase_fdw CASCADE;
CREATE EXTENSION hbase_fdw;
RESET client_min_messages;
SET hbase_fdw.adaptive_costs = off;

CREATE SERVER hb FOREIGN DATA WRAPPER hbase_fdw
  OPTIONS (host 'localhost', port '9090');
CREATE FOREIGN TABLE emp (
  id     text OPTIONS (rowkey 'true') COLLATE "C",
  name   text OPTIONS (column 'info:name') NOT NULL,
  dept   text OPTIONS (column 'info:dept'),
  salary int  OPTIONS (column 'pay:salary')
) SERVER hb OPTIONS (table 'emp', rows '10000');
DELETE FROM emp WHERE id LIKE 'w%';   -- leftovers of an earlier failed run

-- insert, then read back
INSERT INTO emp VALUES ('w000001', 'wendy', 'eng', 100), ('w000002', 'will', NULL, 200);
SELECT * FROM emp WHERE id LIKE 'w%' ORDER BY id;
INSERT INTO emp VALUES ('w000003', 'wu', 'ops', 300) RETURNING id, name;

-- the key must be there
INSERT INTO emp VALUES (NULL, 'nobody', 'x', 1);
INSERT INTO emp VALUES ('', 'nobody', 'x', 1);

-- an existing key is overwritten: HBase puts are upserts
INSERT INTO emp VALUES ('w000001', 'wendy2', 'ops', 101);
SELECT * FROM emp WHERE id = 'w000001';

-- update one column; the others stay
EXPLAIN (COSTS OFF) UPDATE emp SET salary = 7 WHERE id = 'w000001';
UPDATE emp SET salary = 7 WHERE id = 'w000001';
SELECT * FROM emp WHERE id = 'w000001';

-- setting a column to NULL deletes its cell
UPDATE emp SET dept = NULL WHERE id = 'w000003';
SELECT * FROM emp WHERE id = 'w000003';

-- the row key cannot change
UPDATE emp SET id = 'w000009' WHERE id = 'w000001';

-- update and delete by range: the scan finds the keys
UPDATE emp SET salary = salary + 1 WHERE id LIKE 'w%';
SELECT id, salary FROM emp WHERE id LIKE 'w%' ORDER BY id;
DELETE FROM emp WHERE id = 'w000002';
SELECT id FROM emp WHERE id LIKE 'w%' ORDER BY id;
DELETE FROM emp WHERE id LIKE 'w%';
SELECT count(*) FROM emp WHERE id LIKE 'w%';

-- 1000 rows in one statement, then gone again
INSERT INTO emp SELECT 'w' || lpad(g::text, 6, '0'), 'n' || g, 'd' || (g % 7), g
  FROM generate_series(1, 1000) g;
SELECT count(*), sum(salary) FROM emp WHERE id LIKE 'w%';
DELETE FROM emp WHERE id LIKE 'w%';
SELECT count(*) FROM emp WHERE id LIKE 'w%';

-- batched INSERT: insert_batch rows per mutateRows call (default 100);
-- 7 leaves a partial last batch
EXPLAIN (COSTS OFF) INSERT INTO emp SELECT 'w' || g, 'n', 'd', g FROM generate_series(1, 3) g;
ALTER FOREIGN TABLE emp OPTIONS (ADD insert_batch '7');
EXPLAIN (COSTS OFF) INSERT INTO emp SELECT 'w' || g, 'n', 'd', g FROM generate_series(1, 3) g;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  INSERT INTO emp SELECT 'w' || lpad(g::text, 6, '0'), 'n' || g, NULLIF('d' || (g % 7), 'd0'), g
  FROM generate_series(1, 1000) g;
SELECT count(*), count(dept), sum(salary) FROM emp WHERE id LIKE 'w%';
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  DELETE FROM emp WHERE id LIKE 'w%';
ALTER FOREIGN TABLE emp OPTIONS (DROP insert_batch);

-- the original data is untouched
SELECT count(*) FROM emp;

DROP EXTENSION hbase_fdw CASCADE;
