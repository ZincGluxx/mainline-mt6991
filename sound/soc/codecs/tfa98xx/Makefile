# SPDX-License-Identifier: GPL-2.0
# NXP TFA98xx (TFA9874) smart speaker amplifier, ported from the vendor tree.
snd-soc-tfa98xx-objs := src/tfa98xx.o \
			src/tfa_container.o \
			src/tfa_dsp.o \
			src/tfa_init.o

obj-$(CONFIG_SND_SOC_TFA9874) += snd-soc-tfa98xx.o

ccflags-y += -I$(srctree)/sound/soc/codecs/tfa98xx/inc
ccflags-y += -I$(srctree)/sound/soc/mediatek/common
ccflags-y += -DTFA_NON_DSP_SOLUTION
