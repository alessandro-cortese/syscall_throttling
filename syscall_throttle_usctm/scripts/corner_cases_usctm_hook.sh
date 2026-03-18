#!/bin/bash
set -euo pipefail
export LC_ALL=C
export LANG=C

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CTL="$ROOT_DIR/user/scthctl"
TEST_GETPID="$ROOT_DIR/user/tester_getpid"
TEST_OPENAT="$ROOT_DIR/user/tester_openat"

SYSCALL_GETPID="${SYSCALL_GETPID:-39}"
SYSCALL_OPENAT="${SYSCALL_OPENAT:-257}"   # x86_64: openat
MAX1="${MAX1:-5}"
MAX2="${MAX2:-2}"
ME_UID="$(id -u)"

need() { command -v "$1" >/dev/null 2>&1 || { echo "[!] Missing dependency: $1"; exit 1; }; }
need sudo

[[ -x "$CTL" ]] || { echo "[!] Missing $CTL"; exit 1; }
[[ -x "$TEST_GETPID" ]] || { echo "[!] Missing $TEST_GETPID"; exit 1; }

HAVE_OPENAT=0
if [[ -x "$TEST_OPENAT" ]]; then
  HAVE_OPENAT=1
else
  echo "[!] tester_openat not found; 2-syscall test will be partial."
fi

echo "[*] USCTM HOOK corner cases (SAFE)"
echo "ROOT=$ROOT_DIR"
echo

cleanup_all() {
  sudo "$CTL" off >/dev/null 2>&1 || true
  sudo "$CTL" resetstats >/dev/null 2>&1 || true

  sudo "$CTL" delprog tester_getpid >/dev/null 2>&1 || true
  sudo "$CTL" delprog tester_openat >/dev/null 2>&1 || true

  sudo "$CTL" deluid "$ME_UID" >/dev/null 2>&1 || true

  sudo "$CTL" delsys "$SYSCALL_GETPID" >/dev/null 2>&1 || true
  sudo "$CTL" delsys "$SYSCALL_OPENAT" >/dev/null 2>&1 || true
}

config_prog_getpid_only() {
  sudo "$CTL" addprog tester_getpid
  sudo "$CTL" addsys "$SYSCALL_GETPID"
}

config_prog_two_syscalls() {
  sudo "$CTL" addprog tester_getpid
  if (( HAVE_OPENAT )); then sudo "$CTL" addprog tester_openat; fi

  sudo "$CTL" addsys "$SYSCALL_GETPID"
  if (( HAVE_OPENAT )); then sudo "$CTL" addsys "$SYSCALL_OPENAT"; fi
}

check_no_D_state() {
  ps -eo stat,comm | awk '$2=="tester_getpid"{print $1}' | grep -q '^D' && {
    echo "[!] ERROR: found tester_getpid in D state"
    exit 1
  } || echo "[*] OK: no D-state tester_getpid"
}

echo "=============================="
echo "[1] Duplicates are idempotent"
echo "=============================="
cleanup_all
sudo "$CTL" addprog tester_getpid
sudo "$CTL" addprog tester_getpid      # dup
sudo "$CTL" adduid "$ME_UID"
sudo "$CTL" adduid "$ME_UID"           # dup
sudo "$CTL" addsys "$SYSCALL_GETPID"
sudo "$CTL" addsys "$SYSCALL_GETPID"   # dup
echo "[*] Lists (each item should appear once):"
"$CTL" listprog
"$CTL" listuid
"$CTL" listsys

echo
echo "=============================="
echo "[2] setmax 0 semantics (clamp to 1)"
echo "    SAFE: throttle only tester_getpid + syscall 39"
echo "=============================="
cleanup_all
config_prog_getpid_only
sudo "$CTL" setmax 0
sudo "$CTL" on
"$TEST_GETPID" >/dev/null & PID=$!
sleep 1
echo "[*] stats (expect max_next=1 and max_current=1):"
"$CTL" stats
sleep 2
"$CTL" stats
sudo "$CTL" off
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

echo
echo "=============================="
echo "[3] Two syscalls registered (shared budget)"
echo "=============================="
cleanup_all
config_prog_two_syscalls
sudo "$CTL" setmax "$MAX1"
sudo "$CTL" on

"$TEST_GETPID" >/dev/null & PID1=$!
PID2=""
if (( HAVE_OPENAT )); then
  "$TEST_OPENAT" >/dev/null & PID2=$!
fi

sleep 2
echo "[*] stats (expect peak_delay>0, blocked>=1 possible):"
"$CTL" stats

kill "$PID1" 2>/dev/null || true
wait "$PID1" 2>/dev/null || true
if [[ -n "$PID2" ]]; then
  kill "$PID2" 2>/dev/null || true
  wait "$PID2" 2>/dev/null || true
fi
sudo "$CTL" off

echo
echo "=============================="
echo "[4] Zero syscalls -> no throttling"
echo "=============================="
cleanup_all
sudo "$CTL" addprog tester_getpid
sudo "$CTL" setmax "$MAX1"
sudo "$CTL" on
"$TEST_GETPID" >/dev/null & PID=$!
sleep 2
echo "[*] stats (expect peak_delay_ns=0, blocked=0):"
"$CTL" stats
sudo "$CTL" off
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

echo
echo "=============================="
echo "[5] MON_OFF while threads are blocked -> must exit (no D state)"
echo "=============================="
cleanup_all
config_prog_getpid_only
sudo "$CTL" setmax 1
sudo "$CTL" on

pids=()
for _ in $(seq 1 8); do
  "$TEST_GETPID" >/dev/null & pids+=("$!")
done
sleep 1

echo "[*] Turning monitor off..."
sudo "$CTL" off
sleep 1

echo "[*] Checking no tester_getpid stuck in D state:"
check_no_D_state

for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done

echo
echo "=============================="
echo "[6] Changing MAX mid-run is deferred to epoch boundary"
echo "=============================="
cleanup_all
config_prog_getpid_only
sudo "$CTL" setmax "$MAX1"
sudo "$CTL" on

"$TEST_GETPID" >/dev/null & PID=$!
sleep 1

echo "[*] stats before change:"
"$CTL" stats

echo "[*] setmax $MAX2 (should take effect next epoch)"
sudo "$CTL" setmax "$MAX2"

echo "[*] stats immediately after setmax:"
"$CTL" stats

sleep 2
echo "[*] stats after rollover:"
"$CTL" stats

sudo "$CTL" off
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

echo
echo "[*] DONE corner cases (usctm_hook)"