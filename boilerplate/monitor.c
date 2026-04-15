/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Creates /dev/container_monitor.
 * Accepts PIDs from engine.c via ioctl (MONITOR_REGISTER / MONITOR_UNREGISTER).
 * Periodically checks RSS; emits soft-limit warning once and kills on hard limit.
 *
 * Synchronization choice: spinlock (with irqsave)
 *   The timer callback fires in softirq context on non-PREEMPT kernels.
 *   mutex_lock() may sleep and is illegal in softirq context -- it causes a
 *   "BUG: sleeping function called from invalid context" kernel panic.
 *   A spinlock acquired with spin_lock_irqsave() is safe from both process
 *   context (ioctl) and softirq context (timer).
 *
 *   Consequence: get_rss_bytes() calls get_task_mm()/mmput() which may sleep,
 *   so it must NOT be called while holding the spinlock.  The timer callback
 *   uses a three-step snapshot approach: snapshot under lock -> RSS reads
 *   outside lock -> apply decisions under lock.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ------------------------------------------------------------------ */
/*  Linked-list node                                                    */
/* ------------------------------------------------------------------ */
struct monitored_entry {
    pid_t         pid;
    char          container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int           soft_warned;
    struct list_head list;
};

/* ------------------------------------------------------------------ */
/*  Global list + spinlock                                              */
/* ------------------------------------------------------------------ */
static LIST_HEAD(monitored_list);
static DEFINE_SPINLOCK(monitored_lock);

/* ------------------------------------------------------------------ */
/*  Internal device / timer state                                       */
/* ------------------------------------------------------------------ */
static struct timer_list monitor_timer;
static dev_t              dev_num;
static struct cdev        c_dev;
static struct class      *cl;

/* ------------------------------------------------------------------ */
/*  RSS helper - MUST NOT be called under a spinlock                    */
/* ------------------------------------------------------------------ */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    struct pid         *pid_struct;
    long rss_pages = 0;

    rcu_read_lock();
    pid_struct = find_get_pid(pid);
    if (!pid_struct) {
        rcu_read_unlock();
        return -1;
    }
    task = pid_task(pid_struct, PIDTYPE_PID);
    if (!task) {
        put_pid(pid_struct);
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    put_pid(pid_struct);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ------------------------------------------------------------------ */
/*  Soft-limit warning                                                  */
/* ------------------------------------------------------------------ */
static void log_soft_limit_event(const char *container_id, pid_t pid,
                                 unsigned long limit_bytes, long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d "
           "rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ------------------------------------------------------------------ */
/*  Hard-limit kill                                                     */
/* ------------------------------------------------------------------ */
static void kill_process(const char *container_id, pid_t pid,
                         unsigned long limit_bytes, long rss_bytes)
{
    struct task_struct *task;
    struct pid         *pid_struct;

    rcu_read_lock();
    pid_struct = find_get_pid(pid);
    if (pid_struct) {
        task = pid_task(pid_struct, PIDTYPE_PID);
        if (task)
            send_sig(SIGKILL, task, 1);
        put_pid(pid_struct);
    }
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d "
           "rss=%ld limit=%lu -- process killed\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ------------------------------------------------------------------ */
/*  Snapshot buffer - heap allocated to avoid large stack frames        */
/*  MAX_TRACKED=16 keeps the struct well under kernel frame limits      */
/* ------------------------------------------------------------------ */
#define MAX_TRACKED   16
#define ACTION_NONE       0
#define ACTION_DEAD       1
#define ACTION_HARD_KILL  2
#define ACTION_SOFT_WARN  4

struct snap_t {
    struct monitored_entry *ptr;
    pid_t                   pid;
    unsigned long           soft_limit_bytes;
    unsigned long           hard_limit_bytes;
    int                     soft_warned;
    char                    container_id[MONITOR_NAME_LEN];
};

/* Global heap snapshot buffer - allocated in monitor_init */
static struct snap_t *g_snap;

/* ------------------------------------------------------------------ */
/*  Timer callback                                                      */
/* ------------------------------------------------------------------ */
static void timer_callback(struct timer_list *t)
{
    int                     snap_count = 0;
    int                     i;
    unsigned long           flags;
    struct monitored_entry *entry, *tmp;

    if (!g_snap)
        goto rearm;

    /* Step 1: snapshot under spinlock */
    spin_lock_irqsave(&monitored_lock, flags);
    list_for_each_entry(entry, &monitored_list, list) {
        if (snap_count >= MAX_TRACKED)
            break;
        g_snap[snap_count].ptr              = entry;
        g_snap[snap_count].pid              = entry->pid;
        g_snap[snap_count].soft_limit_bytes = entry->soft_limit_bytes;
        g_snap[snap_count].hard_limit_bytes = entry->hard_limit_bytes;
        g_snap[snap_count].soft_warned      = entry->soft_warned;
        memcpy(g_snap[snap_count].container_id, entry->container_id, MONITOR_NAME_LEN);
        snap_count++;
    }
    spin_unlock_irqrestore(&monitored_lock, flags);

    /* Step 2: RSS reads outside spinlock */
    for (i = 0; i < snap_count; i++) {
        long      rss    = get_rss_bytes(g_snap[i].pid);
        uintptr_t raw    = (uintptr_t)g_snap[i].ptr;
        int       action = ACTION_NONE;

        if (rss < 0) {
            printk(KERN_INFO "[container_monitor] PID %d (container=%s) exited, removing.\n",
                   g_snap[i].pid, g_snap[i].container_id);
            action = ACTION_DEAD;
        } else if ((unsigned long)rss >= g_snap[i].hard_limit_bytes) {
            kill_process(g_snap[i].container_id, g_snap[i].pid,
                         g_snap[i].hard_limit_bytes, rss);
            action = ACTION_HARD_KILL;
        } else if ((unsigned long)rss >= g_snap[i].soft_limit_bytes && !g_snap[i].soft_warned) {
            log_soft_limit_event(g_snap[i].container_id, g_snap[i].pid,
                                 g_snap[i].soft_limit_bytes, rss);
            action = ACTION_SOFT_WARN;
        }

        g_snap[i].ptr = (struct monitored_entry *)(raw | (uintptr_t)action);
    }

    /* Step 3: apply decisions under spinlock */
    spin_lock_irqsave(&monitored_lock, flags);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        for (i = 0; i < snap_count; i++) {
            uintptr_t raw    = (uintptr_t)g_snap[i].ptr;
            int       action = (int)(raw & 7UL);
            struct monitored_entry *orig =
                (struct monitored_entry *)(raw & ~(uintptr_t)7UL);

            if (orig != entry)
                continue;

            if (action & (ACTION_DEAD | ACTION_HARD_KILL)) {
                list_del(&entry->list);
                kfree(entry);
            } else if (action & ACTION_SOFT_WARN) {
                entry->soft_warned = 1;
            }
            break;
        }
    }
    spin_unlock_irqrestore(&monitored_lock, flags);

rearm:
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ------------------------------------------------------------------ */
/*  ioctl handler                                                       */
/* ------------------------------------------------------------------ */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    req.container_id[MONITOR_NAME_LEN - 1] = '\0';

    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *entry;
        unsigned long flags;

        printk(KERN_INFO "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_WARNING "[container_monitor] Register rejected: soft > hard (container=%s pid=%d)\n",
                   req.container_id, req.pid);
            return -EINVAL;
        }

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid              = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned      = 0;
        strncpy(entry->container_id, req.container_id, sizeof(entry->container_id) - 1);
        entry->container_id[sizeof(entry->container_id) - 1] = '\0';
        INIT_LIST_HEAD(&entry->list);

        spin_lock_irqsave(&monitored_lock, flags);
        list_add_tail(&entry->list, &monitored_list);
        spin_unlock_irqrestore(&monitored_lock, flags);

        return 0;
    }

    /* UNREGISTER */
    {
        struct monitored_entry *entry, *tmp;
        struct monitored_entry *found_entry = NULL;
        unsigned long flags;

        printk(KERN_INFO "[container_monitor] Unregister request container=%s pid=%d\n",
               req.container_id, req.pid);

        spin_lock_irqsave(&monitored_lock, flags);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid &&
                strncmp(entry->container_id, req.container_id, MONITOR_NAME_LEN) == 0) {
                list_del(&entry->list);
                found_entry = entry;
                break;
            }
        }
        spin_unlock_irqrestore(&monitored_lock, flags);

        if (found_entry) {
            kfree(found_entry);
            printk(KERN_INFO "[container_monitor] Unregistered container=%s pid=%d\n",
                   req.container_id, req.pid);
            return 0;
        }
    }

    return -ENOENT;
}

/* ------------------------------------------------------------------ */
/*  File operations                                                     */
/* ------------------------------------------------------------------ */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ------------------------------------------------------------------ */
/*  Module init                                                         */
/* ------------------------------------------------------------------ */
static int __init monitor_init(void)
{
    /* Heap-allocate snapshot buffer to avoid large stack frame in timer */
    g_snap = kmalloc_array(MAX_TRACKED, sizeof(struct snap_t), GFP_KERNEL);
    if (!g_snap)
        return -ENOMEM;

    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) {
        kfree(g_snap);
        return -1;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        kfree(g_snap);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        kfree(g_snap);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        kfree(g_snap);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Module exit                                                         */
/* ------------------------------------------------------------------ */
static void __exit monitor_exit(void)
{
    /*
     * timer_shutdown_sync() replaces del_timer_sync() in kernel >= 6.11.
     * Falls back to del_timer_sync() for older kernels.
     */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
    timer_shutdown_sync(&monitor_timer);
#else
    del_timer_sync(&monitor_timer);
#endif

    kfree(g_snap);
    g_snap = NULL;

    {
        struct monitored_entry *entry, *tmp;
        unsigned long flags;

        spin_lock_irqsave(&monitored_lock, flags);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            list_del(&entry->list);
            kfree(entry);
        }
        spin_unlock_irqrestore(&monitored_lock, flags);
    }

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
MODULE_AUTHOR("OS-Jackfruit");