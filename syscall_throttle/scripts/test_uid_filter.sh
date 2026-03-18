#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "[*] Reset & config: only UID $(id -u)"
sudo ./user/scthctl off
sudo ./user/scthctl resetstats
sudo ./user/scthctl delprog tester_getpid 2>/dev/null || true
sudo ./user/scthctl deluid "$(id -u)" 2>/dev/null || true
sudo ./user/scthctl delsys 39 2>/dev/null || true

sudo ./user/scthctl adduid "$(id -u)"
sudo ./user/scthctl addsys 39
sudo ./user/scthctl setmax 2000
sudo ./user/scthctl on

./user/tester_getpid &
pid=$!
sleep 2
echo "[*] stats (expected: delay can be >0, peak_uid=$(id -u))"
./user/scthctl stats
kill $pid; wait $pid 2>/dev/null || true

sudo ./user/scthctl deluid "$(id -u)"