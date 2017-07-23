#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <mt-plat/aee.h>
#include <mt-plat/mt_chip.h>
#include <mach/mt_spm_mtcmos_internal.h>

#include "mt_spm_idle.h"

#if !defined(CONFIG_ARCH_MT6580)
#include <irq.h>
#endif

#include <mach/wd_api.h>

#include "mt_spm_internal.h"

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#endif

void __weak aee_kernel_warning_api(const char *file, const int line, const int db_opt,
				   const char *module, const char *msg, ...)
{
}

#ifdef CONFIG_OF
#if !defined(CONFIG_ARCH_MT6580)
void __iomem *spm_base;
void __iomem *scp_i2c0_base;
void __iomem *scp_i2c1_base;
void __iomem *scp_i2c2_base;
void __iomem *i2c4_base;
#include <mt_dramc.h> /* for ucDram_Register_Read () */
#if defined(CONFIG_ARCH_MT6753)
void __iomem *_mcucfg_base;
void __iomem *_mcucfg_phys_base;
#endif
/* device tree + 32 = IRQ number */
u32 spm_irq_0 = 197;		/* 165 */
u32 spm_irq_1 = 198;		/* 166 */
u32 spm_irq_2 = 199;		/* 167 */
u32 spm_irq_3 = 200;		/* 168 */
#else
void __iomem *spm_base;
void __iomem *spm_i2c0_base;
void __iomem *spm_i2c1_base;
void __iomem *spm_i2c2_base;
void __iomem *spm_mcucfg_base;
void __iomem *spm_ddrphy_base;
void __iomem *spm_cksys_base;

/* device tree + 32 = IRQ number */
/* 88 + 32 = 120 */
u32 spm_irq_0 = 120;
u32 spm_irq_1 = 121;
u32 spm_irq_2 = 122;
u32 spm_irq_3 = 123;
#endif /* CONFIG_ARCH_MT6580 */
#endif /* CONFIG_OF */

/*
 * Config and Parameter
 */
#define SPM_MD_DDR_EN_OUT	0


/*
 * Define and Declare
 */
struct spm_irq_desc {
	unsigned int irq;
	irq_handler_t handler;
};

static twam_handler_t spm_twam_handler;

void __attribute__((weak)) mt_gic_cfg_irq2cpu(unsigned int irq, unsigned int cpu, unsigned int set)
{

}

void __attribute__((weak)) spm_deepidle_init(void)
{

}

/*
 * Init and IRQ Function
 */
static irqreturn_t spm_irq0_handler(int irq, void *dev_id)
{
	u32 isr;
	unsigned long flags;
	struct twam_sig twamsig;

	spin_lock_irqsave(&__spm_lock, flags);
	/* get ISR status */
	isr = spm_read(SPM_SLEEP_ISR_STATUS);
	if (isr & ISRS_TWAM) {
		twamsig.sig0 = spm_read(SPM_SLEEP_TWAM_STATUS0);
		twamsig.sig1 = spm_read(SPM_SLEEP_TWAM_STATUS1);
		twamsig.sig2 = spm_read(SPM_SLEEP_TWAM_STATUS2);
		twamsig.sig3 = spm_read(SPM_SLEEP_TWAM_STATUS3);
	}

	/* clean ISR status */
	spm_write(SPM_SLEEP_ISR_MASK, spm_read(SPM_SLEEP_ISR_MASK) | ISRM_ALL_EXC_TWAM);
	spm_write(SPM_SLEEP_ISR_STATUS, isr);
	if (isr & ISRS_TWAM)
		udelay(100);	/* need 3T TWAM clock (32K/26M) */
	spm_write(SPM_PCM_SW_INT_CLEAR, PCM_SW_INT0);
	spin_unlock_irqrestore(&__spm_lock, flags);

	if ((isr & ISRS_TWAM) && spm_twam_handler)
		spm_twam_handler(&twamsig);

	if (isr & (ISRS_SW_INT0 | ISRS_PCM_RETURN))
		spm_err("IRQ0 HANDLER SHOULD NOT BE EXECUTED (0x%x)\n", isr);

	return IRQ_HANDLED;
}

static irqreturn_t spm_irq_aux_handler(u32 irq_id)
{
	u32 isr;
	unsigned long flags;

	spin_lock_irqsave(&__spm_lock, flags);
	isr = spm_read(SPM_SLEEP_ISR_STATUS);
	spm_write(SPM_PCM_SW_INT_CLEAR, (1U << irq_id));
	spin_unlock_irqrestore(&__spm_lock, flags);

	spm_err("IRQ%u HANDLER SHOULD NOT BE EXECUTED (0x%x)\n", irq_id, isr);

	return IRQ_HANDLED;
}

static irqreturn_t spm_irq1_handler(int irq, void *dev_id)
{
	return spm_irq_aux_handler(1);
}

static irqreturn_t spm_irq2_handler(int irq, void *dev_id)
{
	return spm_irq_aux_handler(2);
}

static irqreturn_t spm_irq3_handler(int irq, void *dev_id)
{
	return spm_irq_aux_handler(3);
}

static int spm_irq_register(void)
{
	int i, err, r = 0;
#ifdef CONFIG_OF
	struct spm_irq_desc irqdesc[] = {
		{.irq = 0, .handler = spm_irq0_handler,},
		{.irq = 0, .handler = spm_irq1_handler,},
		{.irq = 0, .handler = spm_irq2_handler,},
		{.irq = 0, .handler = spm_irq3_handler,}
	};

	irqdesc[0].irq = SPM_IRQ0_ID;
	irqdesc[1].irq = SPM_IRQ1_ID;
	irqdesc[2].irq = SPM_IRQ2_ID;
	irqdesc[3].irq = SPM_IRQ3_ID;
#else
	struct spm_irq_desc irqdesc[] = {
		{.irq = SPM_IRQ0_ID, .handler = spm_irq0_handler,},
		{.irq = SPM_IRQ1_ID, .handler = spm_irq1_handler,},
		{.irq = SPM_IRQ2_ID, .handler = spm_irq2_handler,},
		{.irq = SPM_IRQ3_ID, .handler = spm_irq3_handler,}
	};
#endif
	for (i = 0; i < ARRAY_SIZE(irqdesc); i++) {
		err = request_irq(irqdesc[i].irq, irqdesc[i].handler,
				  IRQF_TRIGGER_LOW | IRQF_NO_SUSPEND, "SPM", NULL);
		if (err) {
			spm_err("FAILED TO REQUEST IRQ%d (%d)\n", i, err);
			r = -EPERM;
		}
/* FIXME: for fpga early porting */
#ifndef CONFIG_ARM64
		/* assign each SPM IRQ to each CPU */
		mt_gic_cfg_irq2cpu(irqdesc[i].irq, 0, 0);
		mt_gic_cfg_irq2cpu(irqdesc[i].irq, i % num_possible_cpus(), 1);
#endif
	}

	return r;
}

static void spm_register_init(void)
{
	unsigned long flags;
#if !defined(CONFIG_ARCH_MT6580)
	unsigned int code = mt_get_chip_hw_code();
#endif
#if defined(CONFIG_ARCH_MT6753)
	struct resource r;
#endif

#ifdef CONFIG_OF
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, "mediatek,SLEEP");
	if (!node)
		spm_err("find SLEEP node failed\n");
	spm_base = of_iomap(node, 0);
	if (!spm_base)
		spm_err("base spm_base failed\n");

	spm_irq_0 = irq_of_parse_and_map(node, 0);
	if (!spm_irq_0)
		spm_err("get spm_irq_0 failed\n");
	spm_irq_1 = irq_of_parse_and_map(node, 1);
	if (!spm_irq_1)
		spm_err("get spm_irq_1 failed\n");
	spm_irq_2 = irq_of_parse_and_map(node, 2);
	if (!spm_irq_2)
		spm_err("get spm_irq_2 failed\n");
	spm_irq_3 = irq_of_parse_and_map(node, 3);
	if (!spm_irq_3)
		spm_err("get spm_irq_3 failed\n");
#if defined(CONFIG_ARCH_MT6753)
#define MCUCFG_NODE "mediatek,MCUCFG"

	node = of_find_compatible_node(NULL, NULL, MCUCFG_NODE);
	if (!node) {
		spm_err("error: cannot find node " MCUCFG_NODE);
		BUG();
	}
	if (of_address_to_resource(node, 0, &r)) {
		spm_err("error: cannot get phys addr" MCUCFG_NODE);
		BUG();
	}
	_mcucfg_phys_base = (void *)r.start;

	_mcucfg_base = (void *)of_iomap(node, 0);
	if (!_mcucfg_base) {
		spm_err("error: cannot iomap " MCUCFG_NODE);
		BUG();
	}
#endif

#if !defined(CONFIG_ARCH_MT6580)
	node = of_find_compatible_node(NULL, NULL, "mediatek,SCP_I2C0");
	if (!node)
		spm_err("find SCP_I2C0 node failed\n");
	scp_i2c0_base = of_iomap(node, 0);
	if (!scp_i2c0_base)
		spm_err("base scp_i2c0_base failed\n");

	node = of_find_compatible_node(NULL, NULL, "mediatek,SCP_I2C1");
	if (!node)
		spm_err("find SCP_I2C1 node failed\n");
	scp_i2c1_base = of_iomap(node, 0);
	if (!scp_i2c1_base)
		spm_err("base scp_i2c1_base failed\n");

	node = of_find_compatible_node(NULL, NULL, "mediatek,SCP_I2C2");
	if (!node)
		spm_err("find SCP_I2C2 node failed\n");
	scp_i2c2_base = of_iomap(node, 0);
	if (!scp_i2c2_base)
		spm_err("base scp_i2c2_base failed\n");

	node = of_find_compatible_node(NULL, NULL, "mediatek,I2C4");
	if (!node)
		spm_err("find I2C4 node failed\n");
	i2c4_base = of_iomap(node, 0);
	if (!i2c4_base)
		spm_err("base i2c4_base failed\n");

	spm_err
	    ("spm_base = %p, scp_i2c0_base = %p, scp_i2c1_base = %p, scp_i2c2_base = %p\n",
	     spm_base, scp_i2c0_base, scp_i2c1_base, scp_i2c2_base);
	spm_err("spm_irq_0 = %d, spm_irq_1 = %d, spm_irq_2 = %d, spm_irq_3 = %d\n", spm_irq_0,
		spm_irq_1, spm_irq_2, spm_irq_3);
#else
	node = of_find_compatible_node(NULL, NULL, "mediatek,I2C0");
	if (!node)
		spm_err("find I2C0 node failed\n");
	spm_i2c0_base = of_iomap(node, 0);
	if (!spm_i2c0_base)
		spm_err("base spm_i2c0 failed\n");

	node = of_find_compatible_node(NULL, NULL, "mediatek,I2C1");
	if (!node)
		spm_err("find I2C1 node failed\n");
	spm_i2c1_base = of_iomap(node, 0);
	if (!spm_i2c1_base)
		spm_err("base spm_i2c1 failed\n");

	node = of_find_compatible_node(NULL, NULL, "mediatek,I2C2");
	if (!node)
		spm_err("find I2C2 node failed\n");
	spm_i2c2_base = of_iomap(node, 0);
	if (!spm_i2c2_base)
		spm_err("base spm_i2c2 failed\n");

	node = of_find_compatible_node(NULL, NULL, "mediatek,MCUCFG");	/* mcucfg */
	if (!node)
		spm_err("[MCUCFG] find node failed\n");
	spm_mcucfg_base = of_iomap(node, 0);
	if (!spm_mcucfg_base)
		spm_err("[MCUCFG] base failed\n");

	node = of_find_compatible_node(NULL, NULL, "mediatek,TOPCKGEN");	/* cksys */
	if (!node)
		spm_err("[CLK_CKSYS] find node failed\n");
	spm_cksys_base = of_iomap(node, 0);
	if (!spm_cksys_base)
		spm_err("[CLK_CKSYS] base failed\n");
	node = of_find_compatible_node(NULL, NULL, "mediatek,DDRPHY");
	if (!node)
		spm_err("find DDRPHY node failed\n");
	spm_ddrphy_base = of_iomap(node, 0);
	if (!spm_ddrphy_base)
		spm_err("[DDRPHY] base failed\n");

	spm_err("spm_base = %p, spm_i2c0_base = %p, spm_i2c1_base = %p, spm_i2c2_base = %p\n",
		spm_base, spm_i2c0_base, spm_i2c1_base, spm_i2c2_base);
	spm_err("spm_cksys_base = 0x%p, spm_mcucfg_base = 0x%p, spm_ddrphy_base = 0x%p\n",
		spm_cksys_base, spm_mcucfg_base, spm_ddrphy_base);
	spm_err("spm_irq_0 = %d, spm_irq_1 = %d, spm_irq_2 = %d, spm_irq_3 = %d\n", spm_irq_0,
		spm_irq_1, spm_irq_2, spm_irq_3);
#endif /*CONFIG_ARCH_MT6580*/
#endif /*CONFIG_OF*/

	spin_lock_irqsave(&__spm_lock, flags);

	/* enable register control */
	spm_write(SPM_POWERON_CONFIG_SET, SPM_REGWR_CFG_KEY | SPM_REGWR_EN);

	/* init power control register */
	spm_write(SPM_POWER_ON_VAL0, 0);
	spm_write(SPM_POWER_ON_VAL1, POWER_ON_VAL1_DEF);
	spm_write(SPM_PCM_PWR_IO_EN, 0);

#ifdef SPM_VCORE_EN
	/* init DVFS status register */
	spm_write(SPM_SLEEP_DVFS_STA,
		  HPM_REQ_STA | /*VRF18_0_STA | */ VCORE_STA_1 /* Vcore 1.15 */);
#endif

	/* reset PCM */
	spm_write(SPM_PCM_CON0, CON0_CFG_KEY | CON0_PCM_SW_RESET);
	spm_write(SPM_PCM_CON0, CON0_CFG_KEY);
	/* PCM reset failed */
	/* BUG_ON(spm_read(SPM_PCM_FSM_STA) != PCM_FSM_STA_DEF); */

	/* init PCM control register */
	spm_write(SPM_PCM_CON0, CON0_CFG_KEY | CON0_IM_SLEEP_DVS);
	spm_write(SPM_PCM_CON1, CON1_CFG_KEY | CON1_EVENT_LOCK_EN |
		  CON1_SPM_SRAM_ISO_B | CON1_SPM_SRAM_SLP_B | CON1_MIF_APBEN);
	spm_write(SPM_PCM_IM_PTR, 0);
	spm_write(SPM_PCM_IM_LEN, 0);

#if !defined(CONFIG_ARCH_MT6580)
	/*
	 * SRCLKENA0: POWER_ON_VAL1 (PWR_IO_EN[7]=0) or
	 *            E1: r7|SRCLKENAI0|SRCLKENAI1|MD1_SRCLKENA (PWR_IO_EN[7]=1)
	 *            E2: r7|SRCLKENAI0 (PWR_IO_EN[7]=1)
	 * CLKSQ0_OFF: POWER_ON_VAL0 (PWR_IO_EN[0]=0) or r0 (PWR_IO_EN[0]=1)
	 * SRCLKENA1: MD2_SRCLKENA
	 * CLKSQ1_OFF: !MD2_SRCLKENA
	 */
	spm_write(SPM_CLK_CON,
		  spm_read(SPM_CLK_CON) | CC_SRCLKENA_MASK_0 | CC_SYSCLK1_EN_0 | CC_SYSCLK1_EN_1 |
		  CC_CLKSQ1_SEL | CC_CXO32K_RM_EN_MD2 | CC_CXO32K_RM_EN_MD1 | CC_MD32_DCM_EN);
	spm_write(SPM_PCM_SRC_REQ,
		  SR_CCIF0_TO_AP_MASK_B | SR_CCIF0_TO_MD_MASK_B | SR_CCIF1_TO_AP_MASK_B |
		  SR_CCIF1_TO_MD_MASK_B);
	spm_write(SPM_AP_STANBY_CON, spm_read(SPM_AP_STANBY_CON) | ASC_SRCCLKENI_MASK);

	if (0x335 == code)
		spm_write(SPM_PCM_RESERVE2, spm_read(SPM_PCM_RESERVE2) | (1U << 4));
	else if (0x337 == code)
		spm_write(SPM_PCM_RESERVE2, spm_read(SPM_PCM_RESERVE2) | (1U << 4));
#else
	/* SRCLKENA: POWER_ON_VAL1 (PWR_IO_EN[7]=0) or POWER_ON_VAL1|r7 (PWR_IO_EN[7]=1) */
	/* CLKSQ: POWER_ON_VAL0 (PWR_IO_EN[0]=0) or r0 (PWR_IO_EN[0]=1) */
	/* SRCLKENAI will trigger 26M-wake/sleep event */
	/* spm_write(SPM_CLK_CON, CC_SRCLKENA_MASK_0 | CC_SYSCLK1_EN_0 | CC_SYSCLK1_EN_1 |
	 *  CC_CLKSQ1_SEL | CC_CXO32K_RM_EN_MD2 | CC_CXO32K_RM_EN_MD1 | CC_MD32_DCM_EN); */

	spm_write(SPM_CLK_CON, spm_read(SPM_CLK_CON) | CC_SRCLKENA_MASK_0 | CC_CXO32K_RM_EN_MD1);
	/* CC_CLKSQ0_SEL is DONT-CARE in Suspend since PCM_PWR_IO_EN[0]=1 in Suspend */

	spm_write(SPM_PCM_SRC_REQ, 0);

	/* TODO: check if this means "Set SRCLKENI_MASK=1'b1" */
	spm_write(SPM_AP_STANBY_CON, spm_read(SPM_AP_STANBY_CON) | ASC_SRCCLKENI_MASK);

	/* unmask gce_busy_mask (set to 1b1); otherwise, gce (cmd-q) can not notify SPM to exit EMI self-refresh */
	spm_write(SPM_PCM_MMDDR_MASK, spm_read(SPM_PCM_MMDDR_MASK) | (1U << 4));
#endif

	/* clean ISR status */
	spm_write(SPM_SLEEP_ISR_MASK, ISRM_ALL);
	spm_write(SPM_SLEEP_ISR_STATUS, ISRC_ALL);
	spm_write(SPM_PCM_SW_INT_CLEAR, PCM_SW_INT_ALL);

	/* output md_ddr_en if needed for debug */
#if SPM_MD_DDR_EN_OUT
	__spm_dbgout_md_ddr_en(true);
#endif
	spin_unlock_irqrestore(&__spm_lock, flags);
}

int spm_module_init(void)
{
	int r = 0;
	/* This following setting is moved to LK by WDT init, because of DTS init level issue */
#if !defined(CONFIG_ARCH_MT6580)
	struct wd_api *wd_api;
#endif

	spm_register_init();

	if (spm_irq_register() != 0)
		r = -EPERM;

#ifndef CONFIG_MTK_FPGA
#if defined(CONFIG_PM)
#if defined(CONFIG_ARCH_MT6735) /* || defined(CONFIG_ARCH_MT6735M) || defined(CONFIG_ARCH_MT6753) */
	if (spm_fs_init() != 0)
		r = -EPERM;
#endif
#endif
#endif

#if !defined(CONFIG_ARCH_MT6580)
	get_wd_api(&wd_api);
	if (wd_api->wd_spmwdt_mode_config) {
		wd_api->wd_spmwdt_mode_config(WD_REQ_EN, WD_REQ_RST_MODE);
	} else {
		spm_err("FAILED TO GET WD API\n");
		r = -ENODEV;
	}
#endif

#ifndef CONFIG_MTK_FPGA
	spm_sodi_init();
	/* spm_mcdi_init(); */
#if !defined(CONFIG_ARCH_MT6580)
	spm_deepidle_init();
#endif
#endif

	if (spm_golden_setting_cmp(1) != 0) {
		/* r = -EPERM; */
		aee_kernel_warning("SPM Warring", "dram golden setting mismach");
	}

#if !defined(CONFIG_ARCH_MT6580)
	spm_set_pcm_init_flag();
#endif

#ifdef SPM_VCORE_EN
	spm_go_to_vcore_dvfs(SPM_VCORE_DVFS_EN, 0);
#else
#if defined(CONFIG_ARCH_MT6735)
	/* only for common solution, no DVS */
	spm_go_to_vcore_dvfs(0, 0);
#endif
#endif

	return r;
}

/*
 * PLL Request API
 */
void spm_mainpll_on_request(const char *drv_name)
{
	int req;

	req = atomic_inc_return(&__spm_mainpll_req);
	spm_debug("%s request MAINPLL on (%d)\n", drv_name, req);
}
EXPORT_SYMBOL(spm_mainpll_on_request);

void spm_mainpll_on_unrequest(const char *drv_name)
{
	int req;

	req = atomic_dec_return(&__spm_mainpll_req);
	spm_debug("%s unrequest MAINPLL on (%d)\n", drv_name, req);
}
EXPORT_SYMBOL(spm_mainpll_on_unrequest);


/*
 * TWAM Control API
 */
void spm_twam_register_handler(twam_handler_t handler)
{
	spm_twam_handler = handler;
}
EXPORT_SYMBOL(spm_twam_register_handler);

void spm_twam_enable_monitor(const struct twam_sig *twamsig, bool speed_mode,
			     unsigned int window_len)
{
	u32 sig0 = 0, sig1 = 0, sig2 = 0, sig3 = 0;
	unsigned long flags;

	if (twamsig) {
		sig0 = twamsig->sig0 & 0x1f;
		sig1 = twamsig->sig1 & 0x1f;
		sig2 = twamsig->sig2 & 0x1f;
		sig3 = twamsig->sig3 & 0x1f;
	}

	spin_lock_irqsave(&__spm_lock, flags);
	spm_write(SPM_SLEEP_ISR_MASK, spm_read(SPM_SLEEP_ISR_MASK) & ~ISRM_TWAM);
	spm_write(SPM_SLEEP_TWAM_CON, ((sig3 << 27) |
				       (sig2 << 22) |
				       (sig1 << 17) |
				       (sig0 << 12) |
				       (TWAM_MON_TYPE_HIGH << 4) |
				       (TWAM_MON_TYPE_HIGH << 6) |
				       (TWAM_MON_TYPE_HIGH << 8) |
				       (TWAM_MON_TYPE_HIGH << 10) |
				       (speed_mode ? TWAM_CON_SPEED_EN : 0) | TWAM_CON_EN));
	spm_write(SPM_SLEEP_TWAM_WINDOW_LEN, window_len);
	spin_unlock_irqrestore(&__spm_lock, flags);

	spm_crit("enable TWAM for signal %u, %u, %u, %u (%u)\n",
		 sig0, sig1, sig2, sig3, speed_mode);


}
EXPORT_SYMBOL(spm_twam_enable_monitor);

void spm_twam_disable_monitor(void)
{
	unsigned long flags;

	spin_lock_irqsave(&__spm_lock, flags);
	spm_write(SPM_SLEEP_TWAM_CON, spm_read(SPM_SLEEP_TWAM_CON) & ~TWAM_CON_EN);
	spm_write(SPM_SLEEP_ISR_MASK, spm_read(SPM_SLEEP_ISR_MASK) | ISRM_TWAM);
	spm_write(SPM_SLEEP_ISR_STATUS, ISRC_TWAM);
	spin_unlock_irqrestore(&__spm_lock, flags);

	spm_debug("disable TWAM\n");
}
EXPORT_SYMBOL(spm_twam_disable_monitor);


/*
 * SPM Goldeng Seting API(MEMPLL Control, DRAMC)
 */
struct ddrphy_golden_cfg {
	u32 addr;
	u32 value;
};

static struct ddrphy_golden_cfg ddrphy_setting[] = {
#if !defined(CONFIG_ARCH_MT6580)
#ifdef CONFIG_OF
	{0x5c0, 0x063c0000},
	{0x5c4, 0x00000000},
#if defined(CONFIG_ARCH_MT6753)
	{0x5c8, 0x0000f410},	/* temp remove mempll2/3 control for golden setting refine */
#else
	{0x5c8, 0x0000fC10},	/* temp remove mempll2/3 control for golden setting refine */
#endif
	{0x5cc, 0x40101000},
#else
	{0xf02135c0, 0x063c0000},
	{0xf02135c4, 0x00000000},
#if defined(CONFIG_ARCH_MT6753)
	{0xf02135c8, 0x0000fC10},	/* temp remove mempll2/3 control for golden setting refine */
#else
	{0xf02135c8, 0x0000fC10},	/* temp remove mempll2/3 control for golden setting refine */
#endif
	{0xf02135cc, 0x40101000},
#endif
#else /* CONFIG_ARCH_MT6580 */
#ifdef CONFIG_OF
	{0x5c0, 0x063c0000},
	{0x5c4, 0x00000000},
	{0x5c8, 0x0000fC10},	/* temp remove mempll2/3 control for golden setting refine */
	{0x5cc, 0x40101000},
#else
	{0xf02085c0, 0x063c0000},
	{0xf02085c4, 0x00000000},
	{0xf02085c8, 0x0000fC10},	/* temp remove mempll2/3 control for golden setting refine */
	{0xf02085cc, 0x40101000},
#endif
#endif /* CONFIG_ARCH_MT6580 */
};

int spm_golden_setting_cmp(bool en)
{
	int i, ddrphy_num, r = 0;

	if (!en)
		return r;

	/* Compare Dramc Goldeing Setting */
	ddrphy_num = sizeof(ddrphy_setting) / sizeof(ddrphy_setting[0]);
	for (i = 0; i < ddrphy_num; i++) {
#ifdef CONFIG_OF
#if !defined(CONFIG_ARCH_MT6580)
		if (ucDram_Register_Read(ddrphy_setting[i].addr) != ddrphy_setting[i].value) {
			spm_err("dramc setting mismatch addr: 0x%x, val: 0x%x\n",
				ddrphy_setting[i].addr,
				ucDram_Register_Read(ddrphy_setting[i].addr));
			r = -EPERM;
		}
#else
		if (spm_read(spm_ddrphy_base + ddrphy_setting[i].addr) != ddrphy_setting[i].value) {
			spm_err("dramc setting mismatch addr: %p, val: 0x%x\n",
				spm_ddrphy_base + ddrphy_setting[i].addr,
				spm_read(spm_ddrphy_base + ddrphy_setting[i].addr));
			r = -EPERM;
		}
#endif /*CONFIG_ARCH_MT6580*/
#else /* CONFIG_OF */
		if (spm_read(ddrphy_setting[i].addr) != ddrphy_setting[i].value) {
			spm_err("dramc setting mismatch addr: 0x%x, val: 0x%x\n",
				ddrphy_setting[i].addr, spm_read(ddrphy_setting[i].addr));
			r = -EPERM;
		}
#endif /* CONFIG_OF */
	}

	return r;
}

#if !defined(CONFIG_ARCH_MT6580)
/*
 * SPM AP-BSI Protocol Generator
 */
#define SPM_BSI_START (1U << 0)

void spm_ap_bsi_gen(unsigned int *clk_buf_cfg)
{
	spm_write(SPM_BSI_EN_SR, clk_buf_cfg[BSI_EN_SR]);
	spm_write(SPM_BSI_CLK_SR, clk_buf_cfg[BSI_CLK_SR]);
	spm_write(SPM_BSI_DO_SR, clk_buf_cfg[BSI_D0_SR]);
	spm_write(SPM_BSI_D1_SR, clk_buf_cfg[BSI_D1_SR]);
	spm_write(SPM_BSI_D2_SR, clk_buf_cfg[BSI_D2_SR]);
	spm_write(SPM_BSI_GEN, spm_read(SPM_BSI_GEN) | SPM_BSI_START);	/* SPM_BSI_START */
	/* Polling SPM_BSI_START finish */
	while ((spm_read(SPM_BSI_GEN) & 0x1))
		;
}
#endif

unsigned int spm_get_cpu_pwr_status(void)
{
	unsigned int val[2] = {0};
	unsigned int stat = 0;

	val[0] = spm_read(SPM_PWR_STATUS);
	val[1] = spm_read(SPM_PWR_STATUS_2ND);

	stat = (val[0] & (CA15_CPU3 | CA15_CPU2 | CA15_CPU1 | CA15_CPU0 | CA7_CPU3 | CA7_CPU2 | CA7_CPU1 | CA7_CPU0));
	stat &= (val[1] & (CA15_CPU3 | CA15_CPU2 | CA15_CPU1 | CA15_CPU0 | CA7_CPU3 | CA7_CPU2 | CA7_CPU1 | CA7_CPU0));

	return stat;
}

MODULE_DESCRIPTION("SPM Driver v0.1");
