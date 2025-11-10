// SPDX-License-Identifier: GPL-2.0
/*
 * xcall related proc code
 *
 * Copyright (C) 2025 Huawei Ltd.
 */
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <asm/xcall.h>
#include "internal.h"

static int xcall_show(struct seq_file *m, void *v)
{
	struct inode *inode = m->private;
	struct xcall_info *xinfo;
	struct task_struct *p;
	int l = 0, r = 1;

	if (!static_key_enabled(&xcall_enable))
		return -EACCES;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

	xinfo = TASK_XINFO(p);
	if (!xinfo)
		goto out;

	while (r < __NR_syscalls) {
		if (!xinfo->xcall_enable[l]) {
			l++;
			r = l + 1;
			continue;
		}

		if (!xinfo->xcall_enable[r]) {
			if (r == (l + 1))
				seq_printf(m, "%d,", l);
			else
				seq_printf(m, "%d-%d,", l, r - 1);
			l = r + 1;
			r = l + 1;
			continue;
		}
		r++;
	}

	seq_puts(m, "\n");
out:
	put_task_struct(p);

	return 0;
}

static int xcall_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, xcall_show, inode);
}

static ssize_t xcall_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *offset)
{
	unsigned int sc_no = __NR_syscalls;
	struct task_struct *p;
	char buf[5];
	int ret = 0;

	if (!static_key_enabled(&xcall_enable))
		return -EACCES;

	p = get_proc_task(file_inode(file));
	if (!p || !TASK_XINFO(p))
		return -ESRCH;

	memset(buf, '\0', 5);
	if (!count || (count > 4) || copy_from_user(buf, ubuf, count)) {
		ret = -EFAULT;
		goto out;
	}

	if (kstrtouint((buf + (int)(buf[0] == '!')), 10, &sc_no)) {
		ret = -EINVAL;
		goto out;
	}

	if (sc_no >= __NR_syscalls) {
		ret = -EINVAL;
		goto out;
	}

	(TASK_XINFO(p))->xcall_enable[sc_no] = (int)(buf[0] != '!');
	ret = 0;

out:
	put_task_struct(p);

	return ret ? ret : count;
}

const struct file_operations proc_pid_xcall_operations = {
	.open		= xcall_open,
	.read		= seq_read,
	.write		= xcall_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};
