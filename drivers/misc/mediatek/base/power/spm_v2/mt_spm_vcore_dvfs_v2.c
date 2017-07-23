#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/of_fdt.h>

#include <mt-plat/upmu_common.h>
#include <asm/setup.h>

#include "mt_vcorefs_governor.h"
#include "mt_spm_vcore_dvfs.h"
#include "mt_spm_pmic_wrap.h"
#include "mt_spm_internal.h"
#include "mt_spm_misc.h"

 /* TIMEOUT */
#define SPM_DVFS_TIMEOUT	1000
#define SPM_SCREEN_TIMEOUT	1000

/* PCM_REG6_DATA */
#define SPM_FLAG_DVFS_ACTIVE	(1 << 23)

/* BW threshold for SPM_SW_RSV_4 */
#define HPM_THRES_OFFSET	16
#define LPM_THRES_OFFSET	24

#ifdef CONFIG_MTK_RAM_CONSOLE
#define VCOREFS_AEE_RR_REC 0 /* 1 FIXME */
#else
#define VCOREFS_AEE_RR_REC 0
#endif

/* get Vcore DVFS current state */
#define get_vcore_sta()		(spm_read(SPM_SW_RSV_5) & 0x3)

/* get Vcore DVFS is progress */
#define is_dvfs_in_progress()	(spm_read(PCM_REG6_DATA) & SPM_FLAG_DVFS_ACTIVE)

/* get F/W screen on/off setting status */
#define get_screnn_sta()	(spm_read(SPM_SW_RSV_1) & 0xFFFF)

#if SPM_AEE_RR_REC
enum spm_vcorefs_step {
	SPM_VCOREFS_ENTER = 0,
	SPM_VCOREFS_B1,
	SPM_VCOREFS_DVFS_START,
	SPM_VCOREFS_B3,
	SPM_VCOREFS_B4,
	SPM_VCOREFS_B5,
	SPM_VCOREFS_DVFS_END,
	SPM_VCOREFS_B7,
	SPM_VCOREFS_B8,
	SPM_VCOREFS_B9,
	SPM_VCOREFS_LEAVE,
};

/* FW is loaded by dyna_load_fw from binary */
static const u32 vcore_dvfs_binary[] = {
	0x55aa55aa, 0x10007c1f, 0xf0000000
};

static struct pcm_desc vcore_dvfs_pcm = {
	.version = "pcm_vcore_dvfs_v0.1.2_20150323",
	.base = vcore_dvfs_binary,
	.size = 295,
	.sess = 1,
	.replace = 1,
	.vec0 = EVENT_VEC(23, 1, 0, 78),	/* FUNC_MD_VRF18_WAKEUP */
	.vec1 = EVENT_VEC(28, 1, 0, 101),	/* FUNC_MD_VRF18_SLEEP */
	.vec2 = EVENT_VEC(11, 1, 0, 124),	/* FUNC_VCORE_HIGH */
	.vec3 = EVENT_VEC(12, 1, 0, 159),	/* FUNC_VCORE_LOW */
};

void set_aee_vcore_dvfs_status(int state)
{
#if VCOREFS_AEE_RR_REC
	u32 value = aee_rr_curr_vcore_dvfs_status();

	value &= ~(0xFF);
	value |= (state & 0xFF);
	aee_rr_rec_vcore_dvfs_status(value);
#endif
}
#else
void set_aee_vcore_dvfs_status(int state)
{
	/* nothing */
}
#endif

static struct pwr_ctrl vcore_dvfs_ctrl = {
#if 1
	/* default VCORE DVFS is disabled */
	.pcm_flags = (SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS),
#endif
	.wake_src = WAKE_SRC_R12_PCM_TIMER,
	/* SPM general */
	.r0_ctrl_en = 1,
	.r7_ctrl_en = 1,

	/* VCORE DVFS Logic pwr_ctrl */
	.dvfs_halt_mask_b = 0x07,	/* 5 bit, todo: enable for isp/disp, disable gce */
	.sdio_on_dvfs_req_mask_b = 0,

	.cpu_md_dvfs_erq_merge_mask_b = 1,	/* HPM request by WFD/MHL/MD */

	.md1_ddr_en_dvfs_halt_mask_b = 0,
	.md2_ddr_en_dvfs_halt_mask_b = 0,

	.md_srcclkena_0_dvfs_req_mask_b = 0,
	.md_srcclkena_1_dvfs_req_mask_b = 0,
	.conn_srcclkena_dvfs_req_mask_b = 0,

	.vsync_dvfs_halt_mask_b = 0x0,	/* 5 bit */
	.emi_boost_dvfs_req_mask_b = 0,
	.cpu_md_emi_dvfs_req_prot_dis = 1,	/* todo: enable by MD if need check MD_SRCCLKEMA_0 */

	.spm_dvfs_req = 1,	/* set to 1 for keep high after fw loading */
	.spm_dvfs_force_down = 1,
	.cpu_md_dvfs_sop_force_on = 0,

	.emi_bw_dvfs_req_mask = 1,	/* Total BW default disable */
	.emi_boost_dvfs_req_mask_b = 0,	/* C+G BW default disable, enable by fliper */

	/* +450 SPM_EMI_BW_MODE */
	/* [0]EMI_BW_MODE, [1]EMI_BOOST_MODE default is 0 */
};

struct spm_lp_scen __spm_vcore_dvfs = {
	.pcmdesc = &vcore_dvfs_pcm,
	.pwrctrl = &vcore_dvfs_ctrl,
};

#define wait_spm_complete_by_condition(condition, timeout)	\
({							\
	int i = 0;					\
	while (!(condition)) {				\
		if (i >= (timeout)) {			\
			i = -EBUSY;			\
			break;				\
		}					\
		udelay(1);				\
		i++;					\
	}						\
	i;						\
})

static void __go_to_vcore_dvfs(u32 spm_flags, u8 spm_data)
{
	struct pcm_desc *pcmdesc;
	struct pwr_ctrl *pwrctrl;

#if 0
	if (dyna_load_pcm[DYNA_LOAD_PCM_SODI].ready) {
		pcmdesc = &(dyna_load_pcm[DYNA_LOAD_PCM_SODI].desc);
		pwrctrl = __spm_vcore_dvfs.pwrctrl;
	} else {
		spm_vcorefs_err("[%s] dyna load pcm fail\n", __func__);
		BUG();
	}
#else
	pcmdesc = __spm_vcore_dvfs.pcmdesc;
	pwrctrl = __spm_vcore_dvfs.pwrctrl;
#endif

	set_pwrctrl_pcm_flags(pwrctrl, spm_flags);

	__spm_reset_and_init_pcm(pcmdesc);

	__spm_kick_im_to_fetch(pcmdesc);

	__spm_init_pcm_register();

	__spm_init_event_vector(pcmdesc);

	__spm_set_power_control(pwrctrl);

	__spm_set_wakeup_event(pwrctrl);

	__spm_kick_pcm_to_run(pwrctrl);
}

/*
 * External Function
 */
void dump_pmic_info(void)
{
	u32 ret, reg_val;

	ret = pmic_read_interface_nolock(MT6351_BUCK_VCORE_CON0, &reg_val, 0xffff, 0);
	spm_notice("[PMIC]vcore vosel_ctrl: 0x%x\n", reg_val);

	ret = pmic_read_interface_nolock(MT6351_BUCK_VCORE_CON4, &reg_val, 0xffff, 0);
	spm_notice("[PMIC]vcore vosel: 0x%x\n", reg_val);

	ret = pmic_read_interface_nolock(MT6351_BUCK_VCORE_CON5, &reg_val, 0xffff, 0);
	spm_notice("[PMIC]vcore vosel_on: 0x%x\n", reg_val);
}

char *spm_vcorefs_dump_dvfs_regs(char *p)
{
	if (p) {
		p += sprintf(p, "MD2SPM_DVFS_CON : 0x%x\n", spm_read(MD2SPM_DVFS_CON));
		p += sprintf(p, "CPU_DVFS_REQ    : 0x%x\n", spm_read(CPU_DVFS_REQ));
		p += sprintf(p, "SPM_SRC_REQ     : 0x%x\n", spm_read(SPM_SRC_REQ));
		p += sprintf(p, "SPM_SRC_MASK    : 0x%x\n", spm_read(SPM_SRC_MASK));
		p += sprintf(p, "SPM_SRC2_MASK   : 0x%x\n", spm_read(SPM_SRC2_MASK));
		p += sprintf(p, "SPM_SW_RSV_1    : 0x%x\n", spm_read(SPM_SW_RSV_1));
		p += sprintf(p, "SPM_SW_RSV_3    : 0x%x\n", spm_read(SPM_SW_RSV_3));
		p += sprintf(p, "SPM_SW_RSV_4    : 0x%x\n", spm_read(SPM_SW_RSV_4));
		p += sprintf(p, "SPM_SW_RSV_5    : 0x%x\n", spm_read(SPM_SW_RSV_5));
		p += sprintf(p, "PCM_IM_PTR      : 0x%x (%u)\n", spm_read(PCM_IM_PTR), spm_read(PCM_IM_LEN));
		p += sprintf(p, "PCM_REG6_DATA   : 0x%x\n", spm_read(PCM_REG6_DATA));
		p += sprintf(p, "PCM_REG15_DATA  : 0x%x\n", spm_read(PCM_REG15_DATA));
	} else {
		dump_pmic_info();
		spm_vcorefs_info("SPM_SCREEN_ON_HPM : 0x%x\n", SPM_SCREEN_ON_HPM);
		spm_vcorefs_info("SPM_SCREEN_ON_LPM : 0x%x\n", SPM_SCREEN_ON_LPM);
		spm_vcorefs_info("SPM_SCREEN_OFF_HPM: 0x%x\n", SPM_SCREEN_OFF_HPM);
		spm_vcorefs_info("SPM_SCREEN_OFF_LPM: 0x%x\n", SPM_SCREEN_OFF_LPM);
		spm_vcorefs_info("MD2SPM_DVFS_CON : 0x%x\n", spm_read(MD2SPM_DVFS_CON));
		spm_vcorefs_info("CPU_DVFS_REQ    : 0x%x\n", spm_read(CPU_DVFS_REQ));
		spm_vcorefs_info("SPM_SRC_REQ     : 0x%x\n", spm_read(SPM_SRC_REQ));
		spm_vcorefs_info("SPM_SRC_MASK    : 0x%x\n", spm_read(SPM_SRC_MASK));
		spm_vcorefs_info("SPM_SRC2_MASK   : 0x%x\n", spm_read(SPM_SRC2_MASK));
		spm_vcorefs_info("SPM_SW_RSV_1    : 0x%x\n", spm_read(SPM_SW_RSV_1));
		spm_vcorefs_info("SPM_SW_RSV_3    : 0x%x\n", spm_read(SPM_SW_RSV_3));
		spm_vcorefs_info("SPM_SW_RSV_4    : 0x%x\n", spm_read(SPM_SW_RSV_4));
		spm_vcorefs_info("SPM_SW_RSV_5    : 0x%x\n", spm_read(SPM_SW_RSV_5));
		spm_vcorefs_info("PCM_IM_PTR      : 0x%x (%u)\n", spm_read(PCM_IM_PTR), spm_read(PCM_IM_LEN));
		spm_vcorefs_info("PCM_REG6_DATA   : 0x%x\n", spm_read(PCM_REG6_DATA));
		spm_vcorefs_info("PCM_REG0_DATA   : 0x%x\n", spm_read(PCM_REG0_DATA));
		spm_vcorefs_info("PCM_REG1_DATA   : 0x%x\n", spm_read(PCM_REG1_DATA));
		spm_vcorefs_info("PCM_REG2_DATA   : 0x%x\n", spm_read(PCM_REG2_DATA));
		spm_vcorefs_info("PCM_REG3_DATA   : 0x%x\n", spm_read(PCM_REG3_DATA));
		spm_vcorefs_info("PCM_REG4_DATA   : 0x%x\n", spm_read(PCM_REG4_DATA));
		spm_vcorefs_info("PCM_REG5_DATA   : 0x%x\n", spm_read(PCM_REG5_DATA));
		spm_vcorefs_info("PCM_REG7_DATA   : 0x%x\n", spm_read(PCM_REG7_DATA));
		spm_vcorefs_info("PCM_REG8_DATA   : 0x%x\n", spm_read(PCM_REG8_DATA));
		spm_vcorefs_info("PCM_REG9_DATA   : 0x%x\n", spm_read(PCM_REG9_DATA));
		spm_vcorefs_info("PCM_REG10_DATA  : 0x%x\n", spm_read(PCM_REG10_DATA));
		spm_vcorefs_info("PCM_REG11_DATA  : 0x%x\n", spm_read(PCM_REG11_DATA));
		spm_vcorefs_info("PCM_REG12_DATA  : 0x%x\n", spm_read(PCM_REG12_DATA));
		spm_vcorefs_info("PCM_REG13_DATA  : 0x%x\n", spm_read(PCM_REG13_DATA));
		spm_vcorefs_info("PCM_REG14_DATA  : 0x%x\n", spm_read(PCM_REG14_DATA));
		spm_vcorefs_err("PCM_REG15_DATA   : %u\n", spm_read(PCM_REG15_DATA));
	}

	return p;
}

/*
 * EMIBW
 */
void spm_vcorefs_enable_perform_bw(bool enable)
{
	struct pwr_ctrl *pwrctrl = __spm_vcore_dvfs.pwrctrl;
	unsigned long flags;

	spin_lock_irqsave(&__spm_lock, flags);

	if (enable) {
		pwrctrl->emi_boost_dvfs_req_mask_b = 1;
		spm_write(SPM_SRC2_MASK, spm_read(SPM_SRC2_MASK) | EMI_BOOST_DVFS_REQ_MASK_B_LSB);
	} else {
		pwrctrl->emi_boost_dvfs_req_mask_b = 0;
		spm_write(SPM_SRC2_MASK, spm_read(SPM_SRC2_MASK) & (~EMI_BOOST_DVFS_REQ_MASK_B_LSB));
	}

	spin_unlock_irqrestore(&__spm_lock, flags);

	spm_vcorefs_crit("perform BW enable: %d, SPM_SRC2_MASK: 0x%x\n", enable, spm_read(SPM_SRC2_MASK));
}

int spm_vcorefs_set_perform_bw_threshold(u32 lpm_threshold, u32 hpm_threshold)
{
	u32 value;
	unsigned long flags;

	spin_lock_irqsave(&__spm_lock, flags);
	value = spm_read(SPM_SW_RSV_4) & (~(0xFF << HPM_THRES_OFFSET | 0xFF << LPM_THRES_OFFSET));
	spm_write(SPM_SW_RSV_4, value | (hpm_threshold << HPM_THRES_OFFSET) | (lpm_threshold << LPM_THRES_OFFSET));
	spin_unlock_irqrestore(&__spm_lock, flags);

	spm_vcorefs_crit("perform BW threshold, LPM: %u, HPM: %u, SPM_SW_RSV_4: 0x%x\n",
					lpm_threshold, hpm_threshold, spm_read(SPM_SW_RSV_4));
	return 0;
}

int spm_set_vcore_dvfs(int opp, bool screen_on)
{
	struct pwr_ctrl *pwrctrl = __spm_vcore_dvfs.pwrctrl;
	unsigned long flags;
	u8 dvfs_req;
	u32 target_sta, req;
	int timer = 0;

	spin_lock_irqsave(&__spm_lock, flags);

	set_aee_vcore_dvfs_status(SPM_VCOREFS_ENTER);

	timer = wait_spm_complete_by_condition(!is_dvfs_in_progress(), SPM_DVFS_TIMEOUT);
	if (timer < 0) {
		spm_vcorefs_err("wait F/W idle timeout, PCM_REG6_DATA: 0x%x, opp: %d\n", spm_read(PCM_REG6_DATA), opp);
		spm_vcorefs_dump_dvfs_regs(NULL);
		BUG();
	}

	set_aee_vcore_dvfs_status(SPM_VCOREFS_DVFS_START);

	switch (opp) {
	case OPPI_PERF:
		dvfs_req = 1;
		if (screen_on)
			target_sta = SPM_SCREEN_ON_HPM;
		else
			target_sta = SPM_SCREEN_OFF_HPM;
		break;
	case OPPI_LOW_PWR:
		dvfs_req = 0;
		if (screen_on)
			target_sta = SPM_SCREEN_ON_LPM;
		else
			target_sta = SPM_SCREEN_OFF_LPM;
		break;
	default:
		return -EINVAL;
	}

	spm_vcorefs_crit("dvfs_req: 0x%x, target_sta: 0x%x\n", dvfs_req, target_sta);

	req = spm_read(SPM_SRC_REQ) & ~(SPM_DVFS_REQ_LSB);
	spm_write(SPM_SRC_REQ, req | (dvfs_req << 5));
	pwrctrl->spm_dvfs_req = dvfs_req;

	set_aee_vcore_dvfs_status(SPM_VCOREFS_DVFS_END);

	if (opp == OPPI_PERF) {

		timer = wait_spm_complete_by_condition(get_vcore_sta() == target_sta, SPM_DVFS_TIMEOUT);

		/* DVFS time is out of spec */
		if (timer < 0) {
			spm_vcorefs_err("wait DVFS finish timeout, SPM_SW_RSV_5: 0x%x\n", spm_read(SPM_SW_RSV_5));
			spm_vcorefs_err("dvfs_req: 0x%x, target_sta: 0x%x\n", dvfs_req, target_sta);
			spm_vcorefs_dump_dvfs_regs(NULL);
			BUG();
		}
	}

	set_aee_vcore_dvfs_status(SPM_VCOREFS_LEAVE);

	spin_unlock_irqrestore(&__spm_lock, flags);

	return timer;
}

u32 spm_vcorefs_get_MD_status(void)
{
	return spm_read(MD2SPM_DVFS_CON);
}

int spm_vcorefs_set_cpu_dvfs_req(u32 val, u32 mask)
{
	u32 value = (spm_read(CPU_DVFS_REQ) & ~mask) | (val & mask);

	spm_write(CPU_DVFS_REQ, value);

	return 0;
}

/*
 * screen on/off setting
 */
int spm_vcorefs_screen_on_setting(void)
{
	unsigned long flags;
	int timer = 0;

	spin_lock_irqsave(&__spm_lock, flags);

	spm_write(SPM_SW_RSV_1, (spm_read(SPM_SW_RSV_1) & (~0xFFFF)) | SPM_SCREEN_ON);
	spm_write(CPU_DVFS_REQ, (spm_read(CPU_DVFS_REQ) & (~0xFFFF)) | 0x0);
	spm_write(SPM_SRC2_MASK, spm_read(SPM_SRC2_MASK) & (~CPU_MD_EMI_DVFS_REQ_PROT_DIS_LSB));

	spm_write(SPM_CPU_WAKEUP_EVENT, 1);

	timer = wait_spm_complete_by_condition(get_screnn_sta() == SPM_SCREEN_SETTING_DONE, SPM_SCREEN_TIMEOUT);
	if (timer < 0) {
		spm_vcorefs_err("[%s] CPU waiting F/W ack fail, SPM_SW_RSV_1: 0x%x\n", __func__,
										spm_read(SPM_SW_RSV_1));
		spm_vcorefs_dump_dvfs_regs(NULL);
		BUG();
	}

	spm_write(SPM_CPU_WAKEUP_EVENT, 0);

	spm_write(SPM_SW_RSV_1, (spm_read(SPM_SW_RSV_1) & (~0xFFFF)) | SPM_CLEAN_WAKE_EVENT_DONE);

	spin_unlock_irqrestore(&__spm_lock, flags);

	return 0;
}

int spm_vcorefs_screen_off_setting(u32 cpu_dvfs_req)
{
	unsigned long flags;
	int timer = 0;

	spin_lock_irqsave(&__spm_lock, flags);

	spm_write(SPM_SW_RSV_1, (spm_read(SPM_SW_RSV_1) & (~0xFFFF)) | SPM_SCREEN_OFF);
	spm_write(CPU_DVFS_REQ, (spm_read(CPU_DVFS_REQ) & (~0xFFFF)) | cpu_dvfs_req);
	spm_write(SPM_SRC2_MASK, spm_read(SPM_SRC2_MASK) | CPU_MD_EMI_DVFS_REQ_PROT_DIS_LSB);

	spm_write(SPM_CPU_WAKEUP_EVENT, 1);

	timer = wait_spm_complete_by_condition(get_screnn_sta() == SPM_SCREEN_SETTING_DONE, SPM_SCREEN_TIMEOUT);
	if (timer < 0) {
		spm_vcorefs_err("[%s] CPU waiting F/W ack fail, SPM_SW_RSV_1: 0x%x\n", __func__,
										spm_read(SPM_SW_RSV_1));
		spm_vcorefs_dump_dvfs_regs(NULL);
		BUG();
	}

	spm_write(SPM_CPU_WAKEUP_EVENT, 0);

	spm_write(SPM_SW_RSV_1, (spm_read(SPM_SW_RSV_1) & (~0xFFFF)) | SPM_CLEAN_WAKE_EVENT_DONE);

	spin_unlock_irqrestore(&__spm_lock, flags);

	return 0;
}

static void _spm_vcorefs_init_reg(void)
{
	u32 mask;

	/* set en_spm2cksys_mem_ck_mux_update for SPM control */
	spm_write((spm_cksys_base + 0x40), (spm_read(spm_cksys_base + 0x40) | (0x1 << 13)));

	/* SPM_EMI_BW_MODE[0]&[1] set to 0 */
	mask = (EMI_BW_MODE_LSB | EMI_BOOST_MODE_LSB);
	spm_write(SPM_EMI_BW_MODE, spm_read(SPM_EMI_BW_MODE) & ~(mask));
}

void spm_go_to_vcore_dvfs(u32 spm_flags, u32 spm_data, bool screen_on, u32 cpu_dvfs_req)
{
	unsigned long flags;

	spin_lock_irqsave(&__spm_lock, flags);

	_spm_vcorefs_init_reg();

	mt_spm_pmic_wrap_set_phase(PMIC_WRAP_PHASE_NORMAL);

	__go_to_vcore_dvfs(spm_flags, 0);

	spin_unlock_irqrestore(&__spm_lock, flags);

	if (screen_on)
		spm_vcorefs_screen_on_setting();
	else
		spm_vcorefs_screen_off_setting(cpu_dvfs_req);

}

MODULE_DESCRIPTION("SPM-VCORE_DVFS Driver v0.1");
