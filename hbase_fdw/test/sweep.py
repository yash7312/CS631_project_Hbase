# sweep.py -- join plan choice vs network RTT, static vs measured costs
#
#   python3 test/sweep.py [out.csv]        (proxy running and server hb on 9190)
#
# For each RTT and outer size n it times the join with the plan the planner
# picks (static and measured costs) and with each plan forced, and writes
#   rtt, n, model, chosen, ms, nl_ms, hash_ms, regret
# regret = time of the chosen plan / time of the best plan (1.0 = right choice).
# Every run is a new psql session, so measured costs re-probe the RTT.
import json, subprocess, sys, csv, statistics

RTTS = [0, 1, 5, 10, 20]
NS = [1, 10, 30, 100, 300, 1000]
RUNS = 3
Q = ("SELECT count(e.name) FROM (SELECT emp_id FROM ord1k LIMIT %d) o "
     "JOIN emp e ON e.id = o.emp_id")
NL = "SET enable_hashjoin = off; SET enable_mergejoin = off;"
HASH = "SET enable_nestloop = off; SET enable_mergejoin = off;"

def ctl(*a):
    subprocess.run(["python3", "test/ctl.py"] + [str(x) for x in a],
                   check=True, capture_output=True)

def run(sets, q):
    sql = sets + " EXPLAIN (ANALYZE, FORMAT JSON) " + q
    out = subprocess.run(["psql", "-XAtq", "-d", "test", "-c", sql],
                         check=True, capture_output=True, text=True).stdout
    p = json.loads(out)[0]
    return p["Execution Time"], method(p["Plan"])

def method(node):
    t = node["Node Type"]
    if t in ("Nested Loop", "Hash Join", "Merge Join"):
        return {"Nested Loop": "nl", "Hash Join": "hash", "Merge Join": "merge"}[t]
    for c in node.get("Plans", []):
        m = method(c)
        if m:
            return m
    return None

def med(sets, q):
    ts, m = [], None
    for _ in range(RUNS):
        t, m = run(sets, q)
        ts.append(t)
    return statistics.median(ts), m

out = sys.argv[1] if len(sys.argv) > 1 else "sweep.csv"
w = csv.writer(open(out, "w", newline=""))
w.writerow(["rtt", "n", "model", "chosen", "ms", "nl_ms", "hash_ms", "regret"])
for r in RTTS:
    ctl("rtt", r)
    for n in NS:
        q = Q % n
        run("", q)                              # warm up
        nl, _ = med(NL, q)
        hs, _ = med(HASH, q)
        best = min(nl, hs)
        for model, sets in (("static", "SET hbase_fdw.adaptive_costs = off;"),
                            ("measured", "SET hbase_fdw.adaptive_costs = on;")):
            t, m = med(sets, q)
            row = [r, n, model, m, round(t, 2), round(nl, 2), round(hs, 2), round(t / best, 2)]
            w.writerow(row)
            print(*row, flush=True)
