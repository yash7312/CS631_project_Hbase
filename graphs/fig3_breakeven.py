# Figure 3: the number of outer rows where a hash join starts to beat
# per-row gets - the true point, and where each cost model switches.
# Data: results/sweep_costs.csv     Output: graphs/fig3_breakeven.png
import csv
import matplotlib.pyplot as plt

rows = []
with open("results/sweep_costs.csv") as f:
    for r in csv.DictReader(f):
        rows.append(r)

rtts = [0, 5, 20]
bws = [0, 100, 10]
bw_name = {0: "unlimited", 100: "100 Mbit/s", 10: "10 Mbit/s"}
ns = [1, 10, 15, 20, 25, 30, 40, 100, 300]
NONE = 400          # plotted when there is no switch up to 300 rows


def find(rtt, bw, n, model):
    for r in rows:
        if int(r["rtt"]) == rtt and int(r["bw"]) == bw and int(r["n"]) == n and r["model"] == model:
            return r
    return None


def true_switch(rtt, bw):
    # first n where the forced hash join is not slower than the forced gets
    for n in ns:
        r = find(rtt, bw, n, "static")
        if float(r["hash_ms"]) <= float(r["nl_ms"]):
            return n
    return NONE


def model_switch(rtt, bw, model):
    # first n where this cost model picks the hash join
    for n in ns:
        r = find(rtt, bw, n, model)
        if r["chosen"] == "hash":
            return n
    return NONE


fig, axes = plt.subplots(1, 3, figsize=(10, 3.8), sharey=True)
for i in range(3):
    ax = axes[i]
    bw = bws[i]
    true_n = []
    fixed_n = []
    measured_n = []
    for rtt in rtts:
        true_n.append(true_switch(rtt, bw))
        fixed_n.append(model_switch(rtt, bw, "static"))
        measured_n.append(model_switch(rtt, bw, "measured"))
    ax.plot(rtts, true_n, marker="s", linestyle="--", color="black", label="true break-even")
    ax.plot(rtts, fixed_n, marker="o", color="#eb6834", label="fixed costs switch")
    ax.plot(rtts, measured_n, marker="o", color="#2a78d6", label="measured costs switch")
    ax.axhline(NONE, color="#cccccc")
    ax.set_yscale("log")
    ax.set_ylim(10, 700)
    ax.set_title("Bandwidth " + bw_name[bw])
    ax.set_xlabel("RTT (ms)")
    ax.set_xticks(rtts)
    ax.grid(True, color="#e6e6e3")

axes[0].set_ylabel("Outer rows where a hash join\nbeats per-row gets (log)")
axes[2].text(20, 450, "top line: no switch up to 300 rows", fontsize=8, ha="right")
axes[1].legend(loc="center right", fontsize=8)
fig.suptitle("Fixed costs switch at the same point everywhere; measured costs follow the network")
fig.tight_layout()
fig.savefig("graphs/fig3_breakeven.png", dpi=200)
print("wrote graphs/fig3_breakeven.png")
