#!/bin/bash
set -euo pipefail

export LC_ALL=C
export LANG=C

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
USER_DIR="$ROOT_DIR/user"
CTL="$USER_DIR/scthctl"
TESTER="$USER_DIR/tester_getpid"
LAT="$USER_DIR/tester_latency"

# Params (override via env or args)
MAX_PER_SEC="${MAX_PER_SEC:-5}"
DUR="${DUR:-10}"          # seconds
N_LIST="${N_LIST:-1 8}"   # e.g. "1 8 32"
MODE_LIST="${MODE_LIST:-0 1 2}"
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
OUTDIR="$OUTBASE/kprobe_$STAMP"
mkdir -p "$OUTDIR"

echo "[*] Benchmark KPROBE"
echo "    ROOT_DIR=$ROOT_DIR"
echo "    MAX_PER_SEC=$MAX_PER_SEC  DUR=$DUR  N_LIST=($N_LIST)  MODE_LIST=($MODE_LIST)"
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
}

start_load() {
  local n="$1"
  local pids=()
  for _ in $(seq 1 "$n"); do
    "$TESTER" >/dev/null &
    pids+=("$!")
  done

  # IMPORTANT: uno per riga, così "mapfile -t" funziona davvero
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
  local mode="$1"
  local n="$2"

  local tag="mode${mode}_N${n}_max${MAX_PER_SEC}_dur${DUR}"
  local out="$OUTDIR/$tag"
  mkdir -p "$out"

  echo "=============================="
  echo "[*] KPROBE mode=$mode  N=$n"
  echo "=============================="

  setup_common

  # set mode (kprobe version)
  sudo "$CTL" setmode "$mode"

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
    # fermiamo il carico e misuriamo "sotto throttling" singolo processo:
    kill_pids "${pids[@]}"
    # riavviamo 1 processo latency per $DUR
    "$LAT" "$DUR" | tee "$out/latency.txt" || true
    # riavvio carico per uniformità (non strettamente necessario)
    mapfile -t pids < <(start_load "$n")
    sleep 1
  fi

  kill_pids "${pids[@]}"
  sudo "$CTL" off >/dev/null 2>&1 || true

  echo
}

for mode in $MODE_LIST; do
  for n in $N_LIST; do
    run_one "$mode" "$n"
  done
done

echo "[*] Done. Results in: $OUTDIR"