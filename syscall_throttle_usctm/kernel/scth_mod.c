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

static DEFINE_SPINLOCK(cfg_lock); /* protects hash tables and configuration files */

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
static DEFINE_MUTEX(hook_mutex);              /* serialise install/uninstall hook */
static DEFINE_SPINLOCK(hook_lock);

typedef asmlinkage long (*sys_fn_t)(const struct pt_regs *);

struct hook_ent {
    u32 nr;
    sys_fn_t orig;
    struct hlist_node node;
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

static sys_fn_t hook_get_orig(u32 nr) {
    struct hook_ent *e;
    sys_fn_t out = NULL;
    u32 h = jhash(&nr, sizeof(nr), 0);

    spin_lock(&hook_lock);
    hash_for_each_possible(hook_ht, e, node, h) {
        if (e->nr == nr) {
            out = e->orig;
            break;
        }
    }
    spin_unlock(&hook_lock);

    return out;
}

static bool hook_is_installed(u32 nr) {
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

    if (!sys_call_table)
        return -EINVAL;

    /* already installed? */
    if (hook_is_installed(nr))
        return 0;

    cur = (sys_fn_t)READ_ONCE(sys_call_table[nr]);
    if (!cur)
        return -EINVAL;

    he = kzalloc(sizeof(*he), GFP_KERNEL);
    if (!he)
        return -ENOMEM;

    he->nr = nr;
    he->orig = cur;
    h = jhash(&nr, sizeof(nr), 0);

    /*
     * 1) Patch the sys_call_table BEFORE making the entry visible in hook_ht
     *    so that the stub never sees the “registered” original but the unpatched entry
     */
    scth_begin_syscall_table_patch();
    ret = scth_write_sct_entry(nr, (void *)scth_stub);
    if (ret) {
        kfree(he);
        return ret;
    }
    scth_end_syscall_table_patch();

    /*
     * 2) Now publish the entry in hook_ht under hook_lock.
     *    Check again: if someone has installed it in the meantime, roll back and try again.
     */
    spin_lock(&hook_lock);
    {
        struct hook_ent *e2;
        bool already = false;

        hash_for_each_possible(hook_ht, e2, node, h) {
            if (e2->nr == nr) {
                already = true;
                break;
            }
        }

        if (already) {
            spin_unlock(&hook_lock);

            /* rollback patch */
            scth_begin_syscall_table_patch();
            (void)scth_write_sct_entry(nr, (void *)cur);
            scth_end_syscall_table_patch();

            kfree(he);
            return 0;
        }

        hash_add(hook_ht, &he->node, h);
    }
    spin_unlock(&hook_lock);

    return 0;
}

static int remove_hook(u32 nr) {
    struct hook_ent *he;
    struct hlist_node *tmp;
    u32 h;
    sys_fn_t orig = NULL;
    int ret;

    if (!sys_call_table)
        return -EINVAL;

    h = jhash(&nr, sizeof(nr), 0);

    /* 1) Find and unhook from hook_ht under lock */
    spin_lock(&hook_lock);
    hash_for_each_possible_safe(hook_ht, he, tmp, node, h) {
        if (he->nr == nr) {
            orig = he->orig;
            hash_del(&he->node);
            spin_unlock(&hook_lock);

            /* 2) Restore the sys_call_table OUTSIDE the spinlock */
            scth_begin_syscall_table_patch();
            ret = scth_write_sct_entry(nr, (void *)orig);
            if (ret)
                printk("scth: failed restoring sys_call_table[%u]\n", nr);
            scth_end_syscall_table_patch();

            kfree(he);
            return 0;
        }
    }
    spin_unlock(&hook_lock);

    return -ENOENT;
}

static void remove_all_hooks(void) {
    struct hook_ent *he;
    struct hlist_node *tmp;
    int b;

    if (!sys_call_table)
        return;

    for (b = 0; b < (1 << HT_BITS); b++) {
        for (;;) {
            u32 nr;
            sys_fn_t orig;

            /* 1) Take an element and remove it from the list under a spinlock */
            spin_lock(&hook_lock);

            he = NULL;
            hlist_for_each_entry_safe(he, tmp, &hook_ht[b], node) {
                hlist_del(&he->node);
                /* The first one breaks away and leaves */
                break; 
            }

            spin_unlock(&hook_lock);

            if (!he)
                break; /* empty bucket */

            nr = he->nr;
            orig = he->orig;

            /* 2) Restore the entry OUTSIDE the spinlock */
            scth_begin_syscall_table_patch();
            WRITE_ONCE(sys_call_table[nr], (void *)orig);
            scth_end_syscall_table_patch();

            kfree(he);
        }
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

static void __exit scth_exit(void) {

    if (atomic_cmpxchg(&epoch_timer_on, 1, 0) == 1)
        hrtimer_cancel(&epoch_timer);

    mutex_lock(&hook_mutex);
    remove_all_hooks();
    mutex_unlock(&hook_mutex);    
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