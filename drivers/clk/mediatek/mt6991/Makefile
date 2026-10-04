# SPDX-License-Identifier: GPL-2.0
#
# Self-contained vendor MT6991 clock tree (cksys / vlp_cksys), ported from the
# vendor 6.6 tree.  The vendor clk framework is kept private under the mt6991_
# namespace so it does not clash with the mainline MTK clk framework.
obj-$(CONFIG_CLK_MT6991) += clk-mt6991-drv.o

clk-mt6991-drv-y := clk-mt6991.o \
		    clk-mux.o \
		    clk-mtk.o \
		    clk-gate.o \
		    clk-pll.o \
		    clk-mt6991-adsp.o \
		    clk-fmeter-mt6991.o

ccflags-y += -I$(srctree)/drivers/misc/mediatek/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/scp/include
