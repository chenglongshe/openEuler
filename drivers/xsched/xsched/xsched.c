/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/ioctl.h>
#include <linux/kallsyms.h>
#include <linux/atomic.h>

#include "xsched_ioctl.h"
#include "xcu_group.h"
#include "xsched_npu_interface.h"
#include "vstream.h"
#include "xsched.h"

#define DEVICE_NAME "xsched"
#define CLASS_NAME "xsched_class"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Huawei");
MODULE_DESCRIPTION("XSched ko for XPU");

static int major_number;
static struct class *xsched_class;
static struct device *xsched_dev;

/* sysfs: /sys/class/xsched_class/xsched/pending_tasks */
static ssize_t pending_tasks_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&pending_task_count));
}

static struct device_attribute dev_attr_pending_tasks = {
	.attr = { .name = "pending_tasks", .mode = 0444 },
	.show = pending_tasks_show,
	.store = NULL,
};

struct ioctl_handler {
	unsigned int cmd;
	size_t arg_size;
	int (*handler)(void *kern_arg);
};

static int handle_vstream_kick(void *arg)
{
	return xsched_kick((vstream_args_t *)arg);
}

static int handle_vstream_alloc(void *arg)
{
	return xsched_alloc((vstream_args_t *)arg);
}

static int handle_vstream_free(void *arg)
{
	return xsched_free((vstream_args_t *)arg);
}

static int handle_priority_set(void *arg)
{
	struct priority_args *args = (struct priority_args *)arg;

	return xsched_rt_prio_set(args->pid, args->sched_priority);
}

static int handle_priority_get(void *arg)
{
	struct priority_args *args = (struct priority_args *)arg;
	int ret = xsched_rt_prio_get(args->pid);

	if (ret < 0)
		return ret;

	args->sched_priority = ret;
	return 0;
}

static const struct ioctl_handler handlers[] = {
	{XSCHED_ALLOC,    sizeof(vstream_args_t),       handle_vstream_alloc},
	{XSCHED_FREE,     sizeof(vstream_args_t),       handle_vstream_free},
	{XSCHED_KICK,     sizeof(vstream_args_t),       handle_vstream_kick},
	{XSCHED_SET_PRIO, sizeof(struct priority_args), handle_priority_set},
	{XSCHED_GET_PRIO, sizeof(struct priority_args), handle_priority_get},
};

static long xsched_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	const struct ioctl_handler *handler = NULL;
	void *kern_arg = NULL;
	void __user *user_arg = (void __user *)arg;
	size_t arg_size = _IOC_SIZE(cmd);
	int res, i;

	if (_IOC_TYPE(cmd) != XSCHED_MAGIC) {
		XSCHED_ERR("Invalid magic: %u (need %u)\n", _IOC_TYPE(cmd), XSCHED_MAGIC);
		return -ENOTTY;
	}

	for (i = 0; i < ARRAY_SIZE(handlers); i++) {
		if (handlers[i].cmd == cmd) {
			handler = &handlers[i];
			break;
		}
	}

	if (!handler) {
		XSCHED_ERR("Unknown IOCTL command: 0x%x\n", cmd);
		return -ENOTTY;
	}

	if (arg_size != handler->arg_size)
		return -EINVAL;

	if (!access_ok(user_arg, arg_size)) {
		XSCHED_ERR("User pointer not accessible: %p\n", user_arg);
		return -EFAULT;
	}

	kern_arg = kmalloc(arg_size, GFP_KERNEL);
	if (!kern_arg) {
		XSCHED_ERR("Failed to allocate kernel argument buffer\n");
		return -ENOMEM;
	}

	if (copy_from_user(kern_arg, user_arg, arg_size)) {
		XSCHED_ERR("Fail to copy_to_user\n");
		kfree(kern_arg);
		return -EFAULT;
	}

	res = handler->handler(kern_arg);

	if (copy_to_user(user_arg, kern_arg, arg_size)) {
		XSCHED_ERR("Fail to copy_to_user\n");
		res = -EFAULT;
	}

	kfree(kern_arg);
	return res;
}

static int xsched_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int xsched_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations xsched_fops = {
	.owner = THIS_MODULE,
	.open = xsched_open,
	.release = xsched_release,
	.unlocked_ioctl = xsched_ioctl,
};

bool xsched_try_module_get(void)
{
	return try_module_get(xsched_fops.owner);
}

void xsched_module_put(void)
{
	module_put(xsched_fops.owner);
}

static int __init xsched_init(void)
{
	int ret = 0;
	unsigned int dev_id = 0;

	ret = syms_lookup_init();
	if (ret < 0) {
		XSCHED_ERR("Failed to initialize symbol lookup\n");
		return ret;
	}

	ret = ioctl_trs_sqcq_handler_find();
	if (ret < 0) {
		XSCHED_ERR("Failed to initialize NPU handlers\n");
		goto err_syms;
	}

	xsched_sched_init();

	for (dev_id = 0; dev_id < XSCHED_NR_CUS; dev_id++) {
		ret = xcu_populate(dev_id);
		if (ret)
			goto err_xcu;
	}

	major_number = register_chrdev(0, DEVICE_NAME, &xsched_fops);
	if (major_number < 0) {
		XSCHED_ERR("Failed to register a major number\n");
		ret = major_number;
		goto err_xcu;
	}

	xsched_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(xsched_class)) {
		XSCHED_ERR("Failed to register device class\n");
		ret = PTR_ERR(xsched_class);
		goto err_cdev;
	}

	xsched_dev = device_create(xsched_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
	if (IS_ERR(xsched_dev)) {
		XSCHED_ERR("Failed to create the device\n");
		ret = PTR_ERR(xsched_dev);
		goto err_class;
	}

	ret = sysfs_create_file(&xsched_dev->kobj, &dev_attr_pending_tasks.attr);
	if (ret) {
		XSCHED_WARN("Failed to create open_handle_count attribute\n");
		goto err_device;
	}

	XSCHED_INFO("Device created successfully with major: %d\n", major_number);
	return 0;

err_device:
	device_destroy(xsched_class, MKDEV(major_number, 0));
err_class:
	class_destroy(xsched_class);
err_cdev:
	unregister_chrdev(major_number, DEVICE_NAME);
err_xcu:
	for (dev_id--; dev_id >= 0; dev_id--)
		xcu_depopulate(dev_id);
	xcu_group_free(xcu_group_root);
	xcu_group_root = NULL;
	ioctl_trs_sqcq_handler_free();
err_syms:
	syms_lookup_exit();
	return ret;
}

static void __exit xsched_exit(void)
{
	unsigned int dev_id = 0;

	tgid_prio_cleanup();
	sysfs_remove_file(&xsched_dev->kobj, &dev_attr_pending_tasks.attr);
	device_destroy(xsched_class, MKDEV(major_number, 0));
	class_destroy(xsched_class);
	unregister_chrdev(major_number, DEVICE_NAME);

	for (dev_id = 0; dev_id < XSCHED_NR_CUS; dev_id++)
		xcu_depopulate(dev_id);

	xcu_group_free(xcu_group_root);
	xcu_group_root = NULL;

	ioctl_trs_sqcq_handler_free();
	syms_lookup_exit();
	XSCHED_INFO("Module unloaded\n");
}

module_init(xsched_init);
module_exit(xsched_exit);
