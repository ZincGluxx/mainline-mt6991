/* SPDX-License-Identifier: GPL-2.0 */
/* Minimal AEE-mode shim for the ported SCP driver (no AEE framework here). */
#ifndef __AED_H__
#define __AED_H__

enum AEE_MODE {
	AEE_MODE_MTK_ENG = 1,
	AEE_MODE_MTK_USER,
	AEE_MODE_CUSTOMER_ENG,
	AEE_MODE_CUSTOMER_USER,
	AEE_MODE_NOT_INIT
};

extern int aee_get_mode(void);

#endif /* __AED_H__ */
