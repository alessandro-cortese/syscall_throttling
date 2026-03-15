#!/usr/bin/env python3
import re
import csv
import sys
import glob
import math
import argparse
from pathlib import Path

def parse_number_locale(s: str) -> float:
    """
    Parses numbers in formats like:
      - "3.654.971" (thousands sep '.') -> 3654971
      - "10,001928314" (decimal ',') -> 10.001928314
      - "0,000 CPUs utilized" -> 0.0
    """
    s = s.strip()
    # keep only digits, '.', ',' and '-'
    s = re.sub(r"[^0-9\.,\-]", "", s)
    if s == "" or s == "-" or s == "," or s == ".":
        return float("nan")

    # If both '.' and ',' exist: assume '.' thousands and ',' decimal (it-IT style).
    if "." in s and "," in s:
        s = s.replace(".", "").replace(",", ".")
        return float(s)

    # If only ',' exists: assume decimal comma
    if "," in s:
        s = s.replace(",", ".")
        return float(s)

    # If only '.' exists: could be thousands separators OR decimal.
    # Heuristic: if multiple dots => thousands separators
    if s.count(".") >= 2:
        s = s.replace(".", "")
        return float(s)

    return float(s)

def read_text(path: Path) -> str:
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""

def parse_perf_stat(text: str):
    """
    Extracts task-clock, context-switches, cpu-migrations from perf stat output.
    Returns dict with keys: task_clock_ms, ctx_switches, cpu_migrations
    """
    out = {}
    # Example lines:
    #   3.654.971      task-clock
    #          80      context-switches
    #           8      cpu-migrations
    for key, name in [
        ("task_clock", "task-clock"),
        ("ctx_switches", "context-switches"),
        ("cpu_migrations", "cpu-migrations"),
    ]:
        m = re.search(rf"^\s*([0-9\.,]+)\s+{re.escape(name)}\b", text, re.MULTILINE)
        if m:
            out[key] = parse_number_locale(m.group(1))
        else:
            out[key] = float("nan")

    # perf's task-clock is shown as milliseconds (ms) in the default output
    # In practice it's already "ms" (e.g., 376.598 means 376.598 ms)
    # but sometimes you see huge numbers with thousands separators => still ms.
    out["task_clock_ms"] = out["task_clock"]
    return out

def parse_scth_stats(text: str):
    """
    Parses module stats file containing lines like:
    monitor_on=1  max_current_per_sec=5  max_next_per_sec=5
    peak_delay_ns=1000286381  peak_prog=tester_getpid  peak_uid=1000
    peak_blocked_threads=1  avg_blocked_threads=0.968
    """
    out = {}
    m = re.search(r"monitor_on=(\d+)\s+max_current_per_sec=(\d+)\s+max_next_per_sec=(\d+)", text)
    if m:
        out["monitor_on"] = int(m.group(1))
        out["max_current_per_sec"] = int(m.group(2))
        out["max_next_per_sec"] = int(m.group(3))
    else:
        out["monitor_on"] = None
        out["max_current_per_sec"] = None
        out["max_next_per_sec"] = None

    m = re.search(r"peak_delay_ns=([0-9]+)\s+peak_prog=([^\s]*)\s+peak_uid=([0-9]+)", text)
    if m:
        out["peak_delay_ns"] = int(m.group(1))
        out["peak_prog"] = m.group(2)
        out["peak_uid"] = int(m.group(3))
    else:
        out["peak_delay_ns"] = None
        out["peak_prog"] = ""
        out["peak_uid"] = None

    m = re.search(r"peak_blocked_threads=([0-9]+)\s+avg_blocked_threads=([0-9\.,]+)", text)
    if m:
        out["peak_blocked_threads"] = int(m.group(1))
        out["avg_blocked_threads"] = parse_number_locale(m.group(2))
    else:
        out["peak_blocked_threads"] = None
        out["avg_blocked_threads"] = float("nan")

    return out

def parse_latency(text: str):
    """
    Parses tester_latency output line:
    calls=57671868 avg_ns=136.9 max_ns=354986
    """
    out = {}
    m = re.search(r"calls=([0-9]+)\s+avg_ns=([0-9\.,]+)\s+max_ns=([0-9]+)", text)
    if m:
        out["lat_calls"] = int(m.group(1))
        out["lat_avg_ns"] = parse_number_locale(m.group(2))
        out["lat_max_ns"] = int(m.group(3))
    else:
        out["lat_calls"] = None
        out["lat_avg_ns"] = float("nan")
        out["lat_max_ns"] = None
    return out

def parse_tag_usctm(tag: str):
    # tag like: N8_max5_dur10
    m = re.search(r"N(\d+)_max(\d+)_dur(\d+)", tag)
    if not m:
        return None
    return {"N": int(m.group(1)), "MAX_PER_SEC": int(m.group(2)), "DUR": int(m.group(3)), "mode": None}

def parse_tag_kprobe(tag: str):
    # tag like: mode2_N8_max5_dur10
    m = re.search(r"mode(\d+)_N(\d+)_max(\d+)_dur(\d+)", tag)
    if not m:
        return None
    return {"mode": int(m.group(1)), "N": int(m.group(2)), "MAX_PER_SEC": int(m.group(3)), "DUR": int(m.group(4))}

def collect_runs(results_root: Path, impl: str):
    rows = []
    if impl == "usctm_hook":
        runs = sorted(results_root.glob("usctm_hook_*"))
        for run_dir in runs:
            for sub in sorted(run_dir.iterdir()):
                if not sub.is_dir():
                    continue
                meta = parse_tag_usctm(sub.name)
                if not meta:
                    continue
                rows.append(parse_one_case(sub, impl, meta))
    elif impl == "kprobe":
        runs = sorted(results_root.glob("kprobe_*"))
        for run_dir in runs:
            for sub in sorted(run_dir.iterdir()):
                if not sub.is_dir():
                    continue
                meta = parse_tag_kprobe(sub.name)
                if not meta:
                    continue
                rows.append(parse_one_case(sub, impl, meta))
    else:
        raise ValueError("Unknown impl")
    return rows

def parse_one_case(case_dir: Path, impl: str, meta: dict):
    perf_proc = parse_perf_stat(read_text(case_dir / "perf_proc.txt"))
    perf_sys  = parse_perf_stat(read_text(case_dir / "perf_sys.txt"))
    stats     = parse_scth_stats(read_text(case_dir / "scth_stats.txt"))
    lat       = parse_latency(read_text(case_dir / "latency.txt"))

    row = {
        "impl": impl,
        "mode": meta.get("mode"),
        "N": meta["N"],
        "max_per_sec": meta["MAX_PER_SEC"],
        "dur_s": meta["DUR"],
        "case_dir": str(case_dir),

        # perf per-process
        "task_clock_ms_proc": perf_proc.get("task_clock_ms"),
        "ctx_switches_proc": perf_proc.get("ctx_switches"),
        "cpu_migrations_proc": perf_proc.get("cpu_migrations"),

        # perf system-wide (optional)
        "task_clock_ms_sys": perf_sys.get("task_clock_ms"),
        "ctx_switches_sys": perf_sys.get("ctx_switches"),
        "cpu_migrations_sys": perf_sys.get("cpu_migrations"),

        # module stats
        "peak_delay_ns": stats.get("peak_delay_ns"),
        "peak_blocked_threads": stats.get("peak_blocked_threads"),
        "avg_blocked_threads": stats.get("avg_blocked_threads"),

        # latency
        "lat_calls": lat.get("lat_calls"),
        "lat_avg_ns": lat.get("lat_avg_ns"),
        "lat_max_ns": lat.get("lat_max_ns"),
    }
    return row

def write_csv(path: Path, rows):
    if not rows:
        print(f"[!] No rows to write for {path}")
        return
    cols = list(rows[0].keys())
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

def group_mean(rows, keys):
    # group by keys, compute mean for numeric fields
    groups = {}
    for r in rows:
        k = tuple(r.get(x) for x in keys)
        groups.setdefault(k, []).append(r)

    def is_num(v):
        return isinstance(v, (int, float)) and not (isinstance(v, float) and math.isnan(v))

    out = []
    for k, lst in groups.items():
        base = {keys[i]: k[i] for i in range(len(keys))}
        base["runs"] = len(lst)

        # numeric columns: mean
        for col in lst[0].keys():
            if col in keys or col in ("impl", "case_dir"):
                continue
            vals = [x[col] for x in lst if is_num(x[col])]
            if vals:
                base[col + "_mean"] = sum(vals) / len(vals)
            else:
                base[col + "_mean"] = ""
        out.append(base)
    return out

def plot_all(csv_path: Path, outdir: Path):
    """
    Makes basic plots via matplotlib. No seaborn.
    """
    import pandas as pd
    import matplotlib.pyplot as plt

    df = pd.read_csv(csv_path)

    outdir.mkdir(parents=True, exist_ok=True)

    # Helper: filter numeric safely
    def to_num(series):
        return pd.to_numeric(series, errors="coerce")

    df["N"] = to_num(df["N"])
    df["task_clock_ms_proc"] = to_num(df["task_clock_ms_proc"])
    df["ctx_switches_proc"] = to_num(df["ctx_switches_proc"])
    df["peak_delay_ns"] = to_num(df["peak_delay_ns"])
    df["lat_avg_ns"] = to_num(df["lat_avg_ns"])

    # 1) task-clock vs N
    plt.figure()
    for impl in sorted(df["impl"].dropna().unique()):
        sub = df[df["impl"] == impl]
        if impl == "kprobe":
            # split by mode
            for mode in sorted(sub["mode"].dropna().unique()):
                s2 = sub[sub["mode"] == mode].sort_values("N")
                plt.plot(s2["N"], s2["task_clock_ms_proc"], marker="o", label=f"{impl}-mode{int(mode)}")
        else:
            s2 = sub.sort_values("N")
            plt.plot(s2["N"], s2["task_clock_ms_proc"], marker="o", label=impl)
    plt.xlabel("N processes")
    plt.ylabel("perf task-clock (ms) [per-process group]")
    plt.title("CPU time consumed vs N")
    plt.legend()
    plt.grid(True)
    plt.savefig(outdir / "task_clock_vs_N.png", dpi=200, bbox_inches="tight")
    plt.close()

    # 2) context switches vs N
    plt.figure()
    for impl in sorted(df["impl"].dropna().unique()):
        sub = df[df["impl"] == impl]
        if impl == "kprobe":
            for mode in sorted(sub["mode"].dropna().unique()):
                s2 = sub[sub["mode"] == mode].sort_values("N")
                plt.plot(s2["N"], s2["ctx_switches_proc"], marker="o", label=f"{impl}-mode{int(mode)}")
        else:
            s2 = sub.sort_values("N")
            plt.plot(s2["N"], s2["ctx_switches_proc"], marker="o", label=impl)
    plt.xlabel("N processes")
    plt.ylabel("context-switches [per-process group]")
    plt.title("Context switches vs N")
    plt.legend()
    plt.grid(True)
    plt.savefig(outdir / "ctx_switches_vs_N.png", dpi=200, bbox_inches="tight")
    plt.close()

    # 3) peak_delay vs N (ns -> s)
    plt.figure()
    for impl in sorted(df["impl"].dropna().unique()):
        sub = df[df["impl"] == impl].copy()
        sub["peak_delay_s"] = sub["peak_delay_ns"] / 1e9
        if impl == "kprobe":
            for mode in sorted(sub["mode"].dropna().unique()):
                s2 = sub[sub["mode"] == mode].sort_values("N")
                plt.plot(s2["N"], s2["peak_delay_s"], marker="o", label=f"{impl}-mode{int(mode)}")
        else:
            s2 = sub.sort_values("N")
            plt.plot(s2["N"], s2["peak_delay_s"], marker="o", label=impl)
    plt.xlabel("N processes")
    plt.ylabel("peak_delay (s)")
    plt.title("Peak delay vs N")
    plt.legend()
    plt.grid(True)
    plt.savefig(outdir / "peak_delay_vs_N.png", dpi=200, bbox_inches="tight")
    plt.close()

    # 4) latency avg vs N
    plt.figure()
    for impl in sorted(df["impl"].dropna().unique()):
        sub = df[df["impl"] == impl]
        if impl == "kprobe":
            for mode in sorted(sub["mode"].dropna().unique()):
                s2 = sub[sub["mode"] == mode].sort_values("N")
                plt.plot(s2["N"], s2["lat_avg_ns"], marker="o", label=f"{impl}-mode{int(mode)}")
        else:
            s2 = sub.sort_values("N")
            plt.plot(s2["N"], s2["lat_avg_ns"], marker="o", label=impl)
    plt.xlabel("N processes")
    plt.ylabel("avg syscall latency (ns) [tester_latency]")
    plt.title("Avg syscall latency vs N")
    plt.legend()
    plt.grid(True)
    plt.savefig(outdir / "lat_avg_vs_N.png", dpi=200, bbox_inches="tight")
    plt.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--usctm-results", required=True, help="Path to results/ directory of syscall_throttle_usctm (hook)")
    ap.add_argument("--kprobe-results", required=True, help="Path to results/ directory of syscall_throttle (kprobe)")
    ap.add_argument("--outdir", default="bench_summary", help="Output directory")
    ap.add_argument("--make-plots", action="store_true", help="Generate PNG plots (requires pandas + matplotlib)")
    args = ap.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    usctm_root = Path(args.usctm_results)
    kprobe_root = Path(args.kprobe_results)

    rows = []
    rows += collect_runs(usctm_root, "usctm_hook")
    rows += collect_runs(kprobe_root, "kprobe")

    if not rows:
        print("[!] No benchmark cases found. Check paths and folder names.")
        sys.exit(1)

    wide_csv = outdir / "bench_wide.csv"
    write_csv(wide_csv, rows)
    print(f"[*] Wrote: {wide_csv}")

    agg = group_mean(rows, keys=["impl", "mode", "N", "max_per_sec", "dur_s"])
    agg_csv = outdir / "bench_agg_mean.csv"
    write_csv(agg_csv, agg)
    print(f"[*] Wrote: {agg_csv}")

    if args.make_plots:
        try:
            plot_all(wide_csv, outdir / "plots")
            print(f"[*] Plots in: {outdir / 'plots'}")
        except Exception as e:
            print(f"[!] Plotting failed: {e}")
            print("    (Install pandas + matplotlib or rerun without --make-plots)")

if __name__ == "__main__":
    main()