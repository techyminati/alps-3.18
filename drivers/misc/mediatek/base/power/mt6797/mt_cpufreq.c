/*
 * @file    mt_cpufreq.c
 * @brief   Driver for CPU DVFS
 *
 */

#define __MT_CPUFREQ_C__

/* system includes */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/bitops.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <linux/suspend.h>
#include <linux/topology.h>
#include <mt-plat/sync_write.h>
#include <mt-plat/mt_io.h>
#include <mt-plat/aee.h>

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_address.h>
#endif

/* project includes */
#include "mach/mt_freqhopping.h"
/* #include "mach/mt_ptp.h" */
/* #include "mach/mt_static_power.h" */
#include <mt-plat/upmu_common.h>
#include <mach/upmu_sw.h>
#include <mach/upmu_hw.h>
#include "mach/mt_ppm_api.h"
#include "mach/mt_pbm.h"


/* local includes */
#include "../../../power/mt6797/da9214.h"
#include "mt_cpufreq.h"
#include "mt_idvfs.h"
#include "mt_cpufreq_hybrid.h"

#define DCM_ENABLE 1
#ifdef DCM_ENABLE
#include "mt_dcm.h"
#endif

/*=============================================================*/
/* Macro definition                                            */
/*=============================================================*/
#if defined(CONFIG_OF)
static unsigned long infracfg_ao_base;
static unsigned long topckgen_base;
static unsigned long mcumixed_base;

#define INFRACFG_AO_NODE	"mediatek,infracfg_ao"
#define TOPCKGEN_NODE		"mediatek,topckgen"
#define MCUMIXED_NODE		"mediatek,mcumixed"

#undef INFRACFG_AO_BASE
#undef TOPCKGEN_BASE
#undef MCUMIXED_BASE

#define INFRACFG_AO_BASE		(infracfg_ao_base)	/* 0xF0001000 */
#define TOPCKGEN_BASE			(topckgen_base)		/* 0xF0000000 */
#define MCUMIXED_BASE			(mcumixed_base)		/* 0xF000A000 */

#else				/* #if defined (CONFIG_OF) */
#undef INFRACFG_AO_BASE
#undef TOPCKGEN_BASE
#undef APMIXED_BASE

#define INFRACFG_AO_BASE        0xF0001000
#define TOPCKGEN_BASE           0xF0000000
#define MCUMIXED_BASE           0xF001A000
#endif				/* #if defined (CONFIG_OF) */

/* LL */
#define ARMCAXPLL0_CON0		(MCUMIXED_BASE+0x200)
#define ARMCAXPLL0_CON1		(MCUMIXED_BASE+0x204)
#define ARMCAXPLL0_CON2		(MCUMIXED_BASE+0x208)

/* L */
#define ARMCAXPLL1_CON0		(MCUMIXED_BASE+0x210)
#define ARMCAXPLL1_CON1		(MCUMIXED_BASE+0x214)
#define ARMCAXPLL1_CON2		(MCUMIXED_BASE+0x218)

/* CCI */
#define ARMCAXPLL2_CON0		(MCUMIXED_BASE+0x220)
#define ARMCAXPLL2_CON1		(MCUMIXED_BASE+0x224)
#define ARMCAXPLL2_CON2		(MCUMIXED_BASE+0x228)

/* Backup */
#define ARMCAXPLL3_CON0		(MCUMIXED_BASE+0x230)
#define ARMCAXPLL3_CON1		(MCUMIXED_BASE+0x234)
#define ARMCAXPLL3_CON2		(MCUMIXED_BASE+0x238)

/* ARMPLL DIV */
#define ARMPLLDIV_MUXSEL	(MCUMIXED_BASE+0x270)
#define ARMPLLDIV_CKDIV		(MCUMIXED_BASE+0x274)

/* INFRASYS Register */
/* L VSRAM */
#define CPULDO_CTRL_0		(INFRACFG_AO_BASE+0xF98)
/* 3:0, 4, 11:8, 12, 19:16, 20, 27:24, 28*/
#define CPULDO_CTRL_1		(INFRACFG_AO_BASE+0xF9C)
/* 3:0, 4, 11:8, 12, 19:16, 20, 27:24, 28*/
#define CPULDO_CTRL_2		(INFRACFG_AO_BASE+0xFA0)


/* TOPCKGEN Register */
#undef CLK_MISC_CFG_0
#define CLK_MISC_CFG_0          (TOPCKGEN_BASE + 0x104)

/*
 * CONFIG
 */
#define CONFIG_CPU_DVFS_SHOWLOG 1
/* #define CONFIG_CPU_DVFS_BRINGUP 1 */
#ifdef CONFIG_MTK_RAM_CONSOLE
#define CONFIG_CPU_DVFS_AEE_RR_REC 1
#endif

#ifdef CONFIG_HYBRID_CPU_DVFS
/**
 * 1: get voltage from DVFSP for performance
 * 2: get voltage from PMIC through I2C
 */
static u32 enable_cpuhvfs = 2;		/* FIXME */
#endif

#define MAX(a, b) ((a) >= (b) ? (a) : (b))
#define MIN(a, b) ((a) >= (b) ? (b) : (a))

/* used @ set_cur_volt_extBuck() */
/* #define MIN_DIFF_VSRAM_PROC        1000  */
#define NORMAL_DIFF_VRSAM_VPROC    10000
#define MAX_DIFF_VSRAM_VPROC       25000
#define MIN_VSRAM_VOLT             100000
#define MAX_VSRAM_VOLT             120000
#define MAX_VPROC_VOLT             120000

/* PMIC/PLL settle time (us) */
/* DA9214 */
#define PMIC_SETTLE_TIME(old_mv, new_mv) \
	(((((old_mv) > (new_mv)) ? ((old_mv) - (new_mv)) : ((new_mv) - (old_mv))) + 9) / 10)
/* SRAM LDO*/
#define PMIC_CMD_DELAY_TIME     5
#define MIN_PMIC_SETTLE_TIME    25
#define PMIC_VOLT_UP_SETTLE_TIME(old_volt, new_volt) \
	(((((new_volt) - (old_volt)) + 1250 - 1) / 1250) + PMIC_CMD_DELAY_TIME)
#define PMIC_VOLT_DOWN_SETTLE_TIME(old_volt, new_volt) \
	(((((old_volt) - (new_volt)) * 2)  / 625) + PMIC_CMD_DELAY_TIME)

#define PLL_SETTLE_TIME         (20)

/* 16 steps OPP table*/
#if 1
/* for DVFS OPP table LL/FY */
#define CPU_DVFS_FREQ0_LL_FY    (1391000)	/* KHz */
#define CPU_DVFS_FREQ1_LL_FY    (1339000)	/* KHz */
#define CPU_DVFS_FREQ2_LL_FY    (1287000)	/* KHz */
#define CPU_DVFS_FREQ3_LL_FY    (1222000)	/* KHz */
#define CPU_DVFS_FREQ4_LL_FY    (1118000)	/* KHz */
#define CPU_DVFS_FREQ5_LL_FY    (1066000)	/* KHz */
#define CPU_DVFS_FREQ6_LL_FY    (949000)	/* KHz */
#define CPU_DVFS_FREQ7_LL_FY    (897000)	/* KHz */
#define CPU_DVFS_FREQ8_LL_FY    (806000)	/* KHz */
#define CPU_DVFS_FREQ9_LL_FY    (715000)	/* KHz */
#define CPU_DVFS_FREQ10_LL_FY    (624000)	/* KHz */
#define CPU_DVFS_FREQ11_LL_FY    (559000)	/* KHz */
#define CPU_DVFS_FREQ12_LL_FY    (481000)	/* KHz */
#define CPU_DVFS_FREQ13_LL_FY    (416000)	/* KHz */
#define CPU_DVFS_FREQ14_LL_FY    (338000)	/* KHz */
#define CPU_DVFS_FREQ15_LL_FY    (221000)	/* KHz */

/* for DVFS OPP table L/FY */
#define CPU_DVFS_FREQ0_L_FY    (1950000)	/* KHz */
#define CPU_DVFS_FREQ1_L_FY    (1872000)	/* KHz */
#define CPU_DVFS_FREQ2_L_FY    (1794000)	/* KHz */
#define CPU_DVFS_FREQ3_L_FY    (1716000)	/* KHz */
#define CPU_DVFS_FREQ4_L_FY    (1573000)	/* KHz */
#define CPU_DVFS_FREQ5_L_FY    (1495000)	/* KHz */
#define CPU_DVFS_FREQ6_L_FY    (1352000)	/* KHz */
#define CPU_DVFS_FREQ7_L_FY    (1274000)	/* KHz */
#define CPU_DVFS_FREQ8_L_FY    (1144000)	/* KHz */
#define CPU_DVFS_FREQ9_L_FY    (1014000)	/* KHz */
#define CPU_DVFS_FREQ10_L_FY    (871000)	/* KHz */
#define CPU_DVFS_FREQ11_L_FY    (780000)	/* KHz */
#define CPU_DVFS_FREQ12_L_FY    (689000)	/* KHz */
#define CPU_DVFS_FREQ13_L_FY    (598000)	/* KHz */
#define CPU_DVFS_FREQ14_L_FY    (494000)	/* KHz */
#define CPU_DVFS_FREQ15_L_FY    (338000)	/* KHz */

/* for DVFS OPP table CCI/FY */
#define CPU_DVFS_FREQ0_CCI_FY    (975000)	/* KHz */
#define CPU_DVFS_FREQ1_CCI_FY    (936000)	/* KHz */
#define CPU_DVFS_FREQ2_CCI_FY    (897000)	/* KHz */
#define CPU_DVFS_FREQ3_CCI_FY    (858000)	/* KHz */
#define CPU_DVFS_FREQ4_CCI_FY    (793000)	/* KHz */
#define CPU_DVFS_FREQ5_CCI_FY    (754000)	/* KHz */
#define CPU_DVFS_FREQ6_CCI_FY    (689000)	/* KHz */
#define CPU_DVFS_FREQ7_CCI_FY    (624000)	/* KHz */
#define CPU_DVFS_FREQ8_CCI_FY    (559000)	/* KHz */
#define CPU_DVFS_FREQ9_CCI_FY    (494000)	/* KHz */
#define CPU_DVFS_FREQ10_CCI_FY    (429000)	/* KHz */
#define CPU_DVFS_FREQ11_CCI_FY    (390000)	/* KHz */
#define CPU_DVFS_FREQ12_CCI_FY    (338000)	/* KHz */
#define CPU_DVFS_FREQ13_CCI_FY    (299000)	/* KHz */
#define CPU_DVFS_FREQ14_CCI_FY    (247000)	/* KHz */
#define CPU_DVFS_FREQ15_CCI_FY    (182000)	/* KHz */

/* for DVFS OPP table B/FY */
#define CPU_DVFS_FREQ0_B_FY    (2288000)	/* KHz */
#define CPU_DVFS_FREQ1_B_FY    (2262000)	/* KHz */
#define CPU_DVFS_FREQ2_B_FY    (2197000)	/* KHz */
#define CPU_DVFS_FREQ3_B_FY    (2132000)	/* KHz */
#define CPU_DVFS_FREQ4_B_FY    (2093000)	/* KHz */
#define CPU_DVFS_FREQ5_B_FY    (1989000)	/* KHz */
#define CPU_DVFS_FREQ6_B_FY    (1781000)	/* KHz */
#define CPU_DVFS_FREQ7_B_FY    (1677000)	/* KHz */
#define CPU_DVFS_FREQ8_B_FY    (1495000)	/* KHz */
#define CPU_DVFS_FREQ9_B_FY    (1378000)	/* KHz */
#define CPU_DVFS_FREQ10_B_FY    (1248000)	/* KHz */
#define CPU_DVFS_FREQ11_B_FY    (1131000)	/* KHz */
#define CPU_DVFS_FREQ12_B_FY    (1001000)	/* KHz */
#define CPU_DVFS_FREQ13_B_FY    (845000)	/* KHz */
#define CPU_DVFS_FREQ14_B_FY    (676000)	/* KHz */
#define CPU_DVFS_FREQ15_B_FY    (507000)	/* KHz */

#define CPU_IDVFS_FREQ0_B_FY    (9152)	/* Perc */
#define CPU_IDVFS_FREQ1_B_FY    (9048)	/* Perc */
#define CPU_IDVFS_FREQ2_B_FY    (8788)	/* Perc */
#define CPU_IDVFS_FREQ3_B_FY    (8528)	/* Perc */
#define CPU_IDVFS_FREQ4_B_FY    (8372)	/* Perc */
#define CPU_IDVFS_FREQ5_B_FY    (7956)	/* Perc */
#define CPU_IDVFS_FREQ6_B_FY    (7124)	/* Perc */
#define CPU_IDVFS_FREQ7_B_FY    (6708)	/* Perc */
#define CPU_IDVFS_FREQ8_B_FY    (5980)	/* Perc */
#define CPU_IDVFS_FREQ9_B_FY    (5512)	/* Perc */
#define CPU_IDVFS_FREQ10_B_FY	(4992)	/* Perc */
#define CPU_IDVFS_FREQ11_B_FY	(4524)	/* Perc */
#define CPU_IDVFS_FREQ12_B_FY	(4004)	/* Perc */
#define CPU_IDVFS_FREQ13_B_FY	(3380)	/* Perc */
#define CPU_IDVFS_FREQ14_B_FY	(2704)	/* Perc */
#define CPU_IDVFS_FREQ15_B_FY	(2028)	/* Perc */

/* for DVFS OPP table LL|L|CCI */
#define CPU_DVFS_VOLT0_VPROC1_FY    (120000)	/* 10MV */
#define CPU_DVFS_VOLT1_VPROC1_FY    (118000)	/* 10MV */
#define CPU_DVFS_VOLT2_VPROC1_FY    (115000)	/* 10MV */
#define CPU_DVFS_VOLT3_VPROC1_FY    (113000)	/* 10MV */
#define CPU_DVFS_VOLT4_VPROC1_FY    (109000)	/* 10MV */
#define CPU_DVFS_VOLT5_VPROC1_FY    (107000)	/* 10MV */
#define CPU_DVFS_VOLT6_VPROC1_FY    (104000)	/* 10MV */
#define CPU_DVFS_VOLT7_VPROC1_FY    (100000)	/* 10MV */
#define CPU_DVFS_VOLT8_VPROC1_FY    (97000)		/* 10MV */
#define CPU_DVFS_VOLT9_VPROC1_FY    (93000)		/* 10MV */
#define CPU_DVFS_VOLT10_VPROC1_FY    (90000)	/* 10MV */
#define CPU_DVFS_VOLT11_VPROC1_FY    (88000)	/* 10MV */
#define CPU_DVFS_VOLT12_VPROC1_FY    (85000)	/* 10MV */
#define CPU_DVFS_VOLT13_VPROC1_FY    (83000)	/* 10MV */
#define CPU_DVFS_VOLT14_VPROC1_FY    (80000)	/* 10MV */
#define CPU_DVFS_VOLT15_VPROC1_FY    (78000)	/* 10MV */

/* for DVFS OPP table B */
#define CPU_DVFS_VOLT0_VPROC2_FY    (118000)	/* 10MV */
#define CPU_DVFS_VOLT1_VPROC2_FY    (118000)	/* 10MV */
#define CPU_DVFS_VOLT2_VPROC2_FY    (116000)	/* 10MV */
#define CPU_DVFS_VOLT3_VPROC2_FY    (114000)	/* 10MV */
#define CPU_DVFS_VOLT4_VPROC2_FY    (113000)	/* 10MV */
#define CPU_DVFS_VOLT5_VPROC2_FY    (110000)	/* 10MV */
#define CPU_DVFS_VOLT6_VPROC2_FY    (107000)	/* 10MV */
#define CPU_DVFS_VOLT7_VPROC2_FY    (105000)	/* 10MV */
#define CPU_DVFS_VOLT8_VPROC2_FY    (100000)	/* 10MV */
#define CPU_DVFS_VOLT9_VPROC2_FY    (98000)		/* 10MV */
#define CPU_DVFS_VOLT10_VPROC2_FY    (95000)	/* 10MV */
#define CPU_DVFS_VOLT11_VPROC2_FY    (93000)	/* 10MV */
#define CPU_DVFS_VOLT12_VPROC2_FY    (90000)	/* 10MV */
#define CPU_DVFS_VOLT13_VPROC2_FY    (88000)	/* 10MV */
#define CPU_DVFS_VOLT14_VPROC2_FY    (85000)	/* 10MV */
#define CPU_DVFS_VOLT15_VPROC2_FY    (83000)	/* 10MV */

/* for DVFS OPP table LL/SB */
#define CPU_DVFS_FREQ0_LL_SB    (1547000)	/* KHz */
#define CPU_DVFS_FREQ1_LL_SB    (1495000)	/* KHz */
#define CPU_DVFS_FREQ2_LL_SB    (1443000)	/* KHz */
#define CPU_DVFS_FREQ3_LL_SB    (1391000)	/* KHz */
#define CPU_DVFS_FREQ4_LL_SB    (1339000)	/* KHz */
#define CPU_DVFS_FREQ5_LL_SB    (1274000)	/* KHz */
#define CPU_DVFS_FREQ6_LL_SB    (1222000)	/* KHz */
#define CPU_DVFS_FREQ7_LL_SB    (1118000)	/* KHz */
#define CPU_DVFS_FREQ8_LL_SB    (1014000)	/* KHz */
#define CPU_DVFS_FREQ9_LL_SB    (897000)	/* KHz */
#define CPU_DVFS_FREQ10_LL_SB    (806000)	/* KHz */
#define CPU_DVFS_FREQ11_LL_SB    (715000)	/* KHz */
#define CPU_DVFS_FREQ12_LL_SB    (624000)	/* KHz */
#define CPU_DVFS_FREQ13_LL_SB    (481000)	/* KHz */
#define CPU_DVFS_FREQ14_LL_SB    (338000)	/* KHz */
#define CPU_DVFS_FREQ15_LL_SB    (221000)	/* KHz */

/* for DVFS OPP table L/SB */
#define CPU_DVFS_FREQ0_L_SB		(2197000)	/* KHz */
#define CPU_DVFS_FREQ1_L_SB		(2132000)	/* KHz */
#define CPU_DVFS_FREQ2_L_SB		(2041000)	/* KHz */
#define CPU_DVFS_FREQ3_L_SB		(1963000)	/* KHz */
#define CPU_DVFS_FREQ4_L_SB		(1885000)	/* KHz */
#define CPU_DVFS_FREQ5_L_SB		(1807000)	/* KHz */
#define CPU_DVFS_FREQ6_L_SB		(1716000)	/* KHz */
#define CPU_DVFS_FREQ7_L_SB		(1573000)	/* KHz */
#define CPU_DVFS_FREQ8_L_SB		(1430000)	/* KHz */
#define CPU_DVFS_FREQ9_L_SB		(1274000)	/* KHz */
#define CPU_DVFS_FREQ10_L_SB    (1144000)	/* KHz */
#define CPU_DVFS_FREQ11_L_SB    (1014000)	/* KHz */
#define CPU_DVFS_FREQ12_L_SB    (871000)	/* KHz */
#define CPU_DVFS_FREQ13_L_SB    (689000)	/* KHz */
#define CPU_DVFS_FREQ14_L_SB    (494000)	/* KHz */
#define CPU_DVFS_FREQ15_L_SB    (338000)	/* KHz */

/* for DVFS OPP table L/SB */
#define CPU_IDVFS_FREQ0_L_SB	(9984)	/* Perc */
#define CPU_IDVFS_FREQ1_L_SB	(9724)	/* Perc */
#define CPU_IDVFS_FREQ2_L_SB	(9464)	/* Perc */
#define CPU_IDVFS_FREQ3_L_SB	(9152)	/* Perc */
#define CPU_IDVFS_FREQ4_L_SB	(8944)	/* Perc */
#define CPU_IDVFS_FREQ5_L_SB	(8684)	/* Perc */
#define CPU_IDVFS_FREQ6_L_SB	(8372)	/* Perc */
#define CPU_IDVFS_FREQ7_L_SB	(7540)	/* Perc */
#define CPU_IDVFS_FREQ8_L_SB	(6708)	/* Perc */
#define CPU_IDVFS_FREQ9_L_SB	(5980)	/* Perc */
#define CPU_IDVFS_FREQ10_L_SB	(5512)	/* Perc */
#define CPU_IDVFS_FREQ11_L_SB	(4524)	/* Perc */
#define CPU_IDVFS_FREQ12_L_SB	(4004)	/* Perc */
#define CPU_IDVFS_FREQ13_L_SB	(3380)	/* Perc */
#define CPU_IDVFS_FREQ14_L_SB	(2704)	/* Perc */
#define CPU_IDVFS_FREQ15_L_SB	(2028)	/* Perc */

/* for DVFS OPP table CCI/SB */
#define CPU_DVFS_FREQ0_CCI_SB    (1092000)	/* KHz */
#define CPU_DVFS_FREQ1_CCI_SB    (1066000)	/* KHz */
#define CPU_DVFS_FREQ2_CCI_SB    (1014000)	/* KHz */
#define CPU_DVFS_FREQ3_CCI_SB    (975000)	/* KHz */
#define CPU_DVFS_FREQ4_CCI_SB    (936000)	/* KHz */
#define CPU_DVFS_FREQ5_CCI_SB    (897000)	/* KHz */
#define CPU_DVFS_FREQ6_CCI_SB    (858000)	/* KHz */
#define CPU_DVFS_FREQ7_CCI_SB    (793000)	/* KHz */
#define CPU_DVFS_FREQ8_CCI_SB    (728000)	/* KHz */
#define CPU_DVFS_FREQ9_CCI_SB    (624000)	/* KHz */
#define CPU_DVFS_FREQ10_CCI_SB    (559000)	/* KHz */
#define CPU_DVFS_FREQ11_CCI_SB    (494000)	/* KHz */
#define CPU_DVFS_FREQ12_CCI_SB    (429000)	/* KHz */
#define CPU_DVFS_FREQ13_CCI_SB    (338000)	/* KHz */
#define CPU_DVFS_FREQ14_CCI_SB    (247000)	/* KHz */
#define CPU_DVFS_FREQ15_CCI_SB    (182000)	/* KHz */

/* for DVFS OPP table B/SB */
#define CPU_DVFS_FREQ0_B_SB		(2496000)	/* KHz */
#define CPU_DVFS_FREQ1_B_SB		(2431000)	/* KHz */
#define CPU_DVFS_FREQ2_B_SB		(2366000)	/* KHz */
#define CPU_DVFS_FREQ3_B_SB		(2288000)	/* KHz */
#define CPU_DVFS_FREQ4_B_SB		(2236000)	/* KHz */
#define CPU_DVFS_FREQ5_B_SB		(2171000)	/* KHz */
#define CPU_DVFS_FREQ6_B_SB		(2093000)	/* KHz */
#define CPU_DVFS_FREQ7_B_SB		(1885000)	/* KHz */
#define CPU_DVFS_FREQ8_B_SB		(1677000)	/* KHz */
#define CPU_DVFS_FREQ9_B_SB		(1495000)	/* KHz */
#define CPU_DVFS_FREQ10_B_SB    (1378000)	/* KHz */
#define CPU_DVFS_FREQ11_B_SB    (1131000)	/* KHz */
#define CPU_DVFS_FREQ12_B_SB    (1001000)	/* KHz */
#define CPU_DVFS_FREQ13_B_SB    (845000)	/* KHz */
#define CPU_DVFS_FREQ14_B_SB    (676000)	/* KHz */
#define CPU_DVFS_FREQ15_B_SB    (507000)	/* KHz */

/* for DVFS OPP table LL|L|CCI */
#define CPU_DVFS_VOLT0_VPROC1_SB    (120000)	/* 10MV */
#define CPU_DVFS_VOLT1_VPROC1_SB    (119000)	/* 10MV */
#define CPU_DVFS_VOLT2_VPROC1_SB    (117000)	/* 10MV */
#define CPU_DVFS_VOLT3_VPROC1_SB    (115000)	/* 10MV */
#define CPU_DVFS_VOLT4_VPROC1_SB    (113000)	/* 10MV */
#define CPU_DVFS_VOLT5_VPROC1_SB    (111000)	/* 10MV */
#define CPU_DVFS_VOLT6_VPROC1_SB    (109000)	/* 10MV */
#define CPU_DVFS_VOLT7_VPROC1_SB    (106000)	/* 10MV */
#define CPU_DVFS_VOLT8_VPROC1_SB    (102000)	/* 10MV */
#define CPU_DVFS_VOLT9_VPROC1_SB    (99000)		/* 10MV */
#define CPU_DVFS_VOLT10_VPROC1_SB    (95000)	/* 10MV */
#define CPU_DVFS_VOLT11_VPROC1_SB    (93000)	/* 10MV */
#define CPU_DVFS_VOLT12_VPROC1_SB    (89000)	/* 10MV */
#define CPU_DVFS_VOLT13_VPROC1_SB    (85000)	/* 10MV */
#define CPU_DVFS_VOLT14_VPROC1_SB    (80000)	/* 10MV */
#define CPU_DVFS_VOLT15_VPROC1_SB    (77000)	/* 10MV */

/* for DVFS OPP table B */
#define CPU_DVFS_VOLT0_VPROC2_SB    (118000)	/* 10MV */
#define CPU_DVFS_VOLT1_VPROC2_SB    (117000)	/* 10MV */
#define CPU_DVFS_VOLT2_VPROC2_SB    (116000)	/* 10MV */
#define CPU_DVFS_VOLT3_VPROC2_SB    (115000)	/* 10MV */
#define CPU_DVFS_VOLT4_VPROC2_SB    (114000)	/* 10MV */
#define CPU_DVFS_VOLT5_VPROC2_SB    (113000)	/* 10MV */
#define CPU_DVFS_VOLT6_VPROC2_SB    (111000)	/* 10MV */
#define CPU_DVFS_VOLT7_VPROC2_SB    (108000)	/* 10MV */
#define CPU_DVFS_VOLT8_VPROC2_SB    (104000)	/* 10MV */
#define CPU_DVFS_VOLT9_VPROC2_SB    (100000)	/* 10MV */
#define CPU_DVFS_VOLT10_VPROC2_SB    (98000)	/* 10MV */
#define CPU_DVFS_VOLT11_VPROC2_SB    (93000)	/* 10MV */
#define CPU_DVFS_VOLT12_VPROC2_SB    (90000)	/* 10MV */
#define CPU_DVFS_VOLT13_VPROC2_SB    (88000)	/* 10MV */
#define CPU_DVFS_VOLT14_VPROC2_SB    (85000)	/* 10MV */
#define CPU_DVFS_VOLT15_VPROC2_SB    (83000)	/* 10MV */
#endif

#if 0
/* for DVFS OPP table LL/FY */
#define CPU_DVFS_FREQ0_LL_FY    (1391000)	/* KHz */
#define CPU_DVFS_FREQ1_LL_FY    (1287000)	/* KHz */
#define CPU_DVFS_FREQ2_LL_FY    (1222000)	/* KHz */
#define CPU_DVFS_FREQ3_LL_FY    (1066000)	/* KHz */
#define CPU_DVFS_FREQ4_LL_FY    (897000)	/* KHz */
#define CPU_DVFS_FREQ5_LL_FY    (715000)	/* KHz */
#define CPU_DVFS_FREQ6_LL_FY    (624000)	/* KHz */
#define CPU_DVFS_FREQ7_LL_FY    (338000)	/* KHz */

/* for DVFS OPP table L/FY */
#define CPU_DVFS_FREQ0_L_FY    (1950000)	/* KHz */
#define CPU_DVFS_FREQ1_L_FY    (1794000)	/* KHz */
#define CPU_DVFS_FREQ2_L_FY    (1716000)	/* KHz */
#define CPU_DVFS_FREQ3_L_FY    (1495000)	/* KHz */
#define CPU_DVFS_FREQ4_L_FY    (1274000)	/* KHz */
#define CPU_DVFS_FREQ5_L_FY    (1014000)	/* KHz */
#define CPU_DVFS_FREQ6_L_FY    (871000)		/* KHz */
#define CPU_DVFS_FREQ7_L_FY    (494000)		/* KHz */

/* for DVFS OPP table CCI/FY */
#define CPU_DVFS_FREQ0_CCI_FY    (975000)	/* KHz */
#define CPU_DVFS_FREQ1_CCI_FY    (897000)	/* KHz */
#define CPU_DVFS_FREQ2_CCI_FY    (858000)	/* KHz */
#define CPU_DVFS_FREQ3_CCI_FY    (754000)	/* KHz */
#define CPU_DVFS_FREQ4_CCI_FY    (624000)	/* KHz */
#define CPU_DVFS_FREQ5_CCI_FY    (494000)	/* KHz */
#define CPU_DVFS_FREQ6_CCI_FY    (429000)	/* KHz */
#define CPU_DVFS_FREQ7_CCI_FY    (247000)	/* KHz */

/* for DVFS OPP table B/FY */
#define CPU_DVFS_FREQ0_B_FY    (2288000)	/* KHz */
#define CPU_DVFS_FREQ1_B_FY    (2093000)	/* KHz */
#define CPU_DVFS_FREQ2_B_FY    (1781000)	/* KHz */
#define CPU_DVFS_FREQ3_B_FY    (1495000)	/* KHz */
#define CPU_DVFS_FREQ4_B_FY    (1248000)	/* KHz */
#define CPU_DVFS_FREQ5_B_FY    (1001000)	/* KHz */
#define CPU_DVFS_FREQ6_B_FY    (676000)		/* KHz */
#define CPU_DVFS_FREQ7_B_FY    (507000)		/* KHz */

/* for DVFS OPP table LL|L|CCI */
#define CPU_DVFS_VOLT0_VPROC1_FY    (120000)	/* 10MV */
#define CPU_DVFS_VOLT1_VPROC1_FY    (115000)	/* 10MV */
#define CPU_DVFS_VOLT2_VPROC1_FY    (113000)	/* 10MV */
#define CPU_DVFS_VOLT3_VPROC1_FY    (107000)	/* 10MV */
#define CPU_DVFS_VOLT4_VPROC1_FY    (100000)	/* 10MV */
#define CPU_DVFS_VOLT5_VPROC1_FY    (93000)		/* 10MV */
#define CPU_DVFS_VOLT6_VPROC1_FY    (90000)		/* 10MV */
#define CPU_DVFS_VOLT7_VPROC1_FY    (80000)		/* 10MV */

/* for DVFS OPP table B */
#define CPU_DVFS_VOLT0_VPROC2_FY    (118000)	/* 10MV */
#define CPU_DVFS_VOLT1_VPROC2_FY    (113000)	/* 10MV */
#define CPU_DVFS_VOLT2_VPROC2_FY    (107000)	/* 10MV */
#define CPU_DVFS_VOLT3_VPROC2_FY    (100000)	/* 10MV */
#define CPU_DVFS_VOLT4_VPROC2_FY    (95000)		/* 10MV */
#define CPU_DVFS_VOLT5_VPROC2_FY    (90000)		/* 10MV */
#define CPU_DVFS_VOLT6_VPROC2_FY    (85000)		/* 10MV */
#define CPU_DVFS_VOLT7_VPROC2_FY    (83000)		/* 10MV */

/* for DVFS OPP table LL/SB */
#define CPU_DVFS_FREQ0_LL_SB    (1547000)	/* KHz */
#define CPU_DVFS_FREQ1_LL_SB    (1443000)	/* KHz */
#define CPU_DVFS_FREQ2_LL_SB    (1339000)	/* KHz */
#define CPU_DVFS_FREQ3_LL_SB    (1222000)	/* KHz */
#define CPU_DVFS_FREQ4_LL_SB    (1118000)	/* KHz */
#define CPU_DVFS_FREQ5_LL_SB    (897000)	/* KHz */
#define CPU_DVFS_FREQ6_LL_SB    (624000)	/* KHz */
#define CPU_DVFS_FREQ7_LL_SB    (221000)	/* KHz */

/* for DVFS OPP table L/SB */
#define CPU_DVFS_FREQ0_L_SB    (2197000)	/* KHz */
#define CPU_DVFS_FREQ1_L_SB    (2041000)	/* KHz */
#define CPU_DVFS_FREQ2_L_SB    (1885000)	/* KHz */
#define CPU_DVFS_FREQ3_L_SB    (1716000)	/* KHz */
#define CPU_DVFS_FREQ4_L_SB    (1573000)	/* KHz */
#define CPU_DVFS_FREQ5_L_SB    (1274000)	/* KHz */
#define CPU_DVFS_FREQ6_L_SB    (871000)		/* KHz */
#define CPU_DVFS_FREQ7_L_SB    (338000)		/* KHz */

/* for DVFS OPP table CCI/SB */
#define CPU_DVFS_FREQ0_CCI_SB    (1092000)	/* KHz */
#define CPU_DVFS_FREQ1_CCI_SB    (1014000)	/* KHz */
#define CPU_DVFS_FREQ2_CCI_SB    (936000)	/* KHz */
#define CPU_DVFS_FREQ3_CCI_SB    (858000)	/* KHz */
#define CPU_DVFS_FREQ4_CCI_SB    (793000)	/* KHz */
#define CPU_DVFS_FREQ5_CCI_SB    (624000)	/* KHz */
#define CPU_DVFS_FREQ6_CCI_SB    (429000)	/* KHz */
#define CPU_DVFS_FREQ7_CCI_SB    (182000)	/* KHz */

/* for DVFS OPP table B/SB */
#define CPU_DVFS_FREQ0_B_SB    (2496000)	/* KHz */
#define CPU_DVFS_FREQ1_B_SB    (2288000)	/* KHz */
#define CPU_DVFS_FREQ2_B_SB    (2093000)	/* KHz */
#define CPU_DVFS_FREQ3_B_SB    (1677000)	/* KHz */
#define CPU_DVFS_FREQ4_B_SB    (1495000)	/* KHz */
#define CPU_DVFS_FREQ5_B_SB    (1131000)	/* KHz */
#define CPU_DVFS_FREQ6_B_SB    (1001000)	/* KHz */
#define CPU_DVFS_FREQ7_B_SB    (507000)		/* KHz */

/* for DVFS OPP table LL|L|CCI */
#define CPU_DVFS_VOLT0_VPROC1_SB    (120000)	/* 10MV */
#define CPU_DVFS_VOLT1_VPROC1_SB    (117000)	/* 10MV */
#define CPU_DVFS_VOLT2_VPROC1_SB    (113000)	/* 10MV */
#define CPU_DVFS_VOLT3_VPROC1_SB    (109000)	/* 10MV */
#define CPU_DVFS_VOLT4_VPROC1_SB    (106000)	/* 10MV */
#define CPU_DVFS_VOLT5_VPROC1_SB    (99000)		/* 10MV */
#define CPU_DVFS_VOLT6_VPROC1_SB    (89000)		/* 10MV */
#define CPU_DVFS_VOLT7_VPROC1_SB    (77000)		/* 10MV */

/* for DVFS OPP table B */
#define CPU_DVFS_VOLT0_VPROC2_SB    (118000)	/* 10MV */
#define CPU_DVFS_VOLT1_VPROC2_SB    (115000)	/* 10MV */
#define CPU_DVFS_VOLT2_VPROC2_SB    (111000)	/* 10MV */
#define CPU_DVFS_VOLT3_VPROC2_SB    (104000)	/* 10MV */
#define CPU_DVFS_VOLT4_VPROC2_SB    (101000)	/* 10MV */
#define CPU_DVFS_VOLT5_VPROC2_SB    (94000)		/* 10MV */
#define CPU_DVFS_VOLT6_VPROC2_SB    (92000)		/* 10MV */
#define CPU_DVFS_VOLT7_VPROC2_SB    (84000)		/* 10MV */

#define CPUFREQ_BOUNDARY_FOR_FHCTL   (CPU_DVFS_FREQ4_L_SB)
#endif
#define CPUFREQ_LAST_FREQ_LEVEL    (CPU_DVFS_FREQ15_LL_SB)

/* Debugging */
#undef TAG
#define TAG     "[Power/cpufreq] "

#define cpufreq_err(fmt, args...)       \
	pr_err(TAG"[ERROR]"fmt, ##args)
#define cpufreq_warn(fmt, args...)      \
	pr_warn(TAG"[WARNING]"fmt, ##args)
#define cpufreq_info(fmt, args...)      \
	pr_warn(TAG""fmt, ##args)
#define cpufreq_dbg(fmt, args...)       \
	pr_debug(TAG""fmt, ##args)
#define cpufreq_ver(fmt, args...)       \
	do {                                \
		if (func_lv_mask)           \
			cpufreq_info(TAG""fmt, ##args);    \
	} while (0)

#define FUNC_LV_MODULE         BIT(0)  /* module, platform driver interface */
#define FUNC_LV_CPUFREQ        BIT(1)  /* cpufreq driver interface          */
#define FUNC_LV_API                BIT(2)  /* mt_cpufreq driver global function */
#define FUNC_LV_LOCAL            BIT(3)  /* mt_cpufreq driver local function  */
#define FUNC_LV_HELP              BIT(4)  /* mt_cpufreq driver help function   */
#define FUNC_ENTER(lv) \
	do { if ((lv) & func_lv_mask) cpufreq_dbg(">> %s()\n", __func__); } while (0)
#define FUNC_EXIT(lv) \
	do { if ((lv) & func_lv_mask) cpufreq_dbg("<< %s():%d\n", __func__, __LINE__); } while (0)

#define FUNC_LV_MODULE         BIT(0)	/* module, platform driver interface */
#define FUNC_LV_CPUFREQ        BIT(1)	/* cpufreq driver interface          */
#define FUNC_LV_API                BIT(2)	/* mt_cpufreq driver global function */
#define FUNC_LV_LOCAL            BIT(3)	/* mt_cpufreq driver local function  */
#define FUNC_LV_HELP              BIT(4)	/* mt_cpufreq driver help function   */

/*
* static unsigned int func_lv_mask =
* (FUNC_LV_MODULE | FUNC_LV_CPUFREQ | FUNC_LV_API | FUNC_LV_LOCAL | FUNC_LV_HELP);
*/
static unsigned int func_lv_mask;
static unsigned int do_dvfs_stress_test;
static unsigned int dvfs_power_mode;

enum ppb_power_mode {
	Default = 0,
	Low_power_Mode = 1,
	Just_Make_Mode = 2,
	Performance_Mode = 3,
};

#ifdef CONFIG_CPU_DVFS_SHOWLOG
#define FUNC_ENTER(lv) \
	do { if ((lv) & func_lv_mask) cpufreq_dbg(">> %s()\n", __func__); } while (0)
#define FUNC_EXIT(lv) \
	do { if ((lv) & func_lv_mask) cpufreq_dbg("<< %s():%d\n", __func__, __LINE__); } while (0)
#else
#define FUNC_ENTER(lv)
#define FUNC_EXIT(lv)
#endif				/* CONFIG_CPU_DVFS_SHOWLOG */

/*
 * BIT Operation
 */
#define _BIT_(_bit_)                    (unsigned)(1 << (_bit_))
#define _BITS_(_bits_, _val_) \
	((((unsigned) -1 >> (31 - ((1) ? _bits_))) & ~((1U << ((0) ? _bits_)) - 1)) & ((_val_)<<((0) ? _bits_)))
#define _BITMASK_(_bits_)               (((unsigned) -1 >> (31 - ((1) ? _bits_))) & ~((1U << ((0) ? _bits_)) - 1))
#define _GET_BITS_VAL_(_bits_, _val_)   (((_val_) & (_BITMASK_(_bits_))) >> ((0) ? _bits_))

/*
 * REG ACCESS
 */
#define cpufreq_read(addr)                  __raw_readl(IOMEM(addr))
#define cpufreq_write(addr, val)            mt_reg_sync_writel((val), ((void *)addr))
#define cpufreq_write_mask(addr, mask, val) \
cpufreq_write(addr, (cpufreq_read(addr) & ~(_BITMASK_(mask))) | _BITS_(mask, val))

/*
 * LOCK
 */
#if 0
static DEFINE_SPINLOCK(cpufreq_lock);
#define cpufreq_lock(flags) spin_lock_irqsave(&cpufreq_lock, flags)
#define cpufreq_unlock(flags) spin_unlock_irqrestore(&cpufreq_lock, flags)
#else				/* mutex */
static DEFINE_MUTEX(cpufreq_mutex);
bool is_in_cpufreq = 0;
#define cpufreq_lock(flags) \
	do { \
		flags = (unsigned long)&flags; \
		mutex_lock(&cpufreq_mutex); \
		is_in_cpufreq = 1;\
	} while (0)

#define cpufreq_unlock(flags) \
	do { \
		flags = (unsigned long)&flags; \
		is_in_cpufreq = 0;\
		mutex_unlock(&cpufreq_mutex); \
	} while (0)
#endif

/*
 * EFUSE
 */

int dvfs_disable_flag = 0;

#define CPUFREQ_EFUSE_INDEX     (3)
#define FUNC_CODE_EFUSE_INDEX	(28)

#define CPU_LEVEL_0             (0x0)
#define CPU_LEVEL_1             (0x1)
#define CPU_LEVEL_2             (0x2)
#define CPU_LEVEL_3             (0x3)
#define CPU_LV_TO_OPP_IDX(lv)   ((lv))	/* cpu_level to opp_idx */

#ifdef __KERNEL__
static unsigned int _mt_cpufreq_get_cpu_level(void)
{
	unsigned int lv = 0;
	unsigned int func_code_0 = _GET_BITS_VAL_(27:24, get_devinfo_with_index(FUNC_CODE_EFUSE_INDEX));
	unsigned int func_code_1 = _GET_BITS_VAL_(31:28, get_devinfo_with_index(FUNC_CODE_EFUSE_INDEX));

	cpufreq_ver("from efuse: function code 0 = 0x%x, function code 1 = 0x%x\n", func_code_0,
		     func_code_1);

	if (func_code_1 == 0)
		return CPU_LEVEL_0;
	else
		return CPU_LEVEL_1;

	/* get CPU clock-frequency from DT */
#ifdef CONFIG_OF
	{
		struct device_node *node = of_find_node_by_type(NULL, "cpu");
		unsigned int cpu_speed = 0;

		if (!of_property_read_u32(node, "clock-frequency", &cpu_speed))
			cpu_speed = cpu_speed / 1000 / 1000;	/* MHz */
		else {
			cpufreq_err
			    ("@%s: missing clock-frequency property, use default CPU level\n",
			     __func__);
			return CPU_LEVEL_1;
		}

		cpufreq_ver("CPU clock-frequency from DT = %d MHz\n", cpu_speed);

		if (cpu_speed >= 1700)
			lv = CPU_LEVEL_1;	/* 1.7G */
		else if (cpu_speed >= 1500)
			lv = CPU_LEVEL_2;	/* 1.5G */
		else if (cpu_speed >= 1300)
			lv = CPU_LEVEL_3;	/* 1.3G */
		else {
			cpufreq_err
			    ("No suitable DVFS table, set to default CPU level! clock-frequency=%d\n",
			     cpu_speed);
			lv = CPU_LEVEL_1;
		}
	}
#endif

	return lv;
}
#else
static unsigned int _mt_cpufreq_get_cpu_level(void)
{
	return CPU_LEVEL_0;
}
#endif


/*
 * AEE (SRAM debug)
 */
#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
enum cpu_dvfs_state {
	CPU_DVFS_LL_IS_DOING_DVFS = 0,
	CPU_DVFS_L_IS_DOING_DVFS,
	CPU_DVFS_B_IS_DOING_DVFS,
	CPU_DVFS_CCI_IS_DOING_DVFS,
};

static void _mt_cpufreq_aee_init(void)
{
	aee_rr_rec_cpu_dvfs_vproc_big(0xFF);
	aee_rr_rec_cpu_dvfs_vproc_little(0xFF);
	aee_rr_rec_cpu_dvfs_oppidx(0xFF);
	aee_rr_rec_cpu_dvfs_status(0xF0);
}
#endif

/*
 * PMIC_WRAP
 */
#define VOLT_TO_PMIC_VAL(volt)  ((((volt) - 90000) / 2500) + 3)
#define PMIC_VAL_TO_VOLT(val)  (((val - 3) * 2500) + 90000)

/* DA9214 */
#define VOLT_TO_EXTBUCK_VAL(volt) (((((volt) - 30000) + 900) / 1000) & 0x7F)
#define EXTBUCK_VAL_TO_VOLT(val)  (30000 + ((val) & 0x7F) * 1000)
/*#define VOLT_TO_EXTBUCK_VAL(volt) (((((volt) - 300) + 9) / 10) & 0x7F)
#define EXTBUCK_VAL_TO_VOLT(val)  (300 + ((val) & 0x7F) * 10)*/


/* cpu voltage sampler */
static cpuVoltsampler_func g_pCpuVoltSampler;

void mt_cpufreq_setvolt_registerCB(cpuVoltsampler_func pCB)
{
	g_pCpuVoltSampler = pCB;
}
EXPORT_SYMBOL(mt_cpufreq_setvolt_registerCB);

/*
 * mt_cpufreq driver
 */
#define FP(khz, pos, clk) {	\
	.target_f = khz,		\
	.vco_dds = khz*pos*clk,		\
	.pos_div = pos,			\
	.clk_div = clk,			\
}
struct mt_cpu_freq_method {
	const unsigned int target_f;
	const unsigned int vco_dds;
	const unsigned int pos_div;
	const unsigned int clk_div;
};

struct opp_idx_tbl {
	struct mt_cpu_dvfs *p;
	struct mt_cpu_freq_method *slot;
};

enum opp_idx_type {
		CUR_OPP_IDX = 0,
		TARGET_OPP_IDX = 1,

		NR_OPP_IDX,
};
struct opp_idx_tbl opp_tbl_m[NR_OPP_IDX];

#define OP(khz, volt) {            \
	.cpufreq_khz = khz,             \
	.cpufreq_volt = volt,           \
	.cpufreq_volt_org = volt,       \
}

struct mt_cpu_freq_info {
	const unsigned int cpufreq_khz;
	unsigned int cpufreq_volt;
	const unsigned int cpufreq_volt_org;
};

struct mt_cpu_dvfs {
	const char *name;
	unsigned int cpu_id;	/* for cpufreq */
	unsigned int cpu_level;
	struct mt_cpu_dvfs_ops *ops;
	unsigned int *armpll_addr;	/* for armpll clock address */
	int hopping_id;	/* for frequency hopping */
	struct mt_cpu_freq_method *freq_tbl;	/* Frequency table */
	struct mt_cpu_freq_info *opp_tbl;	/* OPP table */
	int nr_opp_tbl;		/* size for OPP table */
	int idx_opp_tbl;	/* current OPP idx */
	int idx_opp_ppm_base;	/* ppm update base */
	int idx_opp_ppm_limit;	/* ppm update limit */
	int armpll_is_available;	/* For CCI clock switch flag */
	int idx_normal_max_opp;	/* idx for normal max OPP */

	struct cpufreq_frequency_table *freq_tbl_for_cpufreq;	/* freq table for cpufreq */

	/* enable/disable DVFS function */
	bool dvfs_disable_by_suspend;
	bool dvfs_disable_by_procfs;

	/* turbo mode */
	unsigned int turbo_mode;

};

struct mt_cpu_dvfs_ops {
	/* for freq change (PLL/MUX) */
	unsigned int (*get_cur_phy_freq)(struct mt_cpu_dvfs *p);	/* return (physical) freq (KHz) */
	void (*set_cur_freq)(struct mt_cpu_dvfs *p, unsigned int cur_khz, unsigned int target_khz);	/* set freq  */
	/* for vproc change (extBuck) */
	unsigned int (*get_cur_volt)(struct mt_cpu_dvfs *p);	/* return volt (mV * 100) */
	int (*set_cur_volt)(struct mt_cpu_dvfs *p, unsigned int volt);	/* set volt (mv * 100) */
	/* for vsram change (LDO) */
	unsigned int (*get_cur_vsram)(struct mt_cpu_dvfs *p);	/* return volt (mV * 100) */
	int (*set_cur_vsram)(struct mt_cpu_dvfs *p, unsigned int volt);	/* set volt (mv * 100) */
	int (*set_sync_dcm)(unsigned int mhz);
};

/* for freq change (PLL/MUX) */
static unsigned int get_cur_phy_freq(struct mt_cpu_dvfs *p);
static void set_cur_freq(struct mt_cpu_dvfs *p, unsigned int cur_khz, unsigned int target_khz);

/* for volt change (PMICWRAP/extBuck) */
static unsigned int get_cur_volt_extbuck(struct mt_cpu_dvfs *p);
static int set_cur_volt_extbuck(struct mt_cpu_dvfs *p, unsigned int volt);	/* volt: mv * 100 */
static unsigned int get_cur_volt_sram_l(struct mt_cpu_dvfs *p);
static int set_cur_volt_sram_l(struct mt_cpu_dvfs *p, unsigned int volt);	/* volt: mv * 100 */
static unsigned int get_cur_volt_sram_b(struct mt_cpu_dvfs *p);
static unsigned int get_cur_phy_freq_b(struct mt_cpu_dvfs *p);
static int set_cur_volt_sram_b(struct mt_cpu_dvfs *p, unsigned int volt);	/* volt: mv * 100 */

#ifdef ENABLE_IDVFS
int disable_idvfs_flag = 0;
#else
int disable_idvfs_flag = 1;
#endif

#ifdef ENABLE_IDVFS
static unsigned int idvfs_get_cur_phy_freq_b(struct mt_cpu_dvfs *p);
static void idvfs_set_cur_freq(struct mt_cpu_dvfs *p, unsigned int cur_khz, unsigned int target_khz);
static int idvfs_set_cur_volt_extbuck(struct mt_cpu_dvfs *p, unsigned int volt);	/* volt: mv * 100 */
static int idvfs_set_cur_volt_sram_b(struct mt_cpu_dvfs *p, unsigned int volt);	/* volt: mv * 100 */
int idvfs_sync_dcm_set_mp2_freq(unsigned int mp2)
{
	return 0;
}
#endif

static int _search_available_freq_idx_under_v(struct mt_cpu_dvfs *p, unsigned int volt);

/* CPU callback */
static int __cpuinit _mt_cpufreq_cpu_CB(struct notifier_block *nfb, unsigned long action,
					void *hcpu);
/* static unsigned int max_cpu_num = 8; */

static struct mt_cpu_dvfs_ops dvfs_ops_LL = {
	.get_cur_phy_freq = get_cur_phy_freq,
	.set_cur_freq = set_cur_freq,
	.get_cur_volt = get_cur_volt_extbuck,
	.set_cur_volt = set_cur_volt_extbuck,
	.get_cur_vsram = get_cur_volt_sram_l,
	.set_cur_vsram = set_cur_volt_sram_l,
	.set_sync_dcm = sync_dcm_set_mp0_freq,
};

static struct mt_cpu_dvfs_ops dvfs_ops_L = {
	.get_cur_phy_freq = get_cur_phy_freq,
	.set_cur_freq = set_cur_freq,
	.get_cur_volt = get_cur_volt_extbuck,
	.set_cur_volt = set_cur_volt_extbuck,
	.get_cur_vsram = get_cur_volt_sram_l,
	.set_cur_vsram = set_cur_volt_sram_l,
	.set_sync_dcm = sync_dcm_set_mp1_freq,
};

#ifdef ENABLE_IDVFS
static struct mt_cpu_dvfs_ops idvfs_ops_B = {
	.get_cur_phy_freq = idvfs_get_cur_phy_freq_b,
	.set_cur_freq = idvfs_set_cur_freq,
	.get_cur_volt = get_cur_volt_extbuck,
	.set_cur_volt = idvfs_set_cur_volt_extbuck,
	.get_cur_vsram = get_cur_volt_sram_b,
	.set_cur_vsram = idvfs_set_cur_volt_sram_b,
	.set_sync_dcm = idvfs_sync_dcm_set_mp2_freq,
};
#endif

static struct mt_cpu_dvfs_ops dvfs_ops_B = {
	.get_cur_phy_freq = get_cur_phy_freq_b,
	.set_cur_freq = set_cur_freq,
	.get_cur_volt = get_cur_volt_extbuck,
	.set_cur_volt = set_cur_volt_extbuck,
	.get_cur_vsram = get_cur_volt_sram_b,
	.set_cur_vsram = set_cur_volt_sram_b,
	.set_sync_dcm = sync_dcm_set_mp2_freq,
};

static struct mt_cpu_dvfs_ops dvfs_ops_CCI = {
	.get_cur_phy_freq = get_cur_phy_freq,
	.set_cur_freq = set_cur_freq,
	.get_cur_volt = get_cur_volt_extbuck,
	.set_cur_volt = set_cur_volt_extbuck,
	.get_cur_vsram = get_cur_volt_sram_l,
	.set_cur_vsram = set_cur_volt_sram_l,
	.set_sync_dcm = sync_dcm_set_cci_freq,
};

static struct mt_cpu_dvfs cpu_dvfs[] = {
	[MT_CPU_DVFS_LL] = {
				.name = __stringify(MT_CPU_DVFS_LL),
				.cpu_id = MT_CPU_DVFS_LL,
				.cpu_level = CPU_LEVEL_0,
				.idx_normal_max_opp = -1,
				.idx_opp_ppm_base = -1,
				.idx_opp_ppm_limit = -1,
				.ops = &dvfs_ops_LL,
				.hopping_id = MCU_FH_PLL0,
				},

	[MT_CPU_DVFS_L] = {
				.name = __stringify(MT_CPU_DVFS_L),
				.cpu_id = MT_CPU_DVFS_L,
				.cpu_level = CPU_LEVEL_0,
				.idx_normal_max_opp = -1,
				.idx_opp_ppm_base = -1,
				.idx_opp_ppm_limit = -1,
				.ops = &dvfs_ops_L,
				.hopping_id = MCU_FH_PLL1,
				},

	[MT_CPU_DVFS_B] = {
				.name = __stringify(MT_CPU_DVFS_B),
				.cpu_id = MT_CPU_DVFS_B,
				.cpu_level = CPU_LEVEL_0,
				.idx_normal_max_opp = -1,
				.idx_opp_ppm_base = -1,
				.idx_opp_ppm_limit = -1,
#ifdef ENABLE_IDVFS
				.ops = &idvfs_ops_B,
#else
				.ops = &dvfs_ops_B,
#endif
				.hopping_id = -1,
				},

	[MT_CPU_DVFS_CCI] = {
				.name = __stringify(MT_CPU_DVFS_CCI),
				.cpu_id = MT_CPU_DVFS_CCI,
				.cpu_level = CPU_LEVEL_0,
				.idx_normal_max_opp = -1,
				.idx_opp_ppm_base = -1,
				.idx_opp_ppm_limit = -1,
				.ops = &dvfs_ops_CCI,
				.hopping_id = MCU_FH_PLL2,
				},
};

#define for_each_cpu_dvfs(i, p)			for (i = 0, p = cpu_dvfs; i < NR_MT_CPU_DVFS; i++, p = &cpu_dvfs[i])
#define for_each_cpu_dvfs_only(i, p)	\
	for (i = 0, p = cpu_dvfs; (i < NR_MT_CPU_DVFS) && (i != MT_CPU_DVFS_CCI); i++, p = &cpu_dvfs[i])
#define cpu_dvfs_is(p, id)				(p == &cpu_dvfs[id])
#define cpu_dvfs_is_available(p)		(p->opp_tbl)
#define cpu_dvfs_get_name(p)			(p->name)

#define cpu_dvfs_get_cur_freq(p)		(p->opp_tbl[p->idx_opp_tbl].cpufreq_khz)
#define cpu_dvfs_get_freq_by_idx(p, idx)		(p->opp_tbl[idx].cpufreq_khz)

#define cpu_dvfs_get_max_freq(p)				(p->opp_tbl[0].cpufreq_khz)
#define cpu_dvfs_get_normal_max_freq(p)			(p->opp_tbl[p->idx_normal_max_opp].cpufreq_khz)
#define cpu_dvfs_get_min_freq(p)				(p->opp_tbl[p->nr_opp_tbl - 1].cpufreq_khz)

#define cpu_dvfs_get_cur_volt(p)				(p->opp_tbl[p->idx_opp_tbl].cpufreq_volt)
#define cpu_dvfs_get_volt_by_idx(p, idx)		(p->opp_tbl[idx].cpufreq_volt)

#define cpu_dvfs_is_extbuck_valid()     (is_da9214_exist() && is_da9214_sw_ready())

static struct mt_cpu_dvfs *id_to_cpu_dvfs(enum mt_cpu_dvfs_id id)
{
	return (id < NR_MT_CPU_DVFS) ? &cpu_dvfs[id] : NULL;
}

#ifdef CONFIG_HYBRID_CPU_DVFS
static inline unsigned int id_to_cluster(enum mt_cpu_dvfs_id id)
{
	if (id == MT_CPU_DVFS_LL)
		return CPU_CLUSTER_LL;
	if (id == MT_CPU_DVFS_L)
		return CPU_CLUSTER_L;
	if (id == MT_CPU_DVFS_B)
		return CPU_CLUSTER_B;
	if (id == MT_CPU_DVFS_CCI)
		return CPU_CLUSTER_CCI;

	BUG();
	return NUM_CPU_CLUSTER;
}

static inline unsigned int cpu_dvfs_to_cluster(struct mt_cpu_dvfs *p)
{
	if (p == &cpu_dvfs[MT_CPU_DVFS_LL])
		return CPU_CLUSTER_LL;
	if (p == &cpu_dvfs[MT_CPU_DVFS_L])
		return CPU_CLUSTER_L;
	if (p == &cpu_dvfs[MT_CPU_DVFS_B])
		return CPU_CLUSTER_B;
	if (p == &cpu_dvfs[MT_CPU_DVFS_CCI])
		return CPU_CLUSTER_CCI;

	BUG();
	return NUM_CPU_CLUSTER;
}

static inline struct mt_cpu_dvfs *cluster_to_cpu_dvfs(unsigned int cluster)
{
	if (cluster == CPU_CLUSTER_LL)
		return &cpu_dvfs[MT_CPU_DVFS_LL];
	if (cluster == CPU_CLUSTER_L)
		return &cpu_dvfs[MT_CPU_DVFS_L];
	if (cluster == CPU_CLUSTER_B)
		return &cpu_dvfs[MT_CPU_DVFS_B];
	if (cluster == CPU_CLUSTER_CCI)
		return &cpu_dvfs[MT_CPU_DVFS_CCI];

	BUG();
	return NULL;
}
#endif

static void aee_record_cpu_volt(struct mt_cpu_dvfs *p, unsigned int volt)
{
#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
	if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
		aee_rr_rec_cpu_dvfs_vproc_big(VOLT_TO_EXTBUCK_VAL(volt));
	else
		aee_rr_rec_cpu_dvfs_vproc_little(VOLT_TO_EXTBUCK_VAL(volt));
#endif
}

static void notify_cpu_volt_sampler(struct mt_cpu_dvfs *p, unsigned int volt)
{
	unsigned int mv = volt / 100;
	enum mt_cpu_dvfs_id id;

	if (!g_pCpuVoltSampler)
		return;

	id = (cpu_dvfs_is(p, MT_CPU_DVFS_LL) ? MT_CPU_DVFS_LL :
	      cpu_dvfs_is(p, MT_CPU_DVFS_L) ? MT_CPU_DVFS_L :
	      cpu_dvfs_is(p, MT_CPU_DVFS_B) ? MT_CPU_DVFS_B : MT_CPU_DVFS_CCI);
	g_pCpuVoltSampler(id, mv);
}


/* DVFS OPP table */
/* LL, FY, 1, 1*/
#define OPP_TBL(cluster, seg, lv, vol)	\
static struct mt_cpu_freq_info opp_tbl_##cluster##_e##lv##_0[] = {	\
	OP(CPU_DVFS_FREQ0_##cluster##_##seg, CPU_DVFS_VOLT0_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ1_##cluster##_##seg, CPU_DVFS_VOLT1_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ2_##cluster##_##seg, CPU_DVFS_VOLT2_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ3_##cluster##_##seg, CPU_DVFS_VOLT3_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ4_##cluster##_##seg, CPU_DVFS_VOLT4_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ5_##cluster##_##seg, CPU_DVFS_VOLT5_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ6_##cluster##_##seg, CPU_DVFS_VOLT6_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ7_##cluster##_##seg, CPU_DVFS_VOLT7_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ8_##cluster##_##seg, CPU_DVFS_VOLT8_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ9_##cluster##_##seg, CPU_DVFS_VOLT9_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ10_##cluster##_##seg, CPU_DVFS_VOLT10_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ11_##cluster##_##seg, CPU_DVFS_VOLT11_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ12_##cluster##_##seg, CPU_DVFS_VOLT12_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ13_##cluster##_##seg, CPU_DVFS_VOLT13_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ14_##cluster##_##seg, CPU_DVFS_VOLT14_VPROC##vol##_##seg),	\
	OP(CPU_DVFS_FREQ15_##cluster##_##seg, CPU_DVFS_VOLT15_VPROC##vol##_##seg),	\
}

OPP_TBL(LL, FY, 1, 1);
OPP_TBL(L, FY, 1, 1);
OPP_TBL(CCI, FY, 1, 1);
OPP_TBL(B, FY, 1, 2);
OPP_TBL(LL, SB, 2, 1);
OPP_TBL(L, SB, 2, 1);
OPP_TBL(CCI, SB, 2, 1);
OPP_TBL(B, SB, 2, 2);

#ifdef ENABLE_IDVFS
#define IDVFS_OPP_MAP(idx) CPU_IDVFS_FREQ##idx##_B_FY
/* #define IDVFS_OPP_MAP(idx) CPU_IDVFS_FREQ##idx##_B_SB */
static unsigned int idvfs_opp_tbls[] = {
	IDVFS_OPP_MAP(0),
	IDVFS_OPP_MAP(1),
	IDVFS_OPP_MAP(2),
	IDVFS_OPP_MAP(3),
	IDVFS_OPP_MAP(4),
	IDVFS_OPP_MAP(5),
	IDVFS_OPP_MAP(6),
	IDVFS_OPP_MAP(7),
	IDVFS_OPP_MAP(8),
	IDVFS_OPP_MAP(9),
	IDVFS_OPP_MAP(10),
	IDVFS_OPP_MAP(11),
	IDVFS_OPP_MAP(12),
	IDVFS_OPP_MAP(13),
	IDVFS_OPP_MAP(14),
	IDVFS_OPP_MAP(15),
};
#endif

/* 16 steps OPP table*/
#if 1
static struct mt_cpu_freq_method opp_tbl_method_LL_e1[] = {
	/* Target Frequency, POS, CLK */
	FP(CPU_DVFS_FREQ0_LL_FY,	1,	1),
	FP(CPU_DVFS_FREQ1_LL_FY,	1,	1),
	FP(CPU_DVFS_FREQ2_LL_FY,	1,	1),
	FP(CPU_DVFS_FREQ3_LL_FY,	1,	1),
	FP(CPU_DVFS_FREQ4_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ5_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ6_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ7_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ8_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ9_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ10_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ11_LL_FY,	2,	2),
	FP(CPU_DVFS_FREQ12_LL_FY,	2,	2),
	FP(CPU_DVFS_FREQ13_LL_FY,	2,	2),
	FP(CPU_DVFS_FREQ14_LL_FY,	2,	2),
	FP(CPU_DVFS_FREQ15_LL_FY,	2,	4),
};

static struct mt_cpu_freq_method opp_tbl_method_L_e1[] = {
	/* Target Frequency, POS, CLK */
	FP(CPU_DVFS_FREQ0_L_FY,		1,	1),
	FP(CPU_DVFS_FREQ1_L_FY,		1,	1),
	FP(CPU_DVFS_FREQ2_L_FY,		1,	1),
	FP(CPU_DVFS_FREQ3_L_FY,		1,	1),
	FP(CPU_DVFS_FREQ4_L_FY,		1,	1),
	FP(CPU_DVFS_FREQ5_L_FY,		1,	1),
	FP(CPU_DVFS_FREQ6_L_FY,		1,	1),
	FP(CPU_DVFS_FREQ7_L_FY,		1,	1),
	FP(CPU_DVFS_FREQ8_L_FY,		2,	1),
	FP(CPU_DVFS_FREQ9_L_FY,		2,	1),
	FP(CPU_DVFS_FREQ10_L_FY,	2,	1),
	FP(CPU_DVFS_FREQ11_L_FY,	2,	1),
	FP(CPU_DVFS_FREQ12_L_FY,	2,	1),
	FP(CPU_DVFS_FREQ13_L_FY,	2,	2),
	FP(CPU_DVFS_FREQ14_L_FY,	2,	2),
	FP(CPU_DVFS_FREQ15_L_FY,	2,	2),
};

static struct mt_cpu_freq_method opp_tbl_method_CCI_e1[] = {
	/* Target Frequency, POS, CLK */
	FP(CPU_DVFS_FREQ0_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ1_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ2_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ3_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ4_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ5_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ6_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ7_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ8_CCI_FY,	2,	2),
	FP(CPU_DVFS_FREQ9_CCI_FY,	2,	2),
	FP(CPU_DVFS_FREQ10_CCI_FY,	2,	2),
	FP(CPU_DVFS_FREQ11_CCI_FY,	2,	2),
	FP(CPU_DVFS_FREQ12_CCI_FY,	2,	2),
	FP(CPU_DVFS_FREQ13_CCI_FY,	2,	2),
	FP(CPU_DVFS_FREQ14_CCI_FY,	2,	4),
	FP(CPU_DVFS_FREQ15_CCI_FY,	2,	4),
};

static struct mt_cpu_freq_method opp_tbl_method_B_e1[] = {
	/* Target Frequency, POS, CLK */
	FP(CPU_DVFS_FREQ0_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ1_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ2_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ3_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ4_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ5_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ6_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ7_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ8_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ9_B_FY,		1,	1),
	FP(CPU_DVFS_FREQ10_B_FY,	1,	1),
	FP(CPU_DVFS_FREQ11_B_FY,	2,	1),
	FP(CPU_DVFS_FREQ12_B_FY,	2,	1),
	FP(CPU_DVFS_FREQ13_B_FY,	2,	1),
	FP(CPU_DVFS_FREQ14_B_FY,	2,	1),
	FP(CPU_DVFS_FREQ15_B_FY,	2,	2),
};

static struct mt_cpu_freq_method opp_tbl_method_LL_e2[] = {
	/* Target Frequency,		POS, CLK */
	FP(CPU_DVFS_FREQ0_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ1_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ2_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ3_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ4_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ5_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ6_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ7_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ8_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ9_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ10_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ11_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ12_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ13_LL_SB,	2,	2),
	FP(CPU_DVFS_FREQ14_LL_SB,	2,	2),
	FP(CPU_DVFS_FREQ15_LL_SB,	2,	4),
};

static struct mt_cpu_freq_method opp_tbl_method_L_e2[] = {
	/* Target Frequency,		POS, CLK */
	FP(CPU_DVFS_FREQ0_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ1_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ2_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ3_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ4_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ5_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ6_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ7_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ8_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ9_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ10_L_SB,	2,	1),
	FP(CPU_DVFS_FREQ11_L_SB,	2,	1),
	FP(CPU_DVFS_FREQ12_L_SB,	2,	1),
	FP(CPU_DVFS_FREQ13_L_SB,	2,	1),
	FP(CPU_DVFS_FREQ14_L_SB,	2,	2),
	FP(CPU_DVFS_FREQ15_L_SB,	2,	2),
};

static struct mt_cpu_freq_method opp_tbl_method_CCI_e2[] = {
	/* Target Frequency,	POS, CLK */
	FP(CPU_DVFS_FREQ0_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ1_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ2_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ3_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ4_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ5_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ6_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ7_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ8_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ9_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ10_CCI_SB,	2,	2),
	FP(CPU_DVFS_FREQ11_CCI_SB,	2,	2),
	FP(CPU_DVFS_FREQ12_CCI_SB,	2,	2),
	FP(CPU_DVFS_FREQ13_CCI_SB,	2,	2),
	FP(CPU_DVFS_FREQ14_CCI_SB,	2,	4),
	FP(CPU_DVFS_FREQ15_CCI_SB,	2,	4),
};

static struct mt_cpu_freq_method opp_tbl_method_B_e2[] = {
	/* Target Frequency,	POS, CLK */
	FP(CPU_DVFS_FREQ0_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ1_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ2_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ3_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ4_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ5_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ6_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ7_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ8_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ9_B_SB,		1,	1),
	FP(CPU_DVFS_FREQ10_B_SB,	1,	1),
	FP(CPU_DVFS_FREQ11_B_SB,	2,	1),
	FP(CPU_DVFS_FREQ12_B_SB,	2,	1),
	FP(CPU_DVFS_FREQ13_B_SB,	2,	1),
	FP(CPU_DVFS_FREQ14_B_SB,	2,	1),
	FP(CPU_DVFS_FREQ15_B_SB,	2,	2),
};
#endif

/* Method Table */
#if 0
static struct mt_cpu_freq_method opp_tbl_method_LL_e1[] = {
	/* Target Frequency, POS, CLK */
	FP(CPU_DVFS_FREQ0_LL_FY,	1,	1),
	FP(CPU_DVFS_FREQ1_LL_FY,	1,	1),
	FP(CPU_DVFS_FREQ2_LL_FY,	1,	1),
	FP(CPU_DVFS_FREQ3_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ4_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ5_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ6_LL_FY,	2,	1),
	FP(CPU_DVFS_FREQ7_LL_FY,	2,	2),
};

static struct mt_cpu_freq_method opp_tbl_method_L_e1[] = {
	/* Target Frequency, POS, CLK */
	FP(CPU_DVFS_FREQ0_L_FY,	1,	1),
	FP(CPU_DVFS_FREQ1_L_FY,	1,	1),
	FP(CPU_DVFS_FREQ2_L_FY,	1,	1),
	FP(CPU_DVFS_FREQ3_L_FY,	1,	1),
	FP(CPU_DVFS_FREQ4_L_FY,	1,	1),
	FP(CPU_DVFS_FREQ5_L_FY,	2,	1),
	FP(CPU_DVFS_FREQ6_L_FY,	2,	1),
	FP(CPU_DVFS_FREQ7_L_FY,	2,	2),
};

static struct mt_cpu_freq_method opp_tbl_method_CCI_e1[] = {
	/* Target Frequency, POS, CLK */
	FP(CPU_DVFS_FREQ0_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ1_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ2_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ3_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ4_CCI_FY,	2,	1),
	FP(CPU_DVFS_FREQ5_CCI_FY,	2,	2),
	FP(CPU_DVFS_FREQ6_CCI_FY,	2,	2),
	FP(CPU_DVFS_FREQ7_CCI_FY,	2,	4),
};

static struct mt_cpu_freq_method opp_tbl_method_B_e1[] = {
	/* Target Frequency, POS, CLK */
	FP(CPU_DVFS_FREQ0_B_FY,	1,	1),
	FP(CPU_DVFS_FREQ1_B_FY,	1,	1),
	FP(CPU_DVFS_FREQ2_B_FY,	1,	1),
	FP(CPU_DVFS_FREQ3_B_FY,	1,	1),
	FP(CPU_DVFS_FREQ4_B_FY,	1,	1),
	FP(CPU_DVFS_FREQ5_B_FY,	2,	1),
	FP(CPU_DVFS_FREQ6_B_FY,	2,	1),
	FP(CPU_DVFS_FREQ7_B_FY,	2,	2),
};

static struct mt_cpu_freq_method opp_tbl_method_LL_e2[] = {
	/* Target Frequency,		POS, CLK */
	FP(CPU_DVFS_FREQ0_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ1_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ2_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ3_LL_SB,	1,	1),
	FP(CPU_DVFS_FREQ4_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ5_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ6_LL_SB,	2,	1),
	FP(CPU_DVFS_FREQ7_LL_SB,	2,	4),
};

static struct mt_cpu_freq_method opp_tbl_method_L_e2[] = {
	/* Target Frequency,		POS, CLK */
	FP(CPU_DVFS_FREQ0_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ1_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ2_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ3_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ4_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ5_L_SB,		1,	1),
	FP(CPU_DVFS_FREQ6_L_SB,		2,	1),
	FP(CPU_DVFS_FREQ7_L_SB,		2,	2),
};

static struct mt_cpu_freq_method opp_tbl_method_CCI_e2[] = {
	/* Target Frequency,	POS, CLK */
	FP(CPU_DVFS_FREQ0_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ1_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ2_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ3_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ4_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ5_CCI_SB,	2,	1),
	FP(CPU_DVFS_FREQ6_CCI_SB,	2,	2),
	FP(CPU_DVFS_FREQ7_CCI_SB,	2,	4),
};

static struct mt_cpu_freq_method opp_tbl_method_B_e2[] = {
	/* Target Frequency,	POS, CLK */
	FP(CPU_DVFS_FREQ0_B_SB,	1,	1),
	FP(CPU_DVFS_FREQ1_B_SB,	1,	1),
	FP(CPU_DVFS_FREQ2_B_SB,	1,	1),
	FP(CPU_DVFS_FREQ3_B_SB,	1,	1),
	FP(CPU_DVFS_FREQ4_B_SB,	1,	1),
	FP(CPU_DVFS_FREQ5_B_SB,	1,	1),
	FP(CPU_DVFS_FREQ6_B_SB,	2,	1),
	FP(CPU_DVFS_FREQ7_B_SB,	2,	2),
};
#endif

struct opp_tbl_info {
	struct mt_cpu_freq_info *const opp_tbl;
	const int size;
};

static struct opp_tbl_info opp_tbls[NR_MT_CPU_DVFS][2] = {
	/* LL */
	{
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)] = {opp_tbl_LL_e1_0, ARRAY_SIZE(opp_tbl_LL_e1_0),},
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_1)] = {opp_tbl_LL_e2_0, ARRAY_SIZE(opp_tbl_LL_e2_0),},
	},
	/* L */
	{
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)] = {opp_tbl_L_e1_0, ARRAY_SIZE(opp_tbl_L_e1_0),},
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_1)] = {opp_tbl_L_e2_0, ARRAY_SIZE(opp_tbl_L_e2_0),},
	},
	/* B */
	{
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)] = {opp_tbl_B_e1_0, ARRAY_SIZE(opp_tbl_B_e1_0),},
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_1)] = {opp_tbl_B_e2_0, ARRAY_SIZE(opp_tbl_B_e2_0),},
	},
	/* CCI */
	{
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)] = {opp_tbl_CCI_e1_0, ARRAY_SIZE(opp_tbl_CCI_e1_0),},
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_1)] = {opp_tbl_CCI_e2_0, ARRAY_SIZE(opp_tbl_CCI_e2_0),},
	},
};

struct opp_tbl_m_info {
	struct mt_cpu_freq_method *const opp_tbl_m;
	const int size;
};

static struct opp_tbl_m_info opp_tbls_m[NR_MT_CPU_DVFS][2] = {
	/* LL */
	{
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)] = {opp_tbl_method_LL_e1, ARRAY_SIZE(opp_tbl_method_LL_e1),},
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_1)] = {opp_tbl_method_LL_e2, ARRAY_SIZE(opp_tbl_method_LL_e2),},
	},
	/* L */
	{
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)] = {opp_tbl_method_L_e1, ARRAY_SIZE(opp_tbl_method_L_e1),},
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_1)] = {opp_tbl_method_L_e2, ARRAY_SIZE(opp_tbl_method_L_e2),},
	},
	/* B */
	{
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)] = {opp_tbl_method_B_e1, ARRAY_SIZE(opp_tbl_method_B_e1),},
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_1)] = {opp_tbl_method_B_e2, ARRAY_SIZE(opp_tbl_method_B_e2),},
	},
	/* CCI */
	{
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)] = {opp_tbl_method_CCI_e1, ARRAY_SIZE(opp_tbl_method_CCI_e1),},
		[CPU_LV_TO_OPP_IDX(CPU_LEVEL_1)] = {opp_tbl_method_CCI_e2, ARRAY_SIZE(opp_tbl_method_CCI_e2),},
	},
};

static struct notifier_block __refdata _mt_cpufreq_cpu_notifier = {
	.notifier_call = _mt_cpufreq_cpu_CB,
};

/* for freq change (PLL/MUX) */
#define PLL_FREQ_STEP		(13000)	/* KHz */

#define PLL_MIN_FREQ		(130000)	/* KHz */
#define PLL_DIV1_FREQ		(1001000)	/* KHz */
#define PLL_DIV2_FREQ		(520000)	/* KHz */
#define PLL_DIV4_FREQ		(260000)	/* KHz */
#define PLL_DIV8_FREQ		(PLL_MIN_FREQ)	/* KHz */

#define DDS_DIV1_FREQ		(0x0009A000)	/* 1001MHz */
#define DDS_DIV2_FREQ		(0x010A0000)	/* 520MHz  */
#define DDS_DIV4_FREQ		(0x020A0000)	/* 260MHz  */
#define DDS_DIV8_FREQ		(0x030A0000)	/* 130MHz  */

int get_turbo_freq(int chip_id, int freq)
{
	return freq;
}

int get_turbo_volt(int chip_id, int volt)
{
	return volt;
}

static int _search_available_freq_idx(struct mt_cpu_dvfs *p, unsigned int target_khz,
				      unsigned int relation)
{
	int new_opp_idx = -1;
	int i;

	FUNC_ENTER(FUNC_LV_HELP);

	if (CPUFREQ_RELATION_L == relation) {
		for (i = (signed)(p->nr_opp_tbl - 1); i >= 0; i--) {
			if (cpu_dvfs_get_freq_by_idx(p, i) >= target_khz) {
				new_opp_idx = i;
				break;
			}
		}
	} else {		/* CPUFREQ_RELATION_H */
		for (i = 0; i < (signed)p->nr_opp_tbl; i++) {
			if (cpu_dvfs_get_freq_by_idx(p, i) <= target_khz) {
				new_opp_idx = i;
				break;
			}
		}
	}

	FUNC_EXIT(FUNC_LV_HELP);

	return new_opp_idx;
}

/* for PTP-OD */
static mt_cpufreq_set_ptbl_funcPTP mt_cpufreq_update_private_tbl;

void mt_cpufreq_set_ptbl_registerCB(mt_cpufreq_set_ptbl_funcPTP pCB)
{
	mt_cpufreq_update_private_tbl = pCB;
}
EXPORT_SYMBOL(mt_cpufreq_set_ptbl_registerCB);

static int _set_cur_volt_locked(struct mt_cpu_dvfs *p, unsigned int volt)	/* volt: mv * 100 */
{
	int ret = -1;

	FUNC_ENTER(FUNC_LV_HELP);

	BUG_ON(NULL == p);

	if (!cpu_dvfs_is_available(p)) {
		FUNC_EXIT(FUNC_LV_HELP);
		return 0;
	}

	/* set volt */
	ret = p->ops->set_cur_volt(p, volt);

	FUNC_EXIT(FUNC_LV_HELP);

	return ret;
}

static int _restore_default_volt_b(struct mt_cpu_dvfs *p, enum mt_cpu_dvfs_id id)
{
	unsigned long flags;
	int i;
	int ret = -1;
	unsigned int freq = 0;
	unsigned int volt = 0;
	int idx = 0;

	FUNC_ENTER(FUNC_LV_HELP);

	BUG_ON(NULL == p);

	if (!cpu_dvfs_is_available(p)) {
		FUNC_EXIT(FUNC_LV_HELP);
		return 0;
	}

	cpufreq_lock(flags);

	/* restore to default volt */
	for (i = 0; i < p->nr_opp_tbl; i++)
		p->opp_tbl[i].cpufreq_volt = p->opp_tbl[i].cpufreq_volt_org;

	if (NULL != mt_cpufreq_update_private_tbl)
			mt_cpufreq_update_private_tbl(id, 1);

	freq = p->ops->get_cur_phy_freq(p);
	volt = p->ops->get_cur_volt(p);

	if (freq > cpu_dvfs_get_max_freq(p))
		idx = 0;
	else
		idx = _search_available_freq_idx(p, freq, CPUFREQ_RELATION_L);

	/* set volt */
	ret = _set_cur_volt_locked(p,
					   get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)));

	cpufreq_unlock(flags);

	FUNC_EXIT(FUNC_LV_HELP);

	return ret;
}

static int _restore_default_volt(struct mt_cpu_dvfs *p, enum mt_cpu_dvfs_id id)
{
	unsigned long flags;
	int i;
	int ret = -1;
	unsigned int freq = 0;
	unsigned int volt = 0;
	int idx = 0;
	struct mt_cpu_dvfs *p_ll, *p_l, *p_cci;

	p_ll = id_to_cpu_dvfs(MT_CPU_DVFS_LL);
	p_l = id_to_cpu_dvfs(MT_CPU_DVFS_L);
	p_cci = id_to_cpu_dvfs(MT_CPU_DVFS_CCI);

	FUNC_ENTER(FUNC_LV_HELP);

	if (!cpu_dvfs_is_available(p_ll) || !cpu_dvfs_is_available(p_l) || !cpu_dvfs_is_available(p_cci)) {
		FUNC_EXIT(FUNC_LV_HELP);
		return 0;
	}

	cpufreq_lock(flags);

	/* restore to default volt */
	for (i = 0; i < p->nr_opp_tbl; i++)
		p->opp_tbl[i].cpufreq_volt = p->opp_tbl[i].cpufreq_volt_org;

	if (NULL != mt_cpufreq_update_private_tbl)
			mt_cpufreq_update_private_tbl(id, 1);

	freq = p->ops->get_cur_phy_freq(p);
	volt = p->ops->get_cur_volt(p);

	if (freq > cpu_dvfs_get_max_freq(p))
		idx = 0;
	else
		idx = _search_available_freq_idx(p, freq, CPUFREQ_RELATION_L);

	/* set volt */
	if (cpu_dvfs_is(p, MT_CPU_DVFS_CCI)) {
		if (get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)) >
			MAX(get_turbo_volt(p_ll->cpu_id, cpu_dvfs_get_volt_by_idx(p_ll, p_ll->idx_opp_tbl)),
				get_turbo_volt(p_l->cpu_id, cpu_dvfs_get_volt_by_idx(p_l, p_l->idx_opp_tbl))))
				ret = _set_cur_volt_locked(p,
					get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)));
	} else if (cpu_dvfs_is(p, MT_CPU_DVFS_LL)) {
		if (get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)) >
			MAX(get_turbo_volt(p_cci->cpu_id, cpu_dvfs_get_volt_by_idx(p_cci, p_cci->idx_opp_tbl)),
				get_turbo_volt(p_l->cpu_id, cpu_dvfs_get_volt_by_idx(p_l, p_l->idx_opp_tbl))))
				ret = _set_cur_volt_locked(p,
					get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)));
	} else {
		if (get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)) >
			MAX(get_turbo_volt(p_cci->cpu_id, cpu_dvfs_get_volt_by_idx(p_cci, p_cci->idx_opp_tbl)),
				get_turbo_volt(p_ll->cpu_id, cpu_dvfs_get_volt_by_idx(p_ll, p_ll->idx_opp_tbl))))
				ret = _set_cur_volt_locked(p,
					get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)));
	}

	cpufreq_unlock(flags);

	FUNC_EXIT(FUNC_LV_HELP);

	return ret;
}

unsigned int mt_cpufreq_get_freq_by_idx(enum mt_cpu_dvfs_id id, int idx)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	FUNC_ENTER(FUNC_LV_API);

	BUG_ON(NULL == p);

	if (!cpu_dvfs_is_available(p)) {
		FUNC_EXIT(FUNC_LV_API);
		return 0;
	}

	BUG_ON(idx >= p->nr_opp_tbl);

	FUNC_EXIT(FUNC_LV_API);

	return cpu_dvfs_get_freq_by_idx(p, idx);
}
EXPORT_SYMBOL(mt_cpufreq_get_freq_by_idx);

int mt_cpufreq_update_volt_b(enum mt_cpu_dvfs_id id, unsigned int *volt_tbl, int nr_volt_tbl)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);
	unsigned long flags;
	int i;
	int ret = -1;
	unsigned int freq = 0;
	unsigned int volt = 0;
	int idx = 0;

	FUNC_ENTER(FUNC_LV_API);

	BUG_ON(NULL == p);

	if (!cpu_dvfs_is_available(p)) {
		FUNC_EXIT(FUNC_LV_API);
		return 0;
	}

	BUG_ON(nr_volt_tbl > p->nr_opp_tbl);

	cpufreq_lock(flags);

	/* update volt table */
	for (i = 0; i < nr_volt_tbl; i++)
		p->opp_tbl[i].cpufreq_volt = EXTBUCK_VAL_TO_VOLT(volt_tbl[i]);

	if (NULL != mt_cpufreq_update_private_tbl)
			mt_cpufreq_update_private_tbl(id, 0);

	freq = p->ops->get_cur_phy_freq(p);
	volt = p->ops->get_cur_volt(p);

	if (freq > cpu_dvfs_get_max_freq(p))
		idx = 0;
	else
		idx = _search_available_freq_idx(p, freq, CPUFREQ_RELATION_L);

	/* set volt */
		ret = _set_cur_volt_locked(p,
					   get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)));

	cpufreq_unlock(flags);

	FUNC_EXIT(FUNC_LV_API);

	return ret;
}
EXPORT_SYMBOL(mt_cpufreq_update_volt_b);

int mt_cpufreq_update_volt(enum mt_cpu_dvfs_id id, unsigned int *volt_tbl, int nr_volt_tbl)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);
	unsigned long flags;
	int i;
	int ret = -1;
	unsigned int freq = 0;
	unsigned int volt = 0;
	int idx = 0;
	struct mt_cpu_dvfs *p_ll, *p_l, *p_cci;

	p_ll = id_to_cpu_dvfs(MT_CPU_DVFS_LL);
	p_l = id_to_cpu_dvfs(MT_CPU_DVFS_L);
	p_cci = id_to_cpu_dvfs(MT_CPU_DVFS_CCI);

	if (!cpu_dvfs_is_available(p_ll) || !cpu_dvfs_is_available(p_l) || !cpu_dvfs_is_available(p_cci)) {
		FUNC_EXIT(FUNC_LV_HELP);
		return 0;
	}

	BUG_ON(nr_volt_tbl > p->nr_opp_tbl);

	cpufreq_lock(flags);

	/* update volt table */
	for (i = 0; i < nr_volt_tbl; i++)
		p->opp_tbl[i].cpufreq_volt = EXTBUCK_VAL_TO_VOLT(volt_tbl[i]);

	if (NULL != mt_cpufreq_update_private_tbl)
			mt_cpufreq_update_private_tbl(id, 0);

	freq = p->ops->get_cur_phy_freq(p);
	volt = p->ops->get_cur_volt(p);

	if (freq > cpu_dvfs_get_max_freq(p))
		idx = 0;
	else
		idx = _search_available_freq_idx(p, freq, CPUFREQ_RELATION_L);

	/* set volt */
	if (cpu_dvfs_is(p, MT_CPU_DVFS_CCI)) {
		if (get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)) >
			MAX(get_turbo_volt(p_ll->cpu_id, cpu_dvfs_get_volt_by_idx(p_ll, p_ll->idx_opp_tbl)),
				get_turbo_volt(p_l->cpu_id, cpu_dvfs_get_volt_by_idx(p_l, p_l->idx_opp_tbl))))
				ret = _set_cur_volt_locked(p,
					get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)));
	} else if (cpu_dvfs_is(p, MT_CPU_DVFS_LL)) {
		if (get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)) >
			MAX(get_turbo_volt(p_cci->cpu_id, cpu_dvfs_get_volt_by_idx(p_cci, p_cci->idx_opp_tbl)),
				get_turbo_volt(p_l->cpu_id, cpu_dvfs_get_volt_by_idx(p_l, p_l->idx_opp_tbl))))
				ret = _set_cur_volt_locked(p,
					get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)));
	} else {
		if (get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)) >
			MAX(get_turbo_volt(p_cci->cpu_id, cpu_dvfs_get_volt_by_idx(p_cci, p_cci->idx_opp_tbl)),
				get_turbo_volt(p_ll->cpu_id, cpu_dvfs_get_volt_by_idx(p_ll, p_ll->idx_opp_tbl))))
				ret = _set_cur_volt_locked(p,
					get_turbo_volt(p->cpu_id, cpu_dvfs_get_volt_by_idx(p, idx)));
	}

	cpufreq_unlock(flags);

	FUNC_EXIT(FUNC_LV_API);

	return ret;
}
EXPORT_SYMBOL(mt_cpufreq_update_volt);

void mt_cpufreq_restore_default_volt(enum mt_cpu_dvfs_id id)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	FUNC_ENTER(FUNC_LV_API);

	BUG_ON(NULL == p);

	if (!cpu_dvfs_is_available(p)) {
		FUNC_EXIT(FUNC_LV_API);
		return;
	}

	if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
		_restore_default_volt_b(p, id);
	else
		_restore_default_volt(p, id);

	FUNC_EXIT(FUNC_LV_API);
}
EXPORT_SYMBOL(mt_cpufreq_restore_default_volt);
/* for PTP-OD End*/

/* for PBM */
unsigned int mt_cpufreq_get_leakage_mw(enum mt_cpu_dvfs_id id)
{
	return 0;
}

#ifndef DISABLE_PBM_FEATURE
static void _kick_PBM_by_cpu(struct mt_cpu_dvfs *p)
{
#if 1
	struct mt_cpu_dvfs *p_dvfs[NR_MT_CPU_DVFS];
	struct cpumask dvfs_cpumask[NR_MT_CPU_DVFS];
	struct cpumask cpu_online_cpumask[NR_MT_CPU_DVFS];
	struct ppm_cluster_status ppm_data[NR_MT_CPU_DVFS];
	int i;

	for_each_cpu_dvfs_only(i, p) {
		arch_get_cluster_cpus(&dvfs_cpumask[i], i);
		cpumask_and(&cpu_online_cpumask[i], &dvfs_cpumask[i], cpu_online_mask);

		p_dvfs[i] = id_to_cpu_dvfs(i);
		ppm_data[i].core_num = cpumask_weight(&cpu_online_cpumask[i]);
		ppm_data[i].freq_idx = p_dvfs[i]->idx_opp_tbl;
		ppm_data[i].volt = p_dvfs[i]->ops->get_cur_volt(p_dvfs[i]) / 100;

		cpufreq_ver("%d: core = %d, idx = %d, volt = %d\n",
			i, ppm_data[i].core_num, ppm_data[i].freq_idx, ppm_data[i].volt);
	}

	mt_ppm_dlpt_kick_PBM(ppm_data, (unsigned int)arch_get_nr_clusters());
#endif
}
#endif
/* for PBM End */

/* Frequency API */
static unsigned int _cpu_freq_calc(unsigned int con1, unsigned int ckdiv1)
{
	unsigned int freq = 0;
	unsigned int posdiv = 0;

	posdiv = _GET_BITS_VAL_(26:24, con1);

	con1 &= _BITMASK_(20:0);
	freq = ((con1 * 26) >> 14) * 1000;

	FUNC_ENTER(FUNC_LV_HELP);

	switch (posdiv) {
	case 0:
		break;
	case 1:
		freq = freq / 2;
		break;
	case 2:
		freq = freq / 4;
		break;
	default:
		break;
	};

	switch (ckdiv1) {
	case 9:
		freq = freq * 3 / 4;
		break;

	case 10:
		freq = freq * 2 / 4;
		break;

	case 11:
		freq = freq * 1 / 4;
		break;

	case 17:
		freq = freq * 4 / 5;
		break;

	case 18:
		freq = freq * 3 / 5;
		break;

	case 19:
		freq = freq * 2 / 5;
		break;

	case 20:
		freq = freq * 1 / 5;
		break;

	case 25:
		freq = freq * 5 / 6;
		break;

	case 26:
		freq = freq * 4 / 6;
		break;

	case 27:
		freq = freq * 3 / 6;
		break;

	case 28:
		freq = freq * 2 / 6;
		break;

	case 29:
		freq = freq * 1 / 6;
		break;

	case 8:
	case 16:
	case 24:
	default:
		break;
	}

	FUNC_EXIT(FUNC_LV_HELP);

	return freq;
}

#ifdef ENABLE_IDVFS
static unsigned int idvfs_get_cur_phy_freq_b(struct mt_cpu_dvfs *p)
{
	return cpu_dvfs_get_cur_freq(p);
}
#endif

static unsigned int get_cur_phy_freq_b(struct mt_cpu_dvfs *p)
{
	unsigned int freq;
	unsigned int pcw;
	unsigned int posdiv;
	unsigned int ckdiv1;

	FUNC_ENTER(FUNC_LV_LOCAL);

	BUG_ON(NULL == p);

	freq = BigiDVFSPllGetFreq(); /* Mhz */
	pcw = BigiDVFSPLLGetPCW();
	posdiv = BigiDVFSPOSDIVGet();

	ckdiv1 = cpufreq_read(ARMPLLDIV_CKDIV);
	ckdiv1 = _GET_BITS_VAL_(4:0, ckdiv1);

	if (ckdiv1 == 10)
		freq = freq * 1000 / 2;
	else
		freq = freq * 1000;

	cpufreq_ver("@%s: freq = %d, pcw = 0x%x, posdiv = 0x%x, ckdiv1 = 0x%x\n",
		__func__, freq, pcw, posdiv, ckdiv1);

	FUNC_EXIT(FUNC_LV_LOCAL);

	return freq;
}

static unsigned int get_cur_phy_freq(struct mt_cpu_dvfs *p)
{
	unsigned int con1;
	unsigned int ckdiv1;
	unsigned int cur_khz;

	FUNC_ENTER(FUNC_LV_LOCAL);

	BUG_ON(NULL == p);

	con1 = cpufreq_read(p->armpll_addr);
	ckdiv1 = cpufreq_read((unsigned int *)ARMPLLDIV_CKDIV);
	ckdiv1 = (cpu_dvfs_is(p, MT_CPU_DVFS_LL)) ? _GET_BITS_VAL_(9:5, ckdiv1) :
		(cpu_dvfs_is(p, MT_CPU_DVFS_L)) ? _GET_BITS_VAL_(14:10, ckdiv1) :
		(cpu_dvfs_is(p, MT_CPU_DVFS_CCI)) ? _GET_BITS_VAL_(19:15, ckdiv1) : _GET_BITS_VAL_(9:5, ckdiv1);

	cur_khz = _cpu_freq_calc(con1, ckdiv1);

	cpufreq_ver("@%s: cur_khz = %d, con1 = 0x%x, ckdiv1_val = 0x%x\n", __func__, cur_khz, con1,
		    ckdiv1);

	FUNC_EXIT(FUNC_LV_LOCAL);

	return cur_khz;
}

unsigned int mt_cpufreq_get_cur_phy_freq(enum mt_cpu_dvfs_id id)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);
	unsigned int freq = 0;
	unsigned long flags;

	FUNC_ENTER(FUNC_LV_LOCAL);

	BUG_ON(NULL == p);

	cpufreq_lock(flags);
	freq = p->ops->get_cur_phy_freq(p);
	cpufreq_unlock(flags);

	FUNC_EXIT(FUNC_LV_LOCAL);

	return freq;
}

static unsigned int _cpu_dds_calc(unsigned int khz)
{
	unsigned int dds = 0;

	FUNC_ENTER(FUNC_LV_HELP);

	dds = ((khz / 1000) << 14) / 26;

	FUNC_EXIT(FUNC_LV_HELP);

	return dds;
}

static void _cpu_clock_switch(struct mt_cpu_dvfs *p, enum top_ckmuxsel sel)
{
	FUNC_ENTER(FUNC_LV_HELP);

	switch (sel) {
	case TOP_CKMUXSEL_CLKSQ:
	case TOP_CKMUXSEL_ARMPLL:
		cpufreq_write_mask(CLK_MISC_CFG_0, 5 : 4, 0x0);
		break;
	case TOP_CKMUXSEL_MAINPLL:
	case TOP_CKMUXSEL_UNIVPLL:
		cpufreq_write_mask(CLK_MISC_CFG_0, 5 : 4, 0x3);
		break;
	default:
		break;
	};

	BUG_ON(sel >= NR_TOP_CKMUXSEL);

	if (cpu_dvfs_is(p, MT_CPU_DVFS_LL))
		cpufreq_write_mask(ARMPLLDIV_MUXSEL, 3 : 2, sel);
	else if (cpu_dvfs_is(p, MT_CPU_DVFS_L))
		cpufreq_write_mask(ARMPLLDIV_MUXSEL, 5 : 4, sel);
	else if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
		cpufreq_write_mask(ARMPLLDIV_MUXSEL, 1 : 0, sel);
	else /* CCI */
		cpufreq_write_mask(ARMPLLDIV_MUXSEL, 7 : 6, sel);

	FUNC_EXIT(FUNC_LV_HELP);
}

static enum top_ckmuxsel _get_cpu_clock_switch(struct mt_cpu_dvfs *p)
{
	unsigned int val = cpufreq_read(ARMPLLDIV_MUXSEL);
	unsigned int mask;

	FUNC_ENTER(FUNC_LV_HELP);

	mask = (cpu_dvfs_is(p, MT_CPU_DVFS_LL)) ? _BITMASK_(3:2) :
		(cpu_dvfs_is(p, MT_CPU_DVFS_L)) ? _BITMASK_(5:4) :
		(cpu_dvfs_is(p, MT_CPU_DVFS_B)) ? _BITMASK_(1:0) :
		(cpu_dvfs_is(p, MT_CPU_DVFS_CCI)) ? _BITMASK_(7:6) : _BITMASK_(3:2);

	val &= mask;

	val = (cpu_dvfs_is(p, MT_CPU_DVFS_LL)) ? val >> 2 :
		(cpu_dvfs_is(p, MT_CPU_DVFS_L)) ? val >> 4 :
		(cpu_dvfs_is(p, MT_CPU_DVFS_B)) ? val :
		(cpu_dvfs_is(p, MT_CPU_DVFS_CCI)) ? val >> 6 : val >> 2;

	FUNC_EXIT(FUNC_LV_HELP);

	return val;
}

int mt_cpufreq_clock_switch(enum mt_cpu_dvfs_id id, enum top_ckmuxsel sel)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	if (!p)
		return -1;

	_cpu_clock_switch(p, sel);

	return 0;
}

enum top_ckmuxsel mt_cpufreq_get_clock_switch(enum mt_cpu_dvfs_id id)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	if (!p)
		return -1;

	return _get_cpu_clock_switch(p);
}

#define POS_SETTLE_TIME (2)
static void adjust_armpll_dds(struct mt_cpu_dvfs *p, unsigned int vco, unsigned int pos_div)
{
	unsigned int cur_volt = 0;
	int restore_volt = 0;
	unsigned int dds;
	int shift;
	unsigned int reg;

	if (cpu_dvfs_is(p, MT_CPU_DVFS_B)) {
		cur_volt = p->ops->get_cur_volt(p);
		if (cur_volt < cpu_dvfs_get_volt_by_idx(p, 9)) {
			restore_volt = 1;
			p->ops->set_cur_volt(p, cpu_dvfs_get_volt_by_idx(p, 9));
		}
	}
	_cpu_clock_switch(p, TOP_CKMUXSEL_MAINPLL);

	if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
		BigiDVFSPllSetFreq(vco/pos_div/1000); /* Mhz */
	else {
		shift = (pos_div == 1) ? 0 :
			(pos_div == 2) ? 1 :
			(pos_div == 4) ? 2 : 0;

		dds = _cpu_dds_calc(vco);
		/* dds = _GET_BITS_VAL_(20:0, _cpu_dds_calc(vco)); */
		reg = cpufreq_read(p->armpll_addr);
		dds = (((reg & ~(_BITMASK_(26:24))) | (shift << 24)) & ~(_BITMASK_(20:0))) | dds;
		/* dbg_print("DVFS: Set ARMPLL CON1: 0x%x as 0x%x\n", p->armpll_addr, dds | _BIT_(31)); */
		cpufreq_write(p->armpll_addr, dds | _BIT_(31)); /* CHG */
	}
	udelay(PLL_SETTLE_TIME);
	_cpu_clock_switch(p, TOP_CKMUXSEL_ARMPLL);

	if (restore_volt > 0)
		p->ops->set_cur_volt(p, cur_volt);
}

static void adjust_posdiv(struct mt_cpu_dvfs *p, unsigned int pos_div)
{
	unsigned int cur_volt = 0;
	unsigned int dds;
	int shift;
	unsigned int reg;

	if (0) {
		cur_volt = p->ops->get_cur_volt(p);
		p->ops->set_cur_volt(p, cpu_dvfs_get_volt_by_idx(p, 0));
	}

	_cpu_clock_switch(p, TOP_CKMUXSEL_MAINPLL);

	if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
		BigiDVFSPOSDIVSet(pos_div);
	else {
		shift = (pos_div == 1) ? 0 :
			(pos_div == 2) ? 1 :
			(pos_div == 4) ? 2 : 0;

		reg = cpufreq_read(p->armpll_addr);
		dds = (reg & ~(_BITMASK_(26:24))) | (shift << 24);
		/* dbg_print("DVFS: Set POSDIV CON1: 0x%x as 0x%x\n", p->armpll_addr, dds | _BIT_(31)); */
		cpufreq_write(p->armpll_addr, dds | _BIT_(31)); /* CHG */
	}
	udelay(POS_SETTLE_TIME);
	_cpu_clock_switch(p, TOP_CKMUXSEL_ARMPLL);

	if (0)
		p->ops->set_cur_volt(p, cur_volt);
}

static void adjust_clkdiv(struct mt_cpu_dvfs *p, unsigned int clk_div)
{
	unsigned int sel = 0;

	sel = (clk_div == 1) ? 8 :
		(clk_div == 2) ? 10 :
		(clk_div == 4) ? 11 : 8;

	if (cpu_dvfs_is(p, MT_CPU_DVFS_LL))
		cpufreq_write_mask(ARMPLLDIV_CKDIV, 9 : 5, sel);
	else if (cpu_dvfs_is(p, MT_CPU_DVFS_L))
		cpufreq_write_mask(ARMPLLDIV_CKDIV, 14 : 10, sel);
	else if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
		cpufreq_write_mask(ARMPLLDIV_CKDIV, 4 : 0, sel);
	else
		cpufreq_write_mask(ARMPLLDIV_CKDIV, 19 : 15, sel);

	udelay(POS_SETTLE_TIME);
}

static int search_table_idx_by_freq(struct mt_cpu_dvfs *p, unsigned int freq)
{
	int i;
	int idx = -1;

	for (i = 0; i < p->nr_opp_tbl; i++) {
		if (p->freq_tbl[i].target_f == freq) {
			idx = i;
			break;
		}
	}

	if (idx == -1)
		idx = p->idx_opp_tbl;

	return idx;
}

#ifdef ENABLE_IDVFS
static void idvfs_set_cur_freq(struct mt_cpu_dvfs *p, unsigned int cur_khz, unsigned int target_khz)
{
	int ret;
	int idx;

	idx = _search_available_freq_idx(p, target_khz, CPUFREQ_RELATION_L);

	if (idx < 0)
		idx = 0;

	ret = BigIDVFSFreq(idvfs_opp_tbls[idx]);
}
#endif

static void set_cur_freq(struct mt_cpu_dvfs *p, unsigned int cur_khz, unsigned int target_khz)
{
	int idx;
	unsigned int dds = 0;

	FUNC_ENTER(FUNC_LV_LOCAL);

	cpufreq_ver("%s: cur_khz = %d, target_khz = %d\n",
		__func__, cur_khz, target_khz);

	/* CUR_OPP_IDX */
	opp_tbl_m[CUR_OPP_IDX].p = p;
	opp_tbl_m[CUR_OPP_IDX].slot = &p->freq_tbl[p->idx_opp_tbl];
	cpufreq_ver("[CUR_OPP_IDX][NAME][IDX][FREQ] => %s:%d:%d\n",
		cpu_dvfs_get_name(p), p->idx_opp_tbl, cpu_dvfs_get_freq_by_idx(p, p->idx_opp_tbl));

	/* TARGET_OPP_IDX */
	opp_tbl_m[TARGET_OPP_IDX].p = p;
	idx = search_table_idx_by_freq(opp_tbl_m[TARGET_OPP_IDX].p,
		target_khz);
	opp_tbl_m[TARGET_OPP_IDX].slot = &p->freq_tbl[idx];
	cpufreq_ver("[TARGET_OPP_IDX][NAME][IDX][FREQ] => %s:%d:%d\n",
		cpu_dvfs_get_name(p), idx, cpu_dvfs_get_freq_by_idx(p, idx));

	if (cur_khz == target_khz && p->armpll_is_available)
		return;

#ifdef DCM_ENABLE
	/* DCM (freq: high -> low) */
	if (cur_khz > target_khz) {
		if (cpu_dvfs_is(p, MT_CPU_DVFS_CCI))
			p->ops->set_sync_dcm(0);
		else
			p->ops->set_sync_dcm(target_khz/1000);
	}
#endif
	if (!cpu_dvfs_is(p, MT_CPU_DVFS_B)) {
		/* post_div 1 -> 2 */
		if (opp_tbl_m[CUR_OPP_IDX].slot->pos_div < opp_tbl_m[TARGET_OPP_IDX].slot->pos_div)
			adjust_posdiv(p, opp_tbl_m[TARGET_OPP_IDX].slot->pos_div);
	}
	/* armpll_div 1 -> 2 */
	if (opp_tbl_m[CUR_OPP_IDX].slot->clk_div < opp_tbl_m[TARGET_OPP_IDX].slot->clk_div)
		adjust_clkdiv(p, opp_tbl_m[TARGET_OPP_IDX].slot->clk_div);

#if 0
	adjust_armpll_dds(p, opp_tbl_m[TARGET_OPP_IDX].slot->vco_dds, opp_tbl_m[TARGET_OPP_IDX].slot->pos_div);
#else
	if (p->hopping_id != -1) {
		/* actual FHCTL */
#if 0
		dds = _cpu_dds_calc(opp_tbl_m[TARGET_OPP_IDX].slot->vco_dds);
		dds &= ~(_BITMASK_(26:24));
#endif
		dds = _GET_BITS_VAL_(20:0, _cpu_dds_calc(opp_tbl_m[TARGET_OPP_IDX].slot->vco_dds));
		mt_dfs_armpll(p->hopping_id, dds);
	} else /* No hopping for B */
		adjust_armpll_dds(p, opp_tbl_m[TARGET_OPP_IDX].slot->vco_dds, opp_tbl_m[TARGET_OPP_IDX].slot->pos_div);
#endif

	/* armpll_div 2 -> 1 */
	if (opp_tbl_m[CUR_OPP_IDX].slot->clk_div > opp_tbl_m[TARGET_OPP_IDX].slot->clk_div)
		adjust_clkdiv(p, opp_tbl_m[TARGET_OPP_IDX].slot->clk_div);

	if (!cpu_dvfs_is(p, MT_CPU_DVFS_B)) {
		/* post_div 2 -> 1 */
		if (opp_tbl_m[CUR_OPP_IDX].slot->pos_div > opp_tbl_m[TARGET_OPP_IDX].slot->pos_div)
			adjust_posdiv(p, opp_tbl_m[TARGET_OPP_IDX].slot->pos_div);
	}
#ifdef DCM_ENABLE
	/* DCM (freq: low -> high)*/
	if (cur_khz < target_khz)
		p->ops->set_sync_dcm(target_khz/1000);
#endif

	FUNC_EXIT(FUNC_LV_LOCAL);
}

#ifdef CONFIG_HYBRID_CPU_DVFS
static void set_cur_freq_hybrid(struct mt_cpu_dvfs *p, unsigned int cur_khz, unsigned int target_khz)
{
	int r, index;
	unsigned int cluster, volt, volt_val;

	BUG_ON(!enable_cpuhvfs);

	cluster = cpu_dvfs_to_cluster(p);
	index = search_table_idx_by_freq(p, target_khz);
	volt = cpu_dvfs_get_volt_by_idx(p, index);

	cpufreq_ver("cluster%u: target_khz = %u (%u), index = %d (%u)\n",
		    cluster, target_khz, cur_khz, index, volt);

	aee_record_cpu_volt(p, volt);

	r = cpuhvfs_set_target_opp(cluster, index, &volt_val);
	BUG_ON(r);

	notify_cpu_volt_sampler(p, EXTBUCK_VAL_TO_VOLT(volt_val));
}
#endif

/* for vsram change */
#ifdef ENABLE_IDVFS
static int idvfs_set_cur_volt_sram_b(struct mt_cpu_dvfs *p, unsigned int volt)
{
	return 0;
}
#endif

static int set_cur_volt_sram_b(struct mt_cpu_dvfs *p, unsigned int volt)
{
	int ret;

	ret = BigiDVFSSRAMLDOSet(volt);

	return ret;
}

static unsigned int get_cur_volt_sram_b(struct mt_cpu_dvfs *p)
{
	unsigned int volt = 0;

	volt = BigiDVFSSRAMLDOGet();

	return volt;
}

static int set_cur_volt_sram_l(struct mt_cpu_dvfs *p, unsigned int volt)
{
	unsigned int rdata = 0;
	unsigned int wdata = 0;
	unsigned int i;

	if (volt > MAX_VSRAM_VOLT || volt < MIN_VSRAM_VOLT)
		BUG_ON(1);

	/* Make sure 8 LDO is enable */
	cpufreq_write_mask(CPULDO_CTRL_0, 7 : 0, 0xFF);

	if ((volt / 2500) * 2500 != volt)
		volt = ((volt / 2500) + 1) * 2500;

	switch (volt) {
	case 60000:
		rdata = 1;
		break;
	case 70000:
		rdata = 2;
		break;
	default:
		if (volt < 90000)
			volt = 90000;
		rdata = VOLT_TO_PMIC_VAL(volt);
		break;
	};

	for (i = 0; i < 4; i++)
		wdata = wdata | (rdata << (i*8));

	cpufreq_write(CPULDO_CTRL_1, wdata);
	cpufreq_write(CPULDO_CTRL_2, wdata);

	return 0;
}

static unsigned int get_cur_volt_sram_l(struct mt_cpu_dvfs *p)
{
	unsigned int enable;
	unsigned int rdata, rdata2 = 0;
	unsigned int volt = 0;

	rdata = cpufreq_read(CPULDO_CTRL_0);
	enable = _GET_BITS_VAL_(7:0, rdata);
	BUG_ON(enable != 0xFF);

	rdata = cpufreq_read(CPULDO_CTRL_1);
	rdata = _GET_BITS_VAL_(3:0, rdata);

	rdata2 = cpufreq_read(CPULDO_CTRL_2);
	rdata2 = _GET_BITS_VAL_(3:0, rdata2);

	/* should be config to the same volt */
	BUG_ON(rdata != rdata2);

	switch (rdata) {
	case 0:
		volt = 105000;
		break;
	case 1:
		volt = 60000;
		break;
	case 2:
		volt = 70000;
		break;
	default:
		volt = PMIC_VAL_TO_VOLT(rdata);
		break;
	};

	if (volt > MAX_VSRAM_VOLT || volt < MIN_VSRAM_VOLT)
		BUG_ON(1);

	return volt;
}

/* for vproc change */
#define DA9214_VPROC1 0xD7
#define DA9214_VPROC2 0xD9

static unsigned int get_cur_volt_extbuck(struct mt_cpu_dvfs *p)
{
	unsigned char ret_val = 0;
	unsigned int ret_volt = 0;	/* volt: mv * 100 */
	unsigned int retry_cnt = 5;
	unsigned int addr;

	FUNC_ENTER(FUNC_LV_LOCAL);

	if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
		addr = DA9214_VPROC2;
	else
		addr = DA9214_VPROC1;

	if (cpu_dvfs_is_extbuck_valid()) {
		do {
			if (!da9214_read_interface(addr, &ret_val, 0x7F, 0)) {
				cpufreq_err("%s(), fail to read ext buck volt\n", __func__);
				ret_volt = 0;
			} else {
				ret_volt = EXTBUCK_VAL_TO_VOLT(ret_val);
				cpufreq_ver("@%s: volt = %d\n", __func__, ret_volt);
			}
		} while (ret_volt == EXTBUCK_VAL_TO_VOLT(0) && retry_cnt--);
	} else
		cpufreq_err("%s(), can not use ext buck!\n", __func__);

	FUNC_EXIT(FUNC_LV_LOCAL);

	return ret_volt;
}

unsigned int mt_cpufreq_get_cur_volt(enum mt_cpu_dvfs_id id)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	FUNC_ENTER(FUNC_LV_API);

	BUG_ON(NULL == p);
	BUG_ON(NULL == p->ops);

	FUNC_EXIT(FUNC_LV_API);

	return p->ops->get_cur_volt(p);	/* mv * 100 */
}
EXPORT_SYMBOL(mt_cpufreq_get_cur_volt);

static unsigned int _calc_pmic_settle_time(unsigned int old_vproc, unsigned int old_vsram,
					   unsigned int new_vproc, unsigned int new_vsram)
{
	unsigned delay = 100;

	if (new_vproc == old_vproc && new_vsram == old_vsram)
		return 0;

	/* VSRAM is UP */
	if (new_vsram >= old_vsram)
		delay = MAX(PMIC_VOLT_UP_SETTLE_TIME(old_vsram, new_vsram),
			PMIC_SETTLE_TIME(old_vproc, new_vproc)
		);
	/* VSRAM is DOWN */
	else
		delay = MAX(PMIC_VOLT_DOWN_SETTLE_TIME(old_vsram, new_vsram),
			PMIC_SETTLE_TIME(old_vproc, new_vproc)
		);

	if (delay < MIN_PMIC_SETTLE_TIME)
		delay = MIN_PMIC_SETTLE_TIME;

	return delay;
}

static void dump_opp_table(struct mt_cpu_dvfs *p)
{
	int i;

	cpufreq_err("[%s/%d]\n" "cpufreq_oppidx = %d\n", p->name, p->cpu_id, p->idx_opp_tbl);

	for (i = 0; i < p->nr_opp_tbl; i++) {
		cpufreq_err("\tOP(%d, %d),\n",
			    cpu_dvfs_get_freq_by_idx(p, i), cpu_dvfs_get_volt_by_idx(p, i)
		    );
	}
}

#ifdef ENABLE_IDVFS
static int idvfs_set_cur_volt_extbuck(struct mt_cpu_dvfs *p, unsigned int volt)
{
	cpufreq_ver("Not allow to set VPROC when iDVFS enabled\n");

	return 0;
}
#endif

static int set_cur_volt_extbuck(struct mt_cpu_dvfs *p, unsigned int volt)
{				/* volt: vproc (mv*100) */
	unsigned int cur_vsram;
	unsigned int cur_vproc;
	unsigned int delay_us = 0;
	int ret = 0;

	FUNC_ENTER(FUNC_LV_LOCAL);

	aee_record_cpu_volt(p, volt);

	cur_vsram = p->ops->get_cur_vsram(p);
	cur_vproc = p->ops->get_cur_volt(p);

	if (cur_vproc == 0 || !cpu_dvfs_is_extbuck_valid()) {
		cpufreq_err("@%s():%d, can not use ext buck!\n", __func__, __LINE__);
		return -1;
	}

	if (unlikely
	    (!((cur_vsram >= cur_vproc) && (MAX_DIFF_VSRAM_VPROC >= (cur_vsram - cur_vproc))))) {
#ifdef __KERNEL__
		aee_kernel_warning(TAG, "@%s():%d, cur_vsram = %d, cur_vproc = %d\n",
				   __func__, __LINE__, cur_vsram, cur_vproc);
#endif
		cur_vproc = cpu_dvfs_get_cur_volt(p);
		cur_vsram = cur_vproc + NORMAL_DIFF_VRSAM_VPROC;
	}

	/* UP */
	if (volt > cur_vproc) {
		unsigned int target_vsram = volt + NORMAL_DIFF_VRSAM_VPROC;
		unsigned int next_vsram;

		do {
			unsigned int old_vproc = cur_vproc;
			unsigned int old_vsram = cur_vsram;

			next_vsram = MIN(((MAX_DIFF_VSRAM_VPROC - 2500) + cur_vproc), target_vsram);

			/* update vsram */
			cur_vsram = MAX(next_vsram, MIN_VSRAM_VOLT);

			if (cur_vsram > MAX_VSRAM_VOLT) {
				cur_vsram = MAX_VSRAM_VOLT;
				target_vsram = MAX_VSRAM_VOLT;	/* to end the loop */
			}

			if (unlikely
			    (!((cur_vsram >= cur_vproc)
			       && (MAX_DIFF_VSRAM_VPROC >= (cur_vsram - cur_vproc))))) {
				dump_opp_table(p);
				cpufreq_err("@%s():%d, old_vsram=%d, old_vproc=%d, cur_vsram = %d, cur_vproc = %d\n",
					__func__, __LINE__, old_vsram, old_vproc, cur_vsram, cur_vproc);
				BUG();
			}

			/* update vsram */
			p->ops->set_cur_vsram(p, cur_vsram);

			/* update vproc */
			if (next_vsram > MAX_VSRAM_VOLT)
				cur_vproc = volt;	/* Vsram was limited, set to target vproc directly */
			else
				cur_vproc = next_vsram - NORMAL_DIFF_VRSAM_VPROC;

			if (unlikely
			    (!((cur_vsram >= cur_vproc)
			       && (MAX_DIFF_VSRAM_VPROC >= (cur_vsram - cur_vproc))))) {
				dump_opp_table(p);
				cpufreq_err("@%s():%d, old_vsram=%d, old_vproc=%d, cur_vsram = %d, cur_vproc = %d\n",
					__func__, __LINE__, old_vsram, old_vproc, cur_vsram, cur_vproc);
				BUG();
			}

			if (cpu_dvfs_is_extbuck_valid()) {
				if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
					da9214_vosel_buck_b(cur_vproc);
				else
					da9214_vosel_buck_a(cur_vproc);
			} else {
				cpufreq_err("%s(), fail to set ext buck volt\n", __func__);
				ret = -1;
				break;
			}

			delay_us =
			    _calc_pmic_settle_time(old_vproc, old_vsram, cur_vproc, cur_vsram);
			udelay(delay_us);

			cpufreq_ver
			    ("@%s(): UP --> old_vsram=%d, cur_vsram=%d, old_vproc=%d, cur_vproc=%d, delay=%d\n",
			     __func__, old_vsram, cur_vsram, old_vproc, cur_vproc, delay_us);
		} while (target_vsram > cur_vsram);
	}
	/* DOWN */
	else if (volt < cur_vproc) {
		unsigned int next_vproc;
		unsigned int next_vsram = cur_vproc + NORMAL_DIFF_VRSAM_VPROC;

		do {
			unsigned int old_vproc = cur_vproc;
			unsigned int old_vsram = cur_vsram;

			next_vproc = MAX((next_vsram - (MAX_DIFF_VSRAM_VPROC - 2500)), volt);

			/* update vproc */
			cur_vproc = next_vproc;

			if (unlikely
			    (!((cur_vsram >= cur_vproc)
			       && (MAX_DIFF_VSRAM_VPROC >= (cur_vsram - cur_vproc))))) {
				dump_opp_table(p);
				cpufreq_err("@%s():%d, old_vsram=%d, old_vproc=%d, cur_vsram = %d, cur_vproc = %d\n",
					__func__, __LINE__, old_vsram, old_vproc, cur_vsram, cur_vproc);
				BUG();
			}

			if (cpu_dvfs_is_extbuck_valid()) {
				if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
					da9214_vosel_buck_b(cur_vproc);
				else
					da9214_vosel_buck_a(cur_vproc);
			} else {
				cpufreq_err("%s(), fail to set ext buck volt\n", __func__);
				ret = -1;
				break;
			}

			/* update vsram */
			next_vsram = cur_vproc + NORMAL_DIFF_VRSAM_VPROC;
			cur_vsram = MAX(next_vsram, MIN_VSRAM_VOLT);
			cur_vsram = MIN(cur_vsram, MAX_VSRAM_VOLT);

			if (unlikely
			    (!((cur_vsram >= cur_vproc)
			       && (MAX_DIFF_VSRAM_VPROC >= (cur_vsram - cur_vproc))))) {
				dump_opp_table(p);
				cpufreq_err("@%s():%d, old_vsram=%d, old_vproc=%d, cur_vsram = %d, cur_vproc = %d\n",
					__func__, __LINE__, old_vsram, old_vproc, cur_vsram, cur_vproc);
				BUG();
			}

			p->ops->set_cur_vsram(p, cur_vsram);

			delay_us =
			    _calc_pmic_settle_time(old_vproc, old_vsram, cur_vproc, cur_vsram);
			udelay(delay_us);

			cpufreq_ver
			    ("@%s(): DOWN --> old_vsram=%d, cur_vsram=%d, old_vproc=%d, cur_vproc=%d, delay=%d\n",
			     __func__, old_vsram, cur_vsram, old_vproc, cur_vproc, delay_us);
		} while (cur_vproc > volt);
	}

	notify_cpu_volt_sampler(p, volt);

	cpufreq_ver("DVFS: End @%s(): %s, cur_vsram = %d, cur_vproc = %d\n", __func__, cpu_dvfs_get_name(p),
		cur_vsram, cur_vproc);

	cpufreq_ver("DVFS: End2 @%s(): %s, cur_vsram = %d, cur_vproc = %d\n", __func__, cpu_dvfs_get_name(p),
		p->ops->get_cur_vsram(p), p->ops->get_cur_volt(p));

	FUNC_EXIT(FUNC_LV_LOCAL);

	return ret;
}

#ifdef CONFIG_HYBRID_CPU_DVFS
static int set_cur_volt_hybrid(struct mt_cpu_dvfs *p, unsigned int volt)
{
	BUG_ON(!enable_cpuhvfs);

	return 0;
}

static unsigned int get_cur_volt_hybrid(struct mt_cpu_dvfs *p)
{
	unsigned int volt, volt_val;

	BUG_ON(!enable_cpuhvfs);

	volt_val = cpuhvfs_get_curr_volt(cpu_dvfs_to_cluster(p));
	if (volt_val != UINT_MAX)
		volt = EXTBUCK_VAL_TO_VOLT(volt_val);
	else
		volt = get_cur_volt_extbuck(p);

	return volt;
}
#endif

/* cpufreq set (freq & volt) */
static unsigned int _search_available_volt(struct mt_cpu_dvfs *p, unsigned int target_khz)
{
	int i;

	FUNC_ENTER(FUNC_LV_HELP);

	BUG_ON(NULL == p);

	/* search available voltage */
	for (i = p->nr_opp_tbl - 1; i >= 0; i--) {
		if (target_khz <= cpu_dvfs_get_freq_by_idx(p, i))
			break;
	}

	BUG_ON(i < 0);		/* i.e. target_khz > p->opp_tbl[0].cpufreq_khz */

	FUNC_EXIT(FUNC_LV_HELP);

	return cpu_dvfs_get_volt_by_idx(p, i);	/* mv * 100 */
}

static int _cpufreq_set_locked_cci(unsigned int cur_cci_khz, unsigned int target_cci_khz, unsigned int target_cci_volt)
{
	int ret = -1;
	enum mt_cpu_dvfs_id id;
	struct mt_cpu_dvfs *p_cci;
	unsigned int cur_cci_volt;

	FUNC_ENTER(FUNC_LV_HELP);

#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
	aee_rr_rec_cpu_dvfs_status(aee_rr_curr_cpu_dvfs_status() |
		(1 << CPU_DVFS_CCI_IS_DOING_DVFS));
#endif
	/* This is for cci */
	id = MT_CPU_DVFS_CCI;
	p_cci = id_to_cpu_dvfs(id);

	if (cur_cci_khz == target_cci_khz)
		goto out;
	else
		cpufreq_dbg
		    ("@%s(), %s: cur_cci_khz = %d, target_cci_khz = %d\n",
		     __func__, cpu_dvfs_get_name(p_cci), cur_cci_khz, target_cci_khz);

	cur_cci_volt = p_cci->ops->get_cur_volt(p_cci);
	/* target_cci_volt = _search_available_volt(p_cci, target_cci_khz); */

	cpufreq_dbg
		    ("@%s(), %s: cur_cci_volt = %d, target_cci_volt = %d\n",
		     __func__, cpu_dvfs_get_name(p_cci), cur_cci_volt, target_cci_volt);

	/* Set cci voltage (UP) */
	if (target_cci_volt > cur_cci_volt) {
		ret = p_cci->ops->set_cur_volt(p_cci, target_cci_volt);
		if (ret)	/* set volt fail */
			goto out;
	}

	/* set cci freq (UP/DOWN) */
	if (cur_cci_khz != target_cci_khz)
		p_cci->ops->set_cur_freq(p_cci, cur_cci_khz, target_cci_khz);

	/* Set cci voltage (DOWN) */
	if (target_cci_volt != 0 && (target_cci_volt < cur_cci_volt)) {
		ret = p_cci->ops->set_cur_volt(p_cci, target_cci_volt);
		if (ret)	/* set volt fail */
			goto out;
	}

	cpufreq_ver("@%s(): Vproc = %dmv, Vsram = %dmv, freq(%s) = %dKHz\n",
		    __func__,
		    (p_cci->ops->get_cur_volt(p_cci)) / 100,
		    (p_cci->ops->get_cur_vsram(p_cci) / 100), p_cci->name, p_cci->ops->get_cur_phy_freq(p_cci));
out:
#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
	aee_rr_rec_cpu_dvfs_status(aee_rr_curr_cpu_dvfs_status() &
		~(1 << CPU_DVFS_CCI_IS_DOING_DVFS));
#endif
	return ret;
}
static int _cpufreq_set_locked(struct mt_cpu_dvfs *p, unsigned int cur_khz, unsigned int target_khz,
			       struct cpufreq_policy *policy, unsigned int cur_cci_khz, unsigned int target_cci_khz,
				   unsigned int target_volt_vproc1)
{
	int ret = -1;
	unsigned int target_volt;
	unsigned int cur_volt;

#ifdef CONFIG_CPU_FREQ
	struct cpufreq_freqs freqs;
	unsigned int target_khz_orig = target_khz;
#endif

	FUNC_ENTER(FUNC_LV_HELP);

	cpufreq_ver("DVFS: _cpufreq_set_locked: target_vproc1 = %d\n", target_volt_vproc1);

	if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
		target_volt = _search_available_volt(p, target_khz);
	else {
		/* VPROC1 came from MCSI */
		target_volt = target_volt_vproc1;
		/* a little tricky here, no need to set volt at cci set_locked */
		target_volt_vproc1 = 0;
	}

	if (cur_khz != get_turbo_freq(p->cpu_id, target_khz)) {
		cpufreq_dbg
		    ("@%s(), %s: target_khz = %d (%d), target_volt = %d (%d), num_online_cpus = %d, cur_khz = %d\n",
		     __func__, cpu_dvfs_get_name(p), target_khz, get_turbo_freq(p->cpu_id, target_khz), target_volt,
		     get_turbo_volt(p->cpu_id, target_volt), num_online_cpus(), cur_khz);
	}

	target_volt = get_turbo_volt(p->cpu_id, target_volt);
	target_khz = get_turbo_freq(p->cpu_id, target_khz);

	if (cur_khz == target_khz)
		goto out;

#ifdef CONFIG_HYBRID_CPU_DVFS
	if (!enable_cpuhvfs) {
#endif
		/* set volt (UP) */
		cur_volt = p->ops->get_cur_volt(p);
		if (target_volt > cur_volt) {
			ret = p->ops->set_cur_volt(p, target_volt);

			if (ret)	/* set volt fail */
				goto out;
		}
#ifdef CONFIG_HYBRID_CPU_DVFS
	}
#endif

#ifdef CONFIG_CPU_FREQ
	freqs.old = cur_khz;
	freqs.new = target_khz_orig;
	freqs.cpu = policy->cpu;
	if (policy)
		cpufreq_freq_transition_begin(policy, &freqs);
#endif

	/* set freq (UP/DOWN) */
	if (cur_khz != target_khz)
		p->ops->set_cur_freq(p, cur_khz, target_khz);

#ifdef CONFIG_CPU_FREQ
	if (policy)
		cpufreq_freq_transition_end(policy, &freqs, 0);
#endif

#ifdef CONFIG_HYBRID_CPU_DVFS
	if (!enable_cpuhvfs) {
#endif
		/* set cci freq/volt */
		_cpufreq_set_locked_cci(cur_cci_khz, target_cci_khz, target_volt_vproc1);

		/* set volt (DOWN) */
		cur_volt = p->ops->get_cur_volt(p);
		if (cur_volt > target_volt) {
			ret = p->ops->set_cur_volt(p, target_volt);

			if (ret)	/* set volt fail */
				goto out;
		}
#ifdef CONFIG_HYBRID_CPU_DVFS
	}
#endif

#ifdef ENABLE_IDVFS
	if (cpu_dvfs_is(p, MT_CPU_DVFS_B) && !disable_idvfs_flag)
#else
	if (0)
#endif
		cpufreq_ver("DVFS: @%s(): freq(%s) = %d\n",
				__func__, p->name, BigiDVFSSWAvgStatus());
	else {
		cpufreq_ver("DVFS: @%s(): Vproc = %dmv, Vsram = %dmv, freq(%s) = %dKHz\n",
		    __func__,
		    (p->ops->get_cur_volt(p)) / 100,
		    (p->ops->get_cur_vsram(p) / 100), p->name, p->ops->get_cur_phy_freq(p));

		/* trigger exception if freq/volt not correct during stress */
		if (do_dvfs_stress_test) {
			unsigned int volt = p->ops->get_cur_volt(p);
			unsigned int freq = p->ops->get_cur_phy_freq(p);

			if (volt < target_volt || freq != target_khz) {
				cpufreq_err("volt = %u, target_volt = %u, freq = %u, target_khz = %u\n",
					    volt, target_volt, freq, target_khz);
				dump_opp_table(p);
				BUG();
			}
		}
	}

	FUNC_EXIT(FUNC_LV_HELP);

	return 0;

out:
	return ret;
}

static unsigned int _calc_new_opp_idx(struct mt_cpu_dvfs *p, int new_opp_idx);
static unsigned int _calc_new_cci_opp_idx(struct mt_cpu_dvfs *p, int new_opp_idx);

int release_dvfs = 0;
int thres_ll = 8;
int thres_l = 8;
int thres_b = 15;

static void _mt_cpufreq_set(struct cpufreq_policy *policy, enum mt_cpu_dvfs_id id, int new_opp_idx)
{
	unsigned long flags;
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	unsigned int cur_freq;
	unsigned int target_freq;
	int new_cci_opp_idx;
	unsigned int cur_cci_freq;
	unsigned int target_cci_freq;

	enum mt_cpu_dvfs_id id_cci;
	struct mt_cpu_dvfs *p_cci;
	int ret = -1;
	unsigned int target_volt_vpro1 = 0;

	FUNC_ENTER(FUNC_LV_LOCAL);

#ifndef __TRIAL_RUN__
	if (!policy)
#endif
		return;

	BUG_ON(NULL == p);
	BUG_ON(new_opp_idx >= p->nr_opp_tbl);

	/* This is for cci */
	id_cci = MT_CPU_DVFS_CCI;
	p_cci = id_to_cpu_dvfs(id_cci);

	cpufreq_lock(flags);

	if (p->armpll_is_available != 1) {
		cpufreq_unlock(flags);
		return;
	}

	/* get current idx here to avoid idx synchronization issue */
	if (new_opp_idx == -1)
		new_opp_idx = p->idx_opp_tbl;

	if (do_dvfs_stress_test) {
		/* new_opp_idx = jiffies & 0x7; */
		new_opp_idx = jiffies & 0xF;

		if (cpu_dvfs_is(p, MT_CPU_DVFS_B))
			if (new_opp_idx < thres_b)
				new_opp_idx = thres_b;

		if (cpu_dvfs_is(p, MT_CPU_DVFS_L))
			if (new_opp_idx < thres_l)
				new_opp_idx = thres_l;

		if (cpu_dvfs_is(p, MT_CPU_DVFS_LL))
			if (new_opp_idx < thres_ll)
				new_opp_idx = thres_ll;
	} else
		new_opp_idx = _calc_new_opp_idx(id_to_cpu_dvfs(id), new_opp_idx);

	/* Get cci opp idx */
	new_cci_opp_idx = _calc_new_cci_opp_idx(id_to_cpu_dvfs(id), new_opp_idx);

	cur_cci_freq = p_cci->ops->get_cur_phy_freq(p_cci);
	target_cci_freq = cpu_dvfs_get_freq_by_idx(p_cci, new_cci_opp_idx);
	target_volt_vpro1 = cpu_dvfs_get_volt_by_idx(p_cci, new_cci_opp_idx);

	cur_freq = p->ops->get_cur_phy_freq(p);
	target_freq = cpu_dvfs_get_freq_by_idx(p, new_opp_idx);

#ifdef CONFIG_CPU_FREQ
	ret = _cpufreq_set_locked(p, cur_freq, target_freq, policy, cur_cci_freq, target_cci_freq, target_volt_vpro1);
#else
	ret = _cpufreq_set_locked(p, cur_freq, target_freq, NULL, cur_cci_freq, target_cci_freq, target_volt_vpro1);
#endif

	p->idx_opp_tbl = new_opp_idx;
	p_cci->idx_opp_tbl = new_cci_opp_idx;

#ifndef DISABLE_PBM_FEATURE
	if (!ret && !p->dvfs_disable_by_suspend)
		_kick_PBM_by_cpu(p);
#endif

	cpufreq_unlock(flags);

	FUNC_EXIT(FUNC_LV_LOCAL);
}

static int _search_available_freq_idx_under_v(struct mt_cpu_dvfs *p, unsigned int volt)
{
	int i;

	FUNC_ENTER(FUNC_LV_HELP);

	BUG_ON(NULL == p);

	/* search available voltage */
	for (i = p->nr_opp_tbl - 1; i >= 0; i--) {
		if (volt <= cpu_dvfs_get_volt_by_idx(p, i))
			break;
	}

	if (i < 0)
		return 0; /* i.e. target_khz > p->opp_tbl[0].cpufreq_khz */

	FUNC_EXIT(FUNC_LV_HELP);

	return i;
}

static int _cpufreq_dfs_locked(struct cpufreq_policy *policy, struct mt_cpu_dvfs *p,
			       unsigned int freq_idx, unsigned long action)
{
	int ret = 0;
#ifdef CONFIG_CPU_FREQ
	struct cpufreq_freqs freqs;
#endif

#ifdef CONFIG_CPU_FREQ
	freqs.old = cpu_dvfs_get_cur_freq(p);
	freqs.new = cpu_dvfs_get_freq_by_idx(p, freq_idx);
	freqs.cpu = policy->cpu;
	if (policy)
		cpufreq_freq_transition_begin(policy, &freqs);
#endif

#ifdef CONFIG_HYBRID_CPU_DVFS
	if (enable_cpuhvfs) {
		action &= ~CPU_TASKS_FROZEN;
		if (action == CPU_DOWN_PREPARE) {
			cpuhvfs_notify_cluster_off(cpu_dvfs_to_cluster(p));
			goto DFS_DONE;
		} else if (action == CPU_DOWN_FAILED) {
			cpuhvfs_notify_cluster_on(cpu_dvfs_to_cluster(p));
		}
	}
#endif

	if (cpu_dvfs_get_cur_freq(p) != cpu_dvfs_get_freq_by_idx(p, freq_idx))
		p->ops->set_cur_freq(p, cpu_dvfs_get_cur_freq(p)
			, cpu_dvfs_get_freq_by_idx(p, freq_idx));

#ifdef CONFIG_HYBRID_CPU_DVFS
DFS_DONE:
#endif

#ifdef CONFIG_CPU_FREQ
	if (policy)
		cpufreq_freq_transition_end(policy, &freqs, 0);
	cpufreq_ver("CB - notify %d to governor\n", freqs.new);
#endif

	p->idx_opp_tbl = freq_idx;

#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
	if (cpu_dvfs_is(p, MT_CPU_DVFS_LL))
		aee_rr_rec_cpu_dvfs_oppidx((aee_rr_curr_cpu_dvfs_oppidx() & 0xF0) | p->idx_opp_tbl);
	else if (cpu_dvfs_is(p, MT_CPU_DVFS_L))
		aee_rr_rec_cpu_dvfs_oppidx((aee_rr_curr_cpu_dvfs_oppidx() & 0x0F) | (p->idx_opp_tbl << 4));
	else
		aee_rr_rec_cpu_dvfs_status((aee_rr_curr_cpu_dvfs_status() & 0x0F) | (p->idx_opp_tbl << 4));
#endif

	return ret;
}
/* 750Mhz */
#define DEFAULT_B_FREQ_IDX 13
static unsigned int num_online_cpus_delta;
static int __cpuinit _mt_cpufreq_cpu_CB(struct notifier_block *nfb, unsigned long action,
					void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	unsigned int online_cpus = num_online_cpus();
	struct device *dev;

	enum mt_cpu_dvfs_id cluster_id;
	struct mt_cpu_dvfs *p;
	enum mt_cpu_dvfs_id id_cci;
	struct mt_cpu_dvfs *p_cci;
	int new_opp_idx;
	int new_cci_opp_idx;
	unsigned int cur_cci_freq;
	unsigned int cur_freq;
	unsigned int target_cci_freq;
	unsigned int target_freq;
	unsigned int target_volt_vpro1 = 0;

	struct cpufreq_policy *policy;
	unsigned int cur_volt;
	int freq_idx;

	/* CPU mask - Get on-line cpus per-cluster */
	struct cpumask dvfs_cpumask;
	struct cpumask cpu_online_cpumask;
	unsigned int cpus;
	unsigned long flags;

	/* This is for cci */
	id_cci = MT_CPU_DVFS_CCI;
	p_cci = id_to_cpu_dvfs(id_cci);

	cluster_id = arch_get_cluster_id(cpu);
	arch_get_cluster_cpus(&dvfs_cpumask, cluster_id);
	cpumask_and(&cpu_online_cpumask, &dvfs_cpumask, cpu_online_mask);
	p = id_to_cpu_dvfs(cluster_id);

	cpufreq_ver("@%s():%d, cpu = %d, action = %lu, oppidx = %d, num_online_cpus = %d\n"
	, __func__, __LINE__, cpu, action, p->idx_opp_tbl, online_cpus);

	dev = get_cpu_device(cpu);

	if (dev) {
		switch (action & ~CPU_TASKS_FROZEN) {
		case CPU_ONLINE:
			cpus = cpumask_weight(&cpu_online_cpumask);
			cpufreq_ver("CPU_ONLINE -> cpus = %d\n", cpus);
			if (cpus == 1) {
				cpufreq_dbg("CPU_ONLINE first CPU of %s\n",
					cpu_dvfs_get_name(p));
				cpufreq_lock(flags);
				p->armpll_is_available = 1;
				if (cpu_dvfs_is(p, MT_CPU_DVFS_B) && (action == CPU_ONLINE)) {
#ifdef CONFIG_HYBRID_CPU_DVFS
					if (!enable_cpuhvfs) {
#endif
						cpufreq_ver("CB - re-adjust cci due to big on\n");
						new_cci_opp_idx = _calc_new_cci_opp_idx(p, p->idx_opp_tbl);
						/* set cci freq/volt */
						_cpufreq_set_locked_cci(cpu_dvfs_get_cur_freq(p_cci),
							cpu_dvfs_get_freq_by_idx(p_cci, new_cci_opp_idx),
							cpu_dvfs_get_volt_by_idx(p_cci, new_cci_opp_idx));
						p_cci->idx_opp_tbl = new_cci_opp_idx;
#ifdef CONFIG_HYBRID_CPU_DVFS
					}
#endif
				} else {
#if 1
					if (action == CPU_ONLINE) {
						cur_volt = p->ops->get_cur_volt(p);
						cpufreq_ver("CB - adjust the freq to V:%d  due to L/LL on\n", cur_volt);
						freq_idx = _search_available_freq_idx_under_v(p, cur_volt);
						freq_idx = MAX(freq_idx, _calc_new_opp_idx(p, freq_idx));
						policy = cpufreq_cpu_get(cpu);
						_cpufreq_dfs_locked(policy, p, freq_idx, action);
						cpufreq_cpu_put(policy);
					}
#endif
				}
				cpufreq_unlock(flags);
			}
			break;
		case CPU_DOWN_PREPARE:
			cpus = cpumask_weight(&cpu_online_cpumask);
			cpufreq_ver("CPU_DOWN_PREPARE -> cpus = %d\n", cpus);
			if (cpus == 1) {
				cpufreq_dbg("CPU_DOWN_PREPARE last CPU of %s\n",
					cpu_dvfs_get_name(p));
				cpufreq_lock(flags);
				p->armpll_is_available = 0;
				if (!cpu_dvfs_is(p, MT_CPU_DVFS_B)) {
#ifdef CONFIG_CPU_FREQ
					policy = cpufreq_cpu_get(cpu);
					_cpufreq_dfs_locked(policy, p, p->nr_opp_tbl - 1, action);
					cpufreq_cpu_put(policy);
#endif
#ifdef CONFIG_HYBRID_CPU_DVFS
					if (!enable_cpuhvfs) {
#endif
						new_cci_opp_idx = _calc_new_cci_opp_idx(p, p->idx_opp_tbl);
						/* set cci freq/volt */
						_cpufreq_set_locked_cci(cpu_dvfs_get_cur_freq(p_cci),
							cpu_dvfs_get_freq_by_idx(p_cci, new_cci_opp_idx),
							cpu_dvfs_get_volt_by_idx(p_cci, new_cci_opp_idx));
						p_cci->idx_opp_tbl = new_cci_opp_idx;
#ifdef CONFIG_HYBRID_CPU_DVFS
					}
#endif
				} else {
					if (disable_idvfs_flag) {
						new_opp_idx = DEFAULT_B_FREQ_IDX;
						/* Get cci opp idx */
						new_cci_opp_idx = _calc_new_cci_opp_idx(p, new_opp_idx);

						cur_cci_freq = p_cci->ops->get_cur_phy_freq(p_cci);
						target_cci_freq = cpu_dvfs_get_freq_by_idx(p_cci, new_cci_opp_idx);
						target_volt_vpro1 = cpu_dvfs_get_volt_by_idx(p_cci, new_cci_opp_idx);

						cur_freq = p->ops->get_cur_phy_freq(p);
						target_freq = cpu_dvfs_get_freq_by_idx(p, new_opp_idx);

#ifdef CONFIG_CPU_FREQ
						policy = cpufreq_cpu_get(cpu);
						_cpufreq_set_locked(p, cur_freq, target_freq, policy,
							cur_cci_freq, target_cci_freq, target_volt_vpro1);
						cpufreq_cpu_put(policy);
						p->idx_opp_tbl = new_opp_idx;
						p_cci->idx_opp_tbl = new_cci_opp_idx;
#endif
					}

#ifdef CONFIG_HYBRID_CPU_DVFS	/* before BigiDVFSDisable */
					if (enable_cpuhvfs)
						cpuhvfs_notify_cluster_off(cpu_dvfs_to_cluster(p));
#endif
#if 0
					if (!disable_idvfs_flag)
						BigiDVFSDisable_hp();
#endif
				}
				cpufreq_unlock(flags);
			}
			break;
		case CPU_DOWN_FAILED:
			cpufreq_lock(flags);
			p->armpll_is_available = 1;
			if (cpu_dvfs_is(p, MT_CPU_DVFS_B)) {
#ifdef CONFIG_HYBRID_CPU_DVFS	/* after BigiDVFSEnable */
				if (enable_cpuhvfs) {
					cpuhvfs_notify_cluster_on(cpu_dvfs_to_cluster(p));
				} else {
#endif
					new_cci_opp_idx = _calc_new_cci_opp_idx(p, p->idx_opp_tbl);
					/* set cci freq/volt */
					_cpufreq_set_locked_cci(cpu_dvfs_get_cur_freq(p_cci),
						cpu_dvfs_get_freq_by_idx(p_cci, new_cci_opp_idx),
						cpu_dvfs_get_volt_by_idx(p_cci, new_cci_opp_idx));
					p_cci->idx_opp_tbl = new_cci_opp_idx;
#ifdef CONFIG_HYBRID_CPU_DVFS
				}
#endif
			} else {
#if 1
				if (action == CPU_DOWN_FAILED) {
					cur_volt = p->ops->get_cur_volt(p);
					freq_idx = _search_available_freq_idx_under_v(p, cur_volt);
					freq_idx = MAX(freq_idx, _calc_new_opp_idx(p, freq_idx));
					policy = cpufreq_cpu_get(cpu);
					_cpufreq_dfs_locked(policy, p, freq_idx, action);
					cpufreq_cpu_put(policy);
				}
#endif
			}
			cpufreq_unlock(flags);
			break;
		}
#ifndef DISABLE_PBM_FEATURE
		/* Notify PBM after CPU on/off */
		if (action == CPU_ONLINE || action == CPU_ONLINE_FROZEN
			|| action == CPU_DEAD || action == CPU_DEAD_FROZEN) {

			cpufreq_lock(flags);

			if (!p->dvfs_disable_by_suspend)
				_kick_PBM_by_cpu(p);

			cpufreq_unlock(flags);
		}
#endif
	}

	cpufreq_ver("@%s():%d, num_online_cpus_delta = %d\n", __func__, __LINE__,
		    num_online_cpus_delta);

	cpufreq_ver("@%s():%d, cpu = %d, action = %lu, oppidx = %d, num_online_cpus = %d\n"
	, __func__, __LINE__, cpu, action, p->idx_opp_tbl, online_cpus);

	return NOTIFY_OK;
}

static int _sync_opp_tbl_idx(struct mt_cpu_dvfs *p)
{
	int ret = -1;
	unsigned int freq;
	int i;

	FUNC_ENTER(FUNC_LV_HELP);

	BUG_ON(NULL == p);
	BUG_ON(NULL == p->opp_tbl);
	BUG_ON(NULL == p->ops);

	freq = p->ops->get_cur_phy_freq(p);

	/* dbg_print("DVFS: _sync_opp_tbl_idx(from reg): %s freq = %d\n", cpu_dvfs_get_name(p), freq); */

	for (i = p->nr_opp_tbl - 1; i >= 0; i--) {
		if (freq <= cpu_dvfs_get_freq_by_idx(p, i)) {
			p->idx_opp_tbl = i;
			break;
		}
	}

	if (i >= 0) {
		/* cpufreq_dbg("%s freq = %d\n", cpu_dvfs_get_name(p), cpu_dvfs_get_cur_freq(p)); */
		/* dbg_print("DVFS: _sync_opp_tbl_idx: %s freq = %d\n",
			cpu_dvfs_get_name(p), cpu_dvfs_get_cur_freq(p)); */
		ret = 0;
	} else
		cpufreq_warn("%s can't find freq = %d\n", cpu_dvfs_get_name(p), freq);

	FUNC_EXIT(FUNC_LV_HELP);

	return ret;
}

static int _mt_cpufreq_sync_opp_tbl_idx(struct mt_cpu_dvfs *p)
{
	int ret = -1;

	FUNC_ENTER(FUNC_LV_LOCAL);


	if (cpu_dvfs_is_available(p))
		ret = _sync_opp_tbl_idx(p);


	cpufreq_info("%s freq = %d\n", cpu_dvfs_get_name(p), cpu_dvfs_get_cur_freq(p));

	FUNC_EXIT(FUNC_LV_LOCAL);

	return ret;
}

static enum mt_cpu_dvfs_id _get_cpu_dvfs_id(unsigned int cpu_id)
{
	int cluster_id;

	cluster_id = arch_get_cluster_id(cpu_id);

	return cluster_id;
}

bool mt_cpufreq_earlysuspend_status_get(void)
{
	return 1;
}
EXPORT_SYMBOL(mt_cpufreq_earlysuspend_status_get);

static int _mt_cpufreq_setup_freqs_table(struct cpufreq_policy *policy,
					 struct mt_cpu_freq_info *freqs, int num)
{
	struct mt_cpu_dvfs *p;
	int ret = 0;

	FUNC_ENTER(FUNC_LV_LOCAL);

	BUG_ON(NULL == policy);
	BUG_ON(NULL == freqs);

	p = id_to_cpu_dvfs(_get_cpu_dvfs_id(policy->cpu));

#ifdef CONFIG_CPU_FREQ
	ret = cpufreq_frequency_table_cpuinfo(policy, p->freq_tbl_for_cpufreq);

	if (!ret)
		policy->freq_table = p->freq_tbl_for_cpufreq;

	cpumask_copy(policy->cpus, topology_core_cpumask(policy->cpu));
	cpumask_copy(policy->related_cpus, policy->cpus);
#endif

	FUNC_EXIT(FUNC_LV_LOCAL);

	return 0;
}

static unsigned int _calc_new_opp_idx(struct mt_cpu_dvfs *p, int new_opp_idx)
{
	FUNC_ENTER(FUNC_LV_HELP);

	BUG_ON(NULL == p);

	cpufreq_ver("new_opp_idx = %d, idx_opp_ppm_base = %d, idx_opp_ppm_limit = %d\n",
		new_opp_idx , p->idx_opp_ppm_base, p->idx_opp_ppm_limit);

	if ((p->idx_opp_ppm_limit != -1) && (new_opp_idx < p->idx_opp_ppm_limit))
		new_opp_idx = p->idx_opp_ppm_limit;

	if ((p->idx_opp_ppm_base != -1) && (new_opp_idx > p->idx_opp_ppm_base))
		new_opp_idx = p->idx_opp_ppm_base;

	if ((p->idx_opp_ppm_base == p->idx_opp_ppm_limit) && p->idx_opp_ppm_base != -1)
		new_opp_idx = p->idx_opp_ppm_base;

	if (cpu_dvfs_is(p, MT_CPU_DVFS_B) && new_opp_idx < 8 && !release_dvfs)
		new_opp_idx = 8;

#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
	if (cpu_dvfs_is(p, MT_CPU_DVFS_LL))
		aee_rr_rec_cpu_dvfs_oppidx((aee_rr_curr_cpu_dvfs_oppidx() & 0xF0) | new_opp_idx);
	else if (cpu_dvfs_is(p, MT_CPU_DVFS_L))
		aee_rr_rec_cpu_dvfs_oppidx((aee_rr_curr_cpu_dvfs_oppidx() & 0x0F) | (new_opp_idx << 4));
	else
		aee_rr_rec_cpu_dvfs_status((aee_rr_curr_cpu_dvfs_status() & 0x0F) | (new_opp_idx << 4));
#endif
	FUNC_EXIT(FUNC_LV_HELP);

	return new_opp_idx;
}

static unsigned int _calc_new_cci_opp_idx(struct mt_cpu_dvfs *p, int new_opp_idx)
{
	/* LL, L, B */
	int freq_idx[NR_MT_CPU_DVFS - 1] = {-1};
	unsigned int volt[NR_MT_CPU_DVFS - 1] = {0};
	int i;

	struct mt_cpu_dvfs *pp, *p_cci;
	enum mt_cpu_dvfs_id id_cci;

	unsigned int target_cci_freq = 0;
	unsigned int target_cci_volt;
	int new_cci_opp_idx;

	/* This is for cci */
	id_cci = MT_CPU_DVFS_CCI;
	p_cci = id_to_cpu_dvfs(id_cci);

	/* Fill the V and F to determine cci state*/
	/* MCSI Algorithm for cci target F */
	for_each_cpu_dvfs_only(i, pp) {
		if (pp->armpll_is_available) {
			/* F */
			if (pp == p)
				freq_idx[i] = new_opp_idx;
			else
				freq_idx[i] = pp->idx_opp_tbl;

			target_cci_freq = MAX(target_cci_freq, cpu_dvfs_get_freq_by_idx(pp, freq_idx[i]));
			/* V */
			volt[i] = cpu_dvfs_get_volt_by_idx(pp, freq_idx[i]);
			cpufreq_ver("DVFS: MCSI: %s, freq = %d, volt = %d\n", cpu_dvfs_get_name(pp),
				cpu_dvfs_get_freq_by_idx(pp, freq_idx[i]), volt[i]);
		}
	}

	target_cci_freq = ((target_cci_freq / 2) / PLL_FREQ_STEP) * PLL_FREQ_STEP;

	if (target_cci_freq > cpu_dvfs_get_freq_by_idx(p_cci, 0)) {
		target_cci_freq = cpu_dvfs_get_freq_by_idx(p_cci, 0);
		target_cci_volt = cpu_dvfs_get_volt_by_idx(p_cci, 0);
	} else {
	target_cci_volt = _search_available_volt(p_cci, target_cci_freq);
	}

	/* Determine dominating voltage */
	target_cci_volt = MAX(target_cci_volt, MAX(volt[MT_CPU_DVFS_LL], volt[MT_CPU_DVFS_L]));
	cpufreq_ver("DVFS: MCSI: target_cci (F,V) = (%d, %d)\n", target_cci_freq, target_cci_volt);

	new_cci_opp_idx = _search_available_freq_idx_under_v(p_cci, target_cci_volt);

	cpufreq_ver("DVFS: MCSI: Final Result, target_cci_volt = %d, target_cci_freq = %d\n",
		target_cci_volt, cpu_dvfs_get_freq_by_idx(p_cci, new_cci_opp_idx));

	return new_cci_opp_idx;
}

#if 1
static void ppm_limit_callback(struct ppm_client_req req)
{
	struct ppm_client_req *ppm = (struct ppm_client_req *)&req;
	unsigned long flags;

	struct mt_cpu_dvfs *p;
	int ignore_ppm[NR_MT_CPU_DVFS] = {0};
	unsigned int i, j;
	struct cpufreq_policy *policy[NR_MT_CPU_DVFS];
	struct cpumask dvfs_cpumask[NR_MT_CPU_DVFS];
	struct cpumask cpu_online_cpumask;

	char str1[32];
	char str2[32];
	unsigned int ret;

	cpufreq_ver("get feedback from PPM module\n");

	for_each_cpu_dvfs_only(i, p)
		arch_get_cluster_cpus(&dvfs_cpumask[i], i);

	cpufreq_lock(flags);
	for (i = 0; i < ppm->cluster_num; i++) {
		cpufreq_ver("[%d]:cluster_id = %d, cpu_id = %d, min_cpufreq_idx = %d, max_cpufreq_idx = %d\n",
			i, ppm->cpu_limit[i].cluster_id, ppm->cpu_limit[i].cpu_id,
			ppm->cpu_limit[i].min_cpufreq_idx, ppm->cpu_limit[i].max_cpufreq_idx);
		cpufreq_ver("has_advise_freq = %d, advise_cpufreq_idx = %d\n",
			ppm->cpu_limit[i].has_advise_freq, ppm->cpu_limit[i].advise_cpufreq_idx);

		p = id_to_cpu_dvfs(i);

		if (ppm->cpu_limit[i].has_advise_freq) {
			p->idx_opp_ppm_base = ppm->cpu_limit[i].advise_cpufreq_idx;
			p->idx_opp_ppm_limit = ppm->cpu_limit[i].advise_cpufreq_idx;
			if (p->idx_opp_tbl == ppm->cpu_limit[i].advise_cpufreq_idx) {
					cpufreq_ver("idx = %d, advise_cpufreq_idx = %d\n", p->idx_opp_tbl ,
						ppm->cpu_limit[i].advise_cpufreq_idx);
					ignore_ppm[i] = 1;
			}
		} else {
			p->idx_opp_ppm_base = ppm->cpu_limit[i].min_cpufreq_idx;	/* ppm update base */
			p->idx_opp_ppm_limit = ppm->cpu_limit[i].max_cpufreq_idx;	/* ppm update limit */
			if ((p->idx_opp_tbl >= ppm->cpu_limit[i].max_cpufreq_idx)
				&& (p->idx_opp_tbl <= ppm->cpu_limit[i].min_cpufreq_idx)) {
				cpufreq_ver("idx = %d, idx_opp_ppm_base = %d, idx_opp_ppm_limit = %d\n",
					p->idx_opp_tbl, p->idx_opp_ppm_base, p->idx_opp_ppm_limit);
				ignore_ppm[i] = 1;
			}
		}
	}
	cpufreq_unlock(flags);

	for_each_cpu_dvfs_only(i, p) {
		get_online_cpus();
		cpumask_and(&cpu_online_cpumask, &dvfs_cpumask[i], cpu_online_mask);
		cpulist_scnprintf(str1, sizeof(str1), (const struct cpumask *)cpu_online_mask);
		cpulist_scnprintf(str2, sizeof(str2), (const struct cpumask *)&cpu_online_cpumask);
		cpufreq_ver("cpu_online_mask = %s, cpu_online_cpumask for little = %s\n", str1, str2);
		ret = -1;
		for_each_cpu(j, &cpu_online_cpumask) {
			policy[i] = cpufreq_cpu_get(j);
			if (policy[i]) {
				if (p->idx_opp_ppm_limit == -1)
					policy[i]->max = cpu_dvfs_get_max_freq(p);
				else
					policy[i]->max = cpu_dvfs_get_freq_by_idx(p, p->idx_opp_ppm_limit);
				if (p->idx_opp_ppm_base == -1)
					policy[i]->min = cpu_dvfs_get_min_freq(p);
				else
					policy[i]->min = cpu_dvfs_get_freq_by_idx(p, p->idx_opp_ppm_base);
				cpufreq_cpu_put(policy[i]);
				ret = 0;
				break;
			}
		}
		put_online_cpus();

		if (!ignore_ppm[i]) {
			if (!ret)
				_mt_cpufreq_set(policy[i], i, -1);
		}
	}
}
#endif

/*
 * cpufreq driver
 */
static int _mt_cpufreq_verify(struct cpufreq_policy *policy)
{
	struct mt_cpu_dvfs *p;
	int ret = 0;		/* cpufreq_frequency_table_verify() always return 0 */

	FUNC_ENTER(FUNC_LV_MODULE);

	p = id_to_cpu_dvfs(_get_cpu_dvfs_id(policy->cpu));

	BUG_ON(NULL == p);

#ifdef CONFIG_CPU_FREQ
	ret = cpufreq_frequency_table_verify(policy, p->freq_tbl_for_cpufreq);
#endif

	FUNC_EXIT(FUNC_LV_MODULE);

	return ret;
}

static int _mt_cpufreq_target(struct cpufreq_policy *policy, unsigned int target_freq,
			      unsigned int relation)
{
	enum mt_cpu_dvfs_id id = _get_cpu_dvfs_id(policy->cpu);
	int ret = 0;
	unsigned int new_opp_idx;

	FUNC_ENTER(FUNC_LV_MODULE);

	/*if (do_dvfs_stress_test != 1)
		return 0;*/

#if 1
	if (dvfs_disable_flag == 1)
		return 0;
#endif

	if (policy->cpu >= num_possible_cpus()
	    || cpufreq_frequency_table_target(policy, id_to_cpu_dvfs(id)->freq_tbl_for_cpufreq,
					      target_freq, relation, &new_opp_idx)
	    || (id_to_cpu_dvfs(id) && id_to_cpu_dvfs(id)->dvfs_disable_by_procfs)
		|| (id_to_cpu_dvfs(id) && id_to_cpu_dvfs(id)->dvfs_disable_by_suspend)
	    )
		return -EINVAL;

#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
	if (id == MT_CPU_DVFS_LL)
		aee_rr_rec_cpu_dvfs_status(aee_rr_curr_cpu_dvfs_status() |
			(1 << CPU_DVFS_LL_IS_DOING_DVFS));
	else if (id == MT_CPU_DVFS_L)
		aee_rr_rec_cpu_dvfs_status(aee_rr_curr_cpu_dvfs_status() |
			(1 << CPU_DVFS_L_IS_DOING_DVFS));
	else
		aee_rr_rec_cpu_dvfs_status(aee_rr_curr_cpu_dvfs_status() |
			(1 << CPU_DVFS_B_IS_DOING_DVFS));
#endif

	_mt_cpufreq_set(policy, id, new_opp_idx);

#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
	if (id == MT_CPU_DVFS_LL)
		aee_rr_rec_cpu_dvfs_status(aee_rr_curr_cpu_dvfs_status() &
			~(1 << CPU_DVFS_LL_IS_DOING_DVFS));
	else if (id == MT_CPU_DVFS_L)
		aee_rr_rec_cpu_dvfs_status(aee_rr_curr_cpu_dvfs_status() &
			~(1 << CPU_DVFS_L_IS_DOING_DVFS));
	else
		aee_rr_rec_cpu_dvfs_status(aee_rr_curr_cpu_dvfs_status() &
			~(1 << CPU_DVFS_B_IS_DOING_DVFS));
#endif

	FUNC_EXIT(FUNC_LV_MODULE);

	return ret;
}

int init_cci_status = 0;
#define IDVFS_FMAX 2500
static int _mt_cpufreq_init(struct cpufreq_policy *policy)
{
	int ret = -EINVAL;
	unsigned long flags;

	FUNC_ENTER(FUNC_LV_MODULE);
#if 0
	max_cpu_num = num_possible_cpus();

	if (policy->cpu >= max_cpu_num)
		return -EINVAL;

	cpufreq_ver("@%s: max_cpu_num: %d\n", __func__, max_cpu_num);
#endif

	policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
	cpumask_setall(policy->cpus);

	policy->cpuinfo.transition_latency = 1000;

	{
		enum mt_cpu_dvfs_id id = _get_cpu_dvfs_id(policy->cpu);
		struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);
		unsigned int lv = _mt_cpufreq_get_cpu_level();
		struct opp_tbl_info *opp_tbl_info;
		struct opp_tbl_m_info *opp_tbl_m_info;
		struct opp_tbl_m_info *opp_tbl_m_cci_info;

		enum mt_cpu_dvfs_id id_cci;
		struct mt_cpu_dvfs *p_cci;

		/* This is for cci */
		id_cci = MT_CPU_DVFS_CCI;
		p_cci = id_to_cpu_dvfs(id_cci);

		cpufreq_ver("DVFS: _mt_cpufreq_init: %s(cpu_id = %d)\n", cpu_dvfs_get_name(p), p->cpu_id);

		opp_tbl_info = &opp_tbls[id][CPU_LV_TO_OPP_IDX(lv)];

		BUG_ON(NULL == p);
		BUG_ON(!
		       (lv == CPU_LEVEL_0 || lv == CPU_LEVEL_1 || lv == CPU_LEVEL_2
			|| lv == CPU_LEVEL_3));

		p->cpu_level = lv;

		ret = _mt_cpufreq_setup_freqs_table(policy,
						    opp_tbl_info->opp_tbl, opp_tbl_info->size);

		policy->cpuinfo.max_freq = cpu_dvfs_get_max_freq(id_to_cpu_dvfs(id));
		policy->cpuinfo.min_freq = cpu_dvfs_get_min_freq(id_to_cpu_dvfs(id));

		policy->cur = mt_cpufreq_get_cur_phy_freq(id);	/* use cur phy freq is better */
		policy->max = cpu_dvfs_get_max_freq(id_to_cpu_dvfs(id));
		policy->min = cpu_dvfs_get_min_freq(id_to_cpu_dvfs(id));

		if (_mt_cpufreq_sync_opp_tbl_idx(p) >= 0)
			if (p->idx_normal_max_opp == -1)
				p->idx_normal_max_opp = p->idx_opp_tbl;

		opp_tbl_m_info = &opp_tbls_m[id][CPU_LV_TO_OPP_IDX(lv)];
		p->freq_tbl = opp_tbl_m_info->opp_tbl_m;

		cpufreq_lock(flags);
		p->armpll_is_available = 1;
#if 0
/* #ifdef ENABLE_IDVFS */
		if (MT_CPU_DVFS_B == id) {
			if (!disable_idvfs_flag) {
				BigiDVFSEnable_hp();
			}
		}
#endif
#ifdef CONFIG_HYBRID_CPU_DVFS	/* after BigiDVFSEnable */
		if (enable_cpuhvfs)
			cpuhvfs_notify_cluster_on(cpu_dvfs_to_cluster(p));
#endif
		cpufreq_unlock(flags);

		if (init_cci_status == 0) {
			/* init cci freq idx */
			if (_mt_cpufreq_sync_opp_tbl_idx(p_cci) >= 0)
				if (p_cci->idx_normal_max_opp == -1)
					p_cci->idx_normal_max_opp = p_cci->idx_opp_tbl;

			opp_tbl_m_cci_info = &opp_tbls_m[id_cci][CPU_LV_TO_OPP_IDX(lv)];
			p_cci->freq_tbl = opp_tbl_m_cci_info->opp_tbl_m;
			p_cci->armpll_is_available = 1;
			init_cci_status = 1;
		}
		/* Sync min/max for PPM */
		/* cpufreq_update_policy(policy->cpu); */
	}

	if (ret)
		cpufreq_err("failed to setup frequency table\n");

	FUNC_EXIT(FUNC_LV_MODULE);

	return ret;
}

static int _mt_cpufreq_exit(struct cpufreq_policy *policy)
{
	return 0;
}

static unsigned int _mt_cpufreq_get(unsigned int cpu)
{
	struct mt_cpu_dvfs *p;

	FUNC_ENTER(FUNC_LV_MODULE);

	p = id_to_cpu_dvfs(_get_cpu_dvfs_id(cpu));

	BUG_ON(NULL == p);

	FUNC_EXIT(FUNC_LV_MODULE);

	return cpu_dvfs_get_cur_freq(p);
}

#ifdef CONFIG_HYBRID_CPU_DVFS
#include <linux/syscore_ops.h>
#include <linux/smp.h>

static void __set_cpuhvfs_init_sta(struct init_sta *sta)
{
	int i, r;
	unsigned int volt;
	struct mt_cpu_dvfs *p;

	for (i = 0; i < NUM_CPU_CLUSTER; i++) {
		p = cluster_to_cpu_dvfs(i);

		r = _sync_opp_tbl_idx(p);	/* find OPP with current frequency */
		BUG_ON(r);

		volt = get_cur_volt_extbuck(p);
		BUG_ON(volt < EXTBUCK_VAL_TO_VOLT(0));

		sta->opp[i] = p->idx_opp_tbl;
		sta->freq[i] = p->ops->get_cur_phy_freq(p);
		sta->volt[i] = VOLT_TO_EXTBUCK_VAL(volt);
		sta->vsram[i] = VOLT_TO_PMIC_VAL(p->ops->get_cur_vsram(p));
		sta->is_on[i] = (p->armpll_is_available ? true : false);
	}
}

static int _mt_cpufreq_syscore_suspend(void)
{
	if (enable_cpuhvfs)
		return cpuhvfs_dvfsp_suspend();

	return 0;
}

static void _mt_cpufreq_syscore_resume(void)
{
	if (enable_cpuhvfs) {
		int i;
		enum mt_cpu_dvfs_id id = _get_cpu_dvfs_id(smp_processor_id());
		struct init_sta sta;
		struct mt_cpu_dvfs *p;

		for (i = 0; i < NUM_CPU_CLUSTER; i++) {
			p = cluster_to_cpu_dvfs(i);

			sta.opp[i] = OPP_AT_SUSPEND;
			sta.freq[i] = p->ops->get_cur_phy_freq(p);
			sta.volt[i] = VOLT_AT_SUSPEND;
			sta.vsram[i] = VSRAM_AT_SUSPEND;
		}

		cpuhvfs_dvfsp_resume(id_to_cluster(id), &sta);
	}
}

static struct syscore_ops _mt_cpufreq_syscore_ops = {
	.suspend	= _mt_cpufreq_syscore_suspend,
	.resume		= _mt_cpufreq_syscore_resume,
};
#endif

#ifdef CONFIG_CPU_FREQ
static struct freq_attr *_mt_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver _mt_cpufreq_driver = {
	.flags = CPUFREQ_ASYNC_NOTIFICATION,
	.verify = _mt_cpufreq_verify,
	.target = _mt_cpufreq_target,
	.init = _mt_cpufreq_init,
	.exit = _mt_cpufreq_exit,
	.get = _mt_cpufreq_get,
	.name = "mt-cpufreq",
	.attr = _mt_cpufreq_attr,
};
#endif

/*
 * Platform driver
 */
static int
_mt_cpufreq_pm_callback(struct notifier_block *nb,
		unsigned long action, void *ptr)
{
	struct mt_cpu_dvfs *p;
	int i;
	unsigned long flags;

	switch (action) {

	case PM_SUSPEND_PREPARE:
		cpufreq_ver("PM_SUSPEND_PREPARE\n");
		cpufreq_lock(flags);
		for_each_cpu_dvfs(i, p) {
			if (!cpu_dvfs_is_available(p))
				continue;
			p->dvfs_disable_by_suspend = true;
		}
		cpufreq_unlock(flags);
		break;
	case PM_HIBERNATION_PREPARE:
		break;

	case PM_POST_SUSPEND:
		cpufreq_ver("PM_POST_SUSPEND\n");
		cpufreq_lock(flags);
		for_each_cpu_dvfs(i, p) {
			if (!cpu_dvfs_is_available(p))
				continue;
			p->dvfs_disable_by_suspend = false;
		}
		cpufreq_unlock(flags);
		break;
	case PM_POST_HIBERNATION:
		break;

	default:
		return NOTIFY_DONE;
	}
	return NOTIFY_OK;
}
static int _mt_cpufreq_suspend(struct device *dev)
{
#if 0
	struct mt_cpu_dvfs *p;
	int i;

	FUNC_ENTER(FUNC_LV_MODULE);

	for_each_cpu_dvfs(i, p) {
		if (!cpu_dvfs_is_available(p))
			continue;

		p->dvfs_disable_by_suspend = true;
	}

	FUNC_EXIT(FUNC_LV_MODULE);
#endif
	return 0;
}

static int _mt_cpufreq_resume(struct device *dev)
{
#if 0
	struct mt_cpu_dvfs *p;
	int i;

	FUNC_ENTER(FUNC_LV_MODULE);

	for_each_cpu_dvfs(i, p) {
		if (!cpu_dvfs_is_available(p))
			continue;

		p->dvfs_disable_by_suspend = false;
	}

	FUNC_EXIT(FUNC_LV_MODULE);
#endif
	return 0;
}

static int _mt_cpufreq_pdrv_probe(struct platform_device *pdev)
{
	/* For table preparing*/
	unsigned int lv = _mt_cpufreq_get_cpu_level();
	struct mt_cpu_dvfs *p;
	int i, j;
	struct opp_tbl_info *opp_tbl_info;
	struct cpufreq_frequency_table *table;

	FUNC_ENTER(FUNC_LV_MODULE);

	BUG_ON(!(lv == CPU_LEVEL_0 || lv == CPU_LEVEL_1 || lv == CPU_LEVEL_2 || lv == CPU_LEVEL_3));

#ifdef CONFIG_CPU_DVFS_AEE_RR_REC
	_mt_cpufreq_aee_init();
#endif

	/* Prepare OPP table for PPM in probe to avoid nested lock */
	for_each_cpu_dvfs(j, p) {
		p->armpll_addr = (cpu_dvfs_is(p, MT_CPU_DVFS_LL) ? (unsigned int *)ARMCAXPLL0_CON1 :
				  cpu_dvfs_is(p, MT_CPU_DVFS_L) ? (unsigned int *)ARMCAXPLL1_CON1 :
				  cpu_dvfs_is(p, MT_CPU_DVFS_B) ? NULL : (unsigned int *)ARMCAXPLL2_CON1);

		opp_tbl_info = &opp_tbls[j][CPU_LV_TO_OPP_IDX(lv)];

		if (NULL == p->freq_tbl_for_cpufreq) {
			table = kzalloc((opp_tbl_info->size + 1) * sizeof(*table), GFP_KERNEL);

			if (NULL == table)
				return -ENOMEM;

			for (i = 0; i < opp_tbl_info->size; i++) {
				table[i].driver_data = i;
				table[i].frequency = opp_tbl_info->opp_tbl[i].cpufreq_khz;
			}

			table[opp_tbl_info->size].driver_data = i;
			table[opp_tbl_info->size].frequency = CPUFREQ_TABLE_END;

			p->opp_tbl = opp_tbl_info->opp_tbl;
			p->nr_opp_tbl = opp_tbl_info->size;
			p->freq_tbl_for_cpufreq = table;
		}
#if 1
		/* lv should be sync with DVFS_TABLE_TYPE_SB */
		if (j != MT_CPU_DVFS_CCI)
				mt_ppm_set_dvfs_table(p->cpu_id, p->freq_tbl_for_cpufreq, opp_tbl_info->size, lv);
#endif
	}

#ifdef CONFIG_HYBRID_CPU_DVFS	/* before cpufreq_register_driver */
	if (enable_cpuhvfs) {
		int r;
		struct init_sta sta;

		__set_cpuhvfs_init_sta(&sta);

		r = cpuhvfs_kick_dvfsp_to_run(&sta);
		if (!r) {
			for_each_cpu_dvfs(i, p) {
				p->ops->set_cur_freq = set_cur_freq_hybrid;
				p->ops->set_cur_volt = set_cur_volt_hybrid;
				if (enable_cpuhvfs == 1)
					p->ops->get_cur_volt = get_cur_volt_hybrid;
			}

			register_syscore_ops(&_mt_cpufreq_syscore_ops);
		} else {
			enable_cpuhvfs = 0;
		}
	}
#endif

#ifdef CONFIG_CPU_FREQ
	cpufreq_register_driver(&_mt_cpufreq_driver);
#endif
	register_hotcpu_notifier(&_mt_cpufreq_cpu_notifier);
	mt_ppm_register_client(PPM_CLIENT_DVFS, &ppm_limit_callback);
	pm_notifier(_mt_cpufreq_pm_callback, 0);
	FUNC_EXIT(FUNC_LV_MODULE);

	return 0;
}

static int _mt_cpufreq_pdrv_remove(struct platform_device *pdev)
{
	FUNC_ENTER(FUNC_LV_MODULE);

	unregister_hotcpu_notifier(&_mt_cpufreq_cpu_notifier);
#ifdef CONFIG_CPU_FREQ
	cpufreq_unregister_driver(&_mt_cpufreq_driver);
#endif

	FUNC_EXIT(FUNC_LV_MODULE);

	return 0;
}

static const struct dev_pm_ops _mt_cpufreq_pm_ops = {
	.suspend = _mt_cpufreq_suspend,
	.resume = _mt_cpufreq_resume,
	.freeze = _mt_cpufreq_suspend,
	.thaw = _mt_cpufreq_resume,
	.restore = _mt_cpufreq_resume,
};

struct platform_device _mt_cpufreq_pdev = {
	.name = "mt-cpufreq",
	.id = -1,
};

static struct platform_driver _mt_cpufreq_pdrv = {
	.probe = _mt_cpufreq_pdrv_probe,
	.remove = _mt_cpufreq_pdrv_remove,
	.driver = {
		   .name = "mt-cpufreq",
		   .pm = &_mt_cpufreq_pm_ops,
		   .owner = THIS_MODULE,
		   },
};

#ifndef __KERNEL__
/*
* For CTP
*/
int mt_cpufreq_pdrv_probe(void)
{
	static struct cpufreq_policy policy_LL;
	static struct cpufreq_policy policy_L;
	static struct cpufreq_policy policy_B;

	_mt_cpufreq_pdrv_probe(NULL);

	policy_LL.cpu = 0;
	_mt_cpufreq_init(&policy_LL);

	policy_L.cpu = 4;
	_mt_cpufreq_init(&policy_L);

	policy_B.cpu = 8;
	_mt_cpufreq_init(&policy_B);

	return 0;
}

int mt_cpufreq_set_opp_volt(enum mt_cpu_dvfs_id id, int idx)
{
	int ret = 0;
	static struct opp_tbl_info *info;
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	info = &opp_tbls[id][CPU_LV_TO_OPP_IDX(CPU_LEVEL_0)];

	if (idx >= info->size)
		return -1;

	return _set_cur_volt_locked(p, info->opp_tbl[idx].cpufreq_volt);

}

int mt_cpufreq_set_freq(enum mt_cpu_dvfs_id id, int idx)
{
	unsigned int cur_freq;
	unsigned int target_freq;
	int ret = -1;
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	int new_cci_opp_idx;
	unsigned int cur_cci_freq;
	unsigned int target_cci_freq;

	enum mt_cpu_dvfs_id id_cci;
	struct mt_cpu_dvfs *p_cci;

	/* This is for cci */
	id_cci = MT_CPU_DVFS_CCI;
	p_cci = id_to_cpu_dvfs(id_cci);

	/* Get cci opp idx */
	new_cci_opp_idx = _calc_new_cci_opp_idx(id_to_cpu_dvfs(id), idx);

	cur_cci_freq = p_cci->ops->get_cur_phy_freq(p_cci);
	target_cci_freq = cpu_dvfs_get_freq_by_idx(p_cci, new_cci_opp_idx);

	cur_freq = p->ops->get_cur_phy_freq(p);
	target_freq = cpu_dvfs_get_freq_by_idx(p, idx);

	ret = _cpufreq_set_locked(p, cur_freq, target_freq, NULL, cur_cci_freq, target_cci_freq);

	if (ret < 0)
		return ret;

	p->idx_opp_tbl = idx;
	p_cci->idx_opp_tbl = new_cci_opp_idx;

	return target_freq;
}

#include "dvfs.h"

unsigned int dvfs_get_cur_oppidx(enum mt_cpu_dvfs_id id)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	return p->idx_opp_tbl;
}

unsigned int _mt_get_cpu_freq(unsigned int num)
{
	int output = 0, i = 0;
	unsigned int temp, clk26cali_0, clk_dbg_cfg, clk_misc_cfg_0, clk26cali_1;

	clk_dbg_cfg = DRV_Reg32(CLK_DBG_CFG);
	/* sel abist_cksw and enable freq meter sel abist */
	DRV_WriteReg32(CLK_DBG_CFG, (clk_dbg_cfg & 0xFFFFFFFE)|(num << 16));

	clk_misc_cfg_0 = DRV_Reg32(CLK_MISC_CFG_0);
	/* select divider, WAIT CONFIRM */
	DRV_WriteReg32(CLK_MISC_CFG_0, (clk_misc_cfg_0 & 0x01FFFFFF));

	clk26cali_1 = DRV_Reg32(CLK26CALI_1);
	/* cycle count default 1024,[25:16]=3FF */
    /* DRV_WriteReg32(CLK26CALI_1, 0x00ff0000);

    temp = DRV_Reg32(CLK26CALI_0); */
	DRV_WriteReg32(CLK26CALI_0, 0x1000);
	DRV_WriteReg32(CLK26CALI_0, 0x1010);

	/* wait frequency meter finish */
	while (DRV_Reg32(CLK26CALI_0) & 0x10) {
		mdelay(10);
		i++;
		if (i > 10)
			break;
	}

	temp = DRV_Reg32(CLK26CALI_1) & 0xFFFF;
	output = ((temp * 26000)) / 1024 * 2; /* Khz */

	DRV_WriteReg32(CLK_DBG_CFG, clk_dbg_cfg);
	DRV_WriteReg32(CLK_MISC_CFG_0, clk_misc_cfg_0);
	DRV_WriteReg32(CLK26CALI_0, clk26cali_0);
	DRV_WriteReg32(CLK26CALI_1, clk26cali_1);

	cpufreq_dbg("CLK26CALI_1 = 0x%x, CPU freq = %d KHz\n", temp, output);

	if (i > 10) {
		cpufreq_dbg("meter not finished!\n");
		return 0;
	} else
		return output;
}

unsigned int dvfs_get_cpu_freq(enum mt_cpu_dvfs_id id)
{
	if (id == MT_CPU_DVFS_LL)
		return _mt_get_cpu_freq(34); /* 42 */
	else if (id == MT_CPU_DVFS_L)
		return _mt_get_cpu_freq(35); /* 43 */
	else if (id == MT_CPU_DVFS_CCI)
		return _mt_get_cpu_freq(36); /* 44 */
	else if (id == MT_CPU_DVFS_B)
		return _mt_get_cpu_freq(37); /* 46 */
	else
		return _mt_get_cpu_freq(45); /* 45 is backup pll */
}

void dvfs_set_cpu_freq_FH(enum mt_cpu_dvfs_id id, int freq)
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);
	int idx;

	if (!p) {
		cpufreq_err("%s(%d, %d), id is wrong\n", __func__, id, freq);
		return;
	}

	idx = _search_available_freq_idx(p, freq, CPUFREQ_RELATION_H);

	if (-1 == idx) {
		cpufreq_err("%s(%d, %d), freq is wrong\n", __func__, id, freq);
		return;
	}

	mt_cpufreq_set_freq(id, idx);
}

void dvfs_set_cpu_volt(enum mt_cpu_dvfs_id id, int volt)	/* volt: mv * 100 */
{
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(id);

	cpufreq_dbg("%s(%d, %d)\n", __func__, id, volt);

	if (!p) {
		cpufreq_err("%s(%d, %d), id is wrong\n", __func__, id, volt);
		return;
	}

	if (_set_cur_volt_locked(p, volt))
		cpufreq_err("%s(%d, %d), set volt fail\n", __func__, id, volt);

	cpufreq_dbg("%s(%d, %d) Vproc = %d, Vsram = %d\n",
		__func__, id, volt, p->ops->get_cur_volt(p), p->ops->get_cur_vsram(p));
}
#endif

#ifdef CONFIG_PROC_FS
/*
* PROC
*/

static char *_copy_from_user_for_proc(const char __user *buffer, size_t count)
{
	char *buf = (char *)__get_free_page(GFP_USER);

	if (!buf)
		return NULL;

	if (count >= PAGE_SIZE)
		goto out;

	if (copy_from_user(buf, buffer, count))
		goto out;

	buf[count] = '\0';

	return buf;

out:
	free_page((unsigned long)buf);

	return NULL;
}

/* cpufreq_debug */
static int cpufreq_debug_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "cpufreq debug (log level) = %d\n", func_lv_mask);

	return 0;
}

static ssize_t cpufreq_debug_proc_write(struct file *file, const char __user *buffer, size_t count,
					loff_t *pos)
{
	unsigned int dbg_lv;
	int rc;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	rc = kstrtoint(buf, 10, &dbg_lv);
	if (rc < 0)
		cpufreq_err("echo dbg_lv (dec) > /proc/cpufreq/cpufreq_debug\n");
	else
		func_lv_mask = dbg_lv;

	free_page((unsigned long)buf);
	return count;
}

static int cpufreq_power_mode_proc_show(struct seq_file *m, void *v)
{
	switch (dvfs_power_mode) {
	case Default:
		seq_puts(m, "Default\n");
		break;
	case Low_power_Mode:
		seq_puts(m, "Low_power_Mode\n");
		break;
	case Just_Make_Mode:
		seq_puts(m, "Just_Make_Mode\n");
		break;
	case Performance_Mode:
		seq_puts(m, "Performance_Mode\n");
		break;
	};

	return 0;
}

static ssize_t cpufreq_power_mode_proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned int do_power_mode;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	if (!kstrtoint(buf, 10, &do_power_mode))
		dvfs_power_mode = do_power_mode;
	else
		cpufreq_err("echo 0/1/2/3 > /proc/cpufreq/cpufreq_power_mode\n");

	free_page((unsigned long)buf);
	return count;
}

static int cpufreq_stress_test_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", do_dvfs_stress_test);

	return 0;
}

static ssize_t cpufreq_stress_test_proc_write(struct file *file, const char __user *buffer,
					      size_t count, loff_t *pos)
{
	unsigned int do_stress;
	int rc;
	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;
	rc = kstrtoint(buf, 10, &do_stress);
	if (rc < 0)
		cpufreq_err("echo 0/1 > /proc/cpufreq/cpufreq_stress_test\n");
	else
		do_dvfs_stress_test = do_stress;

	free_page((unsigned long)buf);
	return count;
}

#ifdef CONFIG_HYBRID_CPU_DVFS
static int __switch_cpuhvfs_on_off(unsigned int state)
{
	int i, r;
	struct init_sta sta;
	struct mt_cpu_dvfs *p;

	if (state) {
		if (enable_cpuhvfs)
			return 0;
		if (disable_idvfs_flag)
			return -EPERM;

		__set_cpuhvfs_init_sta(&sta);

		r = cpuhvfs_restart_dvfsp_running(&sta);
		if (r)
			return r;

		for_each_cpu_dvfs(i, p) {
			p->ops->set_cur_freq = set_cur_freq_hybrid;
			p->ops->set_cur_volt = set_cur_volt_hybrid;
			if (state == 1)
				p->ops->get_cur_volt = get_cur_volt_hybrid;
		}
		enable_cpuhvfs = (state == 1 ? state : 2);
	} else {
		if (!enable_cpuhvfs)
			return 0;

		r = cpuhvfs_stop_dvfsp_running();
		if (r)
			return r;

		for_each_cpu_dvfs(i, p) {
#ifdef ENABLE_IDVFS
			if (cpu_dvfs_is(p, MT_CPU_DVFS_B) && !disable_idvfs_flag) {
				p->ops->set_cur_freq = idvfs_set_cur_freq;
				p->ops->set_cur_volt = idvfs_set_cur_volt_extbuck;
				p->ops->get_cur_volt = get_cur_volt_extbuck;
				continue;
			}
#endif
			p->ops->set_cur_freq = set_cur_freq;
			p->ops->set_cur_volt = set_cur_volt_extbuck;
			p->ops->get_cur_volt = get_cur_volt_extbuck;
		}
		enable_cpuhvfs = 0;
	}

	return 0;
}

static int enable_cpuhvfs_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", enable_cpuhvfs);

	return 0;
}

static ssize_t enable_cpuhvfs_proc_write(struct file *file, const char __user *ubuf, size_t count,
					 loff_t *ppos)
{
	int r;
	unsigned int val;
	unsigned long flags;

	r = kstrtouint_from_user(ubuf, count, 0, &val);
	if (r)
		return -EINVAL;

	cpufreq_lock(flags);
	__switch_cpuhvfs_on_off(val);
	cpufreq_unlock(flags);

	return count;
}
#endif

/* cpufreq_ptpod_freq_volt */
static int cpufreq_ptpod_freq_volt_proc_show(struct seq_file *m, void *v)
{
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)m->private;
	int j;

	for (j = 0; j < p->nr_opp_tbl; j++) {
		seq_printf(m,
			   "[%d] = { .cpufreq_khz = %d,\t.cpufreq_volt = %d,\t.cpufreq_volt_org = %d, },\n",
			   j, p->opp_tbl[j].cpufreq_khz, p->opp_tbl[j].cpufreq_volt,
			   p->opp_tbl[j].cpufreq_volt_org);
	}

	return 0;
}

/* cpufreq_oppidx */
static int cpufreq_oppidx_proc_show(struct seq_file *m, void *v)
{
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)m->private;
	int j;

	seq_printf(m, "[%s/%d]\n" "cpufreq_oppidx = %d\n", p->name, p->cpu_id, p->idx_opp_tbl);

	for (j = 0; j < p->nr_opp_tbl; j++) {
		seq_printf(m, "\tOP(%d, %d),\n",
			   cpu_dvfs_get_freq_by_idx(p, j), cpu_dvfs_get_volt_by_idx(p, j)
		    );
	}

	return 0;
}

static ssize_t cpufreq_oppidx_proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos)
{
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)PDE_DATA(file_inode(file));
	int oppidx;
	int rc;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	BUG_ON(NULL == p);
	rc = kstrtoint(buf, 10, &oppidx);
	if (rc < 0) {
		p->dvfs_disable_by_procfs = false;
		cpufreq_err("echo oppidx > /proc/cpufreq/cpufreq_oppidx (0 <= %d < %d)\n", oppidx,
			p->nr_opp_tbl);
	} else {
		if (0 <= oppidx && oppidx < p->nr_opp_tbl) {
			p->dvfs_disable_by_procfs = true;
			/* _mt_cpufreq_set(cpu_dvfs_is(p, MT_CPU_DVFS_LL) ? MT_CPU_DVFS_LL :
					MT_CPU_DVFS_L, oppidx); */

#ifdef __TRIAL_RUN__
			rc = cpuhvfs_set_target_opp(cpu_dvfs_to_cluster(p), oppidx, NULL);
			BUG_ON(rc);
#endif
		} else {
			p->dvfs_disable_by_procfs = false;
			cpufreq_err("echo oppidx > /proc/cpufreq/cpufreq_oppidx (0 <= %d < %d)\n", oppidx,
				p->nr_opp_tbl);
		}
	}

	free_page((unsigned long)buf);

	return count;
}

/* cpufreq_freq */
static int cpufreq_freq_proc_show(struct seq_file *m, void *v)
{
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)m->private;

	seq_printf(m, "%d KHz\n", p->ops->get_cur_phy_freq(p));

	return 0;
}

static ssize_t cpufreq_freq_proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned long flags;
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)PDE_DATA(file_inode(file));
	unsigned int cur_freq;
	int freq, i, found;
	int rc;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	BUG_ON(NULL == p);
	rc = kstrtoint(buf, 10, &freq);
	if (rc < 0) {
		p->dvfs_disable_by_procfs = false;
		cpufreq_err("echo khz > /proc/cpufreq/cpufreq_freq\n");
	} else {
		if (freq < CPUFREQ_LAST_FREQ_LEVEL) {
			if (freq != 0)
				cpufreq_err("frequency should higher than %dKHz!\n",
					    CPUFREQ_LAST_FREQ_LEVEL);

			p->dvfs_disable_by_procfs = false;
			goto end;
		} else {
			for (i = 0; i < p->nr_opp_tbl; i++) {
				if (freq == p->opp_tbl[i].cpufreq_khz) {
					found = 1;
					break;
				}
			}

			if (found == 1) {
				p->dvfs_disable_by_procfs = true;
				cpufreq_lock(flags);
				cur_freq = p->ops->get_cur_phy_freq(p);

				if (freq != cur_freq) {
					p->ops->set_cur_freq(p, cur_freq, freq);
					p->idx_opp_tbl = i;
				}
#ifndef DISABLE_PBM_FEATURE
				if (!p->dvfs_disable_by_suspend)
					_kick_PBM_by_cpu(p);
#endif
				cpufreq_unlock(flags);
			} else {
				p->dvfs_disable_by_procfs = false;
				cpufreq_err("frequency %dKHz! is not found in CPU opp table\n",
					    freq);
			}
		}
	}

end:
	free_page((unsigned long)buf);

	return count;
}

/* cpufreq_volt */
static int cpufreq_volt_proc_show(struct seq_file *m, void *v)
{
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)m->private;

	if (cpu_dvfs_is_extbuck_valid()) {
		seq_printf(m, "Vproc: %d mv\n", p->ops->get_cur_volt(p) / 100);	/* mv */
		seq_printf(m, "Vsram: %d mv\n", p->ops->get_cur_vsram(p) / 100);	/* mv */
	} else
		seq_printf(m, "%d mv\n", p->ops->get_cur_volt(p) / 100);	/* mv */

	return 0;
}

static ssize_t cpufreq_volt_proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned long flags;
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)PDE_DATA(file_inode(file));
	int mv;
	int rc;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;
	rc = kstrtoint(buf, 10, &mv);
	if (rc < 0) {
		p->dvfs_disable_by_procfs = false;
		cpufreq_err("echo mv > /proc/cpufreq/cpufreq_volt\n");
	} else {
		p->dvfs_disable_by_procfs = true;
		cpufreq_lock(flags);
		_set_cur_volt_locked(p, mv * 100);
		cpufreq_unlock(flags);
	}

	free_page((unsigned long)buf);

	return count;
}

/* cpufreq_turbo_mode */
static int cpufreq_turbo_mode_proc_show(struct seq_file *m, void *v)
{
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)m->private;

	seq_printf(m, "turbo_mode = %d\n", p->turbo_mode);

	return 0;
}

static ssize_t cpufreq_turbo_mode_proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos)
{
	struct mt_cpu_dvfs *p = (struct mt_cpu_dvfs *)PDE_DATA(file_inode(file));
	unsigned int turbo_mode;
	int rc;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;
	rc = kstrtoint(buf, 10, &turbo_mode);
	if (rc < 0)
		cpufreq_err("echo 0/1 > /proc/cpufreq/%s/cpufreq_turbo_mode\n", p->name);
	else
		p->turbo_mode = turbo_mode;

	free_page((unsigned long)buf);

	return count;
}

/* cpufreq_idvfs_mode */
static int cpufreq_idvfs_mode_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "idvfs_mode = %d\n", disable_idvfs_flag ? 0 : 1);

	return 0;
}

static ssize_t cpufreq_idvfs_mode_proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned int idvfs_mode;
	int rc;
	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	rc = kstrtoint(buf, 10, &idvfs_mode);

	if (rc < 0)
		cpufreq_err("echo 0 > /proc/cpufreq/cpufreq_idvfs_mode\n");
	else {
#ifdef ENABLE_IDVFS
		unsigned long flags;
		struct mt_cpu_dvfs *p = id_to_cpu_dvfs(MT_CPU_DVFS_B);

		cpufreq_lock(flags);
		if (!idvfs_mode) {
#ifdef CONFIG_HYBRID_CPU_DVFS
			rc = __switch_cpuhvfs_on_off(0);
#else
			rc = 0;
#endif
			if (!rc) {
				BigiDVFSDisable_hp();
				p->ops = &dvfs_ops_B;
				disable_idvfs_flag = 1;
			}
		}
		cpufreq_unlock(flags);
#endif
	}

	free_page((unsigned long)buf);

	return count;
}

static int cpufreq_up_threshold_ll_proc_show(struct seq_file *m, void *v)
{
	release_dvfs = 1;
	seq_printf(m, "thres_ll = %d\n", thres_ll);

	return 0;
}

static ssize_t cpufreq_up_threshold_ll_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned long flags;
	int mv;
	int rc;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;
	rc = kstrtoint(buf, 10, &mv);
	if (rc < 0) {
		cpufreq_err("echo 0 to 15 > /proc/cpufreq/cpufreq_volt\n");
	} else {
		cpufreq_lock(flags);
		cpufreq_ver("thres_ll change to %d\n", thres_ll);
		thres_ll = mv;
		cpufreq_unlock(flags);
	}

	free_page((unsigned long)buf);

	return count;
}

static int cpufreq_up_threshold_l_proc_show(struct seq_file *m, void *v)
{
	release_dvfs = 0;
	seq_printf(m, "thres_l = %d\n", thres_l);

	return 0;
}

static ssize_t cpufreq_up_threshold_l_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned long flags;
	int mv;
	int rc;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;
	rc = kstrtoint(buf, 10, &mv);
	if (rc < 0) {
		cpufreq_err("echo 0 to 15 > /proc/cpufreq/cpufreq_volt\n");
	} else {
		cpufreq_lock(flags);
		cpufreq_ver("thres_l change to %d\n", thres_l);
		thres_l = mv;
		cpufreq_unlock(flags);
	}

	free_page((unsigned long)buf);

	return count;
}

static int cpufreq_up_threshold_b_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "thres_b = %d\n", thres_b);

	return 0;
}

static ssize_t cpufreq_up_threshold_b_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned long flags;
	int mv;
	int rc;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;
	rc = kstrtoint(buf, 10, &mv);
	if (rc < 0) {
		cpufreq_err("echo 0 to 15 > /proc/cpufreq/cpufreq_volt\n");
	} else {
		cpufreq_lock(flags);
		cpufreq_ver("thres_b change to %d\n", thres_b);
		thres_b = mv;
		cpufreq_unlock(flags);
	}

	free_page((unsigned long)buf);

	return count;
}

#define PROC_FOPS_RW(name)							\
	static int name ## _proc_open(struct inode *inode, struct file *file)	\
{									\
	return single_open(file, name ## _proc_show, PDE_DATA(inode));	\
}									\
static const struct file_operations name ## _proc_fops = {		\
	.owner          = THIS_MODULE,					\
	.open           = name ## _proc_open,				\
	.read           = seq_read,					\
	.llseek         = seq_lseek,					\
	.release        = single_release,				\
	.write          = name ## _proc_write,				\
}

#define PROC_FOPS_RO(name)							\
	static int name ## _proc_open(struct inode *inode, struct file *file)	\
{									\
	return single_open(file, name ## _proc_show, PDE_DATA(inode));	\
}									\
static const struct file_operations name ## _proc_fops = {		\
	.owner          = THIS_MODULE,					\
	.open           = name ## _proc_open,				\
	.read           = seq_read,					\
	.llseek         = seq_lseek,					\
	.release        = single_release,				\
}

#define PROC_ENTRY(name)	{__stringify(name), &name ## _proc_fops}

PROC_FOPS_RW(cpufreq_debug);
PROC_FOPS_RW(cpufreq_stress_test);
PROC_FOPS_RW(cpufreq_power_mode);
#ifdef CONFIG_HYBRID_CPU_DVFS
PROC_FOPS_RW(enable_cpuhvfs);
#endif
PROC_FOPS_RO(cpufreq_ptpod_freq_volt);
PROC_FOPS_RW(cpufreq_oppidx);
PROC_FOPS_RW(cpufreq_freq);
PROC_FOPS_RW(cpufreq_volt);
PROC_FOPS_RW(cpufreq_turbo_mode);
PROC_FOPS_RW(cpufreq_idvfs_mode);
PROC_FOPS_RW(cpufreq_up_threshold_ll);
PROC_FOPS_RW(cpufreq_up_threshold_l);
PROC_FOPS_RW(cpufreq_up_threshold_b);

static int _create_procfs(void)
{
	struct proc_dir_entry *dir = NULL;
	struct proc_dir_entry *cpu_dir = NULL;
	struct mt_cpu_dvfs *p = id_to_cpu_dvfs(0);
	int i, j;

	struct pentry {
		const char *name;
		const struct file_operations *fops;
	};

	const struct pentry entries[] = {
		PROC_ENTRY(cpufreq_debug),
		PROC_ENTRY(cpufreq_stress_test),
		PROC_ENTRY(cpufreq_power_mode),
#ifdef CONFIG_HYBRID_CPU_DVFS
		PROC_ENTRY(enable_cpuhvfs),
#endif
		PROC_ENTRY(cpufreq_up_threshold_ll),
		PROC_ENTRY(cpufreq_up_threshold_l),
		PROC_ENTRY(cpufreq_up_threshold_b),
		PROC_ENTRY(cpufreq_idvfs_mode),
	};

	const struct pentry cpu_entries[] = {
		PROC_ENTRY(cpufreq_oppidx),
		PROC_ENTRY(cpufreq_freq),
		PROC_ENTRY(cpufreq_volt),
		PROC_ENTRY(cpufreq_turbo_mode),
	};

	dir = proc_mkdir("cpufreq", NULL);

	if (!dir) {
		cpufreq_err("fail to create /proc/cpufreq @ %s()\n", __func__);
		return -ENOMEM;
	}

	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		if (!proc_create
		    (entries[i].name, S_IRUGO | S_IWUSR | S_IWGRP, dir, entries[i].fops))
			cpufreq_err("%s(), create /proc/cpufreq/%s failed\n", __func__,
				    entries[i].name);
	}

	for (i = 0; i < ARRAY_SIZE(cpu_entries); i++) {
		if (!proc_create_data
		    (cpu_entries[i].name, S_IRUGO | S_IWUSR | S_IWGRP, dir, cpu_entries[i].fops, p))
			cpufreq_err("%s(), create /proc/cpufreq/%s failed\n", __func__,
				    entries[i].name);
	}

	for_each_cpu_dvfs(j, p) {
		cpu_dir = proc_mkdir(p->name, dir);

		if (!cpu_dir) {
			cpufreq_err("fail to create /proc/cpufreq/%s @ %s()\n", p->name, __func__);
			return -ENOMEM;
		}

		for (i = 0; i < ARRAY_SIZE(cpu_entries); i++) {
			if (!proc_create_data
			    (cpu_entries[i].name, S_IRUGO | S_IWUSR | S_IWGRP, cpu_dir,
			     cpu_entries[i].fops, p))
				cpufreq_err("%s(), create /proc/cpufreq/%s/%s failed\n", __func__,
					    p->name, entries[i].name);
		}
	}

	return 0;
}
#endif

#ifdef CONFIG_OF
static int mt_cpufreq_dts_map(void)
{
	struct device_node *node;

	/* topckgen */
	node = of_find_compatible_node(NULL, NULL, TOPCKGEN_NODE);
	if (!node) {
		cpufreq_info("error: cannot find node " TOPCKGEN_NODE);
		BUG();
	}
	topckgen_base = (unsigned long)of_iomap(node, 0);
	if (!topckgen_base) {
		cpufreq_info("error: cannot iomap " TOPCKGEN_NODE);
		BUG();
	}

	/* infracfg_ao */
	node = of_find_compatible_node(NULL, NULL, INFRACFG_AO_NODE);
	if (!node) {
		cpufreq_info("error: cannot find node " INFRACFG_AO_NODE);
		BUG();
	}
	infracfg_ao_base = (unsigned long)of_iomap(node, 0);
	if (!infracfg_ao_base) {
		cpufreq_info("error: cannot iomap " INFRACFG_AO_NODE);
		BUG();
	}

	/* apmixed */
	node = of_find_compatible_node(NULL, NULL, MCUMIXED_NODE);
	if (!node) {
		cpufreq_info("error: cannot find node " MCUMIXED_NODE);
		BUG();
	}
	mcumixed_base = (unsigned long)of_iomap(node, 0);
	if (!mcumixed_base) {
		cpufreq_info("error: cannot iomap " MCUMIXED_NODE);
		BUG();
	}
	return 0;
}
#else
static int mt_cpufreq_dts_map(void)
{
	return 0;
}
#endif

/*
* Module driver
*/
static int __init _mt_cpufreq_pdrv_init(void)
{
	int ret = 0;
	struct cpumask cpu_mask;
	unsigned int cluster_num;
	unsigned int dvfs_efuse = 0;
	int i;

	FUNC_ENTER(FUNC_LV_MODULE);

	mt_cpufreq_dts_map();

	dvfs_efuse = BigiDVFSSRAMLDOEFUSE();

	if (dvfs_efuse == 0)
		dvfs_disable_flag = 1;

	cluster_num = (unsigned int)arch_get_nr_clusters();
	for (i = 0; i < cluster_num; i++) {
		arch_get_cluster_cpus(&cpu_mask, i);
		cpu_dvfs[i].cpu_id = cpumask_first(&cpu_mask);
		cpufreq_dbg("cluster_id = %d, cluster_cpuid = %d\n",
	       i, cpu_dvfs[i].cpu_id);
	}

	cpufreq_dbg("EFUSE = 0x%x , disable_flag = %d\n", dvfs_efuse, dvfs_disable_flag);

#ifdef CONFIG_HYBRID_CPU_DVFS	/* before platform_driver_register */
	ret = cpuhvfs_module_init();
	if (ret || disable_idvfs_flag)
		enable_cpuhvfs = 0;
#endif

#ifdef CONFIG_PROC_FS
	/* init proc */
	if (_create_procfs())
		goto out;
#endif

	/* register platform device/driver */
	ret = platform_device_register(&_mt_cpufreq_pdev);

	if (ret) {
		cpufreq_err("fail to register cpufreq device @ %s()\n", __func__);
		goto out;
	}

	ret = platform_driver_register(&_mt_cpufreq_pdrv);

	if (ret) {
		cpufreq_err("fail to register cpufreq driver @ %s()\n", __func__);
		platform_device_unregister(&_mt_cpufreq_pdev);
	}

out:
	FUNC_EXIT(FUNC_LV_MODULE);

	return ret;
}

static void __exit _mt_cpufreq_pdrv_exit(void)
{
	FUNC_ENTER(FUNC_LV_MODULE);

	platform_driver_unregister(&_mt_cpufreq_pdrv);
	platform_device_unregister(&_mt_cpufreq_pdev);

	FUNC_EXIT(FUNC_LV_MODULE);
}

late_initcall(_mt_cpufreq_pdrv_init);
module_exit(_mt_cpufreq_pdrv_exit);

MODULE_DESCRIPTION("MediaTek CPU DVFS Driver v0.3");
MODULE_LICENSE("GPL");
