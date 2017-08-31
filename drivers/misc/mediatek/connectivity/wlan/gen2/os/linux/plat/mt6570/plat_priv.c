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
#include <linux/kernel.h>
#include "mach/mt_lbc.h"
#include "gl_typedef.h"

#define CLUSTER_NUM	1
#define MAX_CPU_FREQ	819000

INT32 kalBoostCpu(UINT_32 core_num)
{
	UINT_32 cpu_num;
	struct ppm_limit_data core_to_set, freq_to_set;

	pr_warn("enter kalBoostCpu, core_num:%d\n", core_num);
	cpu_num = core_num;
	if (cpu_num > 4)
		cpu_num = 4;

	freq_to_set.max = -1;	/* -1 means don't care */
	core_to_set.max = -1;	/* -1 means don't care */
	if (cpu_num != 0) {
		core_to_set.min = cpu_num;
		freq_to_set.min = MAX_CPU_FREQ;
	} else {
		core_to_set.min = -1;	/* -1 means don't care */
		freq_to_set.min = -1;	/* -1 means don't care */
	}
	pr_warn("update userlimit with cpuNum=%d freq=%d\n", core_to_set.min, freq_to_set.min);

	update_userlimit_cpu_freq(PPM_KIR_WIFI, CLUSTER_NUM, &core_to_set);
	update_userlimit_cpu_core(PPM_KIR_WIFI, CLUSTER_NUM, &freq_to_set);
	return 0;
}
