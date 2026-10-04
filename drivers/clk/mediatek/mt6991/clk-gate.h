/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: James Liao <jamesjj.liao@mediatek.com>
 */

#ifndef __DRV_CLK_GATE_H
#define __DRV_CLK_GATE_H

#include <linux/regmap.h>
#include <linux/clk-provider.h>

struct clk;

struct mt6991_clk_gate {
	struct clk_hw	hw;
	unsigned long	flags;
	struct regmap	*regmap;
	struct regmap	*hwv_regmap;
	int		set_ofs;
	int		clr_ofs;
	int		sta_ofs;
	int		hwv_set_ofs;
	int		hwv_clr_ofs;
	int		hwv_sta_ofs;
	u8		bit;
};

struct mt6991_gate_regs {
	u32 sta_ofs;
	u32 clr_ofs;
	u32 set_ofs;
};

struct mt6991_gate {
	int id;
	const char *name;
	const char *parent_name;
	const char *hwv_comp;
	const struct mt6991_gate_regs *regs;
	const struct mt6991_gate_regs *hwv_regs;
	int shift;
	const struct clk_ops *ops;
	const struct clk_ops *dma_ops;
	unsigned long flags;
};

static inline struct mt6991_clk_gate *to_mtk_clk_gate(struct clk_hw *hw)
{
	return container_of(hw, struct mt6991_clk_gate, hw);
}

extern const struct clk_ops mt6991_clk_gate_ops_setclr_dummys;
extern const struct clk_ops mt6991_clk_gate_ops_setclr_dummy;
extern const struct clk_ops mt6991_clk_gate_ops_hwv;
extern const struct clk_ops mt6991_clk_gate_ops_hwv_inv;
extern const struct clk_ops mt6991_clk_gate_ops_hwv_dummy;
extern const struct clk_ops mt6991_clk_gate_ops_setclr;
extern const struct clk_ops mt6991_clk_gate_ops_setclr_inv;
extern const struct clk_ops mt6991_clk_gate_ops_setclr_inv_dummy;
extern const struct clk_ops mt6991_clk_gate_ops_no_setclr;
extern const struct clk_ops mt6991_clk_gate_ops_no_setclr_inv;

struct clk *mt6991_clk_register_gate_hwv(
		const struct mt6991_gate *gate,
		struct regmap *regmap,
		struct regmap *hwv_regmap,
		struct device *dev);

struct clk *mt6991_clk_register_gate(
		const struct mt6991_gate *gate,
		struct regmap *regmap,
		struct device *dev);

int mt6991_clk_register_gates(struct device_node *node,
			const struct mt6991_gate *clks, int num,
			struct clk_onecell_data *clk_data);

int mt6991_clk_register_gates_with_dev(struct device_node *node,
		const struct mt6991_gate *clks,
		int num, struct clk_onecell_data *clk_data,
		struct device *dev);

#define GATE_MTK_FLAGS(_id, _name, _parent, _regs, _shift,	\
			_ops, _flags) {				\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = _regs,					\
		.shift = _shift,				\
		.ops = _ops,					\
		.flags = _flags,				\
	}

#define GATE_MTK(_id, _name, _parent, _regs, _shift, _ops)		\
	GATE_MTK_FLAGS(_id, _name, _parent, _regs, _shift, _ops, 0)

#endif /* __DRV_CLK_GATE_H */
