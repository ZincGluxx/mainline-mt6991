// SPDX-License-Identifier: GPL-2.0
/*
 * Compatibility shims for the ported downstream MT6991 SCPSYS driver.
 *
 * The downstream mtk-scpsys.c calls clkchk_external_dump() from its power-on
 * error path.  clkchk is part of the *vendor* clock framework (drivers/clk/
 * mediatek/clkchk*.c), which is not part of this mainline port: MT6991 here
 * runs on the upstream mt8196 clock drivers, so the vendor clkchk ops can
 * never be registered.  Provide the hook as a no-op instead of dragging in
 * the vendor clock stack.
 */

#include <linux/kernel.h>

void clkchk_external_dump(void)
{
}
