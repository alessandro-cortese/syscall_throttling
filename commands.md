# syscall_throttling

# nella cartella HOOK (sys_call_table)
MAX_PER_SEC=5 DUR=10 N_LIST="1 8" ./scripts/bench_usctm_hook.sh

# nella cartella KPROBE
MAX_PER_SEC=5 DUR=10 N_LIST="1 8" MODE_LIST="0 1 2" ./scripts/bench_kprobe.sh

# Per estrarre tutti i risultati e generare i grafici 

cd /home/ale/Documenti/GitHub/syscall_throttling/tools

Per attivare l'ambiente python per l'estrazione: source .venv/bin/activate
Per disattivare l'ambiente python: deactivate

python3 extract_bench_results.py \
  --usctm-results /home/ale/Documenti/GitHub/syscall_throttling/syscall_throttle_usctm/results \
  --kprobe-results /home/ale/Documenti/GitHub/syscall_throttling/syscall_throttle/results \
  --outdir /home/ale/Documenti/GitHub/syscall_throttling/bench_summary \
  --make-plots

# Per estrarre i risultati ma non generare i grafici 

cd /home/ale/Documenti/GitHub/syscall_throttling/tools

python3 extract_bench_results.py \
  --usctm-results /home/ale/Documenti/GitHub/syscall_throttling/syscall_throttle_usctm/results \
  --kprobe-results /home/ale/Documenti/GitHub/syscall_throttling/syscall_throttle/results \
  --outdir /home/ale/Documenti/GitHub/syscall_throttling/bench_summary

# Punti critici - Casi limite

- cosa succede se rimetto la stessa system call che è già stata registrata? La stessa cosa per il program name e lo UID 
- quando imposto max per epoca, il valore 0 è valido? Che succede? Scelta progettuale
- funziona con 2 system call?
- come funziona con 0 syscall?
- cosa succede se ho dei thread a dormire/spinnare e imposto monitor off? 
- generando system call di diverso tipo, lanciandolo in tempo diverso e nel mentre viene cambiato il valore di MAX, cosa succede? Il peak delay ha senso?
