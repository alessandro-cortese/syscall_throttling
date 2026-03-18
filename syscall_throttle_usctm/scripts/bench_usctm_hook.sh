#!/bin/bash
set -euo pipefail

export LC_ALL=C
export LANG=C

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
USER_DIR="$ROOT_DIR/user"
CTL="$USER_DIR/scthctl"
TESTER="$USER_DIR/tester_getpid"
LAT="$USER_DIR/tester_latency"

MAX_PER_SEC="${MAX_PER_SEC:-5}"
DUR="${DUR:-10}"
N_LIST="${N_LIST:-1 8}"
SYSCALL_NR="${SYSCALL_NR:-39}"
PROG_NAME="${PROG_NAME:-tester_getpid}"
OUTBASE="${OUTBASE:-$ROOT_DIR/results}"

need() { command -v "$1" >/dev/null 2>&1 || { echo "[!] Missing dependency: $1"; exit 1; }; }
need perf
need pidstat
need sudo

[[ -x "$CTL" ]] || { echo "[!] Missing $CTL"; exit 1; }
[[ -x "$TESTER" ]] || { echo "[!] Missing $TESTER"; exit 1; }

STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="$OUTBASE/usctm_hook_$STAMP"
mkdir -p "$OUTDIR"

echo "[*] Benchmark USCTM sys_call_table hook (sleep)"
echo "    ROOT_DIR=$ROOT_DIR"
echo "    MAX_PER_SEC=$MAX_PER_SEC  DUR=$DUR  N_LIST=($N_LIST)"
echo "    OUTDIR=$OUTDIR"
echo

setup_common() {
  sudo "$CTL" off >/dev/null 2>&1 || true
  sudo "$CTL" resetstats >/dev/null 2>&1 || true
  sudo "$CTL" delprog "$PROG_NAME" >/dev/null 2>&1 || true
  sudo "$CTL" delsys "$SYSCALL_NR" >/dev/null 2>&1 || true

  sudo "$CTL" addprog "$PROG_NAME"
  sudo "$CTL" addsys "$SYSCALL_NR"
  sudo "$CTL" setmax "$MAX_PER_SEC"
  sudo "$CTL" off >/dev/null 2>&1 || true
  sudo "$CTL" wakewaiters >/dev/null 2>&1 || true
  pkill -9 tester_getpid 2>/dev/null || true
}

start_load() {
  local n="$1"
  local pids=()
  for _ in $(seq 1 "$n"); do
    "$TESTER" >/dev/null &
    pids+=("$!")
  done

  printf "%s\n" "${pids[@]}"
}

kill_pids() {
  local pids=("$@")
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}

pidlist_csv() {
  local pids=("$@")
  (IFS=,; echo "${pids[*]}")
}

run_one() {
  local n="$1"

  local tag="N${n}_max${MAX_PER_SEC}_dur${DUR}"
  local out="$OUTDIR/$tag"
  mkdir -p "$out"

  echo "=============================="
  echo "[*] USCTM_HOOK N=$n"
  echo "=============================="

  setup_common
  sudo "$CTL" on

  mapfile -t pids < <(start_load "$n")
  local csv
  csv="$(pidlist_csv "${pids[@]}")"

  sleep 1

  echo "[*] pidstat -p $csv 1 $DUR"
  pidstat -p "$csv" 1 "$DUR" | tee "$out/pidstat.txt" || true

  echo "[*] perf stat (per-process group) -p $csv  sleep $DUR"
  sudo perf stat -e task-clock,context-switches,cpu-migrations -p "$csv" sleep "$DUR" 2>&1 | tee "$out/perf_proc.txt" || true

  echo "[*] perf stat (system-wide) -a sleep $DUR"
  sudo perf stat -a -e task-clock,context-switches,cpu-migrations sleep "$DUR" 2>&1 | tee "$out/perf_sys.txt" || true

  echo "[*] Module stats:"
  "$CTL" stats | tee "$out/scth_stats.txt"

  if [[ -x "$LAT" ]]; then
    echo "[*] tester_latency ($DUR s) on ONE process (no extra load) ..."
    kill_pids "${pids[@]}"
    "$LAT" "$DUR" | tee "$out/latency.txt" || true
    mapfile -t pids < <(start_load "$n")
    sleep 1
  fi

  kill_pids "${pids[@]}"
  sudo "$CTL" off >/dev/null 2>&1 || true

  echo
}

for n in $N_LIST; do
  run_one "$n"
done

echo "[*] Done. Results in: $OUTDIR"