// SPDX-License-Identifier: GPL-2.0
/*
 * Mainline compatibility stubs for the ported MT6991 CCCI/ECCCI driver.
 *
 * These symbols are provided by vendor-specific subsystems (project config,
 * the AEE mrdump extra-file mechanism and the pinctrl tristate debug helper)
 * that are not part of the mainline port.  The callers treat the failure
 * returns as "feature unavailable", and none of them gate modem bring-up.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/types.h>

int set_rps_map(struct netdev_rx_queue *queue, unsigned long rps_value)
{
	return -ENOTSUPP;
}

unsigned int is_project(int project)
{
	return 0;
}

int gpio_get_tristate_input(unsigned int pin)
{
	return -ENOTSUPP;
}

int mrdump_mini_add_extra_file(unsigned long vaddr, unsigned long paddr,
	unsigned long size, const char *name)
{
	return -ENOTSUPP;
}
