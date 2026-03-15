#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

SAFE_UID="${1:-997}"   # passa un uid a scelta: ./test_uid_filter.sh 997

echo "[*] Reset & config: SOLO UID dedicato $SAFE_UID"
sudo ./user/scthctl off || true
sudo ./user/scthctl resetstats || true
sudo ./user/scthctl delprog tester_getpid 2>/dev/null || true
sudo ./user/scthctl deluid "$SAFE_UID" 2>/dev/null || true
sudo ./user/scthctl delsys 39 2>/dev/null || true

sudo ./user/scthctl adduid "$SAFE_UID"
sudo ./user/scthctl addsys 39
sudo ./user/scthctl setmax 2000
sudo ./user/scthctl on

# esegui tester_getpid come utente SAFE_UID
# richiede che esista un utente con quel uid (es. u997)
if id -u "u${SAFE_UID}" >/dev/null 2>&1; then
  sudo runuser -u "u${SAFE_UID}" -- ./user/tester_getpid >/dev/null &
else
  echo "[!] utente u${SAFE_UID} non trovato. Crealo, es:"
  echo "    sudo useradd -u ${SAFE_UID} -M -r u${SAFE_UID}"
  exit 1
fi

pid=$!
sleep 2

echo "[*] stats (atteso: peak_uid=$SAFE_UID, delay può essere >0)"
./user/scthctl stats

kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true

sudo ./user/scthctl off || true
sudo ./user/scthctl deluid "$SAFE_UID" || true