#include <linux/sched.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/sched/signal.h>
#include <../include/linux/rb_mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ldfnewbie + ChatGPT");
MODULE_DESCRIPTION("RB-Tree Mutex System with Dynamic Instances, Priority Inheritance, and Exported Interface");

#define RB_TASK_TABLE_BITS 6
static DEFINE_HASHTABLE(rb_task_table, RB_TASK_TABLE_BITS);
static DEFINE_SPINLOCK(rb_task_table_lock);

///创新点:死锁统计
static atomic_t rb_mutex_deadlock_count = ATOMIC_INIT(0);

///已知锁，查找是否存在某个任务持有该锁节点 返回rb_held_lock结构体指针
/* 
 * root->某个任务的 held_locks 红黑树 ; mutex->某个任务持有的锁
 * 指针比较大小，通过锁对象的地址
 */
struct rb_held_lock *find_held_lock(struct rb_root *root, struct rb_mutex *mutex) {
    struct rb_node *node = root->rb_node;
    while (node) {
        struct rb_held_lock *hl = rb_entry(node, struct rb_held_lock, node);
        if (mutex < hl->mutex)
            node = node->rb_left;
        else if (mutex > hl->mutex)
            node = node->rb_right;
        else
            return hl;
    }
    return NULL;
}

///在某个任务的 held_locks 红黑树中插入一把新持有的锁
/*
 * info->指向该任务的锁信息结构体，mutex->要插入的锁对象，top_prio->该锁的最高等待者优先级
 * 采用二级指针：插入节点时需要找到“父节点的左/右子树指针”，所以为了修改指针而使用，new是rb_node的二级指针
 * kmalloc分配新的rb_held_lock结构体,GFP_ATOMIC保证原子性
 * 填充新节点内容->mutex->指向锁对象，top_prio->该锁的最高等待者优先级
 * 查找插入位置
 * rb_link_node：把新节点挂到父节点的左/右子树 ; rb_insert_color：调整红黑树颜色，保证树的平衡性
 */
void insert_held_lock(struct rb_task_info *info, struct rb_mutex *mutex, int top_prio) {
    struct rb_node **new = &info->held_locks.rb_node, *parent = NULL;
    struct rb_held_lock *hl;

    hl = kmalloc(sizeof(*hl), GFP_ATOMIC);
    if (!hl) return;

    hl->mutex = mutex;
    hl->top_waiter_prio = top_prio;

    while (*new) {
        struct rb_held_lock *entry = rb_entry(*new, struct rb_held_lock, node);
        parent = *new;
        if (mutex < entry->mutex)
            new = &(*new)->rb_left;
        else
            new = &(*new)->rb_right;
    }
    rb_link_node(&hl->node, parent, new);
    rb_insert_color(&hl->node, &info->held_locks);
}

///从某个任务的 held_locks 红黑树中移除
/*
 * rb_erase 是内核红黑树的删除操作，把该节点从红黑树中移除
 * kfree释放该节点占用的内存，防止内存泄漏
 */
void remove_held_lock(struct rb_task_info *info, struct rb_mutex *mutex) {
    struct rb_held_lock *hl = find_held_lock(&info->held_locks, mutex);
    if (!hl) return;
    rb_erase(&hl->node, &info->held_locks);
    kfree(hl);
}

///计算某个任务持有的所有锁中，等待者的最高优先级
/*
 * rb_first获取最左节点 ; rb_next获取下一个节点,可以从任意节点开始，依次遍历整棵红黑树
 * 找最左子节点作为遍历起点，采用中序遍历，可以从最小节点配合rb_next遍历整棵树 这只是通用方法，即任务的锁红黑树排序没意义，以任何节点为起点都无关紧要
 * struct rb_held_lock hl = rb_entry(node, struct rb_held_lock, node);这样操作结构体本身会把结构体内容拷贝一份到栈上，作为副本hl修改不会影响真实节点
 */
 int get_effective_inherited_prio(struct rb_task_info *info) {
    struct rb_node *node = rb_first(&info->held_locks);
    int max_prio = INT_MAX;
    while (node) {
        struct rb_held_lock *hl = rb_entry(node, struct rb_held_lock, node);
        if (hl->top_waiter_prio < max_prio)
            max_prio = hl->top_waiter_prio;
        node = rb_next(node);
    }
    return (max_prio == INT_MAX) ? -1 : max_prio;
}

///每次锁的等待队列有变化，就把最新的‘最着急等这把锁的人’的优先级，记到锁主人的账上
/*
 * rb_mutex_top_waiter(mutex) 返回等待队列中优先级最高的等待者（即红黑树最左节点）
 * 检查锁是否有 owner
 * 获取 owner 的任务信息:通过 owner 任务指针，查找其 rb_task_info 结构体
 * 在 owner 的 held_locks 红黑树中查找这把锁对应的节点
 */
void update_top_waiter_prio(struct rb_mutex *mutex) {
    struct rb_mutex_waiter *top = rb_mutex_top_waiter(mutex);
    int top_prio = top ? top->prio : -1;

    if (!mutex->owner) return;

    struct rb_task_info *owner_info = get_task_info(mutex->owner);
    if (!owner_info) return;

    struct rb_held_lock *hl = find_held_lock(&owner_info->held_locks, mutex);
    if (hl) {
        hl->top_waiter_prio = top_prio;
    }
}
//------------------------------------------------------------------------------------------------------------------

/*
 * rb_task_table是全局哈希表，key是任务指针，value是rb_task_info结构体指针
 * hash_for_each_possible 遍历哈希表，找到key为task的任务对应的rb_task_info结构体
 */ 
struct rb_task_info *get_task_info(struct task_struct *task) {
    struct rb_task_info *info;
    hash_for_each_possible(rb_task_table, info, hnode, (unsigned long)task) {
        if (info->task == task)
            return info;
    }
    return NULL;
}

///确保指定任务有对应的 rb_task_info 结构体，没有则分配并初始化一个
struct rb_task_info *ensure_task_info(struct task_struct *task) {
    struct rb_task_info *info;
    unsigned long flags;

    spin_lock_irqsave(&rb_task_table_lock, flags);
    info = get_task_info(task);
    if (!info) {
        info = kmalloc(sizeof(*info), GFP_ATOMIC);
        if (info) {
            info->task = task;
            info->waiting_on = NULL;
            info->original_prio = -1;
            info->held_locks = RB_ROOT;
            hash_add(rb_task_table, &info->hnode, (unsigned long)task);
        }
    }
    spin_unlock_irqrestore(&rb_task_table_lock, flags);
    return info;
}

//------------------------------------------------------------------------------------------------------------------

///将一个等待者插入到锁的等待队列红黑树中，按优先级排序
/*
 * 这里修改之前错误，waiter->prio < entry->prio prio越小优先级越高
 */
void rb_mutex_enqueue(struct rb_mutex *mutex, struct rb_mutex_waiter *waiter) {
    struct rb_node **new = &mutex->waiters.rb_node, *parent = NULL;
    struct rb_mutex_waiter *entry;

    while (*new) {
        parent = *new;
        entry = rb_entry(parent, struct rb_mutex_waiter, node);
        if (waiter->prio < entry->prio)
            new = &(*new)->rb_left;
        else
            new = &(*new)->rb_right;
    }

    rb_link_node(&waiter->node, parent, new);
    rb_insert_color(&waiter->node, &mutex->waiters);
    update_top_waiter_prio(mutex);
}

///返回等待队列中优先级最高的等待者 返回rb_mutex_waiter结构体指针
struct rb_mutex_waiter *rb_mutex_top_waiter(struct rb_mutex *mutex) {
    struct rb_node *node = mutex->waiters.rb_node;  //  mutex->waiters: struct rb_root
    if (!node)
        return NULL;
    while (node->rb_left)
        node = node->rb_left;
    return rb_entry(node, struct rb_mutex_waiter, node);
}

///将指定等待者从互斥锁的等待队列中移除，并更新该锁的最高等待者优先级
void rb_mutex_dequeue(struct rb_mutex *mutex, struct rb_mutex_waiter *waiter) {
    rb_erase(&waiter->node, &mutex->waiters);
    waiter->node.rb_left = waiter->node.rb_right = NULL;
    update_top_waiter_prio(mutex);
}

///处理多级锁依赖,递归优先级继承链
void propagate_priority_chain(struct task_struct *task, int new_prio) {
    struct rb_task_info *info = get_task_info(task);
    if (!info) return;

    // 提升当前任务优先级
    if (new_prio < task->prio) {
        if (info->original_prio == -1) // -1表还没有保存过原始优先级，即保存第一次提升前的优先级
            info->original_prio = task->prio;
        task->prio = new_prio;
        pr_info("[rb_mutex] %s inherits prio %d (from %d)\n", task->comm, new_prio, info->original_prio == -1 ? task->prio : info->original_prio);
        wake_up_process(task);
    }

    // 递归传递给 blocked_lock 的 owner
    // 如果当前任务正被某个锁阻塞（blocked_lock 非空），并且该锁有持有者（owner 非空），则递归传播优先级
    if (info->blocked_lock && info->blocked_lock->owner) {
        propagate_priority_chain(info->blocked_lock->owner, new_prio);
    }
}

///检查任务是否还在阻塞其他任务
/*
 * 遍历哈希表，找到key为task的任务对应的rb_task_info结构体
 * 检查该任务是否在等待其他任务持有的锁
 */
bool task_still_blocking_others(struct task_struct *task) {
    struct rb_task_info *info;
    bool blocking = false;
    unsigned long flags;
    int bkt;

    spin_lock_irqsave(&rb_task_table_lock, flags);
    hash_for_each(rb_task_table, bkt, info, hnode) {
        if (info->waiting_on && info->waiting_on->owner == task) {
            blocking = true;
            break;
        }
    }
    spin_unlock_irqrestore(&rb_task_table_lock, flags);

    return blocking;
}

///恢复优先级链
/*
 * 恢复当前任务优先级,如果 original_prio 有效，说明之前发生过优先级继承:恢复任务优先级到 original_prio，并清空 original_prio 字段
 * 递归恢复链路下游:遍历当前任务持有的所有锁,对每一把锁，找到其 owner（即被当前任务阻塞的下游任务）,递归
 */
void restore_priority_chain(struct task_struct *task) {
    struct rb_task_info *info = get_task_info(task);
    if (!info) return;

    // 恢复当前任务优先级
    if (info->original_prio != -1) {
        pr_info("[rb_mutex] %s restore prio to %d\n", task->comm, info->original_prio);
        task->prio = info->original_prio;
        info->original_prio = -1;
    }

    // 递归恢复链路下游
    struct rb_node *node = rb_first(&info->held_locks);
    while (node) {
        struct rb_held_lock *hl = rb_entry(node, struct rb_held_lock, node);
        if (hl->mutex->owner) {
            restore_priority_chain(hl->mutex->owner);
        }
        node = rb_next(node);
    }
}

///在优先级继承链断裂时，让任务的优先级恢复到“应该有的”状态，既不是盲目降级，也不是一直保持提升
/*
 * 合并所有持有锁的最高等待者优先级，得到应继承的优先级
 * 否则，如果 original_prio 有效且当前优先级与 original_prio 不同，则恢复到 original_prio，并清空 original_prio
 * 遍历 held_locks 红黑树，唤醒每把锁的最高优先级等待者,返回是否发生了优先级恢复或降级
 */
bool restore_priority(struct task_struct *task) {
    struct rb_task_info *info;
    int inherited;
    struct rb_node *node;
    bool changed = false;


    info = get_task_info(task);
    if (!info)
        return false;

    inherited = get_effective_inherited_prio(info);
    if (inherited >= 0 && inherited < task->prio) {
        task->prio = inherited;
        pr_info("[rb_mutex] downgrade prio for %s to inherited %d\n", task->comm, inherited);
        changed =true;
    } else if (info->original_prio >= 0 && task->prio != info->original_prio) {
        pr_info("[rb_mutex] restore prio for %s to %d\n", task->comm, info->original_prio);
        task->prio = info->original_prio;
        info->original_prio = -1;
        changed=true;
    }

    // 遍历当前任务持有的所有互斥锁，唤醒各自的最高优先级等待者
    node = rb_first(&info->held_locks);
    while (node) {
        struct rb_held_lock *hl = rb_entry(node, struct rb_held_lock, node);
        struct rb_mutex *mutex = hl->mutex;

        if (!RB_EMPTY_ROOT(&mutex->waiters)) {
            struct rb_mutex_waiter *next = rb_mutex_top_waiter(mutex);
            if (next && next->task)
                wake_up_process(next->task);
        }

        node = rb_next(node);
    }
    return changed;

}

///初始化互斥锁
void rb_mutex_init(struct rb_mutex *mutex) {
    mutex->owner = NULL;
    mutex->waiters = RB_ROOT;
    spin_lock_init(&mutex->lock);
    init_waitqueue_head(&mutex->wait_queue);
}

///检测当前加锁请求是否会导致死锁，打印死锁链路
/*
 * chain 用于文本描述，mermaid 用于可视化
 * prev_node记录上一节点的名字
 * owner -> waiting_on 沿链路每走一步遍历, 记录链路文本和Mermaid可视化描述
 * 若发现owner回到self, 说明形成了环路即死锁, 打印链路和可视化描述
 * 约束: 如果递归深度超过 64，认为链路过长，可能有 bug，防止死循环
 */
static int rb_mutex_detect_deadlock(struct rb_mutex *mutex, struct task_struct *self) {
    struct rb_mutex *cur_mutex = mutex;
    struct task_struct *owner;
    struct rb_task_info *owner_info;
    int depth = 0;
    char chain[512] = {0};
    int offset = 0;
    char mermaid[1024] = "graph TD; ";
    int moffset = strlen(mermaid);
    char prev_node[64];
    snprintf(prev_node, sizeof(prev_node), "T%d", self->pid);
    moffset += snprintf(mermaid+moffset, sizeof(mermaid)-moffset, "%s[\"%s(pid=%d)\"]", prev_node, self->comm, self->pid);
    offset += snprintf(chain+offset, sizeof(chain)-offset, "%s(pid=%d)", self->comm, self->pid);
    while (cur_mutex && (owner = cur_mutex->owner)) {
        owner_info = get_task_info(owner);
        offset += snprintf(chain+offset, sizeof(chain)-offset, " -> [lock %p] -> %s(pid=%d)", cur_mutex, owner->comm, owner->pid);
        char lock_node[64], owner_node[64];
        snprintf(lock_node, sizeof(lock_node), "L%p", cur_mutex);
        snprintf(owner_node, sizeof(owner_node), "T%d", owner->pid);
        moffset += snprintf(mermaid+moffset, sizeof(mermaid)-moffset, "; %s-->|lock %p|%s; %s[\"lock %p\"]", prev_node, cur_mutex, owner_node, lock_node, cur_mutex);
        moffset += snprintf(mermaid+moffset, sizeof(mermaid)-moffset, "; %s[\"%s(pid=%d)\"]", owner_node, owner->comm, owner->pid);
        strncpy(prev_node, owner_node, sizeof(prev_node));
        if (owner == self) {
            atomic_inc(&rb_mutex_deadlock_count);
            pr_warn("[rb_mutex] Deadlock detected: chain: %s\n", chain);
            pr_warn("[rb_mutex] Deadlock chain (Mermaid):\n%s\n", mermaid);
            return 1;
        }
        if (!owner_info) break;
        cur_mutex = owner_info->waiting_on;
        if (++depth > 64) {
            pr_warn("[rb_mutex] Deadlock detection: chain too deep, possible bug\n");
            break;
        }
    }
    return 0;
}

///较上面函数：适合递归检测深层死锁
/*
 * 遍历任务的 blocked_lock 链，检查是否存在环路
 */
int check_deadlock_chain(struct rb_mutex *mutex, struct task_struct *self) {
    struct task_struct *cur = self;
    int depth = 0;
    while (depth < 64) {
        struct rb_task_info *info = get_task_info(cur);
        if (!info) break;
        if (info->blocked_lock == mutex) {
            pr_warn("[rb_mutex] Deadlock detected: chain too deep or cycle found\n");
            return -1;
        }
        if (!info->blocked_lock) break;
        cur = info->blocked_lock->owner;
        depth++;
    }
    return 0;
}

///获取 rb_mutex 互斥锁，支持优先级继承链、递归死锁检测
/*
 * 先采用深度死锁检测方案，如有环路则直接返回错误码
 * rb_mutex_top_waiter(mutex) ? rb_mutex_top_waiter(mutex)->prio : -1 返回当前锁等待队列中优先级最高的等待者（如果有），否则为 NULL
  rb_mutex_lock(mutex):
     死锁检测
     if (锁空闲):
         owner = 当前任务
         清空等待/阻塞状态
         插入 held_locks
         return 成功
     else:
         插入等待队列（按优先级）
         设置等待/阻塞状态
         递归传播优先级
         while (未获得锁):
             阻塞等待
             if (锁空闲 && 自己优先级最高):
                 获得锁，清理状态
                 break
         退出等待队列
         return 成功
 */
int rb_mutex_lock(struct rb_mutex *mutex) {
    DEFINE_WAIT(wait);
    struct rb_mutex_waiter self; // 内核等待队列节点，用于后续阻塞/唤醒当前任务
    struct rb_task_info *info = ensure_task_info(current); // 获取当前任务的 rb_task_info 结构体,如果没有则分配并初始化一个

    // --- 死锁检测 ---
    spin_lock(&mutex->lock);
    if (check_deadlock_chain(mutex, current)) {
        spin_unlock(&mutex->lock);
        return RB_MUTEX_DEADLOCK_ERR;
    }
    self.task = current;
    self.prio = current->prio;
    INIT_LIST_HEAD(&self.list);

    if (!mutex->owner) {
        mutex->owner = current; // 当前任务获得锁 
        if (info) {
            info->waiting_on = NULL;
            info->blocked_lock = NULL; // 获得锁后清空阻塞标记
            insert_held_lock(info, mutex, rb_mutex_top_waiter(mutex) ? rb_mutex_top_waiter(mutex)->prio : -1); // 把这新获得的锁插入到当前任务的 held_locks 红黑树中
        }
        spin_unlock(&mutex->lock);
        return 0;
    }

    rb_mutex_enqueue(mutex, &self);
    if (info) {
        info->waiting_on = mutex;
        info->blocked_lock = mutex; // 进入等待时设置阻塞标记
    }
    propagate_priority_chain(mutex->owner, current->prio); // 递归传播优先级继承链，提升 owner 及其上游 owner 的优先级
    spin_unlock(&mutex->lock);

    // 无限循环，直到当前任务获得锁为止
    for (;;) {
        prepare_to_wait(&mutex->wait_queue, &wait, TASK_UNINTERRUPTIBLE);
        spin_lock(&mutex->lock);
        if (mutex->owner == NULL && rb_mutex_top_waiter(mutex) == &self) {
            rb_mutex_dequeue(mutex, &self);
            mutex->owner = current;

            if (info) {
                info->waiting_on = NULL;
                info->blocked_lock = NULL; // 获得锁后清空阻塞标记
                insert_held_lock(info, mutex, rb_mutex_top_waiter(mutex) ? rb_mutex_top_waiter(mutex)->prio : -1);
            }
            spin_unlock(&mutex->lock);
            break;
        }
        // 解锁互斥锁结构体,调用 schedule()，让出 CPU，当前任务进入休眠，等待被唤醒
        spin_unlock(&mutex->lock);
        schedule();
    }
    // 退出等待队列，恢复任务状态
    finish_wait(&mutex->wait_queue, &wait); // Linux 内核中的一个等待队列辅助函数，用于将当前任务从等待队列中移除，并恢复其调度状态
    return 0;
}

///解锁、等待者唤醒、优先级恢复等核心功能
/*
    if (当前任务不是 owner):
        return

    释放锁（owner = NULL）
    从 held_locks 移除该锁
    清空 blocked_lock

    if (当前任务不再阻塞其他任务):
        标记需要恢复优先级

    if (等待队列非空):
        唤醒优先级最高的等待者

    if (需要恢复优先级):
        递归恢复优先级
        短暂让出 CPU
 */
void rb_mutex_unlock(struct rb_mutex *mutex) {
    struct rb_task_info *info = get_task_info(current);
    struct rb_mutex_waiter *next = NULL;
    bool need_restore = false; // need_restore 标记是否需要恢复优先级

    // 检查当前任务是否是锁的 owner，只有owner才能解锁
    spin_lock(&mutex->lock);
    if (mutex->owner != current) {
        spin_unlock(&mutex->lock);
        return;
    }
    mutex->owner = NULL;

    // 移除持有锁，判断是否需要恢复优先级
    if (info) {
        remove_held_lock(info, mutex);
        info->blocked_lock = NULL; // 释放锁时清空阻塞标记
        if (!task_still_blocking_others(current))
            need_restore = true;
    }

    // 如果等待队列不为空：唤醒等待队列中优先级最高的等待者
    if (!RB_EMPTY_ROOT(&mutex->waiters)) {
        next = rb_mutex_top_waiter(mutex);
        if (next && next->task)
            wake_up_process(next->task);
    }

    spin_unlock(&mutex->lock);

    // 递归恢复优先级（need_restore判断可选）
    if (need_restore) {
        restore_priority_chain(current);
        set_current_state(TASK_UNINTERRUPTIBLE);
        schedule_timeout(usecs_to_jiffies(20));
        __set_current_state(TASK_RUNNING);
    }
}

///尝试获取锁，不阻塞，成功返回1，失败返回0
/*
 * rb_mutex_trylock 是非阻塞的试探性加锁函数，锁被占用时立即返回，不涉及优先级继承和等待队列
 * rb_mutex_lock 是阻塞型加锁，支持优先级继承链和等待队列管理，保证高优先级任务不会被低优先级任务长期阻塞
 */
int rb_mutex_trylock(struct rb_mutex *mutex) {
    int success = 0;
    struct rb_task_info *info = ensure_task_info(current);

    spin_lock(&mutex->lock);
    if (!mutex->owner) {
        mutex->owner = current;
        if (info) {
            info->waiting_on = NULL;
            insert_held_lock(info, mutex, rb_mutex_top_waiter(mutex) ? rb_mutex_top_waiter(mutex)->prio : -1);
        }
        success = 1;
    }
    spin_unlock(&mutex->lock);
    return success;
}

///相较rb_mutex_lock，支持超时机制
/*
 * 超时机制：if (time_after(jiffies, deadline)) {
            spin_lock(&mutex->lock);
            rb_mutex_dequeue(mutex, &self);
            spin_unlock(&mutex->lock);

            if (info) info->waiting_on = NULL;
            finish_wait(&mutex->wait_queue, &wait);
            return -ETIMEDOUT;
        }
        schedule_timeout(msecs_to_jiffies(10));
 */
int rb_mutex_lock_timeout(struct rb_mutex *mutex, unsigned long timeout_ms) {
    DEFINE_WAIT(wait);
    struct rb_mutex_waiter self;
    struct rb_task_info *info = ensure_task_info(current);
    unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

    // --- 死锁检测 ---
    spin_lock(&mutex->lock);
    if (rb_mutex_detect_deadlock(mutex, current)) {
        spin_unlock(&mutex->lock);
        return RB_MUTEX_DEADLOCK_ERR;
    }

    if (mutex->owner == current) {
        pr_warn("[rb_mutex] Deadlock detected: %s tried to re-lock\n", current->comm);
        spin_unlock(&mutex->lock);
        return -EDEADLK;
    }
    self.task = current;
    self.prio = current->prio;
    INIT_LIST_HEAD(&self.list);

    if (!mutex->owner) {
        mutex->owner = current;
        if (info) {
            info->waiting_on = NULL;
            insert_held_lock(info, mutex, rb_mutex_top_waiter(mutex) ? rb_mutex_top_waiter(mutex)->prio : -1);
        }
        spin_unlock(&mutex->lock);
        return 0;
    }

    rb_mutex_enqueue(mutex, &self);
    if (info) info->waiting_on = mutex;
    propagate_priority_chain(mutex->owner, current->prio);
    spin_unlock(&mutex->lock);

    for (;;) {
        prepare_to_wait(&mutex->wait_queue, &wait, TASK_UNINTERRUPTIBLE);

        spin_lock(&mutex->lock);
        if (mutex->owner == NULL && rb_mutex_top_waiter(mutex) == &self) {
            rb_mutex_dequeue(mutex, &self);
            mutex->owner = current;
            if (info) {
                info->waiting_on = NULL;
                insert_held_lock(info, mutex, rb_mutex_top_waiter(mutex) ? rb_mutex_top_waiter(mutex)->prio : -1);
            }
            spin_unlock(&mutex->lock);
            break;
        }
        spin_unlock(&mutex->lock);

        if (time_after(jiffies, deadline)) {
            spin_lock(&mutex->lock);
            rb_mutex_dequeue(mutex, &self);
            spin_unlock(&mutex->lock);

            if (info) info->waiting_on = NULL;
            finish_wait(&mutex->wait_queue, &wait);
            return -ETIMEDOUT;
        }

        schedule_timeout(msecs_to_jiffies(10));
    }

    finish_wait(&mutex->wait_queue, &wait);
    return 0;
}

int rb_mutex_get_deadlock_count(void) {
    return atomic_read(&rb_mutex_deadlock_count);
}

static int __init rb_mutex_module_init(void) {
    pr_info("rb_mutex kernel module loaded\n");
    return 0;
}

static void __exit rb_mutex_module_exit(void) {
    pr_info("rb_mutex kernel module unloaded\n");
}

module_init(rb_mutex_module_init);
module_exit(rb_mutex_module_exit);

EXPORT_SYMBOL(rb_mutex_init);
EXPORT_SYMBOL(rb_mutex_lock);
EXPORT_SYMBOL(rb_mutex_unlock);
EXPORT_SYMBOL(rb_mutex_trylock);
EXPORT_SYMBOL(rb_mutex_lock_timeout);
EXPORT_SYMBOL(rb_mutex_get_deadlock_count);
