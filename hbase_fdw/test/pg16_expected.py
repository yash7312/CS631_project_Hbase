#!/usr/bin/env python3
"""
Derive expected/hbase_fdw_1.out (PG16/17) from expected/hbase_fdw.out (PG18).

The only difference is EXPLAIN ANALYZE: PG18 prints "actual rows=3.00",
older versions "rows=3". hb_explain() strips those parts, so only plain
EXPLAIN ANALYZE blocks change; their psql header is re-padded to the new
width.

    python3 test/pg16_expected.py
"""
import os, re

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "expected", "hbase_fdw.out")
dst = os.path.join(here, "..", "expected", "hbase_fdw_1.out")

lines = open(src).read().split("\n")
out, i = [], 0
while i < len(lines):
    if lines[i].strip() == "QUERY PLAN" and i + 1 < len(lines) and set(lines[i + 1]) == {"-"}:
        j = i + 2
        while not re.match(r"^\(\d+ rows?\)$", lines[j]):
            j += 1
        body = [re.sub(r"(rows=\d+)\.00\b", r"\1", l) for l in lines[i + 2:j]]
        if body != lines[i + 2:j]:
            w = max(len(l) for l in body) - 1          # rows are " " + value
            left = (w - 10) // 2
            out.append(" " * (left + 1) + "QUERY PLAN" + " " * (w - 10 - left + 1))
            out.append("-" * (w + 2))
            out.extend(body)
            i = j
            continue
    out.append(lines[i])
    i += 1
open(dst, "w").write("\n".join(out))
