#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "[*] Setup base"
./scripts/setup_test_env.sh >/dev/null
sudo ./user/scthctl setmode 2   # MODE 2
sudo ./user/scthctl resetstats

echo "[*] Start tester"
./user/tester_getpid >/dev/null &
pid=$!
sleep 1

echo "[*] stats (before):"
./user/scthctl stats

echo "[*] MAX has been set multiple times within the same epoch (only the last setting should apply, but from the next epoch onwards)"
sudo ./user/scthctl setmax 10
sudo ./user/scthctl setmax 20
sudo ./user/scthctl setmax 5000

echo "[*] Stats immediately after setting the maximum number of multiples (max_next must be 5000; max_current remains the old value until the rollover takes effect):"
./user/scthctl stats

echo "[*] I'll wait 2 seconds to get past at least one rollover..."
sleep 2

echo "[*] stats after rollover (max_current should become 5000):"
./user/scthctl stats

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true