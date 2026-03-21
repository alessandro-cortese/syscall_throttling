# `syscall_throttle_usctm/` — Syscall table hook

Questa variante implementa il throttling **patchando la `sys_call_table`** per le syscall registrate tramite `addsys <nr>`.
A differenza della versione kprobe, il thread **non usa busy-wait**: quando il budget è finito va in **sleep** su una wait queue e viene risvegliato alla prossima epoca. La hook table è protetta da **RCU** per eliminare la contesa su spinlock nella hot path.

---

## Architettura

### 1. Installazione hook — protocollo in due fasi

Quando l'utente invoca `addsys <nr>`, `install_hook(nr)` segue un preciso ordinamento per evitare race condition tra installatori concorrenti e tra lo stub e una rimozione concorrente:

**Fase 1 — patch della tabella** (prima di pubblicare in `hook_ht`):

```c
cur = READ_ONCE(sys_call_table[nr]);    // orig salvato localmente
scth_begin_syscall_table_patch();       // CR0.WP off (+ gestione CET su CR4)
WRITE_ONCE(sys_call_table[nr], scth_stub);
scth_end_syscall_table_patch();         // ripristino CR0/CR4
```

**Fase 2 — pubblicazione sotto RCU** dopo che lo stub è già live:

```c
hash_add_rcu(hook_ht, &he->node, h);   // ora hook_get_orig() trova l'originale
```

La finestra tra Fase 1 e Fase 2 è intenzionalmente sicura: se un thread entra nello stub prima che Fase 2 completi, `hook_get_orig()` restituisce `NULL` e lo stub ritorna `-ENOSYS`. Questo è accettabile perché il thread stava facendo una race con l'operazione di amministrazione in corso.

La rimozione, `remove_hook`, inverte l'ordine:
1. `hlist_del_rcu(&he->node)`:  unpublish, nuovi reader non trovano più l'entry;
2. ripristino di `sys_call_table[nr]` con il puntatore originale;
3. `call_rcu(&he->rcu, hook_ent_free_rcu)`: free differito dopo la grace period RCU.

Funzioni correlate: `install_hook`, `remove_hook`, `remove_all_hooks`, `scth_begin_syscall_table_patch`, `scth_end_syscall_table_patch`.

### 2. Protezione RCU sulla hook table

`hook_ht`, la tabella che mappa `nr → puntatore originale`, è protetta con **RCU**:

| Ruolo | Meccanismo | Overhead |
|---|---|---|
| **Reader** (`hook_get_orig`, hot path) | `rcu_read_lock` / `rcu_read_unlock` | nessuno spinning, O(1) |
| **Writer** (`install_hook`, `remove_hook`) | serializzato da `hook_mutex`; `hash_add_rcu` / `hlist_del_rcu` | solo cold path (root) |
| **Free** | `call_rcu` → `hook_ent_free_rcu` | differito post grace-period |
| **Unload** | `rcu_barrier()` in `scth_exit` | attende tutti i `call_rcu` pendenti |

La garanzia formale di **quiescent-state** è questa: `rcu_barrier()` in `scth_exit` assicura che nessun callback `call_rcu` possa eseguire dopo il modulo è stato rimosso dalla memoria, requisito critico per la corretta procedura di unload.

Nella versione precedente con spinlock, questa garanzia era assente: un thread in volo dentro `scth_stub` poteva accedere alla memoria di un `hook_ent` già liberato. Con RCU il free avviene solo dopo che tutti i reader attivi al momento della `hlist_del_rcu` hanno completato il loro read-side critical section.

### 3. Fast path nello stub — single stub pattern

Tutte le syscall monitorate sono redirezionate a un unico wrapper `scth_stub`. Questo funziona perché su x86-64 ogni handler ha lo stesso prototipo:

```c
asmlinkage long handler(const struct pt_regs *regs);
```

e il numero di syscall è sempre disponibile in `regs->orig_ax`, indipendentemente da quale entry della `sys_call_table` è stata invocata.

```c
static asmlinkage long scth_stub(const struct pt_regs *regs) {
    nr   = (u32)regs->orig_ax;
    euid = from_kuid(&init_user_ns, current_euid());
    get_task_comm(comm, current);

    orig = hook_get_orig(nr);       // RCU read-side, no spinning
    if (!orig)
        return -ENOSYS;

    if (match_request(nr, comm, euid)) {
        now_ns = ktime_get_ns();
        throttle_sleeping(comm, euid, now_ns);
    }

    return orig(regs);              // forward trasparente
}
```

Un solo simbolo in `.text` invece di N trampolini generati — più semplice, più piccolo, più facile da verificare.

### 4. Sleeping throttle — wait queue & epoch

La finestra è da **1 secondo**. `MAX_PER_SEC` è il budget per epoca. Se `win_count >= g_max_current`:

- il thread incrementa `blocked_now`;
- legge `my_epoch = atomic64_read(&epoch_id)` **prima** di entrare nella wait (anti-lost-wakeup);
- fa `wait_event_killable(epoch_wq, ...)`:

```c
u64 my_epoch = atomic64_read(&epoch_id);
wait_event_killable(epoch_wq,
    (!READ_ONCE(g_monitor_on)) || (atomic64_read(&epoch_id) != my_epoch));
```

Il pattern `my_epoch` prima della wait è il classico anti-lost-wakeup: anche se il rollover avviene tra il momento in cui si decide di aspettare e il momento in cui il thread si addormenta, la condizione risulta già vera e `wait_event_killable` ritorna immediatamente.

L'uso di `wait_event_killable` invece di `wait_event` permette al thread di essere interrotto da un segnale kill fondamentale per `MON_OFF`: quando il monitor viene spento, `wake_up_all(&epoch_wq)` sveglia tutti i waiters e la condizione `!g_monitor_on` è vera, quindi nessun thread rimane in D-state.

Il cambiamento di epoca è gestito dall'`hrtimer`:
- ogni secondo avviene il reset dei contatori della finestra;
- si applica `g_max_next → g_max_current`, in modo da applicare il prossimo valore di MAX;
- incremento di `epoch_id` via `atomic64_inc`;
- `wake_up_all(&epoch_wq)`.

---

## Implicazioni / trade-off

**Pro:**
- **CPU cost molto più basso** rispetto allo spin, soprattutto con N alto e MAX basso;
- il sistema resta responsivo, nessun soft-lockup;
- **garanzia formale di memory safety** sulla hook table tramite RCU + `rcu_barrier()`;
- hot path, `hook_get_orig`, completamente lockless.

**Contro:**
- patchare `sys_call_table` è più invasivo: richiede disabilitare WP su CR0 e gestire CET su CR4;
- ordinamento install/uninstall più delicato, protocollo in due fasi;
- dipende da `the_usctm` per la discovery dell'indirizzo di `sys_call_table`; se non disponibile, fallback via sysfs, `/sys/module/the_usctm/parameters/sys_call_table_address`;
- più context switch rispetto al busy-wait, sleep/wake overhead.

---

## Corner case verificati

Lo script `corner_cases_usctm_hook.sh` copre:

1. **Idempotenza**: duplicate `add*` non duplicano entry;
2. **setmax 0**: impostato ad 1 per scelta progettuale, evita deny-all;
3. **Budget globale condiviso**: con getpid con openat registrati il budget è unico;
4. **Nessun throttling senza syscall registrate**: `peak_delay_ns == 0` atteso;
5. **MON_OFF con waiters**: verifica esplicita via `ps -eo stat` che nessun `tester_getpid` rimanga in stato `D` dopo lo spegnimento del monitor; il `wake_up_all` su `SCTH_IOC_MON_OFF` e la condizione `!g_monitor_on` nella wait garantiscono l'uscita immediata;
6. **MAX mid-run deferred**:  `max_current` invariato subito dopo `setmax`, aggiornato al rollover successivo.

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
