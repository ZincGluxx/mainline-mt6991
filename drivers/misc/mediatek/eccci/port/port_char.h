/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2016 MediaTek Inc.
 */
#ifndef __PORT_CHAR__
#define __PORT_CHAR__
#include "ccci_core.h"
#include "port_t.h"
/* External API called by port_char object */
extern int rawbulk_push_upstream_buffer(int transfer_id, const void *buffer,
		unsigned int length);
extern void ccci_fs_handles_reset(void);
/* CCCI UART2/AT port tty front-end (port_tty.c) */
extern int ccci_tty_port_register(struct port_t *port);
extern bool ccci_tty_is_open(void);
extern void ccci_tty_rx_skb(struct port_t *port, struct sk_buff *skb);
#endif	/*__PORT_CHAR__*/
