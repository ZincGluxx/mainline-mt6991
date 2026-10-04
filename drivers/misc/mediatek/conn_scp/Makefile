# SPDX-License-Identifier: GPL-2.0
#
# Connectivity (connsys) <-> SCP bridge.  Builds connscp.ko from the vendor
# sources.  (The vendor aolextra/ and aoltest/ parts do not exist in this tree
# and are omitted.)

obj-$(CONFIG_MTK_CONN_SCP) += connscp.o

connscp-y := conn_scp.o \
	     conap_scp/msg_thread.o \
	     conap_scp/conap_scp_core.o \
	     conap_scp/conap_scp_ipi.o \
	     conap_scp/conap_platform_data.o \
	     aolserv/aol_buf_list.o \
	     aolserv/aol_flp.o \
	     aolserv/aol_geofence.o \
	     bleoffload/bleoffload_core.o \
	     bleoffload/bleoffload_netlink.o \
	     aolextra/aolextra_core.o \
	     aolextra/aolextra_netlink.o \
	     aolextra/aolextra_ring_buffer.o

subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/scp/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/connectivity/common
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/connectivity/connadp/common
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/conn_scp/conap_scp
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/conn_scp/aolserv
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/conn_scp/bleoffload
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/conn_scp/aolextra
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
