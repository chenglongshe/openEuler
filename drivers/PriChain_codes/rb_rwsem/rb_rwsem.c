#include <linux/sched.h>
#include <linux/rbtree.h>
#include <linux/ktime.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include "rb_rwsem.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("华中科技大学OS实验室");
MODULE_DESCRIPTION("RB-Tree Resource-Limited RWSem with Priority Inheritance");

/**
 * 等待者结构
 * 用于资源耗尽时的优先级排序
 */
struct rwsem_waiter {
    struct task_struct *task; // 等待任务
    int prio;                // 任务优先级
    ktime_t enter_time;      // 进入等待队列时间（纳秒级记录）
    struct rb_node node;     // 红黑树节点
};

// 创建sysfs接口
static int __init rb_rwsem_sysfs_init(void) {
    proc_create("rb_rwsem_control", 0644, NULL, &rb_rwsem_control_fops);
    return 0;
}

void rb_rwsem_init(struct rb_rwsem *sem, int init_count) {
    atomic_set(&sem->count, init_count);
    atomic_set(&sem->max_count, init_count);
    rb_mutex_init(&sem->lock);
    rb_mutex_init(&sem->inherit_lock);
    sem->wait_tree = RB_ROOT;
}

/**
 * 动态调整资源配额
 */
void rb_rwsem_set_max(struct rb_rwsem *sem, int new_max) {
    int old_count;
    
    rb_mutex_lock(&sem->lock);
    old_count = atomic_read(&sem->max_count);
    atomic_set(&sem->max_count, new_max);
    
    pr_alert("[rb_rwsem] Resource quota changed %d→%d\n", 
             old_count, new_max);
    
    // 如果有更多资源可用，唤醒等待者
    if (new_max > old_count) {
        int free_slots = new_max - old_count;
        wake_up_waiters(sem, free_slots);
    }
    rb_mutex_unlock(&sem->lock);
}

/**
 * 获取读信号量（支持优先级继承）
 */
void rb_rwsem_down_read(struct rb_rwsem *sem) {
    ktime_t start = ktime_get();
    struct rwsem_waiter waiter;
    
    rb_mutex_lock(&sem->lock);
    
    // 资源可用快速路径
    if (atomic_read(&sem->count) > 0) {
        atomic_dec(&sem->count);
        rb_mutex_unlock(&sem->lock);
        return;
    }
    
    // 资源耗尽，进入等待队列
    init_waiter(&waiter, current);
    insert_waiter(sem, &waiter); // 按优先级插入红黑树
    
    // 触发分层优先级继承
    if (rb_first(&sem->wait_tree) == &waiter.node) {
        trigger_priority_inheritance(sem);
    }
    
    rb_mutex_unlock(&sem->lock);
    
    // 阻塞等待（带有优先级继承的唤醒机制）
    wait_for_resource(sem, &waiter);
    
    // 记录时延
    ktime_t duration = ktime_sub(ktime_get(), start);
    pr_alert("[rb_rwsem] %s waited %lld ns (res:%d/%d)",
             current->comm, ktime_to_ns(duration),
             atomic_read(&sem->count), atomic_read(&sem->max_count));
}

/**
 * 分层优先级继承机制
 */
static void trigger_priority_inheritance(struct rb_rwsem *sem) {
    struct rb_node *node = rb_first(&sem->wait_tree);
    if (!node) return;
    
    struct rwsem_waiter *waiter = rb_entry(node, struct rwsem_waiter, node);
    int top_prio = waiter->prio;
    
    rb_mutex_lock(&sem->inherit_lock);
    
    // 提升所有资源持有者的优先级（穿透继承）
    struct task_struct *holder;
    for_each_holder(holder, sem) {
        if (holder->prio > top_prio) {
            // 保存原始优先级
            struct rb_task_info *info = get_task_info(holder);
            if (info && info->original_prio == -1) {
                info->original_prio = holder->prio;
            }
            
            // 提升优先级
            holder->prio = top_prio;
            pr_alert("[rb_rwsem] %s inherited prio %d", 
                     holder->comm, top_prio);
        }
    }
    
    rb_mutex_unlock(&sem->inherit_lock);
}

/**
 * 资源释放后唤醒高优等待者
 */
static void wake_up_waiters(struct rb_rwsem *sem, int count) {
    struct rb_node *node;
    
    // 按优先级顺序唤醒（最高优先）
    while (count-- && (node = rb_first(&sem->wait_tree))) {
        struct rwsem_waiter *waiter = rb_entry(node, struct rwsem_waiter, node);
        rb_erase(node, &sem->wait_tree);
        wake_up_process(waiter->task);
        kfree(waiter);
        atomic_dec(&sem->count);
    }
}

/**
 * 释放读信号量（带配额恢复机制）
 */
void rb_rwsem_up_read(struct rb_rwsem *sem) {
    rb_mutex_lock(&sem->lock);
    atomic_inc(&sem->count);
    
    // 资源释放后唤醒等待者
    if (!rb_empty(&sem->wait_tree)) {
        wake_up_waiters(sem, 1);
    }
    
    // 恢复继承前的优先级
    if (should_restore_priority(current)) {
        restore_original_priority(current);
    }
    
    rb_mutex_unlock(&sem->lock);
}

module_init(rb_rwsem_sysfs_init);
EXPORT_SYMBOL(rb_rwsem_init);
EXPORT_SYMBOL(rb_rwsem_down_read);
EXPORT_SYMBOL(rb_rwsem_up_read);
