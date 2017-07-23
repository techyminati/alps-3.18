/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __DDP_DEBUG_H__
#define __DDP_DEBUG_H__

#include <linux/kernel.h>
#include "ddp_mmp.h"
#include "ddp_dump.h"

extern unsigned int gEnableMutexRisingEdge;
extern unsigned int gPrefetchControl;
extern unsigned int gOVLBackground;
extern unsigned int gUltraEnable;
extern unsigned int gEnableDSIStateCheck;
extern unsigned int gMutexFreeRun;
extern unsigned int disp_low_power_lfr;

void ddp_debug_init(void);
void ddp_debug_exit(void);

unsigned int ddp_debug_analysis_to_buffer(void);
unsigned int ddp_debug_dbg_log_level(void);
unsigned int ddp_debug_irq_log_level(void);
unsigned int ddp_dump_reg_to_buf(unsigned int start_module, unsigned long *addr);

int ddp_mem_test(void);
int ddp_lcd_test(void);

#endif /* __DDP_DEBUG_H__ */
