#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

EVENTS="task-clock,context-switches,minor-faults,major-faults"
# cycles/instructions spesso sono <not supported> in VM; li lasciamo opzionali:
EVENTS_HW="cycles,instructions"

N1=1
N2=8
DUR=5

STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="results/$STAMP"
mkdir -p "$OUTDIR"

need() { command -v "$1" >/dev/null || { echo "Manca $1"; exit 1; }; }
need perf
need pidstat

# Setup comune
./scripts/setup_test_env.sh >/dev/null

run_one() {
  local mode="$1"
  local nprocs="$2"

  echo
  echo "=============================="
  echo "[*] MODE=$mode  N=$nprocs"
  echo "=============================="

  sudo ./user/scthctl setmode "$mode"
  sudo ./user/scthctl resetstats

  # avvio N processi
  pids=()
  for i in $(seq 1 "$nprocs"); do
    ./user/tester_getpid >/dev/null &
    pids+=("$!")
  done

  sleep 1

  echo "[*] pidstat (CPU%) per ${DUR}s (primo pid=${pids[0]})"
  # pidstat su un solo pid per semplicità; se vuoi su tutti, si può estendere
  pidstat -p "${pids[0]}" 1 "$DUR" | tee "$OUTDIR/pidstat_mode${mode}_N${nprocs}.txt" || true

  echo "[*] perf stat software events (${DUR}s) (primo pid=${pids[0]})"
  sudo perf stat -e "$EVENTS" -p "${pids[0]}" sleep "$DUR" 2>&1 | tee "$OUTDIR/perf_mode${mode}_N${nprocs}.txt" || true

  echo "[*] perf stat HW (cycles,instructions) (se supportato)"
  sudo perf stat -e "$EVENTS_HW" -p "${pids[0]}" sleep "$DUR" 2>&1 | tee "$OUTDIR/perf_hw_mode${mode}_N${nprocs}.txt" || true

  echo "[*] stats (modulo):"
  ./user/scthctl stats | tee "$OUTDIR/scth_stats_mode${mode}_N${nprocs}.txt"

  # cleanup
  for pid in "${pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
}

for mode in 0 1 2; do
  run_one "$mode" "$N1"
  run_one "$mode" "$N2"
done

echo
echo "[*] Salvati risultati in: $OUTDIR"