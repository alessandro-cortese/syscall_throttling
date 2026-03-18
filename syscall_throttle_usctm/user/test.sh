#!/bin/bash
set -e

cd "$(dirname "$0")"

# cleanup: switch off the monitor and remove the UID, if present
sudo ./scthctl off || true
sudo ./scthctl deluid "$(id -u)" 2>/dev/null || true

# cleanup: remove and reinsert entries, idempotent
sudo ./scthctl delprog tester_getpid 2>/dev/null || true
sudo ./scthctl delsys 39 2>/dev/null || true

# log only the programme and system calls
sudo ./scthctl addprog tester_getpid
sudo ./scthctl addsys 39

# limit to 5/s and enable
sudo ./scthctl setmax 5
sudo ./scthctl on

echo "[*] Config:"
./scthctl listprog
./scthctl listuid
./scthctl listsys
./scthctl stats

echo "[*] Running tester_getpid..."
./tester_getpid