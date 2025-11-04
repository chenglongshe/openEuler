# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux kernel UB device drivers.
#
ccflags-y += -I$(src)

ubcore-objs := ubcore_log.o \
			ubcm/ubcm_log.o \
			ubcm/ub_mad.o \
			ubcm/ubcm_genl.o \
			ubcm/ub_cm.o \
			net/ubcore_session.o \
			net/ubcore_net.o \
			net/ubcore_sock.o \
			net/ubcore_cm.o \
			ubcm/ubmad_datapath.o \
			ubcore_topo_info.o \
			ubcore_cgroup.o \
			ubcore_dp.o \
			ubcore_hash_table.o \
			ubcore_netdev.o \
			ubcore_umem.o \
			ubcore_workqueue.o \
			ubcore_msg.o \
			ubcore_tp.o \
			ubcore_tp_table.o \
			ubcore_vtp.o \
			ubcore_cmd_tlv.o \
			ubcore_netlink.o \
			ubcore_segment.o \
			ubcore_uvs_cmd.o \
			ubcore_connect_adapter.o \
			ubcore_connect_bonding.o \
			ubcore_cdev_file.o \
			ubcore_device.o \
			ubcore_genl.o \
			ubcore_genl_admin.o \
			ubcore_jetty.o \
			ubcore_main.o \
			ubcore_ctp.o \
			ubcore_tpg.o \
			ubcore_utp.o \
			ubcore_uvs.o

obj-$(CONFIG_UB_URMA) += ubcore.o
