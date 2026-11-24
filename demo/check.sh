#!/usr/bin/env bash
# Check that HBase, the Thrift gateway, PostgreSQL and the wrapper all answer.
cd "$(dirname "$0")/.." || exit 1
source work/env.sh
ok()   { printf '  OK    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; bad=1; }
bad=0

echo "== HBase"
if [ "$(cat work/mode 2>/dev/null)" = 1 ]; then
	ok "mock HBase in use (no Java process)"
else
	jps | grep -q HMaster && ok "HMaster is running" || fail "HMaster is not running (jps shows no HMaster)"
	jps | grep -q ThriftServer && ok "Thrift server process is running" || fail "no ThriftServer process"
fi
(exec 3<>/dev/tcp/127.0.0.1/9090) 2>/dev/null && ok "port 9090 (Thrift) accepts connections" || fail "nothing listens on port 9090"

echo "== PostgreSQL"
pg_isready -h /tmp -p "$PGPORT" >/dev/null && ok "server is accepting connections" || fail "pg_isready says no"
psql -d demo -tAc "SELECT 'hbase_fdw ' || extversion FROM pg_extension WHERE extname = 'hbase_fdw'" 2>/dev/null | grep -q hbase_fdw \
	&& ok "extension hbase_fdw is installed in database demo" || fail "extension missing in database demo"

echo "== PostgreSQL to HBase"
# HBase needs up to a minute after start before it answers queries, so retry
for i in $(seq 30); do
	n=$(psql -d demo -tAc "SELECT count(*) FROM emp" 2>&1)
	case "$n" in '' | *[!0-9]*) [ "$i" = 1 ] && echo "  ...waiting for HBase to finish starting"; sleep 3 ;; *) break ;; esac
done
case "$n" in
'' | *[!0-9]*) fail "SELECT count(*) FROM emp failed: $n" ;;
*) ok "SELECT count(*) FROM emp returned $n rows read from HBase" ;;
esac

[ "$bad" = 0 ] && echo "all good" || { echo "something failed, see above"; exit 1; }
