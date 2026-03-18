#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

SAFE_UID="${1:-997}"   # Pass a UID of your choice: ./test_uid_filter.sh 997

echo "[*] Reset & config: only dedicated UID $SAFE_UID"
sudo ./user/scthctl off || true
sudo ./user/scthctl resetstats || true
sudo ./user/scthctl delprog tester_getpid 2>/dev/null || true
sudo ./user/scthctl deluid "$SAFE_UID" 2>/dev/null || true
sudo ./user/scthctl delsys 39 2>/dev/null || true

sudo ./user/scthctl adduid "$SAFE_UID"
sudo ./user/scthctl addsys 39
sudo ./user/scthctl setmax 2000
sudo ./user/scthctl on

# Run `tester_getpid` as the SAFE_UID user
# This requires a user with that UID to exist (e.g. u997)
if id -u "u${SAFE_UID}" >/dev/null 2>&1; then
  sudo runuser -u "u${SAFE_UID}" -- ./user/tester_getpid >/dev/null &
else
  echo "[!] user u${SAFE_UID} not founde. Create it, es:"
  echo "    sudo useradd -u ${SAFE_UID} -M -r u${SAFE_UID}"
  exit 1
fi

pid=$!
sleep 2

echo "[*] stats (expected: peak_uid=$SAFE_UID, delay can be >0)"
./user/scthctl stats

kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true

sudo ./user/scthctl off || true
sudo ./user/scthctl deluid "$SAFE_UID" || true