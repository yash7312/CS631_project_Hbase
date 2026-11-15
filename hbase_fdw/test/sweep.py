# sweep.py -- join plan choice vs network RTT and bandwidth, static vs measured costs
#
#   python3 test/sweep.py [out.csv]        (proxy running and server hb on 9190)
#
# For each RTT, bandwidth and outer size n it times the join with each plan
# forced (nested loop of gets, hash join over a full scan), then asks the
# planner which plan it picks with static and with measured costs. Writes
#   rtt, bw, n, model, chosen, ms, nl_ms, hash_ms, regret
# where ms is the forced time of the chosen plan and regret = ms / best
# (1.0 = the planner picked the faster plan). Every psql call is a new
# session, so measured costs re-probe the network each time.
import json, subprocess, sys, csv, statistics

RTTS = [0, 5, 20]                                  # ms
BWS = [0, 100, 10]                                 # Mbit/s, 0 = unlimited
NS = [1, 10, 15, 20, 25, 30, 40, 100, 300]         # outer rows
RUNS = 3
Q = ("SELECT count(e.name) FROM (SELECT emp_id FROM ord1k LIMIT %d) o "
     "JOIN emp e ON e.id = o.emp_id")
FORCE = {"nl": "SET enable_hashjoin = off; SET enable_mergejoin = off;",
         "hash": "SET enable_nestloop = off; SET enable_mergejoin = off;"}
MODEL = {"static": "SET hbase_fdw.adaptive_costs = off;",
         "measured": "SET hbase_fdw.adaptive_costs = on;"}

def ctl(*a):
    subprocess.run(["python3", "test/ctl.py"] + [str(x) for x in a],
                   check=True, capture_output=True)

def explain(sets, q, analyze):
    opt = "ANALYZE, FORMAT JSON" if analyze else "FORMAT JSON"
    out = subprocess.run(["psql", "-XAtq", "-d", "test", "-c",
                          sets + " EXPLAIN (" + opt + ") " + q],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)[0]

def method(node):
    t = node["Node Type"]
    if t in ("Nested Loop", "Hash Join", "Merge Join"):
        return {"Nested Loop": "nl", "Hash Join": "hash", "Merge Join": "merge"}[t]
    for c in node.get("Plans", []):
        m = method(c)
        if m:
            return m
    return None

def timed(sets, q):
    explain(sets, q, True)                         # warm up
    return statistics.median(explain(sets, q, True)["Execution Time"] for _ in range(RUNS))

out = sys.argv[1] if len(sys.argv) > 1 else "sweep.csv"
w = csv.writer(open(out, "w", newline=""))
w.writerow(["rtt", "bw", "n", "model", "chosen", "ms", "nl_ms", "hash_ms", "regret"])
for r in RTTS:
    ctl("rtt", r)
    for b in BWS:
        ctl("bw", b)
        for n in NS:
            q = Q % n
            t = {m: timed(s, q) for m, s in FORCE.items()}
            best = min(t.values())
            for model, sets in MODEL.items():
                m = method(explain(sets, q, False)["Plan"])
                ms = t.get(m, float("nan"))
                row = [r, b, n, model, m, round(ms, 2), round(t["nl"], 2),
                       round(t["hash"], 2), round(ms / best, 2)]
                w.writerow(row)
                print(*row, flush=True)
ctl("bw", 0)
ctl("rtt", 0)
