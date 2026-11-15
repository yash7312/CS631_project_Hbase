# Figure 2: join time against outer rows for the three join methods,
# at RTT 5 ms and 20 ms (no bandwidth cap).
# Data: results/sweep_joins.csv     Output: graphs/fig2_join_time.png
import csv
import matplotlib.pyplot as plt

rows = []
with open("results/sweep_joins.csv") as f:
    for r in csv.DictReader(f):
        rows.append(r)

ns = [1, 10, 30, 100, 300, 1000]


def times(rtt, column):
    # the forced time of one method, for each outer size n
    out = []
    for n in ns:
        for r in rows:
            if int(r["rtt"]) == rtt and int(r["bw"]) == 0 and int(r["n"]) == n:
                out.append(float(r[column]))
                break
    return out


fig, axes = plt.subplots(1, 2, figsize=(9, 3.6), sharey=True)
rtts = [5, 20]
for i in range(2):
    ax = axes[i]
    rtt = rtts[i]
    ax.plot(ns, times(rtt, "nl_ms"), marker="o", color="#eb6834", label="one get per outer row")
    ax.plot(ns, times(rtt, "hash_ms"), marker="o", color="#6b6b68", label="hash join over a full scan")
    ax.plot(ns, times(rtt, "batch_ms"), marker="o", color="#2a78d6", label="batched join")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_title("RTT " + str(rtt) + " ms, bandwidth unlimited")
    ax.set_xlabel("Outer rows")
    ax.grid(True, color="#e6e6e3")

axes[0].set_ylabel("Join time (ms, log)")
axes[0].legend()
fig.suptitle("The batched join is fastest from 10 outer rows up")
fig.tight_layout()
fig.savefig("graphs/fig2_join_time.png", dpi=200)
print("wrote graphs/fig2_join_time.png")
