#!/bin/bash
set -euo pipefail
export LC_ALL=C
export LANG=C

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CTL="$ROOT_DIR/user/scthctl"
TEST_GETPID="$ROOT_DIR/user/tester_getpid"
TEST_OPENAT="$ROOT_DIR/user/tester_openat"

MODE_LIST="${MODE_LIST:-0 1 2}"
SYSCALL_GETPID="${SYSCALL_GETPID:-39}"
SYSCALL_OPENAT="${SYSCALL_OPENAT:-257}"
MAX1="${MAX1:-5}"
MAX2="${MAX2:-2}"

need() { command -v "$1" >/dev/null 2>&1 || { echo "[!] Missing dependency: $1"; exit 1; }; }
need sudo
[[ -x "$CTL" ]] || { echo "[!] Missing $CTL"; exit 1; }
[[ -x "$TEST_GETPID" ]] || { echo "[!] Missing $TEST_GETPID"; exit 1; }

cleanup_all() {
  sudo "$CTL" off >/dev/null 2>&1 || true
  sudo "$CTL" resetstats >/dev/null 2>&1 || true
  sudo "$CTL" delprog tester_getpid >/dev/null 2>&1 || true
  sudo "$CTL" delprog tester_openat >/dev/null 2>&1 || true
  sudo "$CTL" deluid "$(id -u)" >/dev/null 2>&1 || true
  sudo "$CTL" delsys "$SYSCALL_GETPID" >/dev/null 2>&1 || true
  sudo "$CTL" delsys "$SYSCALL_OPENAT" >/dev/null 2>&1 || true
}

echo "[*] KPROBE corner cases, MODE_LIST=($MODE_LIST)"
echo

for mode in $MODE_LIST; do
  echo "########################################"
  echo "[MODE=$mode]"
  echo "########################################"

  echo "== [1] Duplicates idempotent =="
  cleanup_all
  sudo "$CTL" setmode "$mode" >/dev/null 2>&1 || true
  sudo "$CTL" addprog tester_getpid
  sudo "$CTL" addprog tester_getpid
  sudo "$CTL" adduid "$(id -u)"
  sudo "$CTL" adduid "$(id -u)"
  sudo "$CTL" addsys "$SYSCALL_GETPID"
  sudo "$CTL" addsys "$SYSCALL_GETPID"
  "$CTL" listprog
  "$CTL" listuid
  "$CTL" listsys
  echo

  echo "== [2] setmax 0 clamp-to-1 =="
  sudo "$CTL" resetstats
  sudo "$CTL" setmax 0
  sudo "$CTL" on
  sleep 1
  "$CTL" stats
  sleep 2
  "$CTL" stats
  sudo "$CTL" off
  echo

  echo "== [3] Two syscalls (shared budget) =="
  cleanup_all
  sudo "$CTL" setmode "$mode" >/dev/null 2>&1 || true
  sudo "$CTL" addprog tester_getpid
  sudo "$CTL" addsys "$SYSCALL_GETPID"
  if [[ -x "$TEST_OPENAT" ]]; then
    sudo "$CTL" addprog tester_openat || true
    sudo "$CTL" addsys "$SYSCALL_OPENAT"
  fi
  sudo "$CTL" setmax "$MAX1"
  sudo "$CTL" on
  "$TEST_GETPID" >/dev/null & PID1=$!
  if [[ -x "$TEST_OPENAT" ]]; then "$TEST_OPENAT" >/dev/null & PID2=$!; else PID2=""; fi
  sleep 2
  "$CTL" stats
  kill "$PID1" 2>/dev/null || true; wait "$PID1" 2>/dev/null || true
  if [[ -n "$PID2" ]]; then kill "$PID2" 2>/dev/null || true; wait "$PID2" 2>/dev/null || true; fi
  sudo "$CTL" off
  echo

  echo "== [4] Zero syscalls -> no throttling =="
  cleanup_all
  sudo "$CTL" setmode "$mode" >/dev/null 2>&1 || true
  sudo "$CTL" addprog tester_getpid
  sudo "$CTL" setmax "$MAX1"
  sudo "$CTL" on
  "$TEST_GETPID" >/dev/null & PID=$!
  sleep 2
  "$CTL" stats
  kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true
  sudo "$CTL" off
  echo

  echo "== [5] MON_OFF while blocked =="
  cleanup_all
  sudo "$CTL" setmode "$mode" >/dev/null 2>&1 || true
  sudo "$CTL" addprog tester_getpid
  sudo "$CTL" addsys "$SYSCALL_GETPID"
  sudo "$CTL" setmax 1
  sudo "$CTL" on
  pids=()
  for _ in $(seq 1 8); do "$TEST_GETPID" >/dev/null & pids+=("$!"); done
  sleep 1
  sudo "$CTL" off
  sleep 1
  ps -eo stat,comm | awk '$2=="tester_getpid"{print $1}' | grep -q '^D' && {
    echo "[!] ERROR: found tester_getpid in D state"; exit 1;
  } || echo "[*] OK: no D-state tester_getpid"
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
  echo

  echo "== [6] Deferred MAX =="
  cleanup_all
  sudo "$CTL" setmode "$mode" >/dev/null 2>&1 || true
  sudo "$CTL" addprog tester_getpid
  sudo "$CTL" addsys "$SYSCALL_GETPID"
  sudo "$CTL" setmax "$MAX1"
  sudo "$CTL" on
  "$TEST_GETPID" >/dev/null & PID=$!
  sleep 1
  "$CTL" stats
  sudo "$CTL" setmax "$MAX2"
  "$CTL" stats
  sleep 2
  "$CTL" stats
  kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true
  sudo "$CTL" off
  echo
done

echo "[*] DONE corner cases (kprobe)"