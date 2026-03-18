#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
USER="$ROOT_DIR/user"
CTL="$USER/scthctl"
TESTER="$USER/tester_getpid"

echo "[*] Root dir: $ROOT_DIR"

if [[ ! -x "$CTL" ]]; then
  echo "[!] scthctl not found: $CTL"
  exit 1
fi
if [[ ! -x "$TESTER" ]]; then
  echo "[!] tester_getpid not found: $TESTER"
  exit 1
fi

echo
echo "=============================="
echo "[0] Cleanup & baseline"
echo "=============================="
sudo "$CTL" off || true
sudo "$CTL" resetstats || true
sudo "$CTL" delprog tester_getpid 2>/dev/null || true
sudo "$CTL" deluid "$(id -u)" 2>/dev/null || true
sudo "$CTL" delsys 39 2>/dev/null || true

echo "[*] Stats after reset:"
"$CTL" stats

echo
echo "=============================="
echo "[1] Install hook + config (program-only)"
echo "=============================="
sudo "$CTL" addprog tester_getpid
sudo "$CTL" addsys 39
sudo "$CTL" setmax 5
sudo "$CTL" on

echo "[*] Lists:"
"$CTL" listprog
"$CTL" listuid
"$CTL" listsys
echo "[*] Stats:"
"$CTL" stats

echo
echo "=============================="
echo "[2] Positive test: tester_getpid should be throttled"
echo "=============================="
"$TESTER" >/dev/null &
PID=$!
sleep 2
echo "[*] Stats while tester is running:"
"$CTL" stats

echo "[*] CPU check (pidstat 3 samples):"
pidstat -p "$PID" 1 3 || true

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

echo
echo "=============================="
echo "[3] Negative test: other program should NOT be throttled"
echo "    (python loop calling getpid)"
echo "=============================="
sudo "$CTL" resetstats
python3 - <<'PY' >/dev/null &
import os, time
t = time.time()
while time.time() - t < 2.5:
    os.getpid()
PY
PY_PID=$!
sleep 3
kill "$PY_PID" 2>/dev/null || true
wait "$PY_PID" 2>/dev/null || true

echo "[*] Stats after python run (expected: peak_delay_ns=0):"
"$CTL" stats

echo
echo "=============================="
echo "[4] UID-only test (SAFE): throttle a dedicated UID"
echo "=============================="

sudo "$CTL" off || true
sudo "$CTL" resetstats || true
sudo "$CTL" delprog tester_getpid 2>/dev/null || true
sudo "$CTL" delsys 39 2>/dev/null || true
sudo "$CTL" deluid "$(id -u)" 2>/dev/null || true

if ! id -u throttletest >/dev/null 2>&1; then
  sudo useradd -r -M -s /usr/sbin/nologin throttletest
fi
TUID="$(id -u throttletest)"

sudo "$CTL" adduid "$TUID"
sudo "$CTL" addsys 39
sudo "$CTL" setmax 5
sudo "$CTL" on

TMP_TESTER="/tmp/tester_getpid"
sudo cp -f "$TESTER" "$TMP_TESTER"
sudo chmod 0755 "$TMP_TESTER"

sudo -u throttletest "$TMP_TESTER" >/dev/null &
PID=$!
sleep 2

echo "[*] Stats (expected: peak_uid=$TUID, delay>0):"
"$CTL" stats

sudo kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

sudo "$CTL" off

echo
echo "=============================="
echo "[5] Cleanup"
echo "=============================="
sudo "$CTL" off
echo "[*] Done."