#!/usr/bin/env bash
# Stop the Thrift gateway, HBase and PostgreSQL.
cd "$(dirname "$0")/.." || exit 1
./setup.sh stop
