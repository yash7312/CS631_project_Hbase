-- Basic demo: read, insert, delete.   Run:  psql -a -d demo -f demo/demo.sql
\pset pager off
\timing off

\echo
\echo '=== READ ==='
\echo '-- how many rows does HBase hold?'
SELECT count(*) FROM emp;

\echo '-- point lookup on the row key (one HBase get)'
SELECT * FROM emp WHERE id = 'e000042';

\echo '-- range on the row key (one range scan, 5 rows)'
SELECT id, name, dept FROM emp WHERE id >= 'e000100' ORDER BY id LIMIT 5;

\echo '-- key prefix'
SELECT id, name FROM emp WHERE id LIKE 'e00001%' ORDER BY id LIMIT 5;

\echo '-- the plan shows what was sent to HBase'
EXPLAIN (costs off) SELECT * FROM emp WHERE id = 'e000042';

\echo
\prompt 'Press Enter to go on to INSERT' x
\echo '=== INSERT ==='
INSERT INTO emp (id, name, dept, salary) VALUES
  ('demo001', 'asha', 'eng', 90000),
  ('demo002', 'ravi', 'ops', 70000),
  ('demo003', 'meera', 'eng', 110000);

SELECT * FROM emp WHERE id LIKE 'demo%' ORDER BY id;

\echo '-- INSERT on an existing key overwrites it (HBase put)'
INSERT INTO emp (id, name, dept, salary) VALUES ('demo002', 'ravi', 'ops', 75000);
SELECT * FROM emp WHERE id = 'demo002';

\echo
\echo In another terminal, in the HBase shell:  get "emp", "demo001"
\prompt 'Press Enter to go on to DELETE' x
\echo '=== DELETE ==='
DELETE FROM emp WHERE id = 'demo003';
SELECT id, name FROM emp WHERE id LIKE 'demo%' ORDER BY id;

\echo '-- clean up the rest'
DELETE FROM emp WHERE id LIKE 'demo%';
SELECT count(*) AS demo_rows_left FROM emp WHERE id LIKE 'demo%';
