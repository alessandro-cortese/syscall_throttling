#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

CTL="./user/scthctl"
TESTER="./user/tester_getpid"

sudo "$CTL" off || true
sudo "$CTL" resetstats || true
sudo "$CTL" delprog tester_getpid 2>/dev/null || true
sudo "$CTL" delsys 39 2>/dev/null || true

sudo "$CTL" addprog tester_getpid
sudo "$CTL" addsys 39
sudo "$CTL" setmax 5
sudo "$CTL" on

"$TESTER" >/dev/null &
pid=$!

echo "[*] stats @t0"
"$CTL" stats

sleep 2

echo "[*] stats @t+2s"
"$CTL" stats

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
sudo "$CTL" off || true