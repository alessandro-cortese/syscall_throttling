#!/bin/bash
set -e

cd "$(dirname "$0")"

# pulizia: spegni monitor e togli UID (se esiste)
sudo ./scthctl off || true
sudo ./scthctl deluid "$(id -u)" 2>/dev/null || true

# pulizia: togli e rimetti le entry (idempotente)
sudo ./scthctl delprog tester_getpid 2>/dev/null || true
sudo ./scthctl delsys 39 2>/dev/null || true

# registra SOLO programma e syscall
sudo ./scthctl addprog tester_getpid
sudo ./scthctl addsys 39

# limita a 5/s e accendi
sudo ./scthctl setmax 5
sudo ./scthctl on

echo "[*] Config:"
./scthctl listprog
./scthctl listuid
./scthctl listsys
./scthctl stats

echo "[*] Running tester_getpid..."
./tester_getpid