# SPDX-License-Identifier: GPL-2.0
#
# SCP sensorhub liveness probe.  Talks to the sensorhub middleware inside
# the stock scp.img over the scp.ko IPI channels, using the frame layouts of
# the vendor stack (drivers/misc/mediatek/sensor/2.0/sensorhub/).

obj-m += sensorhub_probe.o

subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/scp/include
