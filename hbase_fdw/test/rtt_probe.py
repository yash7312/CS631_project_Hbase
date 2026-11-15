# rtt_probe.py -- time 50 isTableEnabled calls on one Thrift connection
#
#   python3 rtt_probe.py [port] [table]     default 9190 (the proxy), emp
#
# The Thrift message is built by hand (strict binary protocol), so this
# needs no Thrift library.
import socket, struct, time, sys

def call(s, seq, tab):
    nm = b"isTableEnabled"
    m  = struct.pack(">I", 0x80010001)                     # version 1, message type CALL
    m += struct.pack(">i", len(nm)) + nm + struct.pack(">i", seq)
    m += struct.pack(">bh", 11, 1)                          # field 1, type string: tableName
    m += struct.pack(">i", len(tab)) + tab
    m += b"\x00"                                            # end of arguments
    t = time.perf_counter()
    s.sendall(m)
    s.recv(4096)                                            # the reply is a few dozen bytes
    return (time.perf_counter() - t) * 1000

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9190
tab = (sys.argv[2] if len(sys.argv) > 2 else "emp").encode()
s = socket.create_connection(("127.0.0.1", port))
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
ts = sorted(call(s, i, tab) for i in range(50))
print("min %.2f  median %.2f  p95 %.2f ms" % (ts[0], ts[25], ts[47]))
