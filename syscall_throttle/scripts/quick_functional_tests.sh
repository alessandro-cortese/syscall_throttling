#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "[*] Setup..."
./scripts/setup_test_env.sh >/dev/null

echo
echo "[TEST 1] monitor OFF -> no peak update (after reset)"
sudo ./user/scthctl off
sudo ./user/scthctl resetstats
./user/tester_getpid &
pid=$!
sleep 2
./user/scthctl stats
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

echo
echo "[TEST 2] monitor ON -> throttling"
sudo ./user/scthctl on
sudo ./user/scthctl resetstats
./user/tester_getpid &
pid=$!
sleep 2
./user/scthctl stats
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

echo
echo "[TEST 3] two instances -> peak_blocked should increase"
sudo ./user/scthctl resetstats
./user/tester_getpid &
p1=$!
./user/tester_getpid &
p2=$!
sleep 2
./user/scthctl stats
kill "$p1" "$p2" 2>/dev/null || true
wait "$p1" "$p2" 2>/dev/null || true

echo "[*] Done."

# to enable perf: sudo sysctl kernel.perf_event_paranoid=1