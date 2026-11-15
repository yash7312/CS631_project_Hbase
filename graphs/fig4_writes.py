# Figure 4: time to write 1000 rows, one HBase call per row against
# batched INSERT, at RTT 0 and 5 ms.
# Data: results/writes.csv          Output: graphs/fig4_writes.png
import csv
import matplotlib.pyplot as plt

labels = []
times = []
colors = []
with open("results/writes.csv") as f:
    for r in csv.DictReader(f):
        labels.append(r["op"].upper() + ", RTT " + r["rtt"] + " ms\n" + r["mode"])
        times.append(float(r["ms"]))
        if r["mode"].startswith("batched"):
            colors.append("#2a78d6")
        else:
            colors.append("#eb6834")

# first row at the top
labels.reverse()
times.reverse()
colors.reverse()

fig, ax = plt.subplots(figsize=(7, 3.4))
ax.barh(labels, times, color=colors)
for i in range(len(times)):
    ax.text(times[i] * 1.1, i, str(round(times[i])) + " ms", va="center", fontsize=8)
ax.set_xscale("log")
ax.set_xlim(20, 40000)
ax.set_xlabel("Time for 1000 rows (ms, log)")
ax.tick_params(axis="y", labelsize=8)
fig.suptitle("Batched INSERT: 7.0 s to 0.12 s at 5 ms RTT")
fig.tight_layout()
fig.savefig("graphs/fig4_writes.png", dpi=200)
print("wrote graphs/fig4_writes.png")
