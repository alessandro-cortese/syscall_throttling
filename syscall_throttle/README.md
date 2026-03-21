# `syscall_throttle/` — KPROBE implementation

Questa directory contiene il modulo di throttling basato su **kprobe** installando un'apposita probe sulla funzione `x64_sys_call`, evitando modifiche alla `sys_call_table`.
Il modulo è `kernel/scth_mod.c` e si configura via `/dev/scth` + `user/scthctl`.

---

## 1. Flusso logico

1. Tramite il programma utente si registra:
   - **syscall**, numero, con `addsys <nr>`;
   - filtro **program**, `comm`, e/o **UID**;
   - `MAX_PER_SEC`, budget per epoca da 1 secondo;
2. La kprobe intercetta ogni invocazione di `x64_sys_call` e:
   - verifica `match_request(nr, comm, euid)`;
   - applica la policy di throttling dipendente dalla modalità selezionata.

Nel path di intercettazione, ovvero l'handler kprobe, la logica è:

1. early-exit se `monitor_off`;
2. match: syscall registrata? **E** (comm registrato **O** uid registrato)?
3. throttling: se budget disponibile, consuma un token e ritorna; se budget esaurito, attesa fino all'epoca successiva dove il tipo della stessa attesa dipende dalla modalità selezionata.

### Nota sul recupero del numero di syscall

A differenza della versione usctm dove `regs->orig_ax` è sempre affidabile, qui la probe si aggancia su `x64_sys_call` e il numero di syscall viene letto da `regs->si`, secondo argomento per ABI interna, con fallback su `regs->di->orig_ax` se il valore da `si` appare non plausibile, > 1024. La soglia è euristica e funziona sui kernel testati, ma è più fragile rispetto all'accesso diretto in uno stub.

### Componenti chiave nel codice

- `kp_pre_handler` — entry point kprobe; recupera nr/comm/euid, verifica match, chiama `throttle_if_needed`;
- `match_request` — verifica: `nr ∈ sys_ht` AND (`comm ∈ prog_ht` OR `euid ∈ uid_ht`);
- `epoch_rollover_*` — gestisce la finestra da 1s: reset contatori + applicazione `g_max_next → g_max_current`;
- `update_peak_delay` — aggiorna `peak_delay_ns`, `peak_prog`, `peak_uid`.

---

## 2. Modalità — mode0/mode1/mode2

L'implementazione include **3 implementazioni** della logica di attesa, selezionabili con `scthctl setmode <0|1|2>`. Lo scopo è mostrare trade-off tra semplicità e overhead, e motivare lo sviluppo della versione sleeping.

### MODE 0 — baseline: spin / busy-wait

Implementazione minimale. Quando il budget è finito il thread rimane in loop con `cpu_relax()` finché `win_start_ns` non cambia, il che avviene solo al rollover dell'hrtimer. È la soluzione più semplice ma con **consumo CPU altissimo** e rischio di spin prolungati in kernel path.

Nei benchmark con `pidstat` si osserva `%system ~100%`; con N=8 si saturano più core. I context switch sono bassi perché il thread non cede mai volontariamente la CPU.

### MODE 1 — backoff esponenziale

Stesso schema di MODE 0, ma l'inner loop di attesa usa un **backoff esponenziale**: inizia con `step=64` iterazioni di `cpu_relax()`, poi raddoppia `step` fino a un massimo di 8192. Riduce la contesa sul bus della cache e il numero di acquisizioni dello spinlock rispetto al baseline, ma rimane CPU-bound se il carico forza il throttling.

Si possono osservare più context switch rispetto a MODE 0 dato che il backoff porta occasionalmente lo scheduler a intervenire ma l'utilizzo della CPU rimane comunque elevato.

### MODE 2 — atomic tokens

Introduce un token bucket gestito con `atomic_dec_if_positive(&g_tokens)`:

- **fast path**: se ci sono token disponibili, decremento atomico senza acquisire nessun lock, O(1) e cache-friendly;
- **slow path**: se i token sono esauriti, attesa con backoff su `g_epoch_sec` aggiornato dal timer;
- i token vengono ricaricati in `epoch_rollover_locked` con `atomic_set(&g_tokens, g_max_current)`.

Riduce l'overhead sul path "allowed" rispetto a MODE 0/1, nessuno spinlock per i thread che passano, ma rimane busy-wait per i thread bloccati.

> In pratica: MODE 0/1/2 usano tutti spin, i benchmark mostrano CPU ~100% per processo sotto throttling. È esattamente la motivazione per la versione sleeping con hook della `sys_call_table`.

---

## 3. Nota su soft lockup

In condizioni estreme, **MAX** basso con carico elevato e con molti thread,  può apparire:

```
watchdog: BUG: soft lockup - CPU#X stuck ...
```

Questo è coerente con attesa attiva in kernel path e non è un bug del modulo: è un limite intrinseco della strategia busy-wait. La versione `syscall_throttle_usctm/` elimina completamente questo problema usando sleep su wait queue.

---

## 4. Riferimenti al codice

File: `kernel/scth_mod.c`.

### 4.1 Config / stato monitor

- `g_monitor_on`, `g_max_current`, `g_max_next`, `win_count`;
- `epoch_timer`: hrtimer, 1s wall-clock, indipendente dal traffico.

### 4.2 Filtri — hash set

- `prog_ht` (comm), `uid_ht` (euid), `sys_ht` (nr syscall).

### 4.3 Statistiche

- `blocked_now`, `peak_blocked`, `sum_blocked`, `n_windows`;
- `peak_delay_ns`, `peak_prog`, `peak_uid`.

### 4.4 Epoch rollover — 1s

1. Campionamento thread bloccati: `sample_window_stats_locked`;
2. Applicazione deferred MAX: `apply_deferred_max_locked`;
3. Reset contatori finestra: `win_start_ns`, `win_count`;
4. Reset token bucket per MODE 2: `atomic_set(&g_tokens, g_max_current)`.

---

## 5. Build & run

```bash
cd syscall_throttle/kernel
./compile.sh
sudo ./install.sh

cd ../user
make
sudo ./scthctl stats
```

Benchmark:

```bash
cd ../scripts
MAX_PER_SEC=5 DUR=10 N_LIST="1 8" MODE_LIST="0 1 2" ./bench_kprobe.sh
```

Corner cases:

```bash
./corner_cases_kprobe.sh
```

---

## 6. Trade-off

**Pro:**
- intercettazione tramite tracing/kprobe, nessuna modifica alla `sys_call_table`;
- install/uninstall semplice, nessuna patch di memoria kernel;
- tre modalità esplicitano i trade-off busy-wait vs overhead per confronto.

**Contro:**
- overhead per chiamata più alto, trap + handler kprobe su ogni `x64_sys_call`;
- recupero del numero di syscall da `regs->si` è euristico e dipende dall'ABI interna del kernel;
- attesa attiva consuma molta CPU e può causare soft-lockup;
- non adatto per syscall bloccanti ad alto volume: i thread in spin potrebbero impedire ad altri thread di ottenere la CPU.
