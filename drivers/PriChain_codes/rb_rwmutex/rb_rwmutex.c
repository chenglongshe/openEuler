#include "../include/linux/rb_rwmutex.h"
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/ktime.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ldfnewbie + ChatGPT");
MODULE_DESCRIPTION("RB-Tree Read/Write Mutex with Priority Inheritance");

void rb_rwmutex_init(struct rb_rwmutex *rw) {
    rb_mutex_init(&rw->lock);
    rb_mutex_init(&rw->wr_lock);
    rw->readers = 0;
}

void rb_rwmutex_read_lock(struct rb_rwmutex *rw) {
    rb_mutex_lock(&rw->lock);
    if (rw->readers == 0) {
        pr_alert("[rb_rwmutex] %s (pid=%d) first reader, acquiring write lock for readers\n", current->comm, current->pid);
        rb_mutex_lock(&rw->wr_lock);
        pr_alert("[rb_rwmutex] %s (pid=%d) acquired write lock for readers\n", current->comm, current->pid);
    }
    rw->readers++;
    pr_alert("[rb_rwmutex] %s (pid=%d) now holds read lock, readers=%d, prio=%d, normal_prio=%d\n", current->comm, current->pid, rw->readers, current->prio, current->normal_prio);
    rb_mutex_unlock(&rw->lock);
}

void rb_rwmutex_read_unlock(struct rb_rwmutex *rw) {
    rb_mutex_lock(&rw->lock);
    rw->readers--;
    pr_alert("[rb_rwmutex] %s (pid=%d) released read lock, readers=%d, prio=%d, normal_prio=%d\n", current->comm, current->pid, rw->readers, current->prio, current->normal_prio);
    if (rw->readers == 0) {
        pr_alert("[rb_rwmutex] %s (pid=%d) last reader, releasing write lock for readers\n", current->comm, current->pid);
        rb_mutex_unlock(&rw->wr_lock);
    }
    rb_mutex_unlock(&rw->lock);
}

void rb_rwmutex_write_lock(struct rb_rwmutex *rw) {
    u64 start_ns = ktime_get_ns();
    pr_alert("[rb_rwmutex] %s (pid=%d) attempting to acquire write lock at time: %llu ns, prio=%d, normal_prio=%d\n",
             current->comm, current->pid, start_ns, current->prio, current->normal_prio);
    rb_mutex_lock(&rw->wr_lock);
    u64 acquire_ns = ktime_get_ns();
    pr_alert("[rb_rwmutex] %s (pid=%d) acquired write lock at time: %llu ns, prio=%d, normal_prio=%d\n",
             current->comm, current->pid, acquire_ns, current->prio, current->normal_prio);
    pr_alert("###\n");
    pr_alert("[rb_rwmutex] Block duration: %llu ns (%llu us)\n",
             acquire_ns - start_ns, (acquire_ns - start_ns) / 1000);
    pr_alert("###\n");
}

void rb_rwmutex_write_unlock(struct rb_rwmutex *rw) {
    pr_alert("[rb_rwmutex] %s (pid=%d) releasing write lock, prio=%d, normal_prio=%d\n", current->comm, current->pid, current->prio, current->normal_prio);
    rb_mutex_unlock(&rw->wr_lock);
}

EXPORT_SYMBOL(rb_rwmutex_init);
EXPORT_SYMBOL(rb_rwmutex_read_lock);
EXPORT_SYMBOL(rb_rwmutex_read_unlock);
EXPORT_SYMBOL(rb_rwmutex_write_lock);
EXPORT_SYMBOL(rb_rwmutex_write_unlock);