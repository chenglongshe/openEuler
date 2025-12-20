subdir-ccflags-y += -I$(src)
subdir-ccflags-y += -I$(srctree)/include/linux/dinghai/

# subdir-ccflags-y += -I$(src)
# subdir-ccflags-y += -I$(CWD)/include/
# subdir-ccflags-y += -include $(CWD)/autoconf.h
ccflags-y += -Werror

ifeq ($(CONFIG_DRIVER_VERSION),)
EXTRA_CFLAGS += -DDRIVER_VERSION_VAL=\"1.0-1\"
$(info CONFIG_PF_MPF_DRIVER_VERSION is null, EXTRA_CFLAGS=$(EXTRA_CFLAGS))
else
EXTRA_CFLAGS += -DDRIVER_VERSION_VAL=\"$(CONFIG_DRIVER_VERSION)\"
$(info CONFIG_PF_MPF_DRIVER_VERSION is not null, EXTRA_CFLAGS=$(EXTRA_CFLAGS))
endif

obj-$(CONFIG_DINGHAI_HPF) += dinghai10e_hpf.o
dinghai10e_hpf-y := zxdh_pcie.o function_hotplug.o pcie_msix.o pcie_common.o bar_msg.o cmd/dh_cmd.o cmd/msg_chan_lock.o
