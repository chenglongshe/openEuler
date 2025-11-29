/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/kprobes.h>
#include <linux/errno.h>

#include "syms_lookup.h"
#include "xsched.h"

kallsyms_lookup_name_t generic_kallsyms_lookup_name;

static struct kprobe kp = {
	/* lookup kallsyms_lookup_name */
	.symbol_name = "kallsyms_lookup_name",
};

/*
 * kprobe pre_handler callback
 */
static int __kprobes handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	return 0;
}

int __init syms_lookup_init(void)
{
	int ret;

	if (generic_kallsyms_lookup_name)
		return 0;

	kp.pre_handler = handler_pre;
	ret = register_kprobe(&kp);
	if (ret < 0) {
		XSCHED_ERR("Failed to register kprobe for kallsyms_lookup_name, error: %d\n", ret);
		return ret;
	}

	generic_kallsyms_lookup_name = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);

	if (!generic_kallsyms_lookup_name) {
		XSCHED_ERR("Failed to get valid address for kallsyms_lookup_name\n");
		return -ENOENT;
	}
	XSCHED_INFO("Found kallsyms_lookup_name at address: %p\n", (void *)generic_kallsyms_lookup_name);

	return 0;
}

void syms_lookup_exit(void)
{
	generic_kallsyms_lookup_name = NULL;
}
