// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/printk.h>
#include <linux/preempt.h>
#include <linux/sched/signal.h>
#include <linux/kernel_ipc.h>
#include <linux/sched/debug.h>

void kernel_ipc_print(const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	int level;

	va_start(args, fmt);

	level = printk_get_level(fmt);
	vaf.fmt = printk_skip_level(fmt);
	vaf.va = &args;
	printk("%c%c kernel_ipc: %pV\n", KERN_SOH_ASCII, level, &vaf);

	va_end(args);
}

#define IPC_ERROR(fmt, ...) kernel_ipc_print(KERN_ERR fmt, ##__VA_ARGS__)
#define IPC_WARNING(fmt, ...) kernel_ipc_print(KERN_WARNING fmt, ##__VA_ARGS__)
//#define IPC_DEBUG(fmt, ...) kernel_ipc_print(KERN_DEBUG fmt, ##__VA_ARGS__)
#define IPC_DEBUG(fmt, ...)


MODULE_LICENSE("GPL");
MODULE_AUTHOR("yangyun");
MODULE_DESCRIPTION("kernel ipc");
MODULE_VERSION("1.0");

static inline void bind_info_lock(struct kernel_ipc_bind_info *bind_info)
{
	spin_lock(&bind_info->lock);
}

static inline void bind_info_unlock(struct kernel_ipc_bind_info *bind_info)
{
	spin_unlock(&bind_info->lock);
}

static inline int kernel_ipc_check_task_consistency(struct task_struct *client,
		struct task_struct *server)
{
	if (client->pid == server->pid) {
		IPC_ERROR("error: client(%s/%d) and server(%s/%d) is same\n", client->comm,
			   client->pid, server->comm, server->pid);
		return -EPERM;
	}

	return 0;
}

static inline ssize_t
kernel_ipc_call_check(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *tsk)
{
	ssize_t ret = 0;
	struct task_struct *server_task;

	if (!bind_info) {
		IPC_ERROR("error: no found bind_info\n");
		return -ENOENT;
	}

	if (bind_info->client_task) {
		IPC_ERROR("error: bind already with client task: %s/%d, current is : %s/%d",
			   bind_info->client_task->comm, bind_info->client_task->pid,
			   tsk->comm, tsk->pid);
		return -EEXIST;
	}

	server_task = bind_info->server_task;
	if (!server_task) {
		IPC_ERROR("error: server thread is not exsit\n");
		return -ESRCH;
	}

	return ret;
}

static inline void kernel_ipc_client_init(
		struct kernel_ipc_bind_info *bind_info, struct task_struct *tsk)
{
	bind_info->client_task = tsk;
}


static inline void kernel_ipc_client_exit(
		struct kernel_ipc_bind_info *bind_info, struct task_struct *tsk)
{
	bind_info->client_task = NULL;
}

static inline int kernel_ipc_get_client_exit_code(
		const struct kernel_ipc_bind_info *bind_info)
{
	return bind_info->client_need_exit ? -ESRCH : 0;
}

static inline void kernel_ipc_wakeup_client_task(
		struct kernel_ipc_bind_info *bind_info)
{
	struct task_struct *client_task;

	client_task = bind_info->client_task;
	bind_info->client_need_exit = true;
	wake_up_process(client_task);
}

void kernel_ipc_wakeup_server_task(struct kernel_ipc_bind_info *bind_info)
{
	struct task_struct *server_task;

	server_task = bind_info->server_task;
	bind_info->server_need_exit = true;
	wake_up_process(server_task);
}
EXPORT_SYMBOL_GPL(kernel_ipc_wakeup_server_task);

void *kernel_ipc_bind(struct task_struct *server_task)
{
	struct kernel_ipc_bind_info *bind_info = NULL;

	bind_info = kcalloc(1, sizeof(struct kernel_ipc_bind_info), GFP_KERNEL);
	if (!bind_info) {
		IPC_ERROR("error: alloc kernel_ipc_bind_info failed\n");
		return ERR_PTR(-ENOMEM);
	}

	bind_info->server_task = server_task;

	return (void *) bind_info;
}
EXPORT_SYMBOL_GPL(kernel_ipc_bind);

void kernel_ipc_release(struct kernel_ipc_bind_info *bind_info)
{
	if (bind_info) {
		if (bind_info->client_task && bind_info->is_calling)
			kernel_ipc_wakeup_client_task(bind_info);
		kfree(bind_info);
	}
}
EXPORT_SYMBOL_GPL(kernel_ipc_release);

void kernel_ipc_unbind(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *server_task)
{
	if (bind_info) {
		if (bind_info->server_task == server_task) {
			bind_info->server_task = NULL;
			if (bind_info->client_task && bind_info->is_calling)
				kernel_ipc_wakeup_client_task(bind_info);
			kfree(bind_info);
		}
	}

}
EXPORT_SYMBOL_GPL(kernel_ipc_unbind);

ssize_t kernel_ipc_do_call(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *tsk)
{
	struct task_struct *server_task;
	ssize_t ret;

	ret = kernel_ipc_call_check(bind_info, tsk);
	if (ret) {
		IPC_ERROR("kernel ipc call check and init failed, errno: %d\n", ret);
		return ret;
	}

	kernel_ipc_client_init(bind_info, tsk);

	server_task = bind_info->server_task;

	bind_info->client_need_exit = false;
	bind_info->is_calling = true;

	preempt_disable(); /* optimize performance if preemption occurs */
	smp_mb();
	wake_up_process(server_task);
	preempt_enable();
	IPC_DEBUG("[cpu/%d][%s/%d]  ipc do call server(%s/%d)\n",
			  smp_processor_id(), tsk->comm, tsk->pid, server_task->comm,
			  server_task->pid);

	set_current_state(TASK_INTERRUPTIBLE);
	while (bind_info->is_calling) {
		IPC_DEBUG("[cpu/%d][%s/%d] client begin schedule\n", smp_processor_id(),
				  tsk->comm, tsk->pid);
		schedule();
		IPC_DEBUG("[cpu/%d][%s/%d] client schedule end\n", smp_processor_id(),
				  tsk->comm, tsk->pid);
		if (signal_pending(current)) {
			ret = -EINTR;
			IPC_WARNING("[cpu/%d][%s/%d] client has signal pending break\n",
				   smp_processor_id(), tsk->comm, tsk->pid);
			break;
		}
		set_current_state(TASK_INTERRUPTIBLE);
	}
	set_current_state(TASK_RUNNING);

	if (bind_info->is_calling) {
		IPC_ERROR("[cpu/%d][%s/%d] server is still calling, but client is waken up\n",
			   smp_processor_id(), tsk->comm, tsk->pid);
		IPC_ERROR("[cpu/%d][%s/%d] servertask(%s/%d) is running on cpu %d\n",
			   smp_processor_id(), tsk->comm, tsk->pid, server_task->comm,
			   server_task->pid, task_cpu(server_task));
		//show_stack(server_task, NULL, KERN_DEBUG);
		IPC_ERROR("[cpu/%d][%s/%d] show_stack end in %s\n",
			   smp_processor_id(), tsk->comm, tsk->pid, __func__);
	}

	kernel_ipc_client_exit(bind_info, tsk);

	if (ret == -EINTR)
		return ret;
	ret = kernel_ipc_get_client_exit_code(bind_info);

	return ret;
}
EXPORT_SYMBOL_GPL(kernel_ipc_do_call);

long kernel_ipc_ret_call(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *tsk)
{
	struct task_struct *client_task;

	if (!bind_info->is_calling)
		return 0;

	bind_info_lock(bind_info);
	client_task = bind_info->client_task;
	if (!client_task) {
		bind_info_unlock(bind_info);
		return -ESRCH;
	}

	bind_info_unlock(bind_info);

	bind_info->is_calling = false;
	preempt_disable();
	/* memory barrier for preempt */
	smp_mb();
	wake_up_process(client_task);
	preempt_enable();
	IPC_DEBUG("[CPU/%d][%s/%d] client task pid: %d, state: %d\n",
			  smp_processor_id(), tsk->comm, tsk->pid, client_task->pid,
			  client_task->state);

	return 0;
}
EXPORT_SYMBOL_GPL(kernel_ipc_ret_call);

long kernel_ipc_wait_call(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *tsk)
{
	long ret = 0;
	sigset_t pending_signals;

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (bind_info->is_calling)
			break;

		if (bind_info->server_need_exit) {
			ret = -ENODEV;
			break;
		}

		schedule();

		if (signal_pending_state(TASK_INTERRUPTIBLE, tsk)
			&& !bind_info->is_calling) {
			if (fatal_signal_pending(tsk)) {
				IPC_WARNING("[CPU/%d][%s/%d] current task has SIGKILL\n",
					   smp_processor_id(), tsk->comm, tsk->pid);
			}

			pending_signals = current->pending.signal;
			ret = -ERESTARTSYS;
			break;
		}
	}

	set_current_state(TASK_RUNNING);
	return ret;
}
EXPORT_SYMBOL_GPL(kernel_ipc_wait_call);

static int __init
kernel_ipc_init(void)
{
	IPC_WARNING("kernel ipc init\n");
	return 0;
}

static void __exit
kernel_ipc_exit(void)
{
	IPC_WARNING("kernel ipc exit\n");
}


module_init(kernel_ipc_init);
module_exit(kernel_ipc_exit);
