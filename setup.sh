#!/usr/bin/env bash
# hbase_fdw: one-command setup on Ubuntu / WSL.
#
#   ./setup.sh          build PostgreSQL 18 and HBase, install hbase_fdw,
#                       load the test data and run the regression test
#   ./setup.sh start    start PostgreSQL, HBase and the Thrift gateway
#   ./setup.sh stop     stop them
#   ./setup.sh test     rebuild hbase_fdw and run the regression test
#
#   MOCK=1 ./setup.sh   same, with the in-memory mock HBase (no Java needed)
#
# Everything is built under ./work (git-ignored).
# Afterwards, in every new terminal:  source work/env.sh
set -euo pipefail
cd "$(dirname "$0")"
R=$PWD
W=$R/work
mkdir -p "$W"
export PGHOST=/tmp
export LD_LIBRARY_PATH="$W/pg/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"


PG_BRANCH=${PG_BRANCH:-REL_18_STABLE}
HBASE_VER=${HBASE_VER:-3.0.0}
HBASE_URL=${HBASE_URL:-https://archive.apache.org/dist/hbase/$HBASE_VER/hbase-$HBASE_VER-bin.tar.gz}
PGPORT=${PGPORT:-9432}
TPORT=${TPORT:-9090}
MOCK=${MOCK:-$(cat "$W/mode" 2>/dev/null || echo 0)}
JOBS=$(nproc 2>/dev/null || echo 2)
APT="sudo apt install -y build-essential bison flex libreadline-dev zlib1g-dev pkg-config curl python3-pip openjdk-17-jdk-headless"

say() { printf '\n== %s\n' "$*"; }
die() { printf '\nerror: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "'$1' not found. Install the prerequisites with:
  $APT"; }
pgbin() { echo "$W/pg/bin/$1"; }
jhome() { dirname "$(dirname "$(readlink -f "$(command -v java)")")"; }

wait_port() {   # port, seconds
	local i
	for ((i = 0; i < $2; i++)); do
		(exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
		sleep 1
	done
	return 1
}

envfile() {
	cat >"$W/env.sh" <<EOF
# source this in every new terminal:  source work/env.sh
export PATH="$W/pg/bin:$W/hbase/bin:\$PATH"
export LD_LIBRARY_PATH="$W/pg/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export PGDATA="$W/pgdata"
export PGPORT=$PGPORT
export PGHOST=/tmp
export HBASE_HOME="$W/hbase"
EOF
	if command -v java >/dev/null 2>&1; then
		echo "export JAVA_HOME=\"$(jhome)\"" >>"$W/env.sh"
	fi
}

build_pg() {
	[ -x "$(pgbin pg_config)" ] && return
	need git; need gcc; need make; need bison; need flex
	say "PostgreSQL ($PG_BRANCH): download"
	rm -rf "$W/postgresql"
	git clone -q --depth 1 -b "$PG_BRANCH" https://github.com/postgres/postgres.git "$W/postgresql"
	rm -rf "$W/postgresql/.git"     # plain source, no link to the upstream repo
	say "PostgreSQL: configure and build (takes a few minutes)"
	(cd "$W/postgresql" &&
		./configure -q --prefix="$W/pg" --enable-debug --without-icu CFLAGS="-O0 -g" &&
		make -s -j"$JOBS" && make -s install) || die "PostgreSQL build failed (see the output above)"
}

init_pg() {
	[ -f "$W/pgdata/PG_VERSION" ] && return
	say "PostgreSQL: initdb (port $PGPORT)"
	"$(pgbin initdb)" -D "$W/pgdata" -A trust -E UTF8 --locale=C.UTF-8 >/dev/null
	echo "port = $PGPORT" >>"$W/pgdata/postgresql.conf"
	echo "unix_socket_directories = '/tmp'" >>"$W/pgdata/postgresql.conf"
}

start_pg() {
	"$(pgbin pg_ctl)" -D "$W/pgdata" status >/dev/null 2>&1 && return
	say "PostgreSQL: start"
	"$(pgbin pg_ctl)" -D "$W/pgdata" -l "$W/pgdata/logfile" -w start >/dev/null
	"$(pgbin psql)" -p "$PGPORT" -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname = 'test'" | grep -q 1 ||
		"$(pgbin createdb)" -p "$PGPORT" test
}

py_deps() {
	need python3
	python3 -c "import happybase, thriftpy2" 2>/dev/null && return
	say "Python packages: happybase"
	python3 -m pip install -q --user happybase 2>/dev/null ||
		python3 -m pip install -q --user --break-system-packages happybase ||
		die "pip install happybase failed"
}

get_hbase() {
	[ -x "$W/hbase/bin/start-hbase.sh" ] && return
	need java; need curl
	say "HBase $HBASE_VER: download"
	curl -fL --progress-bar "$HBASE_URL" | tar -xz -C "$W" ||
		die "could not download $HBASE_URL (set HBASE_URL to another mirror)"
	ln -sfn "$W/hbase-$HBASE_VER" "$W/hbase"
	cat >"$W/hbase/conf/hbase-site.xml" <<EOF
<?xml version="1.0"?>
<configuration>
  <property><name>hbase.cluster.distributed</name><value>false</value></property>
  <property><name>hbase.rootdir</name><value>file://$W/hbase-data/hbase</value></property>
  <property><name>hbase.zookeeper.property.dataDir</name><value>$W/hbase-data/zookeeper</value></property>
  <property><name>hbase.unsafe.stream.capability.enforce</name><value>false</value></property>
</configuration>
EOF
	echo "export JAVA_HOME=\"$(jhome)\"" >>"$W/hbase/conf/hbase-env.sh"
}

build_fdw() {
	say "hbase_fdw: build and install"
	make -s -C "$R/hbase_fdw" USE_PGXS=1 PG_CONFIG="$(pgbin pg_config)"
	make -s -C "$R/hbase_fdw" USE_PGXS=1 PG_CONFIG="$(pgbin pg_config)" install
}

start_hb() {
	wait_port "$TPORT" 1 && return
	if [ "$MOCK" = 1 ]; then
		say "mock HBase on port $TPORT"
		nohup python3 "$R/hbase_fdw/test/mock_hbase.py" "$TPORT" >"$W/mock.log" 2>&1 &
		echo $! >"$W/mock.pid"
	else
		say "HBase and the Thrift gateway (port $TPORT)"
		"$W/hbase/bin/start-hbase.sh"
		"$W/hbase/bin/hbase-daemon.sh" start thrift -p "$TPORT"
	fi
	wait_port "$TPORT" 120 || die "nothing is listening on port $TPORT"
}

load_data() {
	say "load the test data"
	local i
	for i in $(seq 12); do   # HBase can take a minute before it accepts tables
		if [ "$i" -lt 12 ]; then
			(cd "$R/hbase_fdw" && python3 test/load_data.py localhost "$TPORT" 10000 2>/dev/null) && return
		else
			(cd "$R/hbase_fdw" && python3 test/load_data.py localhost "$TPORT" 10000) && return
		fi
		sleep 10
	done
	die "could not load the test data (is HBase up? see $W/hbase/logs)"
}

run_tests() {
	say "regression test"
	PGPORT=$PGPORT make -s -C "$R/hbase_fdw" USE_PGXS=1 PG_CONFIG="$(pgbin pg_config)" installcheck ||
		die "tests failed, see hbase_fdw/regression.diffs"
}

stop_all() {
	[ -x "$(pgbin pg_ctl)" ] && "$(pgbin pg_ctl)" -D "$W/pgdata" -m fast stop 2>/dev/null || true
	if [ -f "$W/mock.pid" ]; then
		kill "$(cat "$W/mock.pid")" 2>/dev/null || true
		rm -f "$W/mock.pid"
	fi
	if [ "$MOCK" != 1 ] && [ -x "$W/hbase/bin/hbase-daemon.sh" ]; then
		"$W/hbase/bin/hbase-daemon.sh" stop thrift || true
		"$W/hbase/bin/stop-hbase.sh" || true
	fi
}

case "${1:-all}" in
all)
	echo "$MOCK" >"$W/mode"
	build_pg; init_pg; envfile; start_pg; py_deps
	[ "$MOCK" = 1 ] || get_hbase
	envfile; build_fdw; start_hb; load_data; run_tests
	say "all set. In a new terminal:  source work/env.sh && psql test"
	;;
start)
	start_pg; start_hb
	if [ "$MOCK" = 1 ]; then load_data; fi   # the mock keeps no data across restarts
	;;
stop) stop_all ;;
test) build_fdw; run_tests ;;
*) die "usage: ./setup.sh [all|start|stop|test]" ;;
esac
