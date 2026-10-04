// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2021 MediaTek Inc.
// Author: Owen Chen <owen.chen@mediatek.com>
//
// Built-in fmeter dispatch layer for the vendor MT6991 fmeter provider
// (clk-mt6991-drv.ko).  It is kept in vmlinux so that both built-in callers
// (the uarthub debug driver) and loadable modules (scp.ko) can resolve
// mt_get_fmeter_freq()/mt_get_fmeter_id() irrespective of provider module
// load order.  The provider registers its ops at runtime via
// mt6991_fmeter_set_ops().

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "mt6991/clk-fmeter-vendor.h"

static const struct fmeter_ops *fm_ops;

const struct fmeter_clk *mt6991_get_fmeter_clks(void)
{
	if (fm_ops == NULL || fm_ops->get_fmeter_clks == NULL)
		return NULL;

	return  fm_ops->get_fmeter_clks();
}
EXPORT_SYMBOL(mt6991_get_fmeter_clks);

unsigned int mt6991_get_ckgen_freq(unsigned int id)
{
	if (fm_ops == NULL || fm_ops->get_ckgen_freq == NULL)
		return 0;

	return  fm_ops->get_ckgen_freq(id);
}
EXPORT_SYMBOL(mt6991_get_ckgen_freq);

unsigned int mt6991_get_abist_freq(unsigned int id)
{
	if (fm_ops == NULL || fm_ops->get_abist_freq == NULL)
		return 0;

	return  fm_ops->get_abist_freq(id);
}
EXPORT_SYMBOL(mt6991_get_abist_freq);

unsigned int mt6991_get_abist2_freq(unsigned int id)
{
	if (fm_ops == NULL || fm_ops->get_abist2_freq == NULL)
		return 0;

	return  fm_ops->get_abist2_freq(id);
}
EXPORT_SYMBOL(mt6991_get_abist2_freq);

unsigned int mt6991_get_vlpck_freq(unsigned int id)
{
	if (fm_ops == NULL || fm_ops->get_vlpck_freq == NULL)
		return 0;

	return  fm_ops->get_vlpck_freq(id);
}
EXPORT_SYMBOL(mt6991_get_vlpck_freq);

unsigned int mt6991_get_subsys_freq(unsigned int id)
{
	if (fm_ops == NULL || fm_ops->get_subsys_freq == NULL)
		return 0;

	return  fm_ops->get_subsys_freq(id);
}
EXPORT_SYMBOL(mt6991_get_subsys_freq);

unsigned int mt6991_get_fmeter_freq(unsigned int id, enum  FMETER_TYPE type)
{
	unsigned int r;

	if (fm_ops == NULL || fm_ops->get_fmeter_freq == NULL) {
		pr_notice("[fmeter-dbg] no ops (id=%u type=%u)\n", id, type);
		return 0;
	}

	r = fm_ops->get_fmeter_freq(id, type);
	pr_notice("[fmeter-dbg] id=%u type=%u -> %u\n", id, type, r);
	return r;
}
EXPORT_SYMBOL(mt6991_get_fmeter_freq);

int mt6991_get_fmeter_id(enum FMETER_ID fid)
{
	if (fm_ops == NULL || fm_ops->get_fmeter_id == NULL)
		return FID_NULL;

	return  fm_ops->get_fmeter_id(fid);
}
EXPORT_SYMBOL(mt6991_get_fmeter_id);

int mt6991_subsys_freq_register(struct fm_subsys *fm, unsigned int size)
{
	if (fm_ops == NULL || fm_ops->subsys_freq_register == NULL)
		return -EINVAL;

	return  fm_ops->subsys_freq_register(fm, size);
}
EXPORT_SYMBOL(mt6991_subsys_freq_register);

void mt6991_fmeter_set_ops(const struct fmeter_ops *ops)
{
	fm_ops = ops;
}
EXPORT_SYMBOL(mt6991_fmeter_set_ops);

/*
 * Vendor-namespace entry points used by the SCP and uarthub drivers.
 */
unsigned int mt_get_fmeter_freq(unsigned int id, enum FMETER_TYPE type)
{
	return mt6991_get_fmeter_freq(id, type);
}
EXPORT_SYMBOL(mt_get_fmeter_freq);

int mt_get_fmeter_id(enum FMETER_ID fid)
{
	return mt6991_get_fmeter_id(fid);
}
EXPORT_SYMBOL(mt_get_fmeter_id);
