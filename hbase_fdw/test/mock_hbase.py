#!/usr/bin/env python3
"""
Tiny in-memory stand-in for the HBase Thrift1 gateway (`hbase thrift start`).

It serves the real Hbase.thrift IDL over buffered transport + binary protocol,
which is what the HBase thrift server uses by default, so hbase_fdw talks to
it exactly as it would to real HBase. Only the calls we need are implemented.

    python3 mock_hbase.py [port]        # default 9090
"""
import bisect, os, sys, time, threading
import thriftpy2
from thriftpy2.rpc import make_server

here = os.path.dirname(os.path.abspath(__file__))
hb = thriftpy2.load(os.path.join(here, "Hbase.thrift"), module_name="Hbase_thrift")


def b(x):
    return x if isinstance(x, bytes) else x.encode()


class Table:
    def __init__(self, fams):
        self.fams = set(fams)
        self.keys = []          # sorted row keys (bytes order, like HBase)
        self.rows = {}          # key -> {b"cf:q": TCell}
        self.on = True


class Handler:
    def __init__(self):
        self.t = {}
        self.sc = {}
        self.nsc = 0
        self.mu = threading.Lock()

    def _tab(self, name):
        name = b(name)
        if name not in self.t:
            raise hb.IOError(message="TableNotFoundException: " + name.decode())
        return self.t[name]

    # ---- admin ----
    def getTableNames(self):
        return sorted(self.t)

    def createTable(self, name, cols):
        name = b(name)
        if name in self.t:
            raise hb.AlreadyExists(message=name.decode())
        self.t[name] = Table(b(c.name).rstrip(b":") for c in cols)

    def deleteTable(self, name):
        self._tab(name)
        del self.t[b(name)]

    def enableTable(self, name):
        self._tab(name).on = True

    def disableTable(self, name):
        self._tab(name).on = False

    def isTableEnabled(self, name):
        return self._tab(name).on

    def getColumnDescriptors(self, name):
        return {f + b":": hb.ColumnDescriptor(name=f + b":")
                for f in self._tab(name).fams}

    # ---- writes ----
    def mutateRow(self, name, row, muts, attrs):
        self.mutateRowTs(name, row, muts, int(time.time() * 1000), attrs)

    def mutateRowTs(self, name, row, muts, ts, attrs):
        t, row = self._tab(name), b(row)
        with self.mu:
            r = t.rows.get(row)
            if r is None:
                r = t.rows[row] = {}
                bisect.insort(t.keys, row)
            for m in muts:
                col = b(m.column)
                fam = col.split(b":", 1)[0]
                if fam not in t.fams:
                    raise hb.IOError(message="NoSuchColumnFamilyException: " + fam.decode())
                if m.isDelete:
                    r.pop(col, None)
                else:
                    r[col] = hb.TCell(value=b(m.value), timestamp=ts)
            if not r:
                del t.rows[row]
                t.keys.remove(row)

    def mutateRows(self, name, batches, attrs):
        for bt in batches:
            self.mutateRow(name, bt.row, bt.mutations, attrs)

    # ---- reads ----
    def _pick(self, r, cols):
        if not cols:
            return dict(r)
        out = {}
        for c in cols:
            c = b(c)
            if b":" not in c or c.endswith(b":"):     # whole family
                f = c.rstrip(b":") + b":"
                out.update({k: v for k, v in r.items() if k.startswith(f)})
            elif c in r:
                out[c] = r[c]
        return out

    def _result(self, key, cells, sort):
        if sort:
            return hb.TRowResult(row=key, sortedColumns=[
                hb.TColumn(columnName=k, cell=v) for k, v in sorted(cells.items())])
        return hb.TRowResult(row=key, columns=cells)

    def getRowWithColumns(self, name, row, cols, attrs):
        t = self._tab(name)
        r = t.rows.get(b(row))
        if r is None:
            return []
        return [self._result(b(row), self._pick(r, cols), False)]

    def getRow(self, name, row, attrs):
        return self.getRowWithColumns(name, row, [], attrs)

    def getRowsWithColumns(self, name, rows, cols, attrs):
        # like HBase: in the order asked, rows with no matching cells left out
        t, out = self._tab(name), []
        for row in rows:
            r = t.rows.get(b(row))
            cells = self._pick(r, cols) if r is not None else None
            if cells:
                out.append(self._result(b(row), cells, False))
        self.calls = getattr(self, "calls", 0) + 1
        return out

    def getRows(self, name, rows, attrs):
        return self.getRowsWithColumns(name, rows, [], attrs)

    def _open(self, name, start, stop, cols, keyonly=False, sort=False):
        t = self._tab(name)
        with self.mu:
            self.nsc += 1
            self.sc[self.nsc] = dict(t=t, start=b(start or b""), stop=b(stop or b""),
                                     cols=cols or [], keyonly=keyonly, sort=sort,
                                     last=None)
            return self.nsc

    def scannerOpen(self, name, start, cols, attrs):
        return self._open(name, start, None, cols)

    def scannerOpenWithStop(self, name, start, stop, cols, attrs):
        return self._open(name, start, stop, cols)

    def scannerOpenWithScan(self, name, scan, attrs):
        f = (scan.filterString or b"")
        f = f.decode() if isinstance(f, bytes) else f
        ko = "FirstKeyOnlyFilter" in f
        if f and not ko:
            raise hb.IOError(message="mock only understands FirstKeyOnlyFilter")
        return self._open(name, scan.startRow, scan.stopRow, scan.columns,
                          keyonly=ko, sort=bool(scan.sortColumns))

    def scannerGetList(self, sid, n):
        s = self.sc.get(sid)
        if s is None:
            raise hb.IllegalArgument(message="scanner id %d not found" % sid)
        t, out = s["t"], []
        with self.mu:
            if s["last"] is None:
                i = bisect.bisect_left(t.keys, s["start"])
            else:
                i = bisect.bisect_right(t.keys, s["last"])
            while i < len(t.keys) and len(out) < n:
                k = t.keys[i]
                i += 1
                if s["stop"] and k >= s["stop"]:
                    break
                cells = self._pick(t.rows[k], s["cols"])
                if not cells:          # HBase skips rows with no matching cells
                    continue
                if s["keyonly"]:
                    c = min(cells)
                    cells = {c: hb.TCell(value=b"", timestamp=cells[c].timestamp)}
                s["last"] = k
                out.append(self._result(k, cells, s["sort"]))
            if not out:
                s["last"] = t.keys[-1] if t.keys else b""
        self.calls = getattr(self, "calls", 0) + 1
        return out

    def scannerGet(self, sid):
        return self.scannerGetList(sid, 1)

    def scannerClose(self, sid):
        if self.sc.pop(sid, None) is None:
            raise hb.IllegalArgument(message="scanner id %d not found" % sid)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9090
    srv = make_server(hb.Hbase, Handler(), "127.0.0.1", port, client_timeout=None)
    print("mock HBase thrift on 127.0.0.1:%d" % port, flush=True)
    srv.serve()
