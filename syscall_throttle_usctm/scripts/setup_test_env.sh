#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

# Config base: solo programma tester_getpid, syscall 39
sudo ./user/scthctl off || true
sudo ./user/scthctl deluid 1000 2>/dev/null || true
sudo ./user/scthctl delprog tester_getpid 2>/dev/null || true
sudo ./user/scthctl delsys 39 2>/dev/null || true

sudo ./user/scthctl addprog tester_getpid
sudo ./user/scthctl addsys 39
sudo ./user/scthctl setmax 5
sudo ./user/scthctl on

echo "[*] Config:"
./user/scthctl listprog
./user/scthctl listuid
./user/scthctl listsys
./user/scthctl stats
