# Figure 1: how much slower the chosen join plan is than the best one,
# for fixed costs and measured costs, at each RTT and bandwidth.
# Data: results/sweep_costs.csv     Output: graphs/fig1_regret.png
import csv
import matplotlib.pyplot as plt

rows = []
with open("results/sweep_costs.csv") as f:
    for r in csv.DictReader(f):
        rows.append(r)

rtts = [0, 5, 20]
bws = [0, 100, 10]
bw_name = {0: "unlimited", 100: "100 Mbit/s", 10: "10 Mbit/s"}


def mean_regret(model, bw, rtt):
    total = 0
    count = 0
    for r in rows:
        if r["model"] == model and int(r["bw"]) == bw and int(r["rtt"]) == rtt:
            total = total + float(r["regret"])
            count = count + 1
    return total / count


fig, axes = plt.subplots(1, 3, figsize=(10, 3.5), sharey=True)
for i in range(3):
    ax = axes[i]
    bw = bws[i]
    fixed = []
    measured = []
    for rtt in rtts:
        fixed.append(mean_regret("static", bw, rtt))
        measured.append(mean_regret("measured", bw, rtt))
    ax.plot(rtts, fixed, marker="o", color="#eb6834", label="fixed costs")
    ax.plot(rtts, measured, marker="o", color="#2a78d6", label="measured costs")
    ax.set_title("Bandwidth " + bw_name[bw])
    ax.set_xlabel("RTT (ms)")
    ax.set_xticks(rtts)
    ax.grid(True, color="#e6e6e3")

axes[0].set_ylabel("Mean plan regret (1.0 = best plan)")
axes[0].legend()
fig.suptitle("Measured costs keep the planner on the fastest join plan")
fig.tight_layout()
fig.savefig("graphs/fig1_regret.png", dpi=200)
print("wrote graphs/fig1_regret.png")
