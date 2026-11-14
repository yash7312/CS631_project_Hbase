-- Benchmark setup: run in an empty database after CREATE EXTENSION, with
-- psql -v fdw=<wrapper name> (hbase_fdw, or hbase_fdw_w1 for the Week-1
-- baseline), from the contrib directory (it loads test/emp.csv).
--   psql -d bench_new -c 'CREATE EXTENSION hbase_fdw' -v fdw=hbase_fdw -f test/bench_setup.sql
CREATE SERVER hb FOREIGN DATA WRAPPER :fdw OPTIONS (host 'localhost', port '9090');
CREATE FOREIGN TABLE emp (id text OPTIONS (rowkey 'true'), name text OPTIONS (column 'info:name') NOT NULL,
  dept text OPTIONS (column 'info:dept'), joined date OPTIONS (column 'info:joined'),
  salary int OPTIONS (column 'pay:salary'), bonus numeric(10,2) OPTIONS (column 'pay:bonus'))
  SERVER hb OPTIONS (table 'emp', rows '10000');
CREATE FOREIGN TABLE emp_c (id text OPTIONS (rowkey 'true') COLLATE "C", name text OPTIONS (column 'info:name') NOT NULL,
  salary int OPTIONS (column 'pay:salary')) SERVER hb OPTIONS (table 'emp', rows '10000');
CREATE TABLE emp_local (id text PRIMARY KEY, name text, dept text, joined date, salary int, bonus numeric(10,2));
\copy emp_local from 'test/emp.csv' with (format csv)
CREATE TABLE ord AS SELECT g AS oid, 'e' || lpad((g * 37 % 10000 + 1)::text, 6, '0') AS emp_id FROM generate_series(1, 20) g;
CREATE TABLE ord1k AS SELECT g AS oid, 'e' || lpad((g * 37 % 10000 + 1)::text, 6, '0') AS emp_id FROM generate_series(1, 1000) g;
ANALYZE emp_local; ANALYZE ord; ANALYZE ord1k;
-- median ms of n runs (after 1 warm-up), planning included
CREATE FUNCTION bench(q text, n int) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE t timestamptz; ms float8[] := '{}'; i int;
BEGIN
  EXECUTE q;
  FOR i IN 1..n LOOP
    t := clock_timestamp(); EXECUTE q;
    ms := ms || extract(epoch FROM clock_timestamp() - t) * 1000;
  END LOOP;
  RETURN round((SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FROM unnest(ms) x)::numeric, 2);
END $$;
