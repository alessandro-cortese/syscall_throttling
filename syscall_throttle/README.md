# `syscall_throttle/` — KPROBE implementation

Questa directory contiene il modulo di throttling basato su **kprobe** installando un'apposita probe sulla funzione di sistema `x64_sys_call` evitando modifiche alla `sys_call_table`.
Il modulo è `kernel/scth_mod.c` e si configura via `/dev/scth` + `user/scthctl`.

---

## 1. Flusso logico 

1. Tramite il programma utente si registra:
   - **syscall**, il numero, con `addsys <nr>`;
   - filtro **program**, `comm`, e/o **UID**;
   - `MAX_PER_SEC`, budget per epoca da 1 secondo;
2. La kprobe intercetta la syscall e:
   - verifica `match_request(nr, comm, euid)`
   - applica la policy di throttling dipendente dalla modalità selezionata impostata dal parametro `MODE`.

Nel path di intercettazione, handler kprobe, la logica è:

1. early-exit se `monitor_off`
2. match:
   - syscall registrata?
   - comm registrato **oppure** uid registrato?
3. throttling:
   - se budget disponibile: consuma token e ritorna
   - se budget esaurito: attesa fino a epoca successiva che varia in base al mode

### Componenti chiave nel codice

- `match_request(...)`  
  verifica: syscall ∈ `sys_ht` AND (comm ∈ `prog_ht` OR euid ∈ `uid_ht`);

- `epoch_rollover_*`  
  gestisce la finestra da 1s: reset contatori + applicazione di `g_max_next → g_max_current`;

- `update_peak_delay(...)`  
  aggiorna `peak_delay_ns`, `peak_prog`, `peak_uid`, che rappresenta la peggior attesa osservata.

---

## 2. Modalità - mode0/mode1/mode2


Il progetto include **3 implementazioni** della callback, selezionabili con `scthctl setmode <0|1|2>`. 
Lo scopo è mostrare trade-off tra semplicità e overhead.

### MODE 0 — baseline: spin / busy-wait
Implementazione minimale. 

Quando il budget per l’epoca è finito il thread rimane in loop, *busy-wait* finché l’epoca non cambia. Questa è la soluzione più semplice ma si ha un **consumo CPU altissimo** con il rischio di una fase di spin troppo lunga.

Si può osservare un utilizzo della CPU altissimo: nei benchmark con l'utilizzo di `pidstat` si nota il valore `%system ~100%`, e con N=8 si saturano più core.

Si possono riscontrare un numero di context-switch bassi: perché si sta eseguendo in kernel mode senza dormire.

---

### MODE 1 — optimized callback: riduzione overhead nel path
Riduce operazioni costose dentro la callback della kprobe esegueno meno lookups/branching, ma **resta basata su spin**. Si ha meno overhead per chiamata in alcuni casi ma è ancora una soluzione CPU-bound se il carico forza il throttling.

Anche un questo caso si nota l'utilizzo molto alto della CPU. Si possono riscontrare anche un numero maggiore di context switch rispetto al caso precedente dato che si cede la CPU. 

---
### MODE 2 — optimized 

Concettualmente rimane un busy wait ma vengono applicate ulteriori ottimizzazioni in modo da ridurre l'overhead quando si è in fase di throttling. In questa modalità si cerca di applicare una logica best-effort per minimizzare overhead nel contesto kprobe ma d'altro canto si mantiene busy-wait e quindi rimane meno efficiente della versione sleeping.

Anche in questo caso l'utilizzo della CPU rimane elevato, si considera sempre una soluzione in cui i thread non vengono mandati in sleep, ma si possono avere dei piccoli miglioramenti considerando l'overhead per syscall e una contesa interna ridotta.

> In pratica: se MODE 0/1/2 usano spin, i benchmark mostrano CPU ~100% per processo sotto throttling.
> È esattamente la motivazione per cui si è poi passati alla versione “sleeping”, ovvero dove si utilizza l'hook della sys_call_table.
--- 

Gli script lanciano tre modalità. La differenza principale è **come si comporta l’attesa** quando il budget è esaurito.

- **mode0**: baseline, attesa più costosa;
- **mode1**: variante ottimizzata in modo da ridurre l'overhead;
- **mode2**: variante più ottimizzata tra le kprobe.

### Nota su soft lockup
In condizioni estreme, ovvero con **MAX** basso e carico elevato, può apparire:

- `watchdog: BUG: soft lockup - CPU#X stuck ...`

Questo è coerente con attesa attiva, spin/yield, in kernel path.
Non è necessariamente un bug dell’unload, ma un limite intrinseco della strategia.

---

## 3. Riferimenti

File: `kernel/scth_mod.c`.
### 3.1 Config / stato monitor
- `g_monitor_on`;
- `g_max_current`, `g_max_next`;
- `win_count`.

### 3.2 Filtri utilizza della hash set
- `prog_ht`, comm;
- `uid_ht`, euid;
- `sys_ht`, nr syscall.

### 3.3 Statistiche
- `blocked_now`, `peak_blocked`, `sum_blocked`, `n_windows`
- `peak_delay_ns`, `peak_prog`, `peak_uid`

### 3.4 Epoch rollover - 1s
- campiona i threads che sono stati bloccati;
- applica l'applicazione del velore di  **MAX** per la prossima epoca;
- resetta contatori della finestra;
- rilascia eventuali attese in base alla modalità scelta.

---

## 4. Build & run

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

## 5. Trade-off

**Pro**
- intercettazione “standard” tramite tracing/kprobe;
- install/uninstall semplice.

**Contro**
- overhead più alto;
- attesa attiva può consumare molta CPU.
