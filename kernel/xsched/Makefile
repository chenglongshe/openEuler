# SPDX-License-Identifier: GPL-2.0
obj-y += vstream.o
xsched_enabled := $(CONFIG_XCU_SCHEDULER)
obj-$(xsched_enabled) += core.o
obj-$(xsched_enabled)-$(CONFIG_XCU_SCHED_RT) += rt.o
obj-$(xsched_enabled)-$(CONFIG_XCU_SCHED_CFS) += cfs.o
obj-$(xsched_enabled)-$(CONFIG_CGROUP_XCU) += cgroup.o
