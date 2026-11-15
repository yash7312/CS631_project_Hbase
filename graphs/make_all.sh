#!/usr/bin/env bash
# Draw every figure. Run from the repo root:  bash graphs/make_all.sh
# Needs python3 with matplotlib (pip install matplotlib).
set -e
cd "$(dirname "$0")/.."
python3 graphs/fig1_regret.py
python3 graphs/fig2_join_time.py
python3 graphs/fig3_breakeven.py
python3 graphs/fig4_writes.py
