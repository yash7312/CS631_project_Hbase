# ctl.py -- talk to netem_proxy.py's control port
#
#   python3 ctl.py rtt 5      set the RTT to 5 ms
#   python3 ctl.py bw 100     cap at 100 Mbit/s (0 = unlimited)
#   python3 ctl.py stats      bytes and chunks each way, connections
#   python3 ctl.py reset      zero the counters
import socket, sys

s = socket.create_connection(("127.0.0.1", 9191))
f = s.makefile("rw")
f.write(" ".join(sys.argv[1:]) + "\n")
f.flush()
print(f.readline().strip())
