# SPDX-License-Identifier: GPL-2.0
EXTRA_CFLAGS += -DPS3_CFG_RELEASE
EXTRA_CFLAGS += -DPS3_HARDWARE_ASIC
EXTRA_CFLAGS += -DPS3_MODEL_V200

ifneq ($(KERNELRELEASE),)
CONFIG_MODULE_SIG=n
obj-m += ps3stor.o

ps3stor-objs := $(patsubst %.c, %.o, $(notdir $(wildcard $(src)/*.c)))
ps3stor-objs += $(patsubst %.c, linux/%.o, $(notdir $(wildcard $(src)/linux/*.c)))

ccflags-y += -Werror -frecord-gcc-switches
ccflags-y += $(call cc-option, -Wmaybe-uninitialized)
ccflags-y += -I$(src) -I$(src)/linux -I$(src)/include/htp_v200 -I$(src)/include/
else
	KDIR ?= /lib/modules/`uname -r`/build

all:
	@echo "ARCH=$(ARCH)"
	@echo "KDIR=$(KDIR)"
	@echo "OS=$(OS_SYSTEM)"
	@echo "EXTRA_CFLAGS=$(EXTRA_CFLAGS)"
	@$(MAKE) -C $(KDIR) M=$(PWD) INCDIR=$(PWD)/linux modules
clean:
	@rm  -rf *.o *.d *.ko Module.* modules.* *.mod.* .*.d .*.cmd .tmp_versions *.o.ur-safe
	@rm  -rf ./linux/*.o
	@rm  -rf ./tags
	@rm  -rf ./output.log
endif

