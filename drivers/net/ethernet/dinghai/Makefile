subdir-ccflags-y += -I$(src)
subdir-ccflags-y += -I$(srctree)/include/linux/dinghai/
#subdir-ccflags-y += -I$(CWD)/include/
#subdir-ccflags-y += -I$(CWD)drivers/net/ethernet/zxdh/include/
#subdir-ccflags-y += -include $(CWD)/autoconf.h
ccflags-y += -Werror


ifeq ($(CONFIG_ZXDH_MSGQ),m)
EXTRA_CFLAGS += -DZXDH_MSGQ
endif

ifeq ($(CONFIG_DINGHAI_SEC),m)
EXTRA_CFLAGS += -DZXDH_SEC
endif

ifeq ($(CONFIG_DINGHAI_PTP),m)
EXTRA_CFLAGS += -DPTP_DRIVER_INTERFACE_EN
endif

#ifneq ($(CONFIG_DRIVER_VERSION),)
#    EXTRA_CFLAGS += -DDRIVER_VERSION_VAL=\"$(CONFIG_DRIVER_VERSION)\"
#    $(info CONFIG_PF_MPF_DRIVER_VERSION is $(CONFIG_DRIVER_VERSION),\
#      EXTRA_CFLAGS=$(EXTRA_CFLAGS))
#else
#    VERSION_FILE := $(shell cat $(CWD)/drivers/version.ini 2>/dev/null)
#    ifneq ($(VERSION_FILE),)
#        EXTRA_CFLAGS += -DDRIVER_VERSION_VAL=\"$(VERSION_FILE)\"
#        $(info version.ini is $(VERSION_FILE), EXTRA_CFLAGS=$(EXTRA_CFLAGS))
#    else
#        EXTRA_CFLAGS += -DDRIVER_VERSION_VAL=\"1.0-1\"
#        $(info Using default version: 1.0-1, EXTRA_CFLAGS=$(EXTRA_CFLAGS))
#    endif
#endif

ifeq ($(CONFIG_DRIVER_VERSION),)
EXTRA_CFLAGS += -DDRIVER_VERSION_VAL=\"1.0-1\"
$(info CONFIG_PF_MPF_DRIVER_VERSION is null, EXTRA_CFLAGS=$(EXTRA_CFLAGS))
else
EXTRA_CFLAGS += -DDRIVER_VERSION_VAL=\"$(CONFIG_DRIVER_VERSION)\"
$(info CONFIG_PF_MPF_DRIVER_VERSION is not null, EXTRA_CFLAGS=$(EXTRA_CFLAGS))
endif

obj-$(CONFIG_DINGHAI_DH_CMD) += dinghai10e_cmd.o
dinghai10e_cmd-y := dh_cmd.o  cmd/msg_main.o cmd/msg_chan_lock.o cmd/msg_chan_test.o log.o

#obj-$(CONFIG_DINGHAI_MPF) += dinghai10e_mpf.o
#dinghai10e_mpf-y := events.o eq.o en_mpf.o pci_irq.o en_mpf/en_mpf_irq.o en_mpf/en_mpf_events.o en_mpf/en_mpf_eq.o devlink.o \
              en_mpf/en_mpf_devlink.o en_mpf/en_mpf_cfg_sf.o
#dinghai10e_mpf-$(CONFIG_ZXDH_SF) += irq_affinity.o en_sf.o en_sf/en_sf_eq.o en_sf/en_sf_irq.o en_sf/en_sf_devlink.o

#obj-$(CONFIG_DINGHAI_ZF_MPF) += dinghai10e_zf_mpf.o

#ifdef PCIE_ZF_EPC_OPEN
#dinghai10e_zf_mpf-y := events.o eq.o pci_irq.o devlink.o \
                zf_mpf/zf_mpf.o zf_mpf/zf_events.o zf_mpf/zf_mpf_irq.o zf_mpf/zf_mpf_eq.o zf_mpf/zf_mpf_devlink.o zf_mpf/zf_mpf_cfg_sf.o log.o \
                zf_mpf/epc/pcie-zte-zf-epc.o zf_mpf/epc/pcie-zte-zf-hdma.o zf_mpf/epc/virt-dma.o zf_mpf/zxdh_reset_zf.o zf_mpf/epc/pcie-zte-zf-json.o\
                zf_mpf/fuc_hotplug/fuc_hotplug_ioctl.o zf_mpf/fuc_hotplug/fuc_hotplug.o zf_mpf/zf_reset_finish_flag.o zf_mpf/zf_chan_ioctl.o
#obj-m += dinghai10e_zf_epf.o
#my_api-objs := zf_mpf/epc/pcie-zte-zf-epc.o
#dinghai10e_zf_epf-y := zf_mpf/epf/pcie-zte-zf-epf.o
#else
#dinghai10e_zf_mpf-y := events.o eq.o pci_irq.o devlink.o \
                zf_mpf/zf_mpf.o zf_mpf/zf_events.o zf_mpf/zf_mpf_irq.o zf_mpf/zf_mpf_eq.o zf_mpf/zf_mpf_devlink.o zf_mpf/zf_mpf_cfg_sf.o zf_mpf/zxdh_reset_zf.o \
                zf_mpf/fuc_hotplug/fuc_hotplug_ioctl.o zf_mpf/fuc_hotplug/fuc_hotplug.o zf_mpf/zf_chan_ioctl.o log.o
#endif
#dinghai10e_zf_mpf-$(CONFIG_ZXDH_SF) += irq_affinity.o en_sf.o en_sf/en_sf_eq.o en_sf/en_sf_irq.o en_sf/en_sf_devlink.o
#dinghai10e_zf_mpf-$(CONFIG_ZF_GDMA) += zf_mpf/gdma.o

obj-$(CONFIG_DINGHAI_PF) += dinghai10e.o
dinghai10e-y := events.o en_pf.o eq.o pci_irq.o en_pf/en_pf_irq.o en_pf/en_pf_eq.o devlink.o en_pf/en_pf_devlink.o en_pf/en_pf_events.o \
    en_pf/msg_func.o dh_procfs.o lag/lag.o lag/lag_procfs.o plcr.o sriov_sysfs.o xarray.o health.o log.o slib.o

dinghai10e-$(CONFIG_ZXDH_SF) += irq_affinity.o en_sf.o en_sf/en_sf_eq.o en_sf/en_sf_irq.o en_sf/en_sf_devlink.o

dinghai10e-$(CONFIG_DINGHAI_EN_AUX) += en_aux.o eq.o pci_irq.o irq_affinity.o en_aux/queue.o en_aux/en_aux_cmd.o en_aux/en_aux_eq.o \
                 en_aux/en_aux_events.o en_ethtool/ethtool.o en_aux/en_aux_ioctl.o \
                 en_aux/dcbnl/en_dcbnl.o en_aux/dcbnl/en_dcbnl_api.o \
                 zxdh_tools/zxdh_tools_ioctl.o zxdh_tools/zxdh_tools_netlink.o \
                 en_aux/en_1588_pkt_proc.o en_aux/en_1588_pkt_proc_func.o xarray.o en_aux/selq.o log.o slib.o
dinghai10e-y += en_aux/drs_sec_dtb.o
dinghai10e-y += en_aux/priv_queue.o
dinghai10e-y += bonding/zxdh_lag.o bonding/rdma_ops.o

obj-$(CONFIG_DINGHAI_PTP)  += dinghai10e_ptp.o
dinghai10e_ptp-y :=en_ptp/tod_driver.o en_ptp/tod_driver_stub.o en_ptp/zxdh_ptp.o log.o

obj-$(CONFIG_DINGHAI_TSN_M)  += dinghai10e_tsn.o
dinghai10e_tsn-y :=en_tsn/zxdh_tsn.o en_tsn/zxdh_tsn_reg.o en_tsn/zxdh_tsn_ioctl.o log.o

include $(src)/en_np/Makefile
#include $(CWD)drivers/net/ethernet/zxdh/drivers/net/ethernet/dinghai/en_np/Makefile

