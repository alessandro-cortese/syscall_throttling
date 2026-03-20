# `syscall_throttle_usctm/` — Syscall table hook 

Questa variante implementa il throttling **patchando la `sys_call_table`** per le syscall registrate, `addsys <nr>` tramite il programma user.
A differenza della versione kprobe, qui il thread **non segue una politica di busy waiting**: quando il budget è finito va in **sleep** su una wait queue, e viene risvegliato alla prossima epoca.

---

## Architettura

### 1. Installazione hook
Quando l’utente invoca `addsys <nr>`:
- legge `sys_call_table[nr]`, pvvero la funzione originale;
- sostituisce l’entry con uno **stub**, `scth_stub`;
- memorizza l’originale in una hashtable, `hook_ht`, per poter ripristinare il contenuto originale.

Snippet semplificato:

```c
// install_hook(nr):
cur = READ_ONCE(sys_call_table[nr]);   // orig
scth_begin_syscall_table_patch();      // CR0.WP off (+ CET handling)
WRITE_ONCE(sys_call_table[nr], scth_stub);
scth_end_syscall_table_patch();

hash_add(hook_ht, &he->node, h);       // salva orig
```

Funzioni correlate nel codice:
- `install_hook(u32 nr)`
- `remove_hook(u32 nr)`
- `remove_all_hooks(void)`
- `scth_begin_syscall_table_patch()` / `scth_end_syscall_table_patch()`

### 2. Fast path nello stub
Lo stub:
- ricava `nr` da `regs->orig_ax`;
- ricava `comm` e `euid`;
- se matcha la policy allora si attua il meccanismo di throttling;
- chiama comunque l’originale.

```c
static asmlinkage long scth_stub(const struct pt_regs *regs) {
  nr   = (u32)regs->orig_ax;
  euid = from_kuid(&init_user_ns, current_euid());
  get_task_comm(comm, current);

  orig = hook_get_orig(nr);
  if (match_request(nr, comm, euid))
      throttle_sleeping(comm, euid, ktime_get_ns());

  return orig(regs);
}
```

### 3. Sleeping throttle - wait queue & epoch
La finestra è da **1 secondo**. `MAX_PER_SEC` è il budget per epoca.
Se `win_count >= g_max_current`:
- il thread incrementa `blocked_now;`
- fa `wait_event_killable(epoch_wq, epoch_changed || monitor_off)`;
- al wakeup aggiorna `peak_delay_ns`.

```c
u64 my_epoch = atomic64_read(&epoch_id);
wait_event_killable(epoch_wq,
   (!g_monitor_on) || (atomic64_read(&epoch_id) != my_epoch));
```

Il cambiamento dell'epoca è gestito da `hrtimer`:
- ogni secondo avviene un reset contatori finestra;
- si applica `g_max_next → g_max_current`;
- incrementa `epoch_id`;
- `wake_up_all(&epoch_wq)`.

---

## Implicazioni / trade-off

Pro:
- **CPU cost molto più basso** rispetto allo spin, soprattutto in caso di N alto e MAX basso;
- il sistema resta responsivo, non si riscontra il problema del soft-lockup del busy-wait.

Contro:
- patchare `sys_call_table` è più invasivo, serve disabilitare WP e gestire CET;
- richiede maggiore cura su ordering/locking durante install/uninstall hook;
- si appoggi si un componente del modulo aggiuntivo, `the_usctm`, che, una volta montato, non può essere smontato.

---

## Build & run

```bash
cd syscall_throttle_usctm/kernel
./compile.sh
sudo ./install.sh

cd ../user
make
sudo ./scthctl stats
```

Benchmark:

```bash
cd ../scripts
MAX_PER_SEC=5 DUR=10 N_LIST="1 8" ./bench_usctm_hook.sh
```

Corner cases:

```bash
./corner_cases_usctm_hook.sh
```
