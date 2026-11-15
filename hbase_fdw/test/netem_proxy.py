# netem_proxy.py -- adds network delay between Postgres and the HBase Thrift gateway
#
#   python3 netem_proxy.py [rtt_ms] [bw_mbit]     Postgres -> 9190 -> 9090 Thrift
#
# Each chunk is held for rtt/2 in each direction; bw (0 = unlimited) adds the
# time to push the bytes through the link. Change settings live with ctl.py.
import socket, threading, time, random, sys, queue

LISTEN, TARGET_HOST, TARGET_PORT, CTL = 9190, "127.0.0.1", 9090, 9191
set_ = {"rtt": 0.0, "bw": 0.0, "jit": 0.0}            # ms, Mbit/s (0 = unlimited), ms
st = {"up": 0, "down": 0, "upn": 0, "downn": 0, "conns": 0}
mu = threading.Lock()
SPIN = 0.002                                           # seconds of busy-wait before each release

def reader(src, q, name):
    last = 0.0
    while True:
        try:
            d = src.recv(65536)
        except OSError:
            d = b""
        now = time.time()
        dl = set_["rtt"] / 2000.0                      # half the RTT, in seconds
        if set_["jit"] > 0:
            dl += random.uniform(-set_["jit"], set_["jit"]) / 2000.0
        rel = max(now + dl, last)                      # never earlier than the previous chunk
        last = rel
        q.put((rel, d))
        if not d:
            return
        with mu:
            st[name] += len(d)
            st[name + "n"] += 1

def writer(dst, q):
    while True:
        rel, d = q.get()                               # blocks until a chunk is queued
        w = rel - time.time()
        if w > SPIN:
            time.sleep(w - SPIN)                       # a laptop CPU wakes late from a long sleep,
        while time.time() < rel:                       # so stop early and spin the rest,
            time.sleep(0)                              # letting the other threads run
        if not d:
            try:
                dst.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            return
        if set_["bw"] > 0:
            time.sleep(len(d) * 8 / (set_["bw"] * 1e6))
        try:
            dst.sendall(d)
        except OSError:
            return

def handle(cl):
    time.sleep(set_["rtt"] / 1000.0)                 # the TCP handshake loopback gives for free
    up = socket.create_connection((TARGET_HOST, TARGET_PORT))
    up.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    with mu:
        st["conns"] += 1
    ws = []
    for a, b, name in ((cl, up, "up"), (up, cl, "down")):
        q = queue.Queue()
        threading.Thread(target=reader, args=(a, q, name), daemon=True).start()
        w = threading.Thread(target=writer, args=(b, q), daemon=True)
        w.start()
        ws.append(w)
    for w in ws:
        w.join()
    cl.close()
    up.close()

def control():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", CTL)); s.listen(5)
    while True:
        c, _ = s.accept()
        f = c.makefile("rw")
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] in set_ and len(p) == 2:
                set_[p[0]] = float(p[1]); f.write("ok\n")
            elif p[0] == "stats":
                with mu:
                    f.write(" ".join(k + "=" + str(v) for k, v in st.items()) + "\n")
            elif p[0] == "reset":
                with mu:
                    for k in st: st[k] = 0
                f.write("ok\n")
            else:
                f.write("error\n")
            f.flush()
        c.close()

if __name__ == "__main__":
    if len(sys.argv) > 1: set_["rtt"] = float(sys.argv[1])
    if len(sys.argv) > 2: set_["bw"] = float(sys.argv[2])
    threading.Thread(target=control, daemon=True).start()
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", LISTEN)); s.listen(64)
    while True:
        c, _ = s.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=handle, args=(c,), daemon=True).start()
