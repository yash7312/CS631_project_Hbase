#!/usr/bin/env python3
"""
Create and fill the test tables in HBase (real or mock) via happybase, and
write the same rows to CSV so they can be loaded into a plain Postgres table
for result comparison.

    python3 load_data.py [host] [port] [nrows]

Tables:
  emp   families info, pay   row key = 'e000001' ...
        info:name, info:dept, info:joined (date), pay:salary (int),
        pay:bonus (numeric, missing on some rows -> NULL)
  odd   family d             edge cases: unicode, empty string, binary,
        row with only a family we don't map
  ikey  family d             integer row keys '1' .. '12' (canonical text),
        d:sq = key squared
  bkey  family d             binary row keys around 0xff (prefix carry),
        d:n = position in byte order
  ukey  family d             text row keys: multibyte, literal % and _,
        d:n = position in byte order
"""
import csv, os, random, sys
import happybase

host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 9090
n = int(sys.argv[3]) if len(sys.argv) > 3 else 10000

c = happybase.Connection(host, port, autoconnect=True)
for t in (b"emp", b"odd", b"ikey", b"bkey", b"ukey"):
    if t in c.tables():
        c.disable_table(t)
        c.delete_table(t)

c.create_table("emp", {"info": dict(), "pay": dict()})
c.create_table("odd", {"d": dict(), "x": dict()})
c.create_table("ikey", {"d": dict()})
c.create_table("bkey", {"d": dict()})
c.create_table("ukey", {"d": dict()})

random.seed(631)
depts = ["eng", "ops", "hr", "sales", "quant"]
names = ["asha", "ravi", "meera", "kabir", "zoya", "dev", "isha", "arjun"]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "emp.csv")

emp = c.table("emp")
with emp.batch(batch_size=1000) as bt, open(out, "w", newline="") as f:
    w = csv.writer(f)
    for i in range(1, n + 1):
        k = "e%06d" % i
        nm = "%s_%d" % (random.choice(names), i)
        d = random.choice(depts)
        j = "20%02d-%02d-%02d" % (random.randint(10, 25), random.randint(1, 12), random.randint(1, 28))
        s = random.randint(30, 300) * 1000
        bo = "%.2f" % (random.random() * 5000) if i % 7 else None   # every 7th: no bonus
        row = {b"info:name": nm.encode(), b"info:dept": d.encode(),
               b"info:joined": j.encode(), b"pay:salary": str(s).encode()}
        if bo is not None:
            row[b"pay:bonus"] = bo.encode()
        bt.put(k.encode(), row)
        w.writerow([k, nm, d, j, s, "" if bo is None else bo])

odd = c.table("odd")
odd.put(b"r1", {b"d:txt": "नमस्ते दुनिया".encode(), b"d:num": b"42"})
odd.put(b"r2", {b"d:txt": b"", b"d:num": b"-7"})
odd.put(b"r3", {b"d:bin": bytes([0, 1, 2, 255]), b"d:num": b"0"})
odd.put(b"r4", {b"x:other": b"only unmapped family"})
odd.put(b"r5", {b"d:num": b"not-a-number"})

ikey = c.table("ikey")
for i in range(1, 13):
    ikey.put(str(i).encode(), {b"d:sq": str(i * i).encode()})

bkeys = [b"a", b"a\x00", b"a\xfe\xff", b"a\xff", b"a\xff\x00", b"a\xff\xff",
         b"b", b"\xff", b"\xff\x01", b"\xff\xff"]
ukeys = ["e%", "e%x", "e_x", "ex", "exy", "e\u00e9", "e\u00e9a", "\u00e91",
         "\u00e92", "\u00ea"]
for t, keys in (("bkey", bkeys), ("ukey", [k.encode() for k in ukeys])):
    tb = c.table(t)
    for i, k in enumerate(sorted(keys)):
        tb.put(k, {b"d:n": str(i + 1).encode()})

print("loaded %d rows into emp, 5 into odd, 12 into ikey, %d into bkey, "
      "%d into ukey; csv -> %s" % (n, len(bkeys), len(ukeys), out))
