#!/usr/bin/env bash
# Start PostgreSQL, HBase and the Thrift gateway, create the demo database, check everything.
#   ./demo/start.sh
cd "$(dirname "$0")/.." || exit 1
./setup.sh start || exit 1
source work/env.sh
psql -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname = 'demo'" | grep -q 1 || createdb demo
psql -q -d demo -f demo/objects.sql || exit 1
./demo/check.sh
