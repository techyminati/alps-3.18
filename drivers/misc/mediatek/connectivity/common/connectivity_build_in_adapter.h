/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#ifndef CONNECTIVITY_BUILD_IN_ADAPTER_H
#define CONNECTIVITY_BUILD_IN_ADAPTER_H

#ifdef CONFIG_ARCH_MT6755
#define CPU_BOOST y
#endif
#ifdef CONFIG_ARCH_MT6757
#define CPU_BOOST y
#endif
#ifdef CONFIG_ARCH_MT6797
#define CPU_BOOST y
#endif

#ifdef CPU_BOOST
#include "mach/mt_ppm_api.h"
#endif

#define KERNEL_show_stack connectivity_export_show_stack
#define KERNEL_tracing_record_cmdline connectivity_export_tracing_record_cmdline

#ifdef CPU_BOOST
#define KERNEL_mt_ppm_sysboost_freq connectivity_export_mt_ppm_sysboost_freq
#define KERNEL_mt_ppm_sysboost_core connectivity_export_mt_ppm_sysboost_core
#define KERNEL_mt_ppm_sysboost_set_core_limit connectivity_export_mt_ppm_sysboost_set_core_limit
#else
#define KERNEL_mt_ppm_sysboost_freq
#define KERNEL_mt_ppm_sysboost_core
#define KERNEL_mt_ppm_sysboost_set_core_limit
#endif

void connectivity_export_show_stack(struct task_struct *tsk, unsigned long *sp);
void connectivity_export_tracing_record_cmdline(struct task_struct *tsk);
#ifdef CPU_BOOST
void connectivity_export_mt_ppm_sysboost_freq(enum ppm_sysboost_user user, unsigned int freq);
void connectivity_export_mt_ppm_sysboost_core(enum ppm_sysboost_user user, unsigned int core_num);
void connectivity_export_mt_ppm_sysboost_set_core_limit(enum ppm_sysboost_user user, unsigned int cluster,
					int min_core, int max_core);
#endif

extern void tracing_record_cmdline(struct task_struct *tsk);
extern void show_stack(struct task_struct *tsk, unsigned long *sp);

/*********************************************
 * copy from
 * kernel-3.18/include/linux/ftrace_event.h
 * kernel-4.4/include/linux/trace_events.h
 *
 * event_trace_printk()
 *********************************************/

#define KERNEL_event_trace_printk(ip, fmt, args...)               \
do {                                                              \
	__trace_printk_check_format(fmt, ##args);                 \
	KERNEL_tracing_record_cmdline(current);                   \
	if (__builtin_constant_p(fmt)) {                          \
		static const char *trace_printk_fmt               \
		__attribute__((section("__trace_printk_fmt"))) =  \
		__builtin_constant_p(fmt) ? fmt : NULL;           \
		__trace_bprintk(ip, trace_printk_fmt, ##args);    \
	} else                                                    \
		__trace_printk(ip, fmt, ##args);                  \
} while (0)
#endif /* CONNECTIVITY_BUILD_IN_ADAPTER_H */
