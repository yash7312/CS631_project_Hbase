# sweep.py -- join plan choice vs network RTT and bandwidth
#
#   python3 test/sweep.py [out.csv]        (proxy running and server hb on 9190)
#
# For each RTT, bandwidth and outer size n it times the join with each plan
# forced: nl (one get per outer row: full HBase scans are disabled, so the
# loop cannot be flipped round a full scan), hash (hash join over a full scan) and
# batch (the batched join). Then it asks the planner what it picks under
#   static   - fixed costs, no batched join
#   measured - measured costs, no batched join
#   batched  - measured costs and the batched join
# and writes  rtt, bw, n, model, chosen, ms, nl_ms, hash_ms, batch_ms, regret
# where ms is the forced time of the chosen plan and regret = ms / best of
# all three (1.0 = the fastest plan there is). A plan that was not forced
# (nlmat: nested loop over a materialized scan) is timed as chosen.
# Every psql call is a new session, so measured costs re-probe the network.
import json, subprocess, sys, csv, statistics

RTTS = [0, 5, 20]                                  # ms
BWS = [0, 100, 10]                                 # Mbit/s, 0 = unlimited
NS = [1, 10, 30, 100, 300, 1000]                   # outer rows
RUNS = 3
Q = ("SELECT count(e.name) FROM (SELECT emp_id FROM ord1k LIMIT %d) o "
     "JOIN emp e ON e.id = o.emp_id")
OFF = "SET hbase_fdw.batch_join = off; "
FORCE = {"nl": OFF + "SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off; "
               "SET hbase_fdw.enable_full_scan = off;",
         "hash": OFF + "SET enable_nestloop = off; SET enable_mergejoin = off;",
         "batch": "SET enable_nestloop = off; SET enable_hashjoin = off; SET enable_mergejoin = off;"}
MODEL = {"static": OFF + "SET hbase_fdw.adaptive_costs = off;",
         "measured": OFF + "SET hbase_fdw.adaptive_costs = on;",
         "batched": "SET hbase_fdw.adaptive_costs = on;"}

def ctl(*a):
    subprocess.run(["python3", "test/ctl.py"] + [str(x) for x in a],
                   check=True, capture_output=True)

def explain(sets, q, analyze):
    opt = "ANALYZE, FORMAT JSON" if analyze else "FORMAT JSON"
    out = subprocess.run(["psql", "-XAtq", "-d", "test", "-c",
                          sets + " EXPLAIN (" + opt + ") " + q],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)[0]

def has(node, t):
    return node["Node Type"] == t or any(has(c, t) for c in node.get("Plans", []))

def method(node):
    # nl = nested loop of gets; nlmat = nested loop over a materialized full scan
    t = node["Node Type"]
    if t == "Custom Scan" and node.get("Custom Plan Provider") == "HBase Batched Join":
        return "batch"
    if t == "Nested Loop":
        return "nlmat" if has(node, "Materialize") else "nl"
    if t in ("Hash Join", "Merge Join"):
        return {"Hash Join": "hash", "Merge Join": "merge"}[t]
    for c in node.get("Plans", []):
        m = method(c)
        if m:
            return m
    return None

def timed(sets, q):
    t = explain(sets, q, True)["Execution Time"]   # warm up
    if t > 2000:
        return t                                   # slow enough that noise doesn't matter
    return statistics.median(explain(sets, q, True)["Execution Time"] for _ in range(RUNS))

out = sys.argv[1] if len(sys.argv) > 1 else "sweep.csv"
w = csv.writer(open(out, "w", newline=""))
w.writerow(["rtt", "bw", "n", "model", "chosen", "ms", "nl_ms", "hash_ms", "batch_ms", "regret"])
for r in RTTS:
    ctl("rtt", r)
    for b in BWS:
        ctl("bw", b)
        for n in NS:
            q = Q % n
            for m, sets in FORCE.items():          # each forced plan is what it says
                got = method(explain(sets, q, False)["Plan"])
                if got != m:
                    print("WARNING: forced %s gave %s at rtt %d bw %d n %d" % (m, got, r, b, n),
                          flush=True)
            t = {m: timed(s, q) for m, s in FORCE.items()}
            for model, sets in MODEL.items():
                m = method(explain(sets, q, False)["Plan"])
                if m not in t:                     # a plan we did not force: time it as is
                    t[m] = timed(sets, q)
                ms = t[m]
                best = min(t.values())
                row = [r, b, n, model, m, round(ms, 2), round(t["nl"], 2),
                       round(t["hash"], 2), round(t["batch"], 2), round(ms / best, 2)]
                w.writerow(row)
                print(*row, flush=True)
ctl("bw", 0)
ctl("rtt", 0)
