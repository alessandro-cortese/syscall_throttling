#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "[*] Setup "
./scripts/setup_test_env.sh >/dev/null
sudo ./user/scthctl resetstats

echo "[*] Start tester"
./user/tester_getpid >/dev/null &
pid=$!
sleep 1

echo "[*] stats (before):"
./user/scthctl stats

echo "[*] MAX has been set multiple times within the same epoch (only the last value will apply from the next epoch onwards)"
sudo ./user/scthctl setmax 10
sudo ./user/scthctl setmax 20
sudo ./user/scthctl setmax 5000

echo "[*] stats immediately afterwards (max_next=5000; max_current still old until rollover):"
./user/scthctl stats

echo "[*] I'll wait 2 seconds to get past at least one rollover..."
sleep 2

echo "[*] stats after rollover (max_current should become 5000):"
./user/scthctl stats

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true