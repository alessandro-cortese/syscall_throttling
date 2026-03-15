#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "[*] Setup base"
./scripts/setup_test_env.sh >/dev/null
sudo ./user/scthctl resetstats

echo "[*] Start tester"
./user/tester_getpid >/dev/null &
pid=$!
sleep 1

echo "[*] stats (prima):"
./user/scthctl stats

echo "[*] Set MAX più volte nella stessa epoca (vale solo l'ultimo dalla prossima epoca)"
sudo ./user/scthctl setmax 10
sudo ./user/scthctl setmax 20
sudo ./user/scthctl setmax 5000

echo "[*] stats subito dopo (max_next=5000; max_current ancora vecchio fino a rollover):"
./user/scthctl stats

echo "[*] Attendo 2 secondi per superare almeno un rollover..."
sleep 2

echo "[*] stats dopo rollover (max_current dovrebbe diventare 5000):"
./user/scthctl stats

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true