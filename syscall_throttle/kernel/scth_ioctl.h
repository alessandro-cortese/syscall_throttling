#ifndef _SCTH_IOCTL_H_
#define _SCTH_IOCTL_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define SCTH_IOC_MAGIC  'T'

/* Limiti semplici per evitare allocazioni complesse */
#define SCTH_MAX_PROG_LEN 16   /* TASK_COMM_LEN */
#define SCTH_MAX_LIST     256  /* max elementi restituiti per lista */

/* payload per ADD/DEL program */
struct scth_prog_req {
    char name[SCTH_MAX_PROG_LEN]; /* NUL-terminated o tagliato */
};

/* payload per ADD/DEL uid */
struct scth_uid_req {
    __u32 euid;
};

/* payload per ADD/DEL syscall */
struct scth_sys_req {
    __u32 nr; /* syscall number x86-64 */
};

/* SET MAX */
struct scth_max_req {
    __u32 max_per_sec;
};

/* LIST response, stesso layout per prog/uid/syscall */
struct scth_list_resp {
    __u32 count;               /* in/out: user passa capienza, kernel scrive count reale */
    __u32 reserved;
    __u64 ptr;                 /* user pointer to array buffer */
};

/* stats */
struct scth_stats {
    __u64 peak_delay_ns;
    char  peak_prog[SCTH_MAX_PROG_LEN];
    __u32 peak_euid;

    __u32 peak_blocked_threads;
    __u64 avg_blocked_threads_x1000; /* media * 1000 */

    __u32 max_current_per_sec;  /* NEW: valore applicato nell'epoca corrente */
    __u32 max_next_per_sec;     /* NEW: ultimo valore impostato via ioctl, vale dalla prossima epoca */
    __u32 monitor_on;
};

struct scth_mode_req {
    __u32 mode;  /* 0=baseline, 1=backoff, 2=atomic_tokens */
};

#define SCTH_IOC_ADD_PROG      _IOW(SCTH_IOC_MAGIC,  1, struct scth_prog_req)
#define SCTH_IOC_DEL_PROG      _IOW(SCTH_IOC_MAGIC,  2, struct scth_prog_req)
#define SCTH_IOC_ADD_UID       _IOW(SCTH_IOC_MAGIC,  3, struct scth_uid_req)
#define SCTH_IOC_DEL_UID       _IOW(SCTH_IOC_MAGIC,  4, struct scth_uid_req)
#define SCTH_IOC_ADD_SYSCALL   _IOW(SCTH_IOC_MAGIC,  5, struct scth_sys_req)
#define SCTH_IOC_DEL_SYSCALL   _IOW(SCTH_IOC_MAGIC,  6, struct scth_sys_req)

#define SCTH_IOC_SET_MAX       _IOW(SCTH_IOC_MAGIC,  7, struct scth_max_req)
#define SCTH_IOC_MON_ON        _IO(SCTH_IOC_MAGIC,   8)
#define SCTH_IOC_MON_OFF       _IO(SCTH_IOC_MAGIC,   9)

#define SCTH_IOC_LIST_PROG     _IOWR(SCTH_IOC_MAGIC, 10, struct scth_list_resp)
#define SCTH_IOC_LIST_UID      _IOWR(SCTH_IOC_MAGIC, 11, struct scth_list_resp)
#define SCTH_IOC_LIST_SYSCALL  _IOWR(SCTH_IOC_MAGIC, 12, struct scth_list_resp)

#define SCTH_IOC_GET_STATS     _IOR(SCTH_IOC_MAGIC,  13, struct scth_stats)

#define SCTH_IOC_RESET_STATS  _IO(SCTH_IOC_MAGIC, 14)

#define SCTH_IOC_SET_MODE     _IOW(SCTH_IOC_MAGIC, 15, struct scth_mode_req)

#endif