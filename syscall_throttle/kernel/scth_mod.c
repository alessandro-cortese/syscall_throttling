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
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/sched/task_stack.h>

#include "scth_ioctl.h"


/* modalità throttling:
 * 0 = baseline (lock+busy)
 * 1 = backoff (deadline + polling raro)
 * 2 = atomic tokens (fast path lockless + rollover lock)
 */
static u32 g_mode = 0;

/* token bucket per MODE 2 */
static atomic_t g_tokens = ATOMIC_INIT(0);
/* epoch finestra (in secondi) per rollover) */
static u64 g_epoch_sec = 0;

module_param(g_mode, uint, 0644);
MODULE_PARM_DESC(g_mode, "Throttle mode: 0=baseline,1=backoff,2=atomic_tokens");

/* -----------------------------
 * Hash sets: prog / uid / syscall
 * ----------------------------- */

#define HT_BITS 10

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

static DEFINE_SPINLOCK(cfg_lock); /* protegge hash tables + monitor config */

/* -----------------------------
 * Monitor state + stats
 * ----------------------------- */

/* configurazione */
static u32 g_max_per_sec = 100;
static bool g_monitor_on = false;

/* finestra 1s */
static u64 win_start_ns = 0;
static u32 win_count = 0;

/* contatori thread bloccati (busy wait) */
static atomic_t blocked_now = ATOMIC_INIT(0);
static u32 peak_blocked = 0;

/* campionamento per "avg blocked threads" */
static u64 sum_blocked = 0;
static u64 n_windows = 0;

/* peak delay */
static u64 peak_delay_ns = 0;
static char peak_prog[SCTH_MAX_PROG_LEN] = {0};
static u32  peak_uid = 0;

/* -----------------------------
 * Helpers hashing / lookup
 * ----------------------------- */

static u32 hash_comm(const char *comm)
{
    /* hash su 16 byte circa */
    return jhash(comm, strnlen(comm, SCTH_MAX_PROG_LEN), 0);
}

static bool prog_is_registered(const char *comm)
{
    struct prog_ent *e;
    u32 h = hash_comm(comm);

    hash_for_each_possible(prog_ht, e, node, h) {
        if (strncmp(e->name, comm, SCTH_MAX_PROG_LEN) == 0)
            return true;
    }
    return false;
}

static bool uid_is_registered(u32 euid)
{
    struct uid_ent *e;
    u32 h = jhash(&euid, sizeof(euid), 0);

    hash_for_each_possible(uid_ht, e, node, h) {
        if (e->euid == euid)
            return true;
    }
    return false;
}

static bool sys_is_registered(u32 nr)
{
    struct sys_ent *e;
    u32 h = jhash(&nr, sizeof(nr), 0);

    hash_for_each_possible(sys_ht, e, node, h) {
        if (e->nr == nr)
            return true;
    }
    return false;
}

/* root-only update come richiesto :contentReference[oaicite:3]{index=3} */
static bool caller_is_root(void)
{
    return (from_kuid(&init_user_ns, current_euid()) == 0);
}

/* -----------------------------
 * Throttle (busy wait)
 * ----------------------------- */

static void sample_window_stats_locked(void)
{
    /* chiamata quando si “ruota” finestra */
    u32 b = (u32)atomic_read(&blocked_now);
    sum_blocked += b;
    n_windows++;

    if (b > peak_blocked)
        peak_blocked = b;
}

/* helper: aggiorna peak delay in modo consistente */
static void update_peak_delay(const char *comm, u32 euid, u64 delay_ns)
{
    unsigned long flags;

    spin_lock_irqsave(&cfg_lock, flags);
    if (delay_ns > peak_delay_ns) {
        peak_delay_ns = delay_ns;
        strncpy(peak_prog, comm, SCTH_MAX_PROG_LEN);
        peak_uid = euid;
    }
    spin_unlock_irqrestore(&cfg_lock, flags);
}

/*
 * MODE 0: baseline (quello che avevi già)
 * lock + polling frequente (ktime_get_ns ad ogni giro)
 */
static void throttle_baseline(const char *comm, u32 euid, u64 now_ns)
{
    u64 local_start = 0;
    bool counted = false;
    u32 local_max;

    if (!READ_ONCE(g_monitor_on))
        return;

    local_max = READ_ONCE(g_max_per_sec);
    if (local_max == 0)
        local_max = 1;

    for (;;) {
        unsigned long flags;

        spin_lock_irqsave(&cfg_lock, flags);

        if (win_start_ns == 0) {
            win_start_ns = now_ns;
            win_count = 0;
        }

        if (now_ns - win_start_ns >= NSEC_PER_SEC) {
            sample_window_stats_locked();
            win_start_ns = now_ns;
            win_count = 0;
        }

        if (win_count < local_max) {
            win_count++;
            counted = true;
        }

        spin_unlock_irqrestore(&cfg_lock, flags);

        if (counted)
            break;

        if (local_start == 0) {
            local_start = now_ns;
            atomic_inc(&blocked_now);
            /* peak_blocked aggiornato in modo conservativo */
            {
                u32 b = (u32)atomic_read(&blocked_now);
                if (b > READ_ONCE(peak_blocked))
                    WRITE_ONCE(peak_blocked, b);
            }
        }

        if (!READ_ONCE(g_monitor_on))
            break;

        cpu_relax();
        now_ns = ktime_get_ns();
    }

    if (local_start) {
        atomic_dec(&blocked_now);
        update_peak_delay(comm, euid, now_ns - local_start);
    }
}

/*
 * MODE 1: backoff progressivo
 * - calcola deadline della finestra una volta
 * - riduce i ktime_get_ns (polling raro con step crescente)
 */
static void throttle_backoff(const char *comm, u32 euid, u64 now_ns)
{
    u64 local_start = 0;
    bool counted = false;
    u32 local_max;
    u64 deadline_ns = 0;

    if (!READ_ONCE(g_monitor_on))
        return;

    local_max = READ_ONCE(g_max_per_sec);
    if (local_max == 0)
        local_max = 1;

    for (;;) {
        unsigned long flags;

        spin_lock_irqsave(&cfg_lock, flags);

        if (win_start_ns == 0) {
            win_start_ns = now_ns;
            win_count = 0;
        }

        if (now_ns - win_start_ns >= NSEC_PER_SEC) {
            sample_window_stats_locked();
            win_start_ns = now_ns;
            win_count = 0;
        }

        if (win_count < local_max) {
            win_count++;
            counted = true;
        } else {
            if (!deadline_ns)
                deadline_ns = win_start_ns + NSEC_PER_SEC;
        }

        spin_unlock_irqrestore(&cfg_lock, flags);

        if (counted)
            break;

        if (local_start == 0) {
            local_start = now_ns;
            atomic_inc(&blocked_now);
        }

        if (!READ_ONCE(g_monitor_on))
            break;

        /* backoff: polling raro */
        {
            unsigned int step = 64;
            while (1) {
                unsigned int i;
                for (i = 0; i < step; i++)
                    cpu_relax();

                now_ns = ktime_get_ns();
                if (now_ns >= deadline_ns)
                    break;

                if (step < 8192)
                    step <<= 1;
            }
        }

        /* resetta deadline: al prossimo giro verrà ricalcolata sotto lock */
        deadline_ns = 0;
    }

    if (local_start) {
        atomic_dec(&blocked_now);
        update_peak_delay(comm, euid, now_ns - local_start);
    }
}

/*
 * MODE 2: atomic tokens
 * - fast path senza lock: atomic_dec_if_positive(&g_tokens)
 * - lock solo quando cambia secondo (rollover)
 * - attesa con backoff fino al prossimo secondo
 */
static void throttle_atomic_tokens(const char *comm, u32 euid, u64 now_ns)
{
    u64 local_start = 0;
    u32 local_max;
    u64 now_sec;
    u64 deadline_ns;

    if (!READ_ONCE(g_monitor_on))
        return;

    local_max = READ_ONCE(g_max_per_sec);
    if (local_max == 0)
        local_max = 1;

    now_sec = now_ns / NSEC_PER_SEC;

    /* rollover: se cambia secondo, reset token */
    if (READ_ONCE(g_epoch_sec) != now_sec) {
        unsigned long flags;
        spin_lock_irqsave(&cfg_lock, flags);
        if (g_epoch_sec != now_sec) {
            sample_window_stats_locked();
            g_epoch_sec = now_sec;
            atomic_set(&g_tokens, (int)local_max);
        }
        spin_unlock_irqrestore(&cfg_lock, flags);
    }

    /* Fast path: consuma token */
    if (atomic_dec_if_positive(&g_tokens) >= 0)
        return;

    /* oltre MAX: aspetta fino al prossimo secondo */
    local_start = now_ns;
    atomic_inc(&blocked_now);

    deadline_ns = (now_sec + 1) * NSEC_PER_SEC;

    while (READ_ONCE(g_monitor_on)) {
        unsigned int step = 64;

        while (1) {
            unsigned int i;
            for (i = 0; i < step; i++)
                cpu_relax();

            now_ns = ktime_get_ns();
            if (now_ns >= deadline_ns)
                break;

            if (step < 8192)
                step <<= 1;
        }

        now_sec = now_ns / NSEC_PER_SEC;

        /* rollover */
        if (READ_ONCE(g_epoch_sec) != now_sec) {
            unsigned long flags;
            spin_lock_irqsave(&cfg_lock, flags);
            if (g_epoch_sec != now_sec) {
                sample_window_stats_locked();
                g_epoch_sec = now_sec;
                atomic_set(&g_tokens, (int)local_max);
            }
            spin_unlock_irqrestore(&cfg_lock, flags);
        }

        if (atomic_dec_if_positive(&g_tokens) >= 0)
            break;

        deadline_ns = (now_sec + 1) * NSEC_PER_SEC;
    }

    atomic_dec(&blocked_now);
    update_peak_delay(comm, euid, now_ns - local_start);
}

/* Wrapper: sceglie modalità */
static void throttle_if_needed(const char *comm, u32 euid, u64 now_ns)
{
    u32 mode = READ_ONCE(g_mode);

    if (mode == 2)
        throttle_atomic_tokens(comm, euid, now_ns);
    else if (mode == 1)
        throttle_backoff(comm, euid, now_ns);
    else
        throttle_baseline(comm, euid, now_ns);
}

/* -----------------------------
 * kprobe: hook su do_syscall_64 (x86-64)
 * ----------------------------- */

static struct kprobe kp;

static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
    struct pt_regs *sys_regs;
    u32 nr = 0;
    u32 euid;
    char comm[SCTH_MAX_PROG_LEN];
    bool match = false;
    u64 now_ns;

    /*
     * Per ABI x86_64:
     *  arg1 -> RDI
     *  arg2 -> RSI
     *
     * In molte build, x64_sys_call riceve (struct pt_regs *regs, unsigned int nr),
     * quindi il numero syscall è spesso in RSI. In alternativa lo recuperiamo da orig_ax
     * del pt_regs passato in RDI (fallback) ma solo se il puntatore è nello stack corrente.
     */

    /* Tentativo 1: nr come secondo argomento */
    nr = (u32)regs->si;

    /* Tentativo 2 (fallback): pt_regs* in RDI e nr = orig_ax */
    sys_regs = (struct pt_regs *)regs->di;
    if (sys_regs) {
        unsigned long sp = (unsigned long)sys_regs;
        unsigned long base = (unsigned long)task_stack_page(current);

        /* Dereferenzia sys_regs solo se sta nello stack del task corrente */
        if (sp >= base && sp < base + THREAD_SIZE) {
            u32 ax = (u32)sys_regs->orig_ax;

            /* Se nr da RSI non è plausibile, usa orig_ax */
            if (nr > 1024)  /* soglia “sanity” (x86_64 syscall <= ~600) */
                nr = ax;
        }
    }

    /* Se ancora non plausibile, esci presto */
    if (nr > 4096)
        return 0;

    euid = (u32)from_kuid(&init_user_ns, current_euid());
    get_task_comm(comm, current);

    spin_lock(&cfg_lock);
    if (sys_is_registered(nr) && (prog_is_registered(comm) || uid_is_registered(euid)))
        match = true;
    spin_unlock(&cfg_lock);

    if (!match)
        return 0;

    now_ns = ktime_get_ns();
    throttle_if_needed(comm, euid, now_ns);
#else
    (void)p; (void)regs;
#endif
    return 0;
}


/* -----------------------------
 * ioctl + device
 * ----------------------------- */

static long scth_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
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
    case SCTH_IOC_SET_MODE:
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
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { ret = -EFAULT; break; }
        req.name[SCTH_MAX_PROG_LEN-1] = '\0';
        if (prog_is_registered(req.name)) break;
        e = kzalloc(sizeof(*e), GFP_ATOMIC);
        if (!e) { ret = -ENOMEM; break; }
        strncpy(e->name, req.name, SCTH_MAX_PROG_LEN);
        e->h = hash_comm(e->name);
        hash_add(prog_ht, &e->node, e->h);
        break;
    }
    case SCTH_IOC_DEL_PROG: {
        struct scth_prog_req req;
        struct prog_ent *e;
        u32 h;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { ret = -EFAULT; break; }
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
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { ret = -EFAULT; break; }
        if (uid_is_registered(req.euid)) break;
        e = kzalloc(sizeof(*e), GFP_ATOMIC);
        if (!e) { ret = -ENOMEM; break; }
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
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { ret = -EFAULT; break; }
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
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { ret = -EFAULT; break; }
        if (sys_is_registered(req.nr)) break;
        e = kzalloc(sizeof(*e), GFP_ATOMIC);
        if (!e) { ret = -ENOMEM; break; }
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
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { ret = -EFAULT; break; }
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
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { ret = -EFAULT; break; }
        g_max_per_sec = (req.max_per_sec == 0 ? 1 : req.max_per_sec);
        break;
    }
    case SCTH_IOC_MON_ON:
        g_monitor_on = true;
        break;
    case SCTH_IOC_MON_OFF:
        g_monitor_on = false;
        break;

    case SCTH_IOC_LIST_PROG:
    case SCTH_IOC_LIST_UID:
    case SCTH_IOC_LIST_SYSCALL: {
        struct scth_list_resp lr;
        u32 cap, out = 0;

        if (copy_from_user(&lr, (void __user *)arg, sizeof(lr))) { ret = -EFAULT; break; }
        cap = lr.count;
        if (cap > SCTH_MAX_LIST) cap = SCTH_MAX_LIST;
        if (lr.ptr == 0) { ret = -EINVAL; break; }

        if (cmd == SCTH_IOC_LIST_PROG) {
            struct prog_ent *e;
            int b;
            char __user *ubuf = (char __user *)(uintptr_t)lr.ptr;
            hash_for_each(prog_ht, b, e, node) {
                if (out >= cap) break;
                if (copy_to_user(ubuf + out*SCTH_MAX_PROG_LEN, e->name, SCTH_MAX_PROG_LEN)) { ret = -EFAULT; break; }
                out++;
            }
        } else if (cmd == SCTH_IOC_LIST_UID) {
            struct uid_ent *e;
            int b;
            u32 __user *ubuf = (u32 __user *)(uintptr_t)lr.ptr;
            hash_for_each(uid_ht, b, e, node) {
                if (out >= cap) break;
                if (put_user(e->euid, &ubuf[out])) { ret = -EFAULT; break; }
                out++;
            }
        } else {
            struct sys_ent *e;
            int b;
            u32 __user *ubuf = (u32 __user *)(uintptr_t)lr.ptr;
            hash_for_each(sys_ht, b, e, node) {
                if (out >= cap) break;
                if (put_user(e->nr, &ubuf[out])) { ret = -EFAULT; break; }
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
        st.max_per_sec = g_max_per_sec;
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

        /* reset finestra */
        win_start_ns = 0;
        win_count = 0;
        atomic_set(&blocked_now, 0);

        /* reset token mode */
        atomic_set(&g_tokens, 0);
        g_epoch_sec = 0;
        break;
    }

    case SCTH_IOC_SET_MODE: {
        struct scth_mode_req req;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) { ret = -EFAULT; break; }
        if (req.mode > 2) { ret = -EINVAL; break; }
        g_mode = req.mode;
        /* reset token state quando cambio mode */
        atomic_set(&g_tokens, 0);
        g_epoch_sec = 0;
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

static struct miscdevice scth_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "scth",
    .fops  = &scth_fops,
    .mode  = 0666, /* aperto a tutti; ma update root-only come richiesto :contentReference[oaicite:7]{index=7} */
};

/* cleanup helpers */
static void free_hashtable_prog(void)
{
    struct prog_ent *e;
    struct hlist_node *tmp;
    int b;
    hash_for_each_safe(prog_ht, b, tmp, e, node) { hash_del(&e->node); kfree(e); }
}

static void free_hashtable_uid(void)
{
    struct uid_ent *e;
    struct hlist_node *tmp;
    int b;
    hash_for_each_safe(uid_ht, b, tmp, e, node) { hash_del(&e->node); kfree(e); }
}

static void free_hashtable_sys(void)
{
    struct sys_ent *e;
    struct hlist_node *tmp;
    int b;
    hash_for_each_safe(sys_ht, b, tmp, e, node) { hash_del(&e->node); kfree(e); }
}

static int __init scth_init(void)
{
    int ret;

    hash_init(prog_ht);
    hash_init(uid_ht);
    hash_init(sys_ht);

    ret = misc_register(&scth_dev);
    if (ret) {
        pr_err("scth: misc_register failed: %d\n", ret);
        return ret;
    }

    /* kprobe */
    memset(&kp, 0, sizeof(kp));
    kp.symbol_name = "x64_sys_call";
    kp.pre_handler = kp_pre_handler;

    ret = register_kprobe(&kp);
    if (ret) {
        pr_err("scth: register_kprobe failed: %d\n", ret);
        misc_deregister(&scth_dev);
        return ret;
    }

    pr_info("scth: loaded (/dev/%s). monitor_off, MAX=%u\n", scth_dev.name, g_max_per_sec);
    return 0;
}

static void __exit scth_exit(void)
{
    unregister_kprobe(&kp);
    misc_deregister(&scth_dev);

    free_hashtable_prog();
    free_hashtable_uid();
    free_hashtable_sys();

    pr_info("scth: unloaded\n");
}

module_init(scth_init);
module_exit(scth_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alessandro Cortese");
MODULE_DESCRIPTION("LKM - Syscall Throttling System");