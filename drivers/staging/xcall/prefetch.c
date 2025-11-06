// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A simple dummy xcall for syscall testing
 *
 * The data struct and functions marked as MANDATORY have to
 * be includes in all of kernel xcall modules.
 *
 * Copyright (C) 2025 Huawei Limited.
 */

#define pr_fmt(fmt)	"dummy_xcall: " fmt

#include <linux/module.h>
#include <linux/xcall.h>
#include <linux/unistd.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include <linux/hash.h>
#include <linux/mmu_notifier.h>
#include <linux/miscdevice.h>
#include <linux/file.h>
#include <linux/socket.h>   // 定义 struct sock 和基本的 socket 类型
#include <net/sock.h>       // 定义 sock 结构体的详细内容
#include <net/tcp_states.h> // 定义 TCP_ESTABLISHED 等 TCP 状态宏
#include <uapi/linux/futex.h>

#include <asm/barrier.h>
#include <asm/xcall.h>

#define MAX_FD 100

static unsigned long xcall_cache_hit;
static unsigned long xcall_cache_miss;

struct proc_dir_entry *xcall_proc_dir;

enum cache_state {
	XCALL_CACHE_NONE = 0,
	XCALL_CACHE_PREFETCH,
	XCALL_CACHE_READY,
	XCALL_CACHE_CANCEL
};

struct prefetch_item {
	int fd;
	int cpu;
	int pos;
	int len;
	atomic_t state;
	struct file *file;
	struct work_struct work;
	char cache[PAGE_SIZE];
};

static struct epoll_event events[MAX_FD] = {0};

static struct prefetch_item prefetch_items[MAX_FD] = {0};
static struct workqueue_struct *rc_work;

static inline bool transition_state(struct prefetch_item *pfi,
				    enum cache_state old, enum cache_state new)
{
	return atomic_cmpxchg(&pfi->state, old, new) == old;
}

static void prefetch_work_fn(struct work_struct *work)
{
	struct prefetch_item *pfi = container_of(work, struct prefetch_item, work);

	if (!transition_state(pfi, XCALL_CACHE_NONE, XCALL_CACHE_PREFETCH))
		return;

	pfi->pos = 0;
	pfi->len = kernel_read(pfi->file, pfi->cache, PAGE_SIZE, &pfi->file->f_pos);

	transition_state(pfi, XCALL_CACHE_PREFETCH, XCALL_CACHE_READY);
}

static long __do_sys_epoll_pwait(struct pt_regs *regs)
{
	struct prefetch_item *pfi;
	int i, fd, err;
	long ret;

	ret = default_sys_call_table()[__NR_epoll_pwait](regs);
	if (ret != 0) {
		err = copy_from_user(events, (void __user *)regs->regs[1],
				     ret * sizeof(struct epoll_event));
		if (err)
			return -EFAULT;

		for (i = 0; i < ret; i++) {
			fd = events[i].data;
			if (events[i].events & EPOLLIN) {
				pfi = &prefetch_items[fd];
				if (!pfi->file)
					pfi->file = fget(fd);

				queue_work_on(250 + (fd % 4), rc_work, &pfi->work);
			}
		}
	}
	return ret;
}

static long __do_sys_read(struct pt_regs *regs)
{
	int fd = regs->regs[0];
	struct prefetch_item *pfi = &prefetch_items[fd];
	void *user_buf = (void *)regs->regs[1];
	int count = regs->regs[2];
	int copy_len;
	long ret;

	if (pfi->file) {
		while (!transition_state(pfi, XCALL_CACHE_READY, XCALL_CACHE_CANCEL)) {
			if (transition_state(pfi, XCALL_CACHE_NONE, XCALL_CACHE_CANCEL))
				goto slow_read;
		}

		xcall_cache_hit++;
		copy_len = pfi->len;

		if (copy_len == 0) {
			transition_state(pfi, XCALL_CACHE_CANCEL, XCALL_CACHE_NONE);
			return 0;
		}

		copy_len = (copy_len >= count) ? count : copy_len;
		copy_len -= copy_to_user(user_buf, (void *)(pfi->cache + pfi->pos), copy_len);
		pfi->len -= copy_len;
		pfi->pos += copy_len;

		if (pfi->len == 0)
			transition_state(pfi, XCALL_CACHE_CANCEL, XCALL_CACHE_NONE);
		else
			transition_state(pfi, XCALL_CACHE_CANCEL, XCALL_CACHE_READY);
		return copy_len;
	}

	goto not_epoll_fd;

slow_read:
	xcall_cache_miss++;
	pfi->len = 0;
	pfi->pos = 0;
	cancel_work_sync(&pfi->work);
	transition_state(pfi, XCALL_CACHE_CANCEL, XCALL_CACHE_NONE);
not_epoll_fd:
	ret = default_sys_call_table()[__NR_read](regs);
	return ret;
}

static long __do_sys_close(struct pt_regs *regs)
{
	int fd = regs->regs[0];
	struct prefetch_item *pfi = &prefetch_items[fd];
	long ret;

	if (pfi->file) {
		fput(pfi->file);
		pfi->file = NULL;
	}

	ret = default_sys_call_table()[__NR_close](regs);
	return ret;
}

/* MANDATORY */
static struct xcall_prog xcall_prefetch_prog = {
	.name	= "xcall_prefetch",
	.owner	= THIS_MODULE,
	.objs	= {
		{
			.scno = (unsigned long)__NR_epoll_pwait,
			.func = (unsigned long)__do_sys_epoll_pwait,
		},
		{
			.scno = (unsigned long)__NR_read,
			.func = (unsigned long)__do_sys_read,
		},
		{
			.scno = (unsigned long)__NR_close,
			.func = (unsigned long)__do_sys_close,
		},
		{}
	}
};

static ssize_t xcall_prefetch_reset(struct file *file, const char __user *buf,
				    size_t count, loff_t *pos)
{
	xcall_cache_hit = 0;
	xcall_cache_miss = 0;

	return count;
}

static int xcall_prefetch_show(struct seq_file *m, void *v)
{
	u64 percent;

	percent = DIV_ROUND_CLOSEST(xcall_cache_hit * 100ULL, xcall_cache_hit + xcall_cache_miss);
	seq_printf(m, "epoll cache_{hit,miss}: %lu,%lu, hit ratio: %llu%%\n",
		   xcall_cache_hit, xcall_cache_miss, percent);
	return 0;
}

static int xcall_prefetch_open(struct inode *inode, struct file *file)
{
	return single_open(file, xcall_prefetch_show, NULL);
}

static const struct proc_ops xcall_prefetch_fops = {
	.proc_open = xcall_prefetch_open,
	.proc_read = seq_read,
	.proc_write = xcall_prefetch_reset,
	.proc_lseek = seq_lseek,
	.proc_release = single_release
};

static int __init init_xcall_prefetch_procfs(void)
{
	struct proc_dir_entry *prefetch_dir;

	xcall_proc_dir = proc_mkdir("xcall_stat", NULL);
	if (!xcall_proc_dir)
		return -ENOMEM;
	prefetch_dir = proc_create("prefetch", 0640, xcall_proc_dir, &xcall_prefetch_fops);
	if (!prefetch_dir)
		goto rm_xcall_proc_dir;

	return 0;

rm_xcall_proc_dir:
	proc_remove(xcall_proc_dir);
	return -ENOMEM;
}

/* MANDATORY */
static int __init dummy_xcall_init(void)
{
	int i;

	rc_work = alloc_workqueue("eventpoll_rc", 0, 0);
	if (!rc_work)
		pr_warn("alloc eventpoll_rc workqueue failed.\n");

	for (i = 0; i < MAX_FD; i++)
		INIT_WORK(&prefetch_items[i].work, prefetch_work_fn);

	init_xcall_prefetch_procfs();

	INIT_LIST_HEAD(&xcall_prefetch_prog.list);
	return xcall_prog_register(&xcall_prefetch_prog);
}

/* MANDATORY */
static void __exit dummy_xcall_exit(void)
{
	proc_remove(xcall_proc_dir);
	xcall_prog_unregister(&xcall_prefetch_prog);
}

module_init(dummy_xcall_init);
module_exit(dummy_xcall_exit);
MODULE_AUTHOR("Liao Chang <liaochang1@huawei.com>");
MODULE_DESCRIPTION("Dummy Xcall");
MODULE_LICENSE("GPL");
