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

#ifndef __DISP_LOG_H__
#define __DISP_LOG_H__
#include <mt-plat/aee.h>
#include <linux/printk.h>
#include "display_recorder.h"
#include "disp_debug.h"

/*  */


#define DISPFUNC()                                                     \
	do {                                                           \
		dprec_logger_pr(DPREC_LOGGER_DEBUG,                    \
			"func|%s\n", __func__);                        \
		if (g_mobilelog)                                       \
			pr_debug("[DISP]func|%s\n", __func__);         \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#define DISPERR(fmt, args...)                                          \
	do {                                                           \
		dprec_logger_pr(DPREC_LOGGER_ERROR, fmt, ##args);      \
		if (g_mobilelog)                                       \
			pr_err("[DISP]"fmt, ##args);                   \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#define DISPMSG(fmt, args...)                                          \
	do {                                                           \
		dprec_logger_pr(DPREC_LOGGER_DEBUG, fmt, ##args);      \
		if (g_mobilelog)                                       \
			pr_debug("[DISP]"fmt, ##args);                 \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#define DISPIRQ(fmt, args...)                                          \
	do {                                                           \
		if (g_loglevel > 0)                                    \
			pr_debug("[DISP]IRQ: "fmt, ##args);            \
		if (g_mobilelog)                                       \
			pr_debug("[DISP]IRQ: "fmt, ##args);            \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#define DISPRCD(fmt, args...)                                          \
	do {                                                           \
		if (g_rcdlevel > 0)                                    \
			dprec_logger_pr(DPREC_LOGGER_DEBUG,            \
				fmt, ##args);                          \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#define DISPINFO(fmt, args...)                                         \
	do {                                                           \
		if (g_mobilelog)                                       \
			pr_debug("[DISP]"fmt, ##args);                 \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#define DISPDMP(fmt, ...)                                              \
	do {                                                           \
		if (ddp_debug_analysis_to_buffer()) {                  \
			char log[512] = {'\0'};                        \
			scnprintf(log, 511, fmt, ##__VA_ARGS__);       \
			dprec_logger_dump(log);                        \
		} else {                                               \
			dprec_logger_pr(DPREC_LOGGER_DUMP,             \
					fmt, ##__VA_ARGS__);           \
			if (g_mobilelog)                               \
				pr_debug("[DISP]"fmt,                  \
					 ##__VA_ARGS__);               \
		}                                                      \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#define DISPPR_ERROR(fmt, args...)                                     \
	do {                                                           \
		dprec_logger_pr(DPREC_LOGGER_ERROR,                    \
				fmt,                                   \
				##args);                               \
		pr_err("[DISP]ERROR:"fmt, ##args);	               \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#define DISPPR_FENCE(fmt, args...)                                     \
	do {                                                           \
		dprec_logger_pr(DPREC_LOGGER_FENCE,                    \
				fmt,                                   \
				##args);                               \
		if (g_mobilelog)                                       \
			pr_debug("[DISP]fence/"fmt, ##args);           \
	} while (0)

/*
 *
 *
 *
 *
 *
 */
#ifndef ASSERT
#define ASSERT(expr)					               \
	do {						               \
		if (expr)				               \
			break;				               \
		pr_err("[DISP]ASSERT FAILED %s, %d\n",	               \
			__FILE__, __LINE__); BUG();	               \
	} while (0)
#endif

/*
 *
 *
 *
 *
 *
 */
#define DISPAEE(fmt, args...)                                          \
	do {                                                           \
		char str[200];                                         \
		snprintf(str, 199, "DISP:"fmt, ##args);                \
		DISPERR(fmt, ##args);                                  \
		aee_kernel_warning_api(__FILE__, __LINE__,             \
			DB_OPT_DEFAULT | DB_OPT_MMPROFILE_BUFFER,      \
			str, fmt, ##args);                             \
		pr_err("[DISP]"fmt, ##args);                           \
	} while (0)

#endif /* __DISP_LOG_H__ */
