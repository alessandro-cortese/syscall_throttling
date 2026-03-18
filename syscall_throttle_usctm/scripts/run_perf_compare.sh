#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

EVENTS="task-clock,context-switches,minor-faults,major-faults"
N1=1
N2=8
DUR=5

STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="results/$STAMP"
mkdir -p "$OUTDIR"

need() { command -v "$1" >/dev/null || { echo "Manca $1"; exit 1; }; }
need perf
need pidstat

./scripts/setup_test_env.sh >/dev/null
sudo ./user/scthctl resetstats

run_one() {
  local nprocs="$1"

  echo
  echo "=============================="
  echo "[*] USCTM  N=$nprocs"
  echo "=============================="

  sudo ./user/scthctl resetstats

  pids=()
  for i in $(seq 1 "$nprocs"); do
    ./user/tester_getpid >/dev/null &
    pids+=("$!")
  done

  sleep 1

  echo "[*] pidstat (CPU%) per ${DUR}s (pid=${pids[0]})"
  pidstat -p "${pids[0]}" 1 "$DUR" | tee "$OUTDIR/pidstat_N${nprocs}.txt" || true

  echo "[*] perf stat software events (${DUR}s) (pid=${pids[0]})"
  sudo perf stat -e "$EVENTS" -p "${pids[0]}" sleep "$DUR" 2>&1 | tee "$OUTDIR/perf_N${nprocs}.txt" || true

  echo "[*] stats (module):"
  ./user/scthctl stats | tee "$OUTDIR/scth_stats_N${nprocs}.txt"

  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}

run_one "$N1"
run_one "$N2"

echo
echo "[*] results saved in: $OUTDIR"