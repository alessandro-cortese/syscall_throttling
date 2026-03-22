// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/timekeeping.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/string.h>
#include <linux/kallsyms.h>
#include <asm/cacheflush.h>
#include <asm/processor.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include "scth_ioctl.h"
#define USCTM_SYSFS_PATH "/sys/module/the_usctm/parameters/sys_call_table_address"
#define HT_BITS 10
extern void **usctm_get_sys_call_table(void);

static unsigned long scth_cr0, scth_cr4;

static struct hrtimer epoch_timer;
static atomic_t epoch_timer_on = ATOMIC_INIT(0);

/* *******************************
 * Hash sets: prog / uid / syscall
 * *******************************
 * */

static bool prog_is_registered(const char *comm);
static bool uid_is_registered(u32 euid);
static bool sys_is_registered(u32 nr);
static void update_peak_delay(const char *comm, u32 euid, u64 delay_ns);

struct prog_ent {
    char name[SCTH_MAX_PROG_LEN];
    u32 h;
    struct hlist_node node;
};

struct uid_ent {
    u32 euid;
    u32 h;
    struct hlist_node node;
};

struct sys_ent {
    u32 nr;
    u32 h;
    struct hlist_node node;
};

static DEFINE_HASHTABLE(prog_ht, HT_BITS);
static DEFINE_HASHTABLE(uid_ht,  HT_BITS);
static DEFINE_HASHTABLE(sys_ht,  HT_BITS);

/* protects hash tables and configuration files */
static DEFINE_SPINLOCK(cfg_lock); 

/* *****************************
 * Monitor state + stats
 * *****************************
 *  */

/* config */
static u32 g_max_current = 100;
static u32 g_max_next = 100;
static bool g_monitor_on = false;

/* 1s window */
static u64 win_start_ns = 0;
static u32 win_count = 0;

/* blocked thread counters, busy wait */
static atomic_t blocked_now = ATOMIC_INIT(0);
static u32 peak_blocked = 0;

/* sampling for ‘avg blocked threads’ */
static u64 sum_blocked = 0;
static u64 n_windows = 0;

/* peak delay */
static u64 peak_delay_ns = 0;
static char peak_prog[SCTH_MAX_PROG_LEN] = {0};
static u32  peak_uid = 0;

/* ***************************************
 * USCTM integration: syscall table hook
 * *************************************** 
 * */

static void **sys_call_table = NULL;          
static DEFINE_MUTEX(hook_mutex);                /* serialise install/uninstall hook */


typedef asmlinkage long (*sys_fn_t)(const struct pt_regs *);

struct hook_ent {
    u32 nr;
    sys_fn_t orig;
    struct hlist_node node;
    struct rcu_head rcu;                        /* RCU callback head  */
};

static DEFINE_HASHTABLE(hook_ht, HT_BITS);

/* *****************************
 * Sleeping throttle support
 * *****************************
 *  */

static wait_queue_head_t epoch_wq;
static atomic64_t epoch_id = ATOMIC64_INIT(0);

static inline void scth_write_cr0_forced(unsigned long val) {
    unsigned long __force_order;
    asm volatile("mov %0, %%cr0" : "+r"(val), "+m"(__force_order));
}

static inline void scth_write_cr4_forced(unsigned long val) {
    unsigned long __force_order;
    asm volatile("mov %0, %%cr4" : "+r"(val), "+m"(__force_order));
}

static inline void scth_conditional_cet_disable(void) {
#ifdef X86_CR4_CET
    if (scth_cr4 & X86_CR4_CET)
        scth_write_cr4_forced(scth_cr4 & ~X86_CR4_CET);
#endif
}

static inline void scth_conditional_cet_enable(void) {
#ifdef X86_CR4_CET
    if (scth_cr4 & X86_CR4_CET)
        scth_write_cr4_forced(scth_cr4);
#endif
}

static inline void scth_begin_syscall_table_patch(void) {
    preempt_disable();
    barrier();
    scth_cr0 = read_cr0();
    scth_cr4 = native_read_cr4();
    scth_conditional_cet_disable();
    scth_write_cr0_forced(scth_cr0 & ~X86_CR0_WP); /* WP off */
    barrier();
}

static inline void scth_end_syscall_table_patch(void) {
    barrier();
    scth_write_cr0_forced(scth_cr0);              /* restore WP */
    scth_conditional_cet_enable();
    barrier();
    preempt_enable();
}

/* write sys_call_table[nr] = fn in safe mode; WP/CET handled by caller */
static int scth_write_sct_entry(u32 nr, void *fn) {
    if (!sys_call_table)
        return -EINVAL;

    WRITE_ONCE(sys_call_table[nr], fn);
    return 0;
}


/*
 * hook_get_orig() — lockless read candidate
 *
 * Currently takes hook_lock (spinlock) for every syscall that
 * passes the match_request() filter.  This is safe but adds
 * lock contention on hot paths.
 *
 * With RCU (see design notes) this becomes an rcu_read_lock()
 * section with no spinning — O(1) and cache-friendly.
 */
static sys_fn_t hook_get_orig(u32 nr) {
    struct hook_ent *e;
    sys_fn_t out = NULL;
    u32 h = jhash(&nr, sizeof(nr), 0);

    /*
     * RCU read-side critical section: no spinning, no sleeping.
     * Safe because writers use synchronize_rcu() (via call_rcu)
     * before freeing hook_ent memory.
     */
    rcu_read_lock();
    hash_for_each_possible_rcu(hook_ht, e, node, h) {
        if (e->nr == nr) {
            out = e->orig;
            break;
        }
    }
    rcu_read_unlock();

    return out;
}

static bool hook_is_installed(u32 nr) {
    /*
     * Called only from install_hook(), which runs under hook_mutex.
     * We can use the mutex-protected path directly; no need for
     * rcu_read_lock here since hook_mutex already excludes writers.
     */
    return hook_get_orig(nr) != NULL;
}

static bool match_request(u32 nr, const char *comm, u32 euid) {
    bool match = false;

    spin_lock(&cfg_lock);
    if (sys_is_registered(nr) && (prog_is_registered(comm) || uid_is_registered(euid)))
        match = true;
    spin_unlock(&cfg_lock);

    return match;
}

/* throttle: consumes the global budget per epoch; if exhausted, sleeps until the next tick */
static void throttle_sleeping(const char *comm, u32 euid, u64 now_ns) {
    u64 local_start = 0;

    if (!READ_ONCE(g_monitor_on))
        return;

    for (;;) {
        unsigned long flags;
        u32 local_max;
        bool allowed = false;

        spin_lock_irqsave(&cfg_lock, flags);

        local_max = READ_ONCE(g_max_current);
        if (local_max == 0)
            local_max = 1;

        if (win_count < local_max) {
            win_count++;
            allowed = true;
        }

        spin_unlock_irqrestore(&cfg_lock, flags);

        if (allowed) {
            if (local_start) {
                atomic_dec(&blocked_now);
                update_peak_delay(comm, euid, ktime_get_ns() - local_start);
            }
            return;
        }

        if (!local_start) {
            local_start = now_ns;
            atomic_inc(&blocked_now);
        }

        /* anti-lost-wakeup */
        {
            u64 my_epoch = atomic64_read(&epoch_id);
            int wret = wait_event_killable(epoch_wq, (!READ_ONCE(g_monitor_on)) || (atomic64_read(&epoch_id) != my_epoch));
            if (wret) { /* signal received (e.g. SIGKILL) */
                break;
            }
        }

        if (!READ_ONCE(g_monitor_on))
            break;

        now_ns = ktime_get_ns();
    }

    if (local_start) {
        atomic_dec(&blocked_now);
        update_peak_delay(comm, euid, ktime_get_ns() - local_start);
    }
}

/*
 * ============================================================
 * SYSCALL TABLE HOOK — DESIGN NOTES
 * ============================================================
 *
 * SINGLE STUB PATTERN
 * -------------------
 * All monitored syscalls are redirected to a single wrapper
 * function: scth_stub(). This works because on x86-64 every
 * syscall handler has the same prototype:
 *
 *   asmlinkage long handler(const struct pt_regs *regs);
 *
 * and the syscall number is always available in regs->orig_ax,
 * regardless of which entry in sys_call_table was invoked.
 * scth_stub() reads orig_ax to identify the syscall, looks up
 * the original handler in hook_ht, applies throttling if the
 * (syscall, comm, euid) triple is registered, and then
 * tail-calls the original handler transparently.
 *
 * This means we only need one function in .text instead of N
 * generated trampolines — simpler, smaller, and easier to audit.
 *
 * INSTALL / REMOVE ORDERING
 * -------------------------
 * install_hook(nr) follows a strict two-phase protocol to avoid
 * races between concurrent installers and between the stub and
 * a concurrent removal:
 *
 *   Phase 1 — patch the table:
 *     Disable WP (and CET if present), write scth_stub into
 *     sys_call_table[nr], re-enable WP/CET.  At this point the
 *     stub is live but hook_ht has no entry for nr yet.
 *
 *   Phase 2 — publish the original:
 *     Under hook_lock, insert the hook_ent (nr -> orig) into
 *     hook_ht.  Only after this point will scth_stub() find
 *     the original and be able to forward the call.
 *
 * The window between Phase 1 and Phase 2 is intentionally safe:
 * if a thread enters scth_stub() before Phase 2 completes,
 * hook_get_orig() returns NULL and the stub returns -ENOSYS.
 * This is acceptable because the hook is being installed at that
 * exact instant — the caller would have raced with the admin
 * operation regardless.
 *
 * remove_hook(nr) reverses the order:
 *
 *   Phase 1 — unpublish:
 *     Under hook_lock, remove the hook_ent from hook_ht and
 *     save the original pointer locally.
 *
 *   Phase 2 — restore the table:
 *     Outside hook_lock, write orig back into sys_call_table[nr].
 *
 * RESIDUAL RACE ON REMOVAL ("threads in flight")
 * -----------------------------------------------
 * After Phase 1 of removal, threads that entered scth_stub()
 * before the hook_ht entry was removed will still complete
 * normally (they already hold orig in a local variable).
 * However, after Phase 2 restores sys_call_table[nr], new
 * callers will go directly to the original handler — correct.
 *
 * The remaining race is: a thread could be inside scth_stub()
 * (between hook_get_orig and the orig() call) while remove_hook
 * is executing Phase 2.  Both paths are safe individually, but
 * there is no synchronization barrier that waits for in-flight
 * stub executions to complete before the hook_ent memory is
 * freed.  In practice this is benign because:
 *   a) the thread already copied orig to a stack-local variable
 *      before we freed hook_ent;
 *   b) scth_stub itself is a kernel .text symbol and is never
 *      freed.
 * A production implementation would use RCU or a per-hook
 * refcount + completion to provide a formal quiescent-state
 * guarantee.  See the companion note on RCU below.
 * ============================================================
 */

static asmlinkage long scth_stub(const struct pt_regs *regs) {
    u32 nr;
    u32 euid;
    char comm[SCTH_MAX_PROG_LEN];
    u64 now_ns;
    sys_fn_t orig;

    if (!regs)
        return -EINVAL;

    nr = (u32)regs->orig_ax;
    euid = (u32)from_kuid(&init_user_ns, current_euid());
    get_task_comm(comm, current);

    /* call */
    orig = hook_get_orig(nr);
    if (!orig)
        return -ENOSYS;

    /* match + throttle */
    if (match_request(nr, comm, euid)) {
        now_ns = ktime_get_ns();
        throttle_sleeping(comm, euid, now_ns);
    }

    return orig(regs);
}

static int install_hook(u32 nr) {
    struct hook_ent *he;
    sys_fn_t cur;
    u32 h;
    int ret;

    /* hook_mutex held by caller (SCTH_IOC_ADD_SYSCALL) */

    if (!sys_call_table)
        return -EINVAL;
    if (hook_is_installed(nr))
        return 0;

    cur = (sys_fn_t)READ_ONCE(sys_call_table[nr]);
    if (!cur)
        return -EINVAL;

    he = kzalloc(sizeof(*he), GFP_KERNEL);
    if (!he)
        return -ENOMEM;

    he->nr   = nr;
    he->orig = cur;
    h = jhash(&nr, sizeof(nr), 0);

    /*
     * Phase 1: patch sys_call_table BEFORE publishing in hook_ht.
     * See design notes for the ordering rationale.
     */
    scth_begin_syscall_table_patch();
    ret = scth_write_sct_entry(nr, (void *)scth_stub);
    scth_end_syscall_table_patch();
    if (ret) {
        kfree(he);
        return ret;
    }

    /*
     * Phase 2: publish under RCU.
     * After this store is visible, hook_get_orig() will find
     * the entry and scth_stub() will forward calls correctly.
     */
    hash_add_rcu(hook_ht, &he->node, h);

    return 0;
}

static void hook_ent_free_rcu(struct rcu_head *head) {
    struct hook_ent *he = container_of(head, struct hook_ent, rcu);
    kfree(he);
}

static int remove_hook(u32 nr) {
    struct hook_ent *he;
    struct hlist_node *tmp;
    u32 h;

    /* hook_mutex held by caller */

    if (!sys_call_table)
        return -EINVAL;

    h = jhash(&nr, sizeof(nr), 0);

    hash_for_each_possible_safe(hook_ht, he, tmp, node, h) {
        if (he->nr != nr)
            continue;

        /*
         * Phase 1: unpublish from RCU-protected hook_ht.
         * After hlist_del_rcu(), new RCU readers will not find
         * this entry.  Existing readers that already found it
         * hold a pointer to he->orig on their stack — safe.
         */
        hlist_del_rcu(&he->node);

        /*
         * Phase 2: restore sys_call_table BEFORE waiting for
         * the RCU grace period.  New syscall invocations will
         * go directly to orig; stub invocations already in
         * flight will complete using their stack-local copy.
         */
        scth_begin_syscall_table_patch();
        scth_write_sct_entry(nr, (void *)he->orig);
        scth_end_syscall_table_patch();

        /*
         * Defer kfree until after a full RCU grace period.
         * This guarantees no reader is still dereferencing
         * he->orig when the memory is released — this is the
         * formal quiescent-state guarantee that was missing
         * in the spinlock version.
         */
        call_rcu(&he->rcu, hook_ent_free_rcu);
        return 0;
    }

    return -ENOENT;
}

static void remove_all_hooks(void) {
    struct hook_ent *he;
    struct hlist_node *tmp;
    int b;

    if (!sys_call_table)
        return;

    /*
     * Removes all hooks and schedules the free operation via call_rcu.
     * After the return, the entries will be freed asynchronously
     * following the RCU grace period. In scth_exit(), we call
     * rcu_barrier() to wait for all pending call_rcu operations
     * to complete before the module is unloaded.
     */
    hash_for_each_safe(hook_ht, b, tmp, he, node) {
        hlist_del_rcu(&he->node);

        scth_begin_syscall_table_patch();
        WRITE_ONCE(sys_call_table[he->nr], (void *)he->orig);
        scth_end_syscall_table_patch();

        call_rcu(&he->rcu, hook_ent_free_rcu);
    }
}

/* *****************************
 * Helpers hashing / lookup
 * *****************************
 *  */

static u32 hash_comm(const char *comm) {
    /* hash on 16 byte */
    return jhash(comm, strnlen(comm, SCTH_MAX_PROG_LEN), 0);
}

static bool prog_is_registered(const char *comm) {
    struct prog_ent *e;
    u32 h = hash_comm(comm);

    hash_for_each_possible(prog_ht, e, node, h) {
        if (strncmp(e->name, comm, SCTH_MAX_PROG_LEN) == 0)
            return true;
    }
    return false;
}

static bool uid_is_registered(u32 euid) {
    struct uid_ent *e;
    u32 h = jhash(&euid, sizeof(euid), 0);

    hash_for_each_possible(uid_ht, e, node, h) {
        if (e->euid == euid)
            return true;
    }
    return false;
}

static bool sys_is_registered(u32 nr) {
    struct sys_ent *e;
    u32 h = jhash(&nr, sizeof(nr), 0);

    hash_for_each_possible(sys_ht, e, node, h) {
        if (e->nr == nr)
            return true;
    }
    return false;
}

/* root-only update :contentReference[oaicite:3]{index=3} */
static bool caller_is_root(void) {
    return (from_kuid(&init_user_ns, current_euid()) == 0);
}

/* *****************************
 * Throttle (busy wait)
 * *****************************
 *  */

static void sample_window_stats_locked(void) {
    /* called on window change */
    u32 b = (u32)atomic_read(&blocked_now);
    sum_blocked += b;
    n_windows++;

    if (b > peak_blocked)
        peak_blocked = b;
}

/* helper: update peak delay */ 
static void update_peak_delay(const char *comm, u32 euid, u64 delay_ns) {
    unsigned long flags;

    spin_lock_irqsave(&cfg_lock, flags);
    if (delay_ns > peak_delay_ns) {
        peak_delay_ns = delay_ns;
        strncpy(peak_prog, comm, SCTH_MAX_PROG_LEN);
        peak_uid = euid;
    }
    spin_unlock_irqrestore(&cfg_lock, flags);
}

static inline void apply_deferred_max_locked(void) {

    /*Last update wins because g_max_next is overwritten; 
    deferred because only copied at each epoch boundary*/

    /* call only under cfg_lock */
    if (g_max_current != g_max_next)
        g_max_current = g_max_next;
}

/* rollover era: call only under cfg_lock */
static void epoch_rollover_locked(u64 now_ns) {
    sample_window_stats_locked();
    apply_deferred_max_locked();

    /* reset window counter */
    win_start_ns = now_ns;
    win_count = 0;

    /* new era: wake up sleeping thread */
    atomic64_inc(&epoch_id);
    wake_up_all(&epoch_wq);
}

/* wall-clock timer: triggers every 1 second regardless of traffic */
static enum hrtimer_restart epoch_timer_cb(struct hrtimer *t) {
    unsigned long flags;
    /* monotonic for internal counter */
    u64 now_ns = ktime_get_ns(); 

    spin_lock_irqsave(&cfg_lock, flags);
    if (READ_ONCE(g_monitor_on)) {
        epoch_rollover_locked(now_ns);
    }
    spin_unlock_irqrestore(&cfg_lock, flags);

    hrtimer_forward_now(t, ktime_set(1, 0));
    return HRTIMER_RESTART;
}

/* ****************************
 * ioctl + device
 * *************************** 
 * */

static long scth_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    unsigned long flags;
    int ret = 0;

    (void)f;

    switch (cmd) {
    case SCTH_IOC_ADD_PROG:
    case SCTH_IOC_DEL_PROG:
    case SCTH_IOC_ADD_UID:
    case SCTH_IOC_DEL_UID:
    case SCTH_IOC_ADD_SYSCALL:
    case SCTH_IOC_DEL_SYSCALL:
    case SCTH_IOC_SET_MAX:
    case SCTH_IOC_MON_ON:
    case SCTH_IOC_MON_OFF:
    case SCTH_IOC_RESET_STATS:
    case SCTH_IOC_WAKE_WAITERS:
        if (!caller_is_root())
            return -EPERM;
        break;
    default:
        break;
    }

    spin_lock_irqsave(&cfg_lock, flags);

    switch (cmd) {
    case SCTH_IOC_ADD_PROG: {
        struct scth_prog_req req;
        struct prog_ent *e;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { 
            ret = -EFAULT; 
            break; 
        }
        req.name[SCTH_MAX_PROG_LEN-1] = '\0';
        if (prog_is_registered(req.name)) break;
        e = kzalloc(sizeof(*e), GFP_ATOMIC);
        if (!e) { 
            ret = -ENOMEM; 
            break; 
        }
        strncpy(e->name, req.name, SCTH_MAX_PROG_LEN);
        e->h = hash_comm(e->name);
        hash_add(prog_ht, &e->node, e->h);
        break;
    }

    case SCTH_IOC_DEL_PROG: {
        struct scth_prog_req req;
        struct prog_ent *e;
        u32 h;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { 
            ret = -EFAULT; 
            break; 
        }
        req.name[SCTH_MAX_PROG_LEN-1] = '\0';
        h = hash_comm(req.name);
        hash_for_each_possible(prog_ht, e, node, h) {
            if (strncmp(e->name, req.name, SCTH_MAX_PROG_LEN) == 0) {
                hash_del(&e->node);
                kfree(e);
                break;
            }
        }
        break;
    }
    
    case SCTH_IOC_ADD_UID: {
        struct scth_uid_req req;
        struct uid_ent *e;
        u32 h;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { 
            ret = -EFAULT; 
            break; 
        }
        if (uid_is_registered(req.euid)) break;
        e = kzalloc(sizeof(*e), GFP_ATOMIC);
        if (!e) { 
            ret = -ENOMEM; 
            break; 
        }
        e->euid = req.euid;
        h = jhash(&e->euid, sizeof(e->euid), 0);
        e->h = h;
        hash_add(uid_ht, &e->node, e->h);
        break;
    }
    
    case SCTH_IOC_DEL_UID: {
        struct scth_uid_req req;
        struct uid_ent *e;
        u32 h;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { 
            ret = -EFAULT; 
            break; 
        }
        h = jhash(&req.euid, sizeof(req.euid), 0);
        hash_for_each_possible(uid_ht, e, node, h) {
            if (e->euid == req.euid) {
                hash_del(&e->node);
                kfree(e);
                break;
            }
        }
        break;
    }

    case SCTH_IOC_ADD_SYSCALL: {
        struct scth_sys_req req;
        struct sys_ent *e;
        u32 h;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { 
            ret = -EFAULT; 
            break; 
        }
        if (!sys_call_table) { 
            ret = -EINVAL; 
            break; 
        }
        if (sys_is_registered(req.nr)) break;

        /* install hook FUORI dallo spinlock cfg_lock */
        spin_unlock_irqrestore(&cfg_lock, flags);
        mutex_lock(&hook_mutex);
        ret = install_hook(req.nr);
        mutex_unlock(&hook_mutex);
        spin_lock_irqsave(&cfg_lock, flags);

        if (ret)
            break;

        /* solo se hook ok, registra la syscall nella ht */
        e = kzalloc(sizeof(*e), GFP_ATOMIC);
        if (!e) { 
            ret = -ENOMEM; 
            break; 
        }
        e->nr = req.nr;
        h = jhash(&e->nr, sizeof(e->nr), 0);
        e->h = h;
        hash_add(sys_ht, &e->node, e->h);

        break;
    }

    case SCTH_IOC_DEL_SYSCALL: {
        struct scth_sys_req req;
        struct sys_ent *e;
        u32 h;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { 
            ret = -EFAULT; 
            break; 
        }
        if (!sys_call_table) { 
            ret = -EINVAL; 
            break; 
        }

        /* rimuove hook fuori cfg_lock */
        spin_unlock_irqrestore(&cfg_lock, flags);
        mutex_lock(&hook_mutex);
        ret = remove_hook(req.nr);
        mutex_unlock(&hook_mutex);
        spin_lock_irqsave(&cfg_lock, flags);

        if (ret)
            break;

        /* solo se hook rimosso, rimuovi anche da sys_ht */
        h = jhash(&req.nr, sizeof(req.nr), 0);
        hash_for_each_possible(sys_ht, e, node, h) {
            if (e->nr == req.nr) {
                hash_del(&e->node);
                kfree(e);
                break;
            }
        }
        break;
    }
    
    case SCTH_IOC_SET_MAX: {
        struct scth_max_req req;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { 
            ret = -EFAULT; 
            break; 
        }
        g_max_next = (req.max_per_sec == 0 ? 1 : req.max_per_sec);
        break;
    }
    
    case SCTH_IOC_MON_ON: {
        g_monitor_on = true;

        u64 now_ns_local = ktime_get_ns();
        epoch_rollover_locked(now_ns_local);

        if (atomic_cmpxchg(&epoch_timer_on, 0, 1) == 0) {
            hrtimer_start(&epoch_timer, ktime_set(1, 0), HRTIMER_MODE_REL);
        }
        break;
    }
    
    case SCTH_IOC_MON_OFF:
        g_monitor_on = false;
        wake_up_all(&epoch_wq);
    
        if (atomic_cmpxchg(&epoch_timer_on, 1, 0) == 1) {
            spin_unlock_irqrestore(&cfg_lock, flags);
            hrtimer_cancel(&epoch_timer);
            spin_lock_irqsave(&cfg_lock, flags);
        }
        break;

    case SCTH_IOC_LIST_PROG:
    case SCTH_IOC_LIST_UID:
    case SCTH_IOC_LIST_SYSCALL: {
        struct scth_list_resp lr;
        u32 cap, out = 0;

        if (copy_from_user(&lr, (void __user *)arg, sizeof(lr))) { 
            ret = -EFAULT; 
            break; 
        }
        cap = lr.count;
        if (cap > SCTH_MAX_LIST) cap = SCTH_MAX_LIST;
        if (lr.ptr == 0) { 
            ret = -EINVAL; 
            break; 
        }

        if (cmd == SCTH_IOC_LIST_PROG) {
            struct prog_ent *e;
            int b;
            char __user *ubuf = (char __user *)(uintptr_t)lr.ptr;
            hash_for_each(prog_ht, b, e, node) {
                if (out >= cap) break;
                if (copy_to_user(ubuf + out*SCTH_MAX_PROG_LEN, e->name, SCTH_MAX_PROG_LEN)) { 
                    ret = -EFAULT; 
                    break; 
                }
                out++;
            }
        } else if (cmd == SCTH_IOC_LIST_UID) {
            struct uid_ent *e;
            int b;
            u32 __user *ubuf = (u32 __user *)(uintptr_t)lr.ptr;
            hash_for_each(uid_ht, b, e, node) {
                if (out >= cap) break;
                if (put_user(e->euid, &ubuf[out])) { 
                    ret = -EFAULT; 
                    break; 
                }
                out++;
            }
        } else {
            struct sys_ent *e;
            int b;
            u32 __user *ubuf = (u32 __user *)(uintptr_t)lr.ptr;
            hash_for_each(sys_ht, b, e, node) {
                if (out >= cap) break;
                if (put_user(e->nr, &ubuf[out])) { 
                    ret = -EFAULT; 
                    break; 
                }
                out++;
            }
        }

        if (!ret) {
            lr.count = out;
            if (copy_to_user((void __user *)arg, &lr, sizeof(lr))) ret = -EFAULT;
        }
        break;
    }

    case SCTH_IOC_GET_STATS: {
        struct scth_stats st;
        u64 avg_x1000 = 0;
        if (n_windows)
            avg_x1000 = div64_u64(sum_blocked * 1000ULL, n_windows);

        memset(&st, 0, sizeof(st));
        st.peak_delay_ns = peak_delay_ns;
        strncpy(st.peak_prog, peak_prog, SCTH_MAX_PROG_LEN);
        st.peak_euid = peak_uid;
        st.peak_blocked_threads = peak_blocked;
        st.avg_blocked_threads_x1000 = avg_x1000;
        st.max_current_per_sec = g_max_current;
        st.max_next_per_sec = g_max_next;
        st.monitor_on = g_monitor_on ? 1 : 0;

        spin_unlock_irqrestore(&cfg_lock, flags);
        if (copy_to_user((void __user *)arg, &st, sizeof(st))) return -EFAULT;
        return 0;
    }

    case SCTH_IOC_RESET_STATS: {
        peak_delay_ns = 0;
        memset(peak_prog, 0, sizeof(peak_prog));
        peak_uid = 0;
        peak_blocked = 0;
        sum_blocked = 0;
        n_windows = 0;

        g_max_current = g_max_next;

        /* reset window */
        win_start_ns = 0;
        win_count = 0;
        atomic_set(&blocked_now, 0);

        break;
    }

    case SCTH_IOC_WAKE_WAITERS: {
        /*
        * Wakes up any threads that have fallen asleep in the throttle:
        * - bump epoch_id => the condition (epoch_id != my_epoch) becomes true
        * - wake_up_all  => we do not wait for the next tick
        *
        * NOTE: does not modify counters/stats.
        */
        atomic64_inc(&epoch_id);
        wake_up_all(&epoch_wq);
        break;
    }

    default:
        ret = -ENOTTY;
        break;
    }

    spin_unlock_irqrestore(&cfg_lock, flags);
    return ret;
}

static const struct file_operations scth_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = scth_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = scth_ioctl,
#endif
};

/* open to all; but updates are restricted to root users: contentReference[oaicite:7]{index=7} */
static struct miscdevice scth_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "scth",
    .fops  = &scth_fops,
    .mode  = 0666, 
};

/* cleanup helpers */
static void free_hashtable_prog(void) {
    struct prog_ent *e;
    struct hlist_node *tmp;
    int b;
    hash_for_each_safe(prog_ht, b, tmp, e, node) { 
        hash_del(&e->node); 
        kfree(e); 
    }
}

static void free_hashtable_uid(void) {
    struct uid_ent *e;
    struct hlist_node *tmp;
    int b;
    hash_for_each_safe(uid_ht, b, tmp, e, node) { 
        hash_del(&e->node); 
        kfree(e); 
    }
}

static void free_hashtable_sys(void) {
    struct sys_ent *e;
    struct hlist_node *tmp;
    int b;
    hash_for_each_safe(sys_ht, b, tmp, e, node) { 
        hash_del(&e->node); 
        kfree(e); 
    }
}

static int read_u64_from_sysfs(const char *path, u64 *out) {
    struct file *f;
    char buf[64];
    loff_t pos = 0;
    ssize_t n;
    unsigned long long v;

    if (!out)
        return -EINVAL;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
    filp_close(f, NULL);

    if (n <= 0)
        return -EIO;

    buf[n] = '\0';

    /* The file contains a decimal number (u64) followed by a newline */
    if (kstrtoull(strim(buf), 10, &v) < 0)
        return -EINVAL;

    *out = (u64)v;
    return 0;
}

static int __init scth_init(void) {
    int ret;

    hash_init(prog_ht);
    hash_init(uid_ht);
    hash_init(sys_ht);

    init_waitqueue_head(&epoch_wq);
    atomic64_set(&epoch_id, 0);
    hash_init(hook_ht);

    /* init hrtimer */
    hrtimer_setup(&epoch_timer, epoch_timer_cb, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    atomic_set(&epoch_timer_on, 0);

    ret = misc_register(&scth_dev);
    if (ret) {
        printk("scth: misc_register failed: %d\n", ret);
        return ret;
    }

    void **(*getter)(void) = NULL;

    getter = symbol_get(usctm_get_sys_call_table);
    if (getter) {
        sys_call_table = getter();
        symbol_put(usctm_get_sys_call_table);
    }

    if (sys_call_table)
        printk("scth: sys_call_table=%px (from usctm symbol_get)\n", sys_call_table);
    else
        printk("scth: cannot get sys_call_table from usctm (symbol not found). Using sysfs/ioctl.\n");

    if (!sys_call_table) {
        u64 addr = 0;
        if (read_u64_from_sysfs(USCTM_SYSFS_PATH, &addr) == 0 && addr) {
            sys_call_table = (void **)(uintptr_t)addr;
            printk("scth: sys_call_table=%px (from sysfs fallback)\n", sys_call_table);
        }
    }

    printk("scth: loaded (/dev/%s). monitor_off, MAXcur=%u MAXnext=%u\n", scth_dev.name, g_max_current, g_max_next);
    return 0;
}

static void __exit scth_exit(void)
{
    if (atomic_cmpxchg(&epoch_timer_on, 1, 0) == 1)
        hrtimer_cancel(&epoch_timer);

    mutex_lock(&hook_mutex);
    remove_all_hooks();
    mutex_unlock(&hook_mutex);

    /*
     * rcu_barrier() waits for all pending call_rcu() callbacks
     * (from remove_hook / remove_all_hooks) to complete.
     * This MUST be called before misc_deregister and before the
     * module's .text is unmapped — otherwise the hook_ent_free_rcu
     * callback could execute after the module is gone.
     */
    rcu_barrier();

    misc_deregister(&scth_dev);
    free_hashtable_prog();
    free_hashtable_uid();
    free_hashtable_sys();

    printk("scth: unloaded\n");
}

module_init(scth_init);
module_exit(scth_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alessandro Cortese");
MODULE_DESCRIPTION("LKM - Syscall Throttling System");