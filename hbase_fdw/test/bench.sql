-- Benchmark: median ms of n runs (after 1 warm-up) per query, planning
-- included. Needs test/bench_setup.sql first. Output: name|ms
--   psql -d bench_new -f test/bench.sql
\pset format unaligned
\pset tuples_only on
SELECT name || '|' || bench(q, n) FROM (VALUES
 (1, 'full count(*)',             'SELECT count(*) FROM emp', 10),
 (2, 'full scan + agg',           'SELECT dept, sum(salary) FROM emp GROUP BY dept', 10),
 (3, 'point lookup',              'SELECT * FROM emp WHERE id = ''e004242''', 50),
 (4, 'point lookup, miss',        'SELECT * FROM emp WHERE id = ''nope''', 50),
 (5, 'IN, 100 keys',              'SELECT count(*) FROM emp WHERE id IN (' ||
     (SELECT string_agg(quote_literal('e' || lpad((g * 97 % 10000 + 1)::text, 6, '0')), ',') FROM generate_series(1, 100) g) || ')', 30),
 (6, 'BETWEEN, 100 rows (C)',     'SELECT count(*) FROM emp_c WHERE id BETWEEN ''e004200'' AND ''e004299''', 30),
 (7, 'LIKE prefix, 100 rows',     'SELECT count(*) FROM emp WHERE id LIKE ''e0042%''', 30),
 (8, 'ORDER BY key LIMIT 10 (C)', 'SELECT id FROM emp_c ORDER BY id LIMIT 10', 30),
 (9, 'join, 20 outer rows',       'SELECT count(e.name) FROM ord o JOIN emp e ON e.id = o.emp_id', 30),
 (10,'join, 1000 outer rows',     'SELECT count(e.name) FROM ord1k o JOIN emp e ON e.id = o.emp_id', 10)
) v(i, name, q, n) ORDER BY i;
