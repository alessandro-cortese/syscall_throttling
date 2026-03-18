#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "[*] Reset & config: only tester_getpid"
sudo ./user/scthctl off
sudo ./user/scthctl resetstats
sudo ./user/scthctl deluid 1000 2>/dev/null || true
sudo ./user/scthctl delprog tester_getpid 2>/dev/null || true
sudo ./user/scthctl delsys 39 2>/dev/null || true

sudo ./user/scthctl addprog tester_getpid
sudo ./user/scthctl addsys 39
sudo ./user/scthctl setmax 5
sudo ./user/scthctl on

./user/tester_getpid &
pid=$!
sleep 2
echo "[*] stats (expected: peak_prog=tester_getpid, delay>0)"
./user/scthctl stats
kill $pid; wait $pid 2>/dev/null || true

echo
echo "[*] Negative test: Python (comm != tester_getpid) it must not match"
sudo ./user/scthctl resetstats
python3 - <<'PY'
import os
for _ in range(2000000):
    os.getpid()
PY
echo "[*] stats (expected: peak_delay=0)"
./user/scthctl stats