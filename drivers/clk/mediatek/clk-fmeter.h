/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CLK_FMETER_H
#define __CLK_FMETER_H

enum FMETER_TYPE {
	FT_NULL,
	ABIST,
	CKGEN,
	ABIST_2,
	ABIST_CK2,
	CKGEN_CK2,
	SUBSYS,
	VLPCK,
};

enum FMETER_ID {
	FID_NULL = -1,
	FID_DISP_PWM = 0,
	FID_ULPOSC1,
	FID_ULPOSC2,
	FID_NUM,
};

/*
 * The MT6991 fmeter dispatch layer is built into vmlinux
 * (clk-fmeter-mt6991-core.c); the actual provider (clk-mt6991-drv.ko)
 * registers its ops at runtime.  Both built-in callers (uarthub debug) and
 * loadable modules (scp.ko) use these symbols.
 */
unsigned int mt_get_fmeter_freq(unsigned int id, enum FMETER_TYPE type);
int mt_get_fmeter_id(enum FMETER_ID fid);

#endif
