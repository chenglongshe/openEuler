# compiler
CC := gcc
LD := ld
KERNEL_DIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# module name
MODULE_NAME := xsched

# source code
KO_SRCS := xsched/xsched_ioctl.c xsched/xsched.c xsched/core.c xsched/rt.c
HAL_SRCS := hal/xcu_group.c hal/xsched_npu_interface.c hal/syms_lookup.c

# header file
DRIVER_PATH := /usr/local/Ascend
INCDIR += -I$(PWD)/include/
INCDIR += -I$(DRIVER_PATH)/driver/kernel/inc/driver/
INCDIR += -I$(DRIVER_PATH)/driver/kernel/rms/trs_drv/trs_core/
INCDIR += -I$(DRIVER_PATH)/driver/kernel/rms/trs_drv/inc
INCDIR += -I$(DRIVER_PATH)/driver/kernel/dev_inc/inc/
INCDIR += -I$(DRIVER_PATH)/driver/kernel/drv_davinci_intf_host/
INCDIR += -I$(DRIVER_PATH)/driver/kernel/tsch/
INCDIR += -I$(DRIVER_PATH)/driver/kernel/dev_inc/inc/dbl/
INCDIR += -I$(DRIVER_PATH)/driver/kernel/dbl/uda/
INCDIR += -I$(LIB_PATH)/include/

# kernel module
obj-m := $(MODULE_NAME).o
KO_OBJS := $(KO_SRCS:.c=.o)
HAL_OBJS := $(HAL_SRCS:.c=.o)
$(MODULE_NAME)-objs := $(KO_OBJS) $(HAL_OBJS)

# compilation flag
# remove -g flag to exclude debug symbols
EXTRA_CFLAGS += $(INCDIR) -Wall -Wno-unused-function -g
CFLAGS := $(INCDIR) -Wall -g

# default
all: ko test

# compile kernel module
ko:
	@echo "Building kernel module $(MODULE_NAME)..."
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" modules
	@echo "Kernel module build complete."

# clean
clean:
	@echo "Cleaning up..."
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -rf test/$(TEST_NAME)
	rm -f *.o *.ko *.mod.c *.mod.o *.symvers *.order .*.cmd *.so
	rm -f xsched/*.o xsched/*.mod.c xsched/.*.cmd
	@echo "Clean complete."

# install kernel module
install: ko
	@echo "Installing kernel module..."
	sudo insmod $(MODULE_NAME).ko
	@echo "Module installed."

# uninstall kernel module
uninstall:
	@echo "Uninstalling kernel module..."
	sudo rmmod $(MODULE_NAME) 2>/dev/null || true
	@echo "Module uninstalled."

# recompile and install
reinstall: uninstall clean ko install

.PHONY: all ko clean install uninstall reinstall
