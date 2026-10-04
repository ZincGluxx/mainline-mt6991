// SPDX-License-Identifier: GPL-2.0
/* Stand-in for the AEE framework query used by scp_excep.c. */
#include <linux/kernel.h>
#include "aed.h"

int aee_get_mode(void)
{
	return AEE_MODE_NOT_INIT;
}
EXPORT_SYMBOL(aee_get_mode);
