#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "[*] Setup base"
./scripts/setup_test_env.sh >/dev/null
sudo ./user/scthctl setmode 2   # prova con MODE 2; ripeti con 0/1 se vuoi
sudo ./user/scthctl resetstats

echo "[*] Start tester"
./user/tester_getpid >/dev/null &
pid=$!
sleep 1

echo "[*] stats (prima):"
./user/scthctl stats

echo "[*] Set MAX più volte nella stessa epoca (deve valere solo l'ultimo, ma dalla prossima epoca)"
sudo ./user/scthctl setmax 10
sudo ./user/scthctl setmax 20
sudo ./user/scthctl setmax 5000

echo "[*] stats subito dopo setmax multipli (max_next deve essere 5000; max_current ancora vecchio finché non scatta rollover):"
./user/scthctl stats

echo "[*] Attendo 2 secondi per superare almeno un rollover..."
sleep 2

echo "[*] stats dopo rollover (max_current dovrebbe diventare 5000):"
./user/scthctl stats

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true