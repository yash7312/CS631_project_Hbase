# CS631 project: querying HBase from PostgreSQL

**Yash Singh (23B0934), J Adarsh (23B0960)**

`hbase_fdw` is a PostgreSQL 18 extension that queries HBase tables in place,
through the HBase Thrift gateway, and uses HBase's row-key order the way
Postgres uses an index. Nothing is copied into Postgres.

- Key lookups, `IN` lists, ranges, `BETWEEN` and `LIKE 'prefix%'` become HBase
  gets and range scans instead of full scans.
- The planner gets row estimates, costs and the key order, so it prefers key
  lookups and skips sorts.
- Joins on the row key look up each match in HBase, like a nested-loop index
  scan.

On a 10,000-row table, a point lookup went from 36.8 ms to 1.5 ms. The full
write-up is in [`docs/hbase_fdw_report.pdf`](docs/hbase_fdw_report.pdf).

## Quick start (Ubuntu or WSL)

```bash
git clone git@github.com:yash7312/CS631_project_Hbase.git
cd CS631_project_Hbase
./setup.sh                 # builds PostgreSQL 18 + HBase under ./work, runs the tests
source work/env.sh
psql test
```

`setup.sh` prints the `apt install` line if a prerequisite is missing
(build tools, bison, flex, readline, zlib, Java 17, python3-pip). The first run
takes a few minutes. For a quick try without Java, `MOCK=1 ./setup.sh` uses the
in-memory mock HBase instead.

| Command | What it does |
| --- | --- |
| `./setup.sh start` | start PostgreSQL (port 9432), HBase and the Thrift gateway (port 9090) |
| `./setup.sh stop` | stop them |
| `./setup.sh test` | rebuild `hbase_fdw` and run the regression test |

Already have your own PostgreSQL build? Build the extension against it with
`make -C hbase_fdw USE_PGXS=1 PG_CONFIG=/path/to/pg_config install`.

## Layout

| Path | Contents |
| --- | --- |
| `hbase_fdw/` | the extension: FDW callbacks (`hbase_fdw.c`), Thrift client (`hbase_thrift.c`), SQL, tests |
| `hbase_fdw/README.md` | options, what is pushed down and when, EXPLAIN fields, design notes |
| `hbase_fdw/test/` | data loader, mock HBase, benchmark scripts, Thrift IDL |
| `docs/` | report (PDF and LaTeX), handoff notes and the work log |
| `setup.sh` | one-command build, start, stop and test |
