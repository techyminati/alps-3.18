#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/delay.h>

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#endif

#include <mach/irqs.h>
#include <mach/mt_cirq.h>
#include <mach/mt_spm_idle.h>
#include <mach/mt_cpuidle.h>
#include <mach/mt_gpt.h>
#include <mach/mt_boot.h>
#include <mach/wd_api.h>
#include <mach/mt_spm_misc.h>
#include <mach/upmu_common.h>

#include "mt_spm_internal.h"
#include "mt_spm_pmic_wrap.h"


/**************************************
 * only for internal debug
 **************************************/

#define SODI3_TAG     "[SODI3] "
#define sodi3_err(fmt, args...)		pr_err(SODI3_TAG fmt, ##args)
#define sodi3_warn(fmt, args...)	pr_warn(SODI3_TAG fmt, ##args)
#define sodi3_debug(fmt, args...)	pr_debug(SODI3_TAG fmt, ##args)

#define SPM_BYPASS_SYSPWREQ         0	/* JTAG is used */

#define REDUCE_SODI3_LOG             1
#if REDUCE_SODI3_LOG
#define LOG_BUF_SIZE                 256
#define SODI3_LOGOUT_TIMEOUT_CRITERA 20
#endif


#if defined(CONFIG_ARCH_MT6755)
#define WAKE_SRC_FOR_SODI					\
	(WAKE_SRC_R12_KP_IRQ_B |				\
	WAKE_SRC_R12_APXGPT1_EVENT_B |			\
	WAKE_SRC_R12_EINT_EVENT_B |				\
	WAKE_SRC_R12_CCIF0_EVENT_B |			\
	WAKE_SRC_R12_USB_CDSC_B |				\
	WAKE_SRC_R12_USB_POWERDWN_B |			\
	WAKE_SRC_R12_C2K_WDT_IRQ_B |			\
	WAKE_SRC_R12_EINT_EVENT_SECURE_B |		\
	WAKE_SRC_R12_CCIF1_EVENT_B |			\
	WAKE_SRC_R12_SYS_CIRQ_IRQ_B |			\
	WAKE_SRC_R12_CSYSPWREQ_B |				\
	WAKE_SRC_R12_MD1_WDT_B |				\
	WAKE_SRC_R12_CLDMA_EVENT_B |			\
	WAKE_SRC_R12_SEJ_WDT_GPT_B)
#elif defined(CONFIG_ARCH_MT6797)
#define WAKE_SRC_FOR_SODI								\
	(WAKE_SRC_R12_KP_IRQ_B |							\
	WAKE_SRC_R12_APXGPT1_EVENT_B |						\
	WAKE_SRC_R12_EINT_EVENT_B |							\
	WAKE_SRC_R12_CCIF0_EVENT_B |						\
	WAKE_SRC_R12_USB0_CDSC_B_AND_USB1_CSDC_B |			\
	WAKE_SRC_R12_USB0_POWERDWN_B_AND_USB1_POWERDWN_B |	\
	WAKE_SRC_R12_C2K_WDT_IRQ_B |						\
	WAKE_SRC_R12_EINT_EVENT_SECURE_B |					\
	WAKE_SRC_R12_CCIF1_EVENT_B |						\
	WAKE_SRC_R12_SYS_CIRQ_IRQ_B |						\
	WAKE_SRC_R12_CSYSPWREQ_B |							\
	WAKE_SRC_R12_MD1_WDT_B |							\
	WAKE_SRC_R12_CLDMA_EVENT_B |						\
	WAKE_SRC_R12_SEJ_WDT_B_AND_SEJ_GPT_B)
#else
#error "Does not support!"
#endif

#define WAKE_SRC_FOR_MD32  0

#define reg_read(addr)         __raw_readl(IOMEM(addr))
#define reg_write(addr, val)   mt_reg_sync_writel((val), ((void *)addr))

#if defined(CONFIG_OF)
#define MCUCFG_NODE "mediatek,MCUCFG"
static unsigned long mcucfg_base;
static unsigned long mcucfg_phys_base;
#undef MCUCFG_BASE
#define MCUCFG_BASE (mcucfg_base)

#define M4U_NODE "mediatek,M4U"
static unsigned long m4u_base;
static unsigned long m4u_phys_base;
#undef M4U_BASE
#define M4U_BASE (m4u_base)

#else /* #if defined (CONFIG_OF) */
#undef MCUCFG_BASE
#define MCUCFG_BASE 0xF0200000 /* 0x1020_0000 */

#undef M4U_BASE
#define M4U_BASE 0xF0205000 /* 0x1020_5000 */
#endif /* #if defined (CONFIG_OF) */

/* MCUCFG registers */
#define MP0_AXI_CONFIG		(MCUCFG_BASE + 0x2C)
#define MP0_AXI_CONFIG_PHYS	(mcucfg_phys_base + 0x2C)
#define MP1_AXI_CONFIG		(MCUCFG_BASE + 0x22C)
#define MP1_AXI_CONFIG_PHYS	(mcucfg_phys_base + 0x22C)
#define ACINACTM		(1 << 4)

/* M4U registers */
#define MMU_SMI_ASYNC_CFG	(M4U_BASE + 0xB80)
#define MMU_SMI_ASYNC_CFG_PHYS	(m4u_phys_base + 0xB80)
#define SMI_COMMON_ASYNC_DCM	(0x3 << 14)

#if defined(CONFIG_ARM_PSCI) || defined(CONFIG_MTK_PSCI)
#include <mach/mt_secure_api.h>
#define MCUSYS_SMC_WRITE(addr, val)  mcusys_smc_write_phy(addr##_PHYS, val)
#else
#define MCUSYS_SMC_WRITE(addr, val)  mcusys_smc_write(addr, val)
#endif

#if SPM_AEE_RR_REC
enum spm_sodi3_step {
	SPM_SODI3_ENTER = 0,
	SPM_SODI3_ENTER_UART_SLEEP,
	SPM_SODI3_ENTER_SPM_FLOW,
	SPM_SODI3_B3,
	SPM_SODI3_B4,
	SPM_SODI3_B5,
	SPM_SODI3_B6,
	SPM_SODI3_ENTER_WFI,
	SPM_SODI3_LEAVE_WFI,
	SPM_SODI3_LEAVE_SPM_FLOW,
	SPM_SODI3_ENTER_UART_AWAKE,
	SPM_SODI3_LEAVE,
};
#endif

static struct pwr_ctrl sodi3_ctrl = {
	.wake_src = WAKE_SRC_FOR_SODI3,

	.wake_src_md32 = WAKE_SRC_FOR_MD32,
	.r0_ctrl_en = 1,
	.r7_ctrl_en = 1,
	.infra_dcm_lock = 1, /* set to be 1 if SODI 3.0 */
	.wfi_op = WFI_OP_AND,

	/* SPM_AP_STANDBY_CON */
	.mp0top_idle_mask = 0,
	.mp1top_idle_mask = 0,
	.mcusys_idle_mask = 0,
	.md_ddr_dbc_en = 0,
	.md1_req_mask_b = 1,
	.md2_req_mask_b = 0, /* bit 20 */
	#if defined(CONFIG_ARCH_MT6755)
	.scp_req_mask_b = 0, /* bit 21 */
	#elif defined(CONFIG_ARCH_MT6797)
	.scp_req_mask_b = 1, /* bit 21 */
	#endif
	.lte_mask_b = 0,
	.md_apsrc1_sel = 0, /* bit 24, set to be 1 for SODI CG mode */
	.md_apsrc0_sel = 0, /* bit 25, set to be 1 for SODI CG mode */
	.conn_mask_b = 1,
	.conn_apsrc_sel = 0, /* bit 27, set to be 1 for SODI CG mode */

	/* SPM_SRC_REQ */
	.spm_apsrc_req = 0,
	.spm_f26m_req = 0,
	.spm_lte_req = 0,
	.spm_infra_req = 0,
	.spm_vrf18_req = 0,
	.spm_dvfs_req = 0,
	.spm_dvfs_force_down = 0,
	.spm_ddren_req = 0,
	.cpu_md_dvfs_sop_force_on = 0,

	/* SPM_SRC_MASK */
	.ccif0_to_md_mask_b = 1,
	.ccif0_to_ap_mask_b = 1,
	.ccif1_to_md_mask_b = 1,
	.ccif1_to_ap_mask_b = 1,
	.ccifmd_md1_event_mask_b = 1,
	.ccifmd_md2_event_mask_b = 1,
	.vsync_mask_b = 0,	/* 5-bit */
	.md_srcclkena_0_infra_mask_b = 0, /* bit 12 */
	.md_srcclkena_1_infra_mask_b = 0, /* bit 13 */
	.conn_srcclkena_infra_mask_b = 0, /* bit 14 */
	.md32_srcclkena_infra_mask_b = 0, /* bit 15 */
	.srcclkeni_infra_mask_b = 0, /* bit 16 */
	.md_apsrcreq_0_infra_mask_b = 1,
	.md_apsrcreq_1_infra_mask_b = 0,
	.conn_apsrcreq_infra_mask_b = 1,
	.md32_apsrcreq_infra_mask_b = 0,
	.md_ddr_en_0_mask_b = 1,
	.md_ddr_en_1_mask_b = 0, /* bit 22 */
	.md_vrf18_req_0_mask_b = 1,
	.md_vrf18_req_1_mask_b = 0, /* bit 24 */
	.emi_bw_dvfs_req_mask = 1,
	.md_srcclkena_0_dvfs_req_mask_b = 0,
	.md_srcclkena_1_dvfs_req_mask_b = 0,
	.conn_srcclkena_dvfs_req_mask_b = 0,

	/* SPM_SRC2_MASK */
	.dvfs_halt_mask_b = 0x1f, /* 5-bit */
	.vdec_req_mask_b = 0, /* bit 6 */
	.gce_req_mask_b = 1, /* bit 7, set to be 1 for SODI */
	.cpu_md_dvfs_erq_merge_mask_b = 0,
	.md1_ddr_en_dvfs_halt_mask_b = 0,
	.md2_ddr_en_dvfs_halt_mask_b = 0,
	.vsync_dvfs_halt_mask_b = 0, /* 5-bit, bit 11 ~ 15 */
	.conn_ddr_en_mask_b = 1,
	.disp_req_mask_b = 1, /* bit 17, set to be 1 for SODI */
	.disp1_req_mask_b = 1, /* bit 18, set to be 1 for SODI */
	#if defined(CONFIG_ARCH_MT6755)
	.mfg_req_mask_b = 0, /* bit 19 */
	#elif defined(CONFIG_ARCH_MT6797)
	.mfg_req_mask_b = 1, /* bit 19, set to be 1 for SODI */
	#endif
	.c2k_ps_rccif_wake_mask_b = 1,
	.c2k_l1_rccif_wake_mask_b = 1,
	.ps_c2k_rccif_wake_mask_b = 1,
	.l1_c2k_rccif_wake_mask_b = 1,
	.sdio_on_dvfs_req_mask_b = 0,
	.emi_boost_dvfs_req_mask_b = 0,
	.cpu_md_emi_dvfs_req_prot_dis = 0,
	#if defined(CONFIG_ARCH_MT6797)
	.disp_od_req_mask_b = 1, /* bit 27, set to be 1 for SODI */
	#endif
	/* SPM_CLK_CON */
	.srclkenai_mask = 1,

	.mp1_cpu0_wfi_en	= 1,
	.mp1_cpu1_wfi_en	= 1,
	.mp1_cpu2_wfi_en	= 1,
	.mp1_cpu3_wfi_en	= 1,
	.mp0_cpu0_wfi_en	= 1,
	.mp0_cpu1_wfi_en	= 1,
	.mp0_cpu2_wfi_en	= 1,
	.mp0_cpu3_wfi_en	= 1,

#if SPM_BYPASS_SYSPWREQ
	.syspwreq_mask = 1,
#endif
};

struct spm_lp_scen __spm_sodi3 = {
	.pwrctrl = &sodi3_ctrl,
};

static bool gSpm_sodi3_en;

#if REDUCE_SODI3_LOG
static unsigned int sodi3_logout_critera = 5000;	/* unit:ms */
static unsigned long int sodi3_logout_prev_time;
static int pre_emi_refresh_cnt;
static int memPllCG_prev_status = 1;	/* 1:CG, 0:pwrdn */
static unsigned int logout_sodi3_cnt;
static unsigned int logout_selfrefresh_cnt;
#endif

#if REDUCE_SODI3_LOG
static long int idle_get_current_time_ms(void)
{
	struct timeval t;

	do_gettimeofday(&t);
	return ((t.tv_sec & 0xFFF) * 1000000 + t.tv_usec) / 1000;
}
#endif

static void spm_trigger_wfi_for_sodi3(struct pwr_ctrl *pwrctrl)
{
	u32 v0, v1;

	if (is_cpu_pdn(pwrctrl->pcm_flags)) {
		mt_cpu_dormant(CPU_SODI_MODE);
	} else {
		/* backup MPx_AXI_CONFIG */
		v0 = reg_read(MP0_AXI_CONFIG);
		v1 = reg_read(MP1_AXI_CONFIG);

		/* disable snoop function */
		MCUSYS_SMC_WRITE(MP0_AXI_CONFIG, v0 | ACINACTM);
		MCUSYS_SMC_WRITE(MP1_AXI_CONFIG, v1 | ACINACTM);

		sodi3_debug("enter legacy WFI, MP0_AXI_CONFIG=0x%x, MP1_AXI_CONFIG=0x%x\n",
			   reg_read(MP0_AXI_CONFIG), reg_read(MP1_AXI_CONFIG));

		/* enter WFI */
		wfi_with_sync();

		/* restore MP0_AXI_CONFIG */
		MCUSYS_SMC_WRITE(MP0_AXI_CONFIG, v0);
		MCUSYS_SMC_WRITE(MP1_AXI_CONFIG, v1);

		sodi3_debug("exit legacy WFI, MP0_AXI_CONFIG=0x%x, MP1_AXI_CONFIG=0x%x\n",
			   reg_read(MP0_AXI_CONFIG), reg_read(MP1_AXI_CONFIG));
	}
}

static u32 mmu_smi_async_cfg;
static void spm_sodi3_pre_process(void)
{
	u32 val;

	__spm_pmic_pg_force_on();

	mmu_smi_async_cfg = reg_read(MMU_SMI_ASYNC_CFG);
	reg_write(MMU_SMI_ASYNC_CFG, mmu_smi_async_cfg | SMI_COMMON_ASYNC_DCM);

	spm_pmic_power_mode(PMIC_PWR_SODI3, 0, 0);

	spm_bypass_boost_gpio_set();
#if defined(CONFIG_ARCH_MT6755)
	pmic_read_interface_nolock(MT6351_PMIC_BUCK_VSRAM_PROC_VOSEL_ON_ADDR,
					&val,
					MT6351_PMIC_BUCK_VSRAM_PROC_VOSEL_ON_MASK,
					MT6351_PMIC_BUCK_VSRAM_PROC_VOSEL_ON_SHIFT);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE, IDX_DI_VSRAM_NORMAL, val);

	pmic_read_interface_nolock(MT6351_TOP_CON, &val, 0x037F, 0);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE,
					IDX_DI_SRCCLKEN_IN2_NORMAL,
					val | (1 << MT6351_PMIC_RG_SRCLKEN_IN2_EN_SHIFT));
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE,
					IDX_DI_SRCCLKEN_IN2_SLEEP,
					val & ~(1 << MT6351_PMIC_RG_SRCLKEN_IN2_EN_SHIFT));
#endif
	/* set PMIC WRAP table for deepidle power control */
	mt_spm_pmic_wrap_set_phase(PMIC_WRAP_PHASE_DEEPIDLE);

	/* Do more low power setting when MD1/C2K/CONN off */
	if (is_md_c2k_conn_power_off())
		__spm_bsi_top_init_setting();
}

static void spm_sodi3_post_process(void)
{
	/* set PMIC WRAP table for normal power control */
	mt_spm_pmic_wrap_set_phase(PMIC_WRAP_PHASE_NORMAL);

	reg_write(MMU_SMI_ASYNC_CFG, mmu_smi_async_cfg);

	__spm_pmic_pg_force_off();
}

wake_reason_t spm_go_to_sodi3(u32 spm_flags, u32 spm_data, u32 sodi_flags)
{
	u32 sec = 2;
	int wd_ret;
	struct wd_api *wd_api;
	u32 con1;
	struct wake_status wakesta;
	unsigned long flags;
	struct mtk_irq_mask mask;
	wake_reason_t wr = WR_NONE;
	struct pcm_desc *pcmdesc;
	struct pwr_ctrl *pwrctrl = __spm_sodi3.pwrctrl;
	int vcore_status = vcorefs_get_curr_ddr();
	u32 cpu = spm_data;
#if REDUCE_SODI3_LOG
	unsigned long int sodi3_logout_curr_time = 0;
	int need_log_out = 0;
#endif

	if (!dyna_load_pcm[DYNA_LOAD_PCM_SODI + cpu / 4].ready) {
		sodi3_err("error: load firmware fail\n");
		BUG();
	}
	pcmdesc = &(dyna_load_pcm[DYNA_LOAD_PCM_SODI + cpu / 4].desc);

#if SPM_AEE_RR_REC
	aee_rr_rec_sodi3_val(1 << SPM_SODI3_ENTER);
#endif

	if (spm_get_sodi_mempll() == 1)
		spm_flags |= SPM_FLAG_SODI_CG_MODE;	/* CG mode */
	else
		spm_flags &= ~SPM_FLAG_SODI_CG_MODE;	/* PDN mode */

	set_pwrctrl_pcm_flags(pwrctrl, spm_flags);

	pwrctrl->timer_val = sec * 32768;
	wd_ret = get_wd_api(&wd_api);
	if (!wd_ret)
		wd_api->wd_suspend_notify();

	/* enable APxGPT timer */
	soidle3_before_wfi(cpu);

	lockdep_off();
	spin_lock_irqsave(&__spm_lock, flags);

	mt_irq_mask_all(&mask);
	mt_irq_unmask_for_sleep(SPM_IRQ0_ID);
	mt_cirq_clone_gic();
	mt_cirq_enable();

#if SPM_AEE_RR_REC
	aee_rr_rec_sodi3_val(aee_rr_curr_sodi3_val() | (1 << SPM_SODI3_ENTER_UART_SLEEP));
#endif

	if (request_uart_to_sleep()) {
		wr = WR_UART_BUSY;
		goto RESTORE_IRQ;
	}
#if SPM_AEE_RR_REC
	aee_rr_rec_sodi3_val(aee_rr_curr_sodi3_val() | (1 << SPM_SODI3_ENTER_SPM_FLOW));
#endif

	__spm_reset_and_init_pcm(pcmdesc);

	__spm_kick_im_to_fetch(pcmdesc);

	__spm_init_pcm_register();

	__spm_init_event_vector(pcmdesc);

#if defined(CONFIG_ARCH_MT6755)
	__spm_check_md_pdn_power_control(pwrctrl);
#endif

	__spm_sync_vcore_dvfs_power_control(pwrctrl, __spm_vcore_dvfs.pwrctrl);

	if (spm_read(SPM_SW_FLAG) & SPM_FLAG_SODI_CG_MODE) {
		/* the following masks set to be 1 only for SODI CG mode */
		pwrctrl->md_apsrc1_sel = 1;
		pwrctrl->md_apsrc0_sel = 1;
		pwrctrl->conn_apsrc_sel = 1;
	} else {
		/* the following masks set to be 0 which dynamic switch by FW */
		pwrctrl->md_apsrc1_sel = 0;
		pwrctrl->md_apsrc0_sel = 0;
		pwrctrl->conn_apsrc_sel = 0;
	}

	__spm_set_power_control(pwrctrl);

	__spm_set_wakeup_event(pwrctrl);

	/* PCM WDT WAKE MODE for lastPC */
	con1 = spm_read(PCM_CON1) & ~(PCM_WDT_WAKE_MODE_LSB | PCM_WDT_EN_LSB);
	spm_write(PCM_CON1, SPM_REGWR_CFG_KEY | con1);
	if (spm_read(PCM_TIMER_VAL) > PCM_TIMER_MAX)
		spm_write(PCM_TIMER_VAL, PCM_TIMER_MAX);
	spm_write(PCM_WDT_VAL, spm_read(PCM_TIMER_VAL) + PCM_WDT_TIMEOUT);
	if (pwrctrl->timer_val_cust == 0)
		spm_write(PCM_CON1, con1 | SPM_REGWR_CFG_KEY | PCM_WDT_EN_LSB | PCM_TIMER_EN_LSB);
	else
		spm_write(PCM_CON1, con1 | SPM_REGWR_CFG_KEY | PCM_WDT_EN_LSB);

	spm_sodi3_pre_process();

	__spm_kick_pcm_to_run(pwrctrl);

#if SPM_AEE_RR_REC
	aee_rr_rec_sodi3_val(aee_rr_curr_sodi3_val() | (1 << SPM_SODI3_ENTER_WFI) |
				(1 << SPM_SODI3_B3) | (1 << SPM_SODI3_B4) |
				(1 << SPM_SODI3_B5) | (1 << SPM_SODI3_B6));
#endif

#ifdef SPM_SODI3_PROFILE_TIME
	gpt_get_cnt(SPM_SODI3_PROFILE_APXGPT, &soidle3_profile[1]);
#endif

	spm_trigger_wfi_for_sodi3(pwrctrl);

#ifdef SPM_SODI3_PROFILE_TIME
	gpt_get_cnt(SPM_SODI3_PROFILE_APXGPT, &soidle3_profile[2]);
#endif

#if SPM_AEE_RR_REC
	aee_rr_rec_sodi3_val(aee_rr_curr_sodi3_val() | (1 << SPM_SODI3_LEAVE_WFI));
#endif

	spm_sodi3_post_process();

	__spm_get_wakeup_status(&wakesta);

	/* disable PCM WDT to stop count if needed */
	spm_write(PCM_CON1, SPM_REGWR_CFG_KEY | (spm_read(PCM_CON1) & ~PCM_WDT_EN_LSB));

	__spm_clean_after_wakeup();

#if SPM_AEE_RR_REC
	aee_rr_rec_sodi3_val(aee_rr_curr_sodi3_val() | (1 << SPM_SODI3_ENTER_UART_AWAKE));
#endif

	request_uart_to_wakeup();

#if REDUCE_SODI3_LOG == 0
	sodi3_warn("vcore_status = %d, self_refresh = 0x%x, sw_flag = 0x%x, 0x%x, %s\n",
			vcore_status, spm_read(SPM_PASR_DPD_0), spm_read(SPM_SW_FLAG),
			spm_read(DUMMY1_PWR_CON), pcmdesc->version);

	wr = __spm_output_wake_reason(&wakesta, pcmdesc, false);
	if (wr == WR_PCM_ASSERT) {
		sodi3_err("PCM ASSERT AT %u (%s), r13 = 0x%x, debug_flag = 0x%x\n",
				wakesta.assert_pc, pcmdesc->version, wakesta.r13, wakesta.debug_flag);
	}
#else
	if (!(sodi_flags & (1 << 1))) {
		sodi3_logout_curr_time = idle_get_current_time_ms();

		if (wakesta.assert_pc != 0) {
			need_log_out = 1;
		} else if ((wakesta.r12 & (0x1 << 4)) == 0) {
			/* not wakeup by GPT */
			need_log_out = 1;
		} else if (wakesta.timer_out <= SODI3_LOGOUT_TIMEOUT_CRITERA) {
			need_log_out = 1;
		} else if ((spm_read(SPM_PASR_DPD_0) == 0 && pre_emi_refresh_cnt > 0) ||
				(spm_read(SPM_PASR_DPD_0) > 0 && pre_emi_refresh_cnt == 0)) {
			need_log_out = 1;
		} else if ((sodi3_logout_curr_time - sodi3_logout_prev_time) > sodi3_logout_critera) {
			/* previous logout time > sodi3_logout_critera */
			need_log_out = 1;
		} else {
			/* check CG/pwrdn status is changed */
			int mem_status = 0;

			/* check mempll CG/pwrdn status change */
			if (((spm_read(SPM_SW_FLAG) & SPM_FLAG_SODI_CG_MODE) != 0) ||
				((spm_read(DUMMY1_PWR_CON) & DUMMY1_PWR_ISO_LSB) != 0))
				mem_status = 1;

			if (memPllCG_prev_status != mem_status) {
				memPllCG_prev_status = mem_status;
				need_log_out = 1;
			}
		}

		logout_sodi3_cnt++;
		logout_selfrefresh_cnt += spm_read(SPM_PASR_DPD_0);
		pre_emi_refresh_cnt = spm_read(SPM_PASR_DPD_0);

		if (need_log_out == 1) {
			sodi3_logout_prev_time = sodi3_logout_curr_time;

			if (wakesta.assert_pc != 0) {
				sodi3_err("vcore_status = %d, self_refresh = 0x%x, sw_flag = 0x%x, 0x%x, %s, sodi3_cnt = %d, self_refresh_cnt = 0x%x\n",
						vcore_status, spm_read(SPM_PASR_DPD_0), spm_read(SPM_SW_FLAG),
						spm_read(DUMMY1_PWR_CON), pcmdesc->version,
						logout_sodi3_cnt, logout_selfrefresh_cnt);

				sodi3_err("wake up by SPM assert, spm_pc = 0x%0x, r13 = 0x%x, debug_flag = 0x%x\n",
						wakesta.assert_pc, wakesta.r13, wakesta.debug_flag);

				sodi3_err("r12 = 0x%x, r12_ext = 0x%x, raw_sta = 0x%x, idle_sta = 0x%x, event_reg = 0x%x, isr = 0x%x\n",
						wakesta.r12, wakesta.r12_ext, wakesta.raw_sta, wakesta.idle_sta,
						wakesta.event_reg, wakesta.isr);
			} else {
				char buf[LOG_BUF_SIZE] = { 0 };
				int i;

				if (wakesta.r12 & WAKE_SRC_R12_PCM_TIMER) {
					if (wakesta.wake_misc & WAKE_MISC_PCM_TIMER)
						strcat(buf, " PCM_TIMER");

					if (wakesta.wake_misc & WAKE_MISC_TWAM)
						strcat(buf, " TWAM");

					if (wakesta.wake_misc & WAKE_MISC_CPU_WAKE)
						strcat(buf, " CPU");
				}
				for (i = 1; i < 32; i++) {
					if (wakesta.r12 & (1U << i)) {
						strcat(buf, wakesrc_str[i]);
						wr = WR_WAKE_SRC;
					}
				}
				BUG_ON(strlen(buf) >= LOG_BUF_SIZE);

				sodi3_warn("vcore_status = %d, self_refresh = 0x%x, sw_flag = 0x%x, 0x%x, %s, sodi3_cnt = %d, self_refresh_cnt = 0x%x\n",
						vcore_status, spm_read(SPM_PASR_DPD_0), spm_read(SPM_SW_FLAG),
						spm_read(DUMMY1_PWR_CON), pcmdesc->version,
						logout_sodi3_cnt, logout_selfrefresh_cnt);

				sodi3_warn("wake up by %s, timer_out = %u, r13 = 0x%x, debug_flag = 0x%x\n",
						buf, wakesta.timer_out, wakesta.r13, wakesta.debug_flag);

				sodi3_warn("r12 = 0x%x, r12_ext = 0x%x, raw_sta = 0x%x, idle_sta = 0x%x, event_reg = 0x%x, isr = 0x%x\n",
						wakesta.r12, wakesta.r12_ext, wakesta.raw_sta, wakesta.idle_sta,
						wakesta.event_reg, wakesta.isr);
			}

			logout_sodi3_cnt = 0;
			logout_selfrefresh_cnt = 0;
		}
	} else {
		sodi3_warn("vcore_status = %d, self_refresh = 0x%x, sw_flag = 0x%x, 0x%x, %s\n",
			vcore_status, spm_read(SPM_PASR_DPD_0), spm_read(SPM_SW_FLAG),
			spm_read(DUMMY1_PWR_CON), pcmdesc->version);

		wr = __spm_output_wake_reason(&wakesta, pcmdesc, false);
		if (wr == WR_PCM_ASSERT)
			sodi3_err("PCM ASSERT AT %u (%s), r13 = 0x%x, debug_flag = 0x%x\n",
					wakesta.assert_pc, pcmdesc->version, wakesta.r13, wakesta.debug_flag);
	}
#endif

#if SPM_AEE_RR_REC
	aee_rr_rec_sodi3_val(aee_rr_curr_sodi3_val() | (1 << SPM_SODI3_LEAVE_SPM_FLOW));
#endif

RESTORE_IRQ:
	mt_cirq_flush();
	mt_cirq_disable();
	mt_irq_mask_restore(&mask);

	spin_unlock_irqrestore(&__spm_lock, flags);
	lockdep_on();

	/* stop APxGPT timer and enable caore0 local timer */
	soidle3_after_wfi(cpu);

	if (!wd_ret)
		wd_api->wd_resume_notify();

#if SPM_AEE_RR_REC
	aee_rr_rec_sodi3_val(0);
#endif

	return wr;
}

void spm_enable_sodi3(bool en)
{
	gSpm_sodi3_en = en;
}

bool spm_get_sodi3_en(void)
{
	return gSpm_sodi3_en;
}

#if SPM_AEE_RR_REC
static void spm_sodi3_aee_init(void)
{
	aee_rr_rec_sodi3_val(0);
}
#endif

void spm_sodi3_init(void)
{
#if defined(CONFIG_OF)
	struct device_node *node;
	struct resource r;

	/* mcucfg */
	node = of_find_compatible_node(NULL, NULL, MCUCFG_NODE);
	if (!node) {
		sodi3_err("error: cannot find node " MCUCFG_NODE);
		goto mcucfg_exit;
	}
	if (of_address_to_resource(node, 0, &r)) {
		sodi3_err("error: cannot get phys addr" MCUCFG_NODE);
		goto mcucfg_exit;
	}
	mcucfg_phys_base = r.start;

	mcucfg_base = (unsigned long)of_iomap(node, 0);
	if (!mcucfg_base) {
		sodi3_err("error: cannot iomap " MCUCFG_NODE);
		goto mcucfg_exit;
	}

	sodi3_debug("mcucfg_base = 0x%u\n", (unsigned int)mcucfg_base);

mcucfg_exit:
	/* m4u */
	node = of_find_compatible_node(NULL, NULL, M4U_NODE);
	if (!node) {
		sodi3_err("error: cannot find node " M4U_NODE);
		goto m4u_exit;
	}
	if (of_address_to_resource(node, 0, &r)) {
		sodi3_err("error: cannot get phys addr" M4U_NODE);
		goto m4u_exit;
	}
	m4u_phys_base = r.start;

	m4u_base = (unsigned long)of_iomap(node, 0);
	if (!m4u_base) {
		sodi3_err("error: cannot iomap " M4U_NODE);
		goto m4u_exit;
	}

	sodi3_debug("m4u_base = 0x%u\n", (unsigned int)m4u_base);

m4u_exit:
	sodi3_debug("spm_sodi3_init\n");
#endif

#if SPM_AEE_RR_REC
	spm_sodi3_aee_init();
#endif
}

MODULE_DESCRIPTION("SPM-SODI3 Driver v0.1");
