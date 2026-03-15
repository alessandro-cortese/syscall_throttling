# syscall_throttling

# nella cartella HOOK (sys_call_table)
MAX_PER_SEC=5 DUR=10 N_LIST="1 8 32" ./scripts/bench_usctm_hook.sh

# nella cartella KPROBE
MAX_PER_SEC=5 DUR=10 N_LIST="1 8 32" MODE_LIST="0 1 2" ./scripts/bench_kprobe.sh