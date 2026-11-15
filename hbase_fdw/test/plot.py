# plot.py -- the report's graphs from the sweep results
#
#   python3 hbase_fdw/test/plot.py [results_dir]      (default: results/)
#
# Reads sweep_costs.csv, sweep_joins.csv and writes.csv (made by sweep.py and
# check_d) and writes one PNG per figure next to them.
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

D = sys.argv[1] if len(sys.argv) > 1 else "results"
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"        # categorical slots 1-3
INK, MUTED, GRID, SURF = "#1f1f1e", "#6b6b68", "#e6e6e3", "#fcfcfb"
BW = {0: "unlimited", 100: "100 Mbit/s", 10: "10 Mbit/s"}

plt.rcParams.update({
    "figure.facecolor": SURF, "axes.facecolor": SURF, "savefig.facecolor": SURF,
    "axes.edgecolor": GRID, "axes.labelcolor": MUTED, "axes.titlecolor": INK,
    "xtick.color": MUTED, "ytick.color": MUTED, "axes.grid": True,
    "grid.color": GRID, "grid.linewidth": 0.8, "axes.axisbelow": True,
    "axes.spines.top": False, "axes.spines.right": False, "font.size": 10,
    "lines.linewidth": 2, "lines.markersize": 7, "legend.frameon": False,
})

def rows(f):
    with open(os.path.join(D, f)) as fh:
        return list(csv.DictReader(fh))

def label_end(ax, x, y, text, color, dy=0):
    # direct label at the last point; text stays ink, the line carries the color
    ax.annotate(text, (x, y), xytext=(6, dy), textcoords="offset points",
                va="center", fontsize=9, color=INK)

def save(fig, name):
    fig.tight_layout()
    fig.savefig(os.path.join(D, name), dpi=200)
    plt.close(fig)
    print("wrote", os.path.join(D, name))

# ---- Figure 1: plan regret vs RTT, static vs measured costs, per bandwidth ----
r = rows("sweep_costs.csv")
reg = defaultdict(list)                     # (model, bw, rtt) -> regrets
for x in r:
    reg[(x["model"], int(x["bw"]), int(x["rtt"]))].append(float(x["regret"]))
rtts = sorted({int(x["rtt"]) for x in r})
fig, axs = plt.subplots(1, 3, figsize=(10, 3.4), sharey=True)
for ax, bw in zip(axs, (0, 100, 10)):
    for model, col, name in (("static", ORANGE, "fixed costs"),
                             ("measured", BLUE, "measured costs")):
        ys = [sum(reg[(model, bw, t)]) / len(reg[(model, bw, t)]) for t in rtts]
        ax.plot(rtts, ys, color=col, marker="o", label=name)
        if model == "static" and max(ys) > 2:  # call out the worst setting only
            i = ys.index(max(ys))
            ax.annotate("%.1f\u00d7 slower" % ys[i], (rtts[i], ys[i]), xytext=(8, -2),
                        textcoords="offset points", fontsize=9, color=INK)
    ax.set_title("Bandwidth " + BW[bw], fontsize=10)
    ax.set_xlabel("RTT (ms)")
    ax.set_xticks(rtts)
    ax.set_xlim(-1, rtts[-1] + 5)
axs[0].set_ylabel("Mean plan regret (1.0 = best plan)")
axs[0].set_ylim(0.8, None)
axs[0].legend(loc="upper left")
fig.suptitle("Measured costs keep the planner on the fastest join plan", x=0.01, ha="left",
             fontsize=11, color=INK)
save(fig, "fig1_regret.png")

# ---- Figure 2: join time vs outer rows for the three methods ----
r = rows("sweep_joins.csv")
t = {}                                      # (rtt, bw, n) -> forced times
for x in r:
    t[(int(x["rtt"]), int(x["bw"]), int(x["n"]))] = (
        float(x["nl_ms"]), float(x["hash_ms"]), float(x["batch_ms"]))
ns = sorted({int(x["n"]) for x in r})
fig, axs = plt.subplots(1, 2, figsize=(9, 3.6), sharey=True)
for ax, rtt in zip(axs, (5, 20)):
    for i, (col, name) in enumerate(((ORANGE, "nested loop (best Postgres finds)"),
                                     (MUTED, "hash join over a full scan"),
                                     (BLUE, "batched join"))):
        ys = [t[(rtt, 0, n)][i] for n in ns]
        ax.plot(ns, ys, color=col, marker="o", label=name)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_title("RTT %d ms, bandwidth unlimited" % rtt, fontsize=10)
    ax.set_xlabel("Outer rows")
axs[0].set_ylabel("Join time (ms, log)")
axs[0].legend(loc="upper left")
fig.suptitle("The batched join is fastest from 10 outer rows up", x=0.01, ha="left",
             fontsize=11, color=INK)
fig.text(0.01, 0.01, "Nested loop: hash and merge joins off. Up to ~30 rows it is one get per outer row; "
         "past that Postgres picks other loop shapes.", fontsize=7.5, color=MUTED)
fig.tight_layout(rect=(0, 0.04, 1, 1))
fig.savefig(os.path.join(D, "fig2_join_time.png"), dpi=200)
plt.close(fig)
print("wrote", os.path.join(D, "fig2_join_time.png"))

# ---- Figure 3: break-even outer rows, true vs where each model switches ----
r = rows("sweep_costs.csv")
by = defaultdict(dict)                      # (rtt, bw) -> n -> row data
for x in r:
    k = (int(x["rtt"]), int(x["bw"]))
    n = int(x["n"])
    by[k].setdefault(n, {"nl": float(x["nl_ms"]), "hash": float(x["hash_ms"])})
    by[k][n][x["model"]] = x["chosen"]
CAP = 400                                   # "no switch within the grid"

def first(k, test):
    for n in sorted(by[k]):
        if test(by[k][n]):
            return n
    return CAP

fig, axs = plt.subplots(1, 3, figsize=(10, 3.4), sharey=True)
for ax, bw in zip(axs, (0, 100, 10)):
    for key, col, name, mk in (
            ("true", INK, "true break-even", "s"),
            ("static", ORANGE, "fixed costs switch", "o"),
            ("measured", BLUE, "measured costs switch", "o")):
        ys = []
        for rtt in rtts:
            k = (rtt, bw)
            if key == "true":
                ys.append(first(k, lambda d: d["hash"] <= d["nl"]))
            else:
                ys.append(first(k, lambda d, m=key: d.get(m) == "hash"))
        ax.plot(rtts, ys, color=col, marker=mk, label=name,
                linestyle="--" if key == "true" else "-")
    ax.axhline(CAP, color=GRID, linewidth=1)
    ax.set_yscale("log")
    ax.set_ylim(10, CAP * 1.7)
    ax.set_title("Bandwidth " + BW[bw], fontsize=10)
    ax.set_xlabel("RTT (ms)")
    ax.set_xticks(rtts)
axs[0].set_ylabel("Outer rows where a hash join\nbeats per-row gets (log)")
axs[-1].text(rtts[-1], CAP * 1.1, "top line: no switch up to 300 rows", fontsize=7.5,
             color=MUTED, ha="right")
h, l = axs[0].get_legend_handles_labels()
fig.legend(h, l, loc="lower center", ncol=3, fontsize=9)
fig.suptitle("Fixed costs switch at the same point everywhere; measured costs follow the network",
             x=0.01, ha="left", fontsize=11, color=INK)
fig.tight_layout(rect=(0, 0.08, 1, 1))
fig.savefig(os.path.join(D, "fig3_breakeven.png"), dpi=200)
plt.close(fig)
print("wrote", os.path.join(D, "fig3_breakeven.png"))

# ---- Figure 4: writes, 1000 rows ----
r = rows("writes.csv")
fig, ax = plt.subplots(figsize=(7, 3.2))
labels, vals, cols = [], [], []
for x in r:
    labels.append("%s, RTT %s ms\n%s" % (x["op"].upper(), x["rtt"], x["mode"]))
    vals.append(float(x["ms"]))
    cols.append(BLUE if x["mode"].startswith("batched") else ORANGE)
y = list(range(len(vals)))[::-1]
ax.barh(y, vals, color=cols, height=0.6)
for yi, v in zip(y, vals):
    ax.annotate("%.0f ms" % v, (v, yi), xytext=(4, 0), textcoords="offset points",
                va="center", fontsize=8, color=INK)
ax.set_yticks(y)
ax.set_yticklabels(labels, fontsize=8)
ax.set_xscale("log")
ax.set_xlabel("Time for 1000 rows (ms, log)")
ax.grid(axis="y", visible=False)
ax.set_xlim(None, max(vals) * 4)
fig.suptitle("Batched INSERT: 7.0 s to 0.12 s at 5 ms RTT", x=0.01, ha="left",
             fontsize=11, color=INK)
save(fig, "fig4_writes.png")
