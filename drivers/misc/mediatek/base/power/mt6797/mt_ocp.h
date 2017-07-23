/**
 * @file    mt_ocp.h
 * @brief   Driver header for Over Current Protect
 *
 */

#ifndef __MT_OCP_H__
#define __MT_OCP_H__


#include <linux/kernel.h>
#include <mach/mt_secure_api.h>

#ifdef	__MT_OCP_C__
/* This is workaround for ocp use */
static noinline int mt_secure_call_ocp(u64 function_id, u64 arg0, u64 arg1, u64 arg2)
{
	register u64 reg0 __asm__("x0") = function_id;
	register u64 reg1 __asm__("x1") = arg0;
	register u64 reg2 __asm__("x2") = arg1;
	register u64 reg3 __asm__("x3") = arg2;
	int ret = 0;

	asm volatile(
	"smc    #0\n"
		: "+r" (reg0)
		: "r" (reg1), "r" (reg2), "r" (reg3));

	ret = (int)reg0;
	return ret;
}
#endif

/*
#ifdef __MT_OCP_C__
	#define OCP_EXTERN
#else
	#define OCP_EXTERN extern
#endif
*/

/*
 * BIT Operation
 */
#undef  BIT_OCP
#define BIT_OCP(_bit_)                    (unsigned)(1 << (_bit_))
#define BITS_OCP(_bits_, _val_)           ((((unsigned) -1 >> (31 - ((1) ? _bits_))) \
& ~((1U << ((0) ? _bits_)) - 1)) & ((_val_)<<((0) ? _bits_)))
#define BITMASK_OCP(_bits_)               (((unsigned) -1 >> (31 - ((1) ? _bits_))) & ~((1U << ((0) ? _bits_)) - 1))
#define GET_BITS_VAL_OCP(_bits_, _val_)   (((_val_) & (BITMASK_OCP(_bits_))) >> ((0) ? _bits_))



/**
 * Read/Write a field of a register.
 * @addr:       Address of the register
 * @range:      The field bit range in the form of MSB:LSB
 * @val:        The value to be written to the field
 */

#define MTK_SIP_KERNEL_OCP_READ 0x8200035F
#define MTK_SIP_KERNEL_OCP_WRITE 0x8200035E

#define ocp_read(addr)	                     mt_secure_call_ocp(MTK_SIP_KERNEL_OCP_READ, addr, 0, 0)
#define ocp_read_field(addr, range)	         GET_BITS_VAL_OCP(range, ocp_read(addr))

/* for DVT only */
#define ocp_write(addr, val)                 mt_secure_call_ocp(MTK_SIP_KERNEL_OCP_WRITE, addr, val, 0)
#define ocp_write_field(addr, range, val)    ocp_write(addr, \
(ocp_read(addr) & ~(BITMASK_OCP(range))) | BITS_OCP(range, val))


#define HW_API_DEBUG_ON 1
#define HW_API_RET_DEBUG_ON 1

/**
 * OCP control register
 */
extern void __iomem *ocp_base;  /*0x10220000 */
#define OCP_BASE_ADDR      (0x10220000) /* ocp_base     */


/* eFuse for BIG */
#define EFUSE_LkgMonTRIM      (0x0)
#define EFUSE_Cpu0LkgTrim     (0x0)
#define EFUSE_Cpu1LkgTrim     (0x0)
#define EFUSE_PowerCal        (0x200)
/* eFuse for LITTLE */
#define EFUSE_LLkgMonTRIM      (0x0)
#define EFUSE_LCpu0LkgTrim     (0x0)
#define EFUSE_LCpu1LkgTrim     (0x0)
#define EFUSE_LCpu2LkgTrim     (0x0)
#define EFUSE_LCpu3LkgTrim     (0x0)
#define EFUSE_LPowerCal        (0x200)

/* BIG */
#define OCPAPBSTATUS00         (OCP_BASE_ADDR + 0x2500)
#define OCPAPBSTATUS01         (OCP_BASE_ADDR + 0x2504)
#define OCPAPBSTATUS02         (OCP_BASE_ADDR + 0x2508)
#define OCPAPBSTATUS03         (OCP_BASE_ADDR + 0x250C)
#define OCPAPBSTATUS04         (OCP_BASE_ADDR + 0x2510)
#define OCPAPBSTATUS05         (OCP_BASE_ADDR + 0x2514)
#define OCPAPBSTATUS06         (OCP_BASE_ADDR + 0x2518)
#define OCPAPBSTATUS07         (OCP_BASE_ADDR + 0x251C)
#define OCPAPBCFG00            (OCP_BASE_ADDR + 0x2520)
#define OCPAPBCFG28            (OCP_BASE_ADDR + 0x2590)

/* LL */
#define MP0_OCP_IRQSTATE       (OCP_BASE_ADDR + 0x1000)
#define MP0_OCP_CAP_STATUS00   (OCP_BASE_ADDR + 0x1004)
#define MP0_OCP_CAP_STATUS01   (OCP_BASE_ADDR + 0x1008)
#define MP0_OCP_CAP_STATUS02   (OCP_BASE_ADDR + 0x100C)
#define MP0_OCP_CAP_STATUS03   (OCP_BASE_ADDR + 0x1010)
#define MP0_OCP_CAP_STATUS04   (OCP_BASE_ADDR + 0x1014)
#define MP0_OCP_CAP_STATUS05   (OCP_BASE_ADDR + 0x1018)
#define MP0_OCP_CAP_STATUS06   (OCP_BASE_ADDR + 0x101C)
#define MP0_OCP_CAP_STATUS07   (OCP_BASE_ADDR + 0x1020)
#define MP0_OCP_ENABLE         (OCP_BASE_ADDR + 0x1040)

/* L */
#define MP1_OCP_IRQSTATE       (OCP_BASE_ADDR + 0x3000)
#define MP1_OCP_CAP_STATUS00   (OCP_BASE_ADDR + 0x3004)
#define MP1_OCP_CAP_STATUS01   (OCP_BASE_ADDR + 0x3008)
#define MP1_OCP_CAP_STATUS02   (OCP_BASE_ADDR + 0x300C)
#define MP1_OCP_CAP_STATUS03   (OCP_BASE_ADDR + 0x3010)
#define MP1_OCP_CAP_STATUS04   (OCP_BASE_ADDR + 0x3014)
#define MP1_OCP_CAP_STATUS05   (OCP_BASE_ADDR + 0x3018)
#define MP1_OCP_CAP_STATUS06   (OCP_BASE_ADDR + 0x301C)
#define MP1_OCP_CAP_STATUS07   (OCP_BASE_ADDR + 0x3020)
#define MP1_OCP_ENABLE         (OCP_BASE_ADDR + 0x3040)

/* for ALL */
#define OCP_MA    (0x0)
#define OCP_MW    (0x1)

#define OCP_OCPI  (0x1)
#define OCP_FPI   (0x2)
#define OCP_ALL   (0x3)

#define  OCP_LL    (0)
#define  OCP_L     (1)

#define OCP_DISABLE    (0)
#define OCP_ENABLE     (1)
#define OCP_RISING     (1)
#define OCP_FALLING    (0)
#define OCP_CAP_IMM    (15)
#define OCP_CAP_IRQ0   (1)
#define OCP_CAP_IRQ1   (2)
#define OCP_CAP_EO     (3)
#define OCP_CAP_EI     (4)

#define MP0_MAX_FREQ_MHZ  (1400)
#define MP1_MAX_FREQ_MHZ  (1950)

/* OCP SMC */
#ifdef CONFIG_ARM64
#define MTK_SIP_KERNEL_BIGOCPCONFIG         0xC2000360
#define MTK_SIP_KERNEL_BIGOCPSETTARGET      0xC2000361
#define MTK_SIP_KERNEL_BIGOCPENABLE1        0xC2000362
#define MTK_SIP_KERNEL_BIGOCPENABLE0        0xC2000363
#define MTK_SIP_KERNEL_BIGOCPDISABLE        0xC2000364
#define MTK_SIP_KERNEL_BIGOCPINTLIMIT       0xC2000365
#define MTK_SIP_KERNEL_BIGOCPINTENDIS       0xC2000366
#define MTK_SIP_KERNEL_BIGOCPINTCLR         0XC2000367
#define MTK_SIP_KERNEL_BIGOCPCAPTURE1       0XC2000369
#define MTK_SIP_KERNEL_BIGOCPCAPTURE0       0XC200036A
#define MTK_SIP_KERNEL_BIGOCPCLKAVG         0XC200036C
#define MTK_SIP_KERNEL_LITTLEOCPCONFIG      0XC200036E
#define MTK_SIP_KERNEL_LITTLEOCPSETTARGET   0XC200036F
#define MTK_SIP_KERNEL_LITTLEOCPENABLE      0XC2000370
#define MTK_SIP_KERNEL_LITTLEOCPDISABLE     0XC2000371
#define MTK_SIP_KERNEL_LITTLEOCPDVFSSET     0XC2000372
#define MTK_SIP_KERNEL_LITTLEOCPINTLIMIT    0XC2000373
#define MTK_SIP_KERNEL_LITTLEOCPINTENDIS    0XC2000374
#define MTK_SIP_KERNEL_LITTLEOCPINTCLR      0XC2000375
#define MTK_SIP_KERNEL_LITTLEOCPCAPTURE00   0XC2000377
#define MTK_SIP_KERNEL_LITTLEOCPCAPTURE10   0XC2000378
#define MTK_SIP_KERNEL_LITTLEOCPCAPTURE11   0XC2000379
/* DREQ SMC */
#define MTK_SIP_KERNEL_BIGSRAMLDOENABLE     0XC2000380
#define MTK_SIP_KERNEL_BIGDREQHWEN          0XC2000381
#define MTK_SIP_KERNEL_BIGDREQSWEN          0XC2000382
#define MTK_SIP_KERNEL_BIGDREQGET           0XC2000383
#define MTK_SIP_KERNEL_LITTLEDREQSWEN       0XC2000384
#define MTK_SIP_KERNEL_LITTLEDREQGET        0XC2000385
#else
#define MTK_SIP_KERNEL_BIGOCPCONFIG         0x82000360
#define MTK_SIP_KERNEL_BIGOCPSETTARGET      0x82000361
#define MTK_SIP_KERNEL_BIGOCPENABLE1        0x82000362
#define MTK_SIP_KERNEL_BIGOCPENABLE0        0x82000363
#define MTK_SIP_KERNEL_BIGOCPDISABLE        0x82000364
#define MTK_SIP_KERNEL_BIGOCPINTLIMIT       0x82000365
#define MTK_SIP_KERNEL_BIGOCPINTENDIS       0x82000366
#define MTK_SIP_KERNEL_BIGOCPINTCLR         0X82000367
#define MTK_SIP_KERNEL_BIGOCPCAPTURE1       0X82000369
#define MTK_SIP_KERNEL_BIGOCPCAPTURE0       0X8200036A
#define MTK_SIP_KERNEL_BIGOCPCLKAVG         0X8200036C
#define MTK_SIP_KERNEL_LITTLEOCPCONFIG      0X8200036E
#define MTK_SIP_KERNEL_LITTLEOCPSETTARGET   0X8200036F
#define MTK_SIP_KERNEL_LITTLEOCPENABLE      0X82000370
#define MTK_SIP_KERNEL_LITTLEOCPDISABLE     0X82000371
#define MTK_SIP_KERNEL_LITTLEOCPDVFSSET     0X82000372
#define MTK_SIP_KERNEL_LITTLEOCPINTLIMIT    0X82000373
#define MTK_SIP_KERNEL_LITTLEOCPINTENDIS    0X82000374
#define MTK_SIP_KERNEL_LITTLEOCPINTCLR      0X82000375
#define MTK_SIP_KERNEL_LITTLEOCPCAPTURE00   0X82000377
#define MTK_SIP_KERNEL_LITTLEOCPCAPTURE10   0X82000378
#define MTK_SIP_KERNEL_LITTLEOCPCAPTURE11   0X82000379
/* DREQ SMC */
#define MTK_SIP_KERNEL_BIGSRAMLDOENABLE     0X82000380
#define MTK_SIP_KERNEL_BIGDREQHWEN          0X82000381
#define MTK_SIP_KERNEL_BIGDREQSWEN          0X82000382
#define MTK_SIP_KERNEL_BIGDREQGET           0X82000383
#define MTK_SIP_KERNEL_LITTLEDREQSWEN       0X82000384
#define MTK_SIP_KERNEL_LITTLEDREQGET        0X82000385
#endif

/* DREQ */
#define BIG_SRAMLDO      (0x102222b0)
#define BIG_SRAMDREQ     (0x102222B8)
#define LITTLE_SRAMDREQ  (0x10001000)


#undef OCP_EXTERN
#endif

extern unsigned int da9214_config_interface(unsigned char RegNum, unsigned char val,
unsigned char MASK, unsigned char SHIFT);
extern int da9214_vosel(unsigned long val);
/* __MT_OCP_H__ */
