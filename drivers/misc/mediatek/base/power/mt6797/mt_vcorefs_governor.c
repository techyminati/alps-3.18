#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/clk.h>	/* CCF */
#include <linux/platform_device.h>

#include <linux/fb.h>
#include <linux/notifier.h>

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_address.h>
#endif

#include <mach/mt_pmic_wrap.h>

#include "spm_v2/mt_spm_pmic_wrap.h"
#include "mt_vcorefs_manager.h"
#include "mt_spm_vcore_dvfs.h"
#include "mt_dramc.h"
#include "mt_ptp.h"

/*
 * AutoK options
 */
#define AUTOK_SD	1
#define AUTOK_EMMC	1
#define	AUTOK_SDIO	0

/*
 * __nosavedata will not be restored after IPO-H boot
 */
static int vcorefs_curr_opp __nosavedata = OPPI_PERF;
static int vcorefs_prev_opp __nosavedata = OPPI_PERF;

static unsigned int	vcorefs_range[NUM_RANGE] = {RANGE_0, RANGE_1, RANGE_2, RANGE_3, RANGE_4, RANGE_5, RANGE_6};
static unsigned long	vcorefs_cnt[NUM_RANGE] = {0};

__weak int emmc_autok(void)
{
	return 0;
}

__weak int sd_autok(void)
{
	return 0;
}

__weak int sdio_autok(void)
{
	return 0;
}

/*
 * PMIC
 */
#define PMIC_VCORE_ADDR MT6351_PMIC_BUCK_VCORE_VOSEL_ON_ADDR
#define VCORE_BASE_UV		600000
#define VCORE_STEP_UV		6250

#define VCORE_INVALID		0x80

#define vcore_uv_to_pmic(uv)	/* pmic >= uv */	\
	((((uv) - VCORE_BASE_UV) + (VCORE_STEP_UV - 1)) / VCORE_STEP_UV)

#define vcore_pmic_to_uv(pmic)	\
	(((pmic) * VCORE_STEP_UV) + VCORE_BASE_UV)

/* CCF clock */
#define CCF_CONFIG	1

#if CCF_CONFIG
struct clk *clk_axi_sel;
struct clk *clk_syspll_d7;
struct clk *clk_mux_ulposc_axi_ck_mux;
#endif

/*
 * struct define
 */
static DEFINE_MUTEX(governor_mutex);

struct governor_profile {
	bool plat_feature_en;
	bool vcore_dvs;
	bool ddr_dfs;
	bool freq_dfs;

	bool isr_debug;
	bool screen_on;
	int init_opp_perf;
	int late_init_opp;
	bool plat_init_done;

	int curr_vcore_uv;
	int curr_ddr_khz;
	int curr_axi_khz;

	u32 active_autok_kir;
	u32 autok_kir_group;
	u32 md_dvfs_req;
	u32 dvfs_timer;

	bool perform_bw_enable;
	u32 perform_bw_lpm_threshold;
	u32 perform_bw_hpm_threshold;
};

static struct governor_profile governor_ctrl = {
	.plat_feature_en = 0,
	.vcore_dvs = 0,
	.ddr_dfs = 0,
	.freq_dfs = 0,

	.isr_debug = 0,
	.screen_on = 1,
	.init_opp_perf = 0,
	.late_init_opp = OPPI_LOW_PWR,
	.plat_init_done = 0,

	.active_autok_kir = 0,
	.autok_kir_group = ((1 << KIR_AUTOK_EMMC) | (1 << KIR_AUTOK_SD)),
	.md_dvfs_req = (1 << MD_CAT6_CA_DATALINK | (1 << MD_NON_CA_DATALINK) | (1 << MD_POSITION)),
	.dvfs_timer = 0,

	.curr_vcore_uv = VCORE_1_P_00_UV,
	.curr_ddr_khz = FDDR_S0_KHZ,
	.curr_axi_khz = FAXI_S0_KHZ,

	.perform_bw_enable = 0,
	.perform_bw_lpm_threshold = 0,
	.perform_bw_hpm_threshold = 0,
};

int kicker_table[NUM_KICKER] __nosavedata;
static unsigned int trans[NUM_TRANS] __nosavedata;

static struct opp_profile opp_table[] __nosavedata = {
	/* performance mode */
	[OPP_0] = {
		.vcore_uv	= VCORE_1_P_00_UV,
		.ddr_khz	= FDDR_S0_KHZ,
		.axi_khz	= FAXI_S0_KHZ,
	},
	/* low power mode */
	[OPP_1] = {
		.vcore_uv	= VCORE_0_P_90_UV,
		.ddr_khz	= FDDR_S1_KHZ,
		.axi_khz	= FAXI_S1_KHZ,
	}
};

static char *kicker_name[] = {
	"KIR_MM",
	"KIR_PERF",
	"KIR_SYSFS",
	"NUM_KICKER",

	"KIR_LATE_INIT",
	"KIR_SYSFSX",
	"KIR_AUTOK_EMMC",
	"KIR_AUTOK_SDIO",
	"KIR_AUTOK_SD",
	"LAST_KICKER",
};

/*
 * set vcore cmd
 */
static void update_vcore_pwrap_cmd(struct opp_profile *opp_ctrl_table)
{
	u32 diff;
	u32 hpm, trans4, trans3, lpm, trans2, trans1, screen_off_mode;

	diff = opp_ctrl_table[OPPI_PERF].vcore_uv - opp_ctrl_table[OPPI_LOW_PWR].vcore_uv;
	trans[TRANS4] = opp_ctrl_table[OPPI_LOW_PWR].vcore_uv + (diff / 3) * 2;
	trans[TRANS3] = opp_ctrl_table[OPPI_LOW_PWR].vcore_uv + (diff / 3) * 1;

	diff = opp_ctrl_table[OPPI_LOW_PWR].vcore_uv - VCORE_0_P_90_UV;
	trans[TRANS2] = VCORE_0_P_90_UV + (diff / 3) * 2;
	trans[TRANS1] = VCORE_0_P_90_UV + (diff / 3) * 1;

	hpm		= vcore_uv_to_pmic(opp_ctrl_table[OPPI_PERF].vcore_uv);
	trans4		= vcore_uv_to_pmic(trans[TRANS4]);
	trans3		= vcore_uv_to_pmic(trans[TRANS3]);
	lpm		= vcore_uv_to_pmic(opp_ctrl_table[OPPI_LOW_PWR].vcore_uv);
	trans2		= vcore_uv_to_pmic(trans[TRANS2]);
	trans1		= vcore_uv_to_pmic(trans[TRANS1]);
	screen_off_mode = vcore_uv_to_pmic(VCORE_0_P_90_UV);

	/* PMIC_WRAP_PHASE_NORMAL */
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_NORMAL, IDX_NM_VCORE_HPM, hpm);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_NORMAL, IDX_NM_VCORE_TRANS4, trans4);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_NORMAL, IDX_NM_VCORE_TRANS3, trans3);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_NORMAL, IDX_NM_VCORE_LPM, lpm);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_NORMAL, IDX_NM_VCORE_TRANS2, trans2);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_NORMAL, IDX_NM_VCORE_TRANS1, trans1);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_NORMAL, IDX_NM_VCORE_SCREEN_OFF_MODE, screen_off_mode);

	/* PMIC_WRAP_PHASE_SUSPEND */
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_SUSPEND, IDX_SP_VCORE_HPM, hpm);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_SUSPEND, IDX_SP_VCORE_TRANS4, trans4);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_SUSPEND, IDX_SP_VCORE_TRANS3, trans3);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_SUSPEND, IDX_SP_VCORE_LPM, lpm);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_SUSPEND, IDX_SP_VCORE_TRANS2, trans2);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_SUSPEND, IDX_SP_VCORE_TRANS1, trans1);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_SUSPEND, IDX_SP_VCORE_SCREEN_OFF_MODE, screen_off_mode);

	/* PMIC_WRAP_PHASE_DEEPIDLE */
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE, IDX_DI_VCORE_HPM, hpm);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE, IDX_DI_VCORE_TRANS4, trans4);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE, IDX_DI_VCORE_TRANS3, trans3);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE, IDX_DI_VCORE_LPM, lpm);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE, IDX_DI_VCORE_TRANS2, trans2);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE, IDX_DI_VCORE_TRANS1, trans1);
	mt_spm_pmic_wrap_set_cmd(PMIC_WRAP_PHASE_DEEPIDLE, IDX_DI_VCORE_SCREEN_OFF_MODE, screen_off_mode);

	vcorefs_crit("HPM   : %u (0x%x)\n", opp_ctrl_table[OPPI_PERF].vcore_uv, hpm);
	vcorefs_crit("TRANS4: %u (0x%x)\n", trans[TRANS4], trans4);
	vcorefs_crit("TRANS3: %u (0x%x)\n", trans[TRANS3], trans3);
	vcorefs_crit("LPM   : %u (0x%x)\n", opp_ctrl_table[OPPI_LOW_PWR].vcore_uv, lpm);
	vcorefs_crit("TRANS2: %u (0x%x)\n", trans[TRANS2], trans2);
	vcorefs_crit("TRANS1: %u (0x%x)\n", trans[TRANS1], trans1);
	vcorefs_crit("SCREEN_OFF_MODE: %u (0x%x)\n", VCORE_0_P_90_UV, screen_off_mode);
}

void vcorefs_update_opp_table(char *cmd, int val)
{
	struct opp_profile *opp_ctrl_table = opp_table;
	int uv = vcore_pmic_to_uv(val);

	if (!strcmp(cmd, "HPM") && val < VCORE_INVALID) {
		if (uv >= opp_ctrl_table[OPPI_LOW_PWR].vcore_uv) {
			opp_ctrl_table[OPPI_PERF].vcore_uv = uv;
			update_vcore_pwrap_cmd(opp_ctrl_table);
		}
	} else if (!strcmp(cmd, "LPM") && val < VCORE_INVALID) {
		if (uv <= opp_ctrl_table[OPPI_PERF].vcore_uv) {
			opp_ctrl_table[OPPI_LOW_PWR].vcore_uv = uv;
			update_vcore_pwrap_cmd(opp_ctrl_table);
		}
	}
}

/*
 * Governor extern API
 */
bool is_vcorefs_feature_enable(void)
{
	struct governor_profile *gvrctrl = &governor_ctrl;

	if (!dram_can_support_fh()) {
		gvrctrl->plat_feature_en = 0;
		vcorefs_err("DISABLE DVFS DUE TO NOT SUPPORT DRAM FH\n");
	}

	return gvrctrl->plat_feature_en;
}

bool vcorefs_get_screen_on_state(void)
{
	struct governor_profile *gvrctrl = &governor_ctrl;
	int r;

	mutex_lock(&governor_mutex);
	r = gvrctrl->screen_on;
	mutex_unlock(&governor_mutex);

	return r;
}

int vcorefs_get_num_opp(void)
{
	return NUM_OPP;
}

int vcorefs_get_curr_opp(void)
{
	return vcorefs_curr_opp;
}

int vcorefs_get_prev_opp(void)
{
	return vcorefs_prev_opp;
}

int vcorefs_get_curr_vcore(void)
{
	int vcore = VCORE_INVALID;

	pwrap_read(PMIC_VCORE_ADDR, &vcore);
	if (vcore >= VCORE_INVALID)
		pwrap_read(PMIC_VCORE_ADDR, &vcore);

	return vcore < VCORE_INVALID ? vcore_pmic_to_uv(vcore) : 0;
}

int vcorefs_get_curr_ddr(void)
{
	int ddr_khz;

	ddr_khz = get_dram_data_rate() * 1000;

	return ddr_khz;
}

int vcorefs_get_vcore_by_steps(u32 steps)
{
	switch (steps) {
	case OPP_0:
		return vcore_pmic_to_uv(get_vcore_ptp_volt(VCORE_1_P_00_UV));
	break;
	case OPP_1:
		return vcore_pmic_to_uv(get_vcore_ptp_volt(VCORE_0_P_90_UV));
	break;
	default:
		break;
	}

	return 0;
}

int vcorefs_get_ddr_by_steps(u32 steps)
{
	int ddr_khz;

	ddr_khz = dram_steps_freq(steps) * 1000;

	BUG_ON(ddr_khz < 0);

	return ddr_khz;
}

char *governor_get_kicker_name(int id)
{
	return kicker_name[id];
}

char *vcorefs_get_opp_table_info(char *p)
{
	struct opp_profile *opp_ctrl_table = opp_table;
	int i;

	for (i = 0; i < NUM_OPP; i++) {
		p += sprintf(p, "[OPP_%d] vcore_uv: %d (0x%x)\n", i, opp_ctrl_table[i].vcore_uv,
			     vcore_uv_to_pmic(opp_ctrl_table[i].vcore_uv));
		p += sprintf(p, "[OPP_%d] ddr_khz : %d\n", i, opp_ctrl_table[i].ddr_khz);
		p += sprintf(p, "[OPP_%d] axi_khz : %d\n", i, opp_ctrl_table[i].axi_khz);
		p += sprintf(p, "\n");
	}

	for (i = 0; i < NUM_TRANS; i++)
		p += sprintf(p, "[TRANS%d] vcore_uv: %d (0x%x)\n", i + 1, trans[i],
			     vcore_uv_to_pmic(trans[i]));

	return p;
}

int vcorefs_output_kicker_id(char *name)
{
	int i;

	for (i = 0; i < NUM_KICKER; i++) {
		if (!strcmp(kicker_name[i], name))
			return i;
	}

	return -1;
}

/*
 * init boot-up OPP from late init
 */
static int set_init_opp_index(void)
{
	struct governor_profile *gvrctrl = &governor_ctrl;

	if (is_vcorefs_feature_enable()) {
		if (gvrctrl->init_opp_perf /*|| reserved_condition*/)
			return OPPI_PERF;
		else
			return OPPI_LOW_PWR;
	} else {
		return vcorefs_get_curr_opp();
	}
}

/*
 *  DVFS working timer
 */
static void record_dvfs_timer(u32 timer)
{
	struct governor_profile *gvrctrl = &governor_ctrl;
	int i;

	gvrctrl->dvfs_timer = timer;

	if (timer != 0) {
		for (i = 0; i < NUM_RANGE; i++) {
			if (i == NUM_RANGE-1) {
				vcorefs_cnt[i]++;
				break;
			} else if (timer > vcorefs_range[i] && timer <= vcorefs_range[i+1]) {
				vcorefs_cnt[i]++;
				break;
			}
		}
	}
}

static void clean_dvfs_counter(int timer)
{
	int i;

	for (i = 0; i < NUM_RANGE; i++)
		vcorefs_cnt[i] = timer;
}

/*
 *  governor debug sysfs
 */
int vcorefs_enable_debug_isr(bool enable)
{

	struct governor_profile *gvrctrl = &governor_ctrl;
	int flag = SPM_FLAG_RUN_COMMON_SCENARIO;

	vcorefs_crit("enable_debug_isr: %d\n", enable);
	mutex_lock(&governor_mutex);

	gvrctrl->isr_debug = enable;

	if (!gvrctrl->vcore_dvs)
		flag |= SPM_FLAG_DIS_VCORE_DVS;
	if (!gvrctrl->ddr_dfs)
		flag |= SPM_FLAG_DIS_VCORE_DFS;

	if (enable)
		flag |= SPM_FLAG_EN_MET_DBG_FOR_VCORE_DVFS;

	spm_go_to_vcore_dvfs(flag, 0, gvrctrl->screen_on, gvrctrl->md_dvfs_req);

	mutex_unlock(&governor_mutex);

	return 0;
}

static void vcorefs_set_cpu_dvfs_req(u32 value)
{

	struct governor_profile *gvrctrl = &governor_ctrl;

	u32 mask = 0xFFFF;

	mutex_lock(&governor_mutex);
	gvrctrl->md_dvfs_req = (value & mask);

	spm_vcorefs_set_cpu_dvfs_req(value, mask);

	mutex_unlock(&governor_mutex);
}

static int vcorefs_enable_vcore(bool enable)
{

	struct governor_profile *gvrctrl = &governor_ctrl;

	mutex_lock(&governor_mutex);

	gvrctrl->vcore_dvs = enable;

	if (!gvrctrl->vcore_dvs && !gvrctrl->ddr_dfs) {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS, 0,
					gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	} else if (!gvrctrl->vcore_dvs && gvrctrl->ddr_dfs) {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DVS, 0,
					gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	} else if (gvrctrl->vcore_dvs && !gvrctrl->ddr_dfs) {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DFS, 0,
					gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	} else {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO, 0, gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	}

	mutex_unlock(&governor_mutex);
	return 0;
}

static int vcorefs_enable_ddr(bool enable)
{
	struct governor_profile *gvrctrl = &governor_ctrl;

	mutex_lock(&governor_mutex);

	gvrctrl->ddr_dfs = enable;

	if (!gvrctrl->vcore_dvs && !gvrctrl->ddr_dfs) {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS, 0,
					gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	} else if (gvrctrl->vcore_dvs && !gvrctrl->ddr_dfs) {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DFS, 0,
					gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	} else if (!gvrctrl->vcore_dvs && gvrctrl->ddr_dfs) {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DVS, 0,
					gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	} else {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO, 0, gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	}

	mutex_unlock(&governor_mutex);
	return 0;
}

static void vcorefs_reload_spm_firmware(int flag)
{

	struct governor_profile *gvrctrl = &governor_ctrl;

	mutex_lock(&governor_mutex);

	if ((flag & SPM_FLAG_DIS_VCORE_DVS) != 0)
		gvrctrl->vcore_dvs = 0;
	else
		gvrctrl->vcore_dvs = 1;

	if ((flag & SPM_FLAG_DIS_VCORE_DFS) != 0)
		gvrctrl->ddr_dfs = 0;
	else
		gvrctrl->ddr_dfs = 1;

	vcorefs_crit("re-kick vcore dvfs FW (vcore_dvs: %d ddr_dfs: %d)\n", gvrctrl->vcore_dvs, gvrctrl->ddr_dfs);
	spm_go_to_vcore_dvfs(flag, 0, gvrctrl->screen_on, gvrctrl->md_dvfs_req);

	mutex_unlock(&governor_mutex);
}

int governor_debug_store(const char *buf)
{
	struct governor_profile *gvrctrl = &governor_ctrl;
	int val, r = 0;
	char cmd[32];

	if (sscanf(buf, "%31s 0x%x", cmd, &val) == 2 ||
		   sscanf(buf, "%31s %d", cmd, &val) == 2) {

		if (!strcmp(cmd, "plat_feature_en")) {
			gvrctrl->plat_feature_en = val;
			vcorefs_late_init_dvfs();
		} else if (!strcmp(cmd, "isr_debug"))
			vcorefs_enable_debug_isr(val);
		else if (!strcmp(cmd, "md_dvfs_req"))
			vcorefs_set_cpu_dvfs_req(val);
		else if (!strcmp(cmd, "vcore_dvs"))
			vcorefs_enable_vcore(val);
		else if (!strcmp(cmd, "ddr_dfs"))
			vcorefs_enable_ddr(val);
		else if (!strcmp(cmd, "freq_dfs"))
			gvrctrl->freq_dfs = val;
		else if (!strcmp(cmd, "load_spm"))
			vcorefs_reload_spm_firmware(val);
		else if (!strcmp(cmd, "dvfs_cnt"))
			clean_dvfs_counter(val);
		else if (!strcmp(cmd, "screen")) {
			if (!is_vcorefs_feature_enable() || !gvrctrl->plat_init_done)
				return 0;

			if (val == 1) {
				mutex_lock(&governor_mutex);
				spm_vcorefs_screen_on_setting();
				gvrctrl->screen_on = 1;
				mutex_unlock(&governor_mutex);
			} else if (val == 0) {
				mutex_lock(&governor_mutex);
				spm_vcorefs_screen_off_setting(gvrctrl->md_dvfs_req);
				gvrctrl->screen_on = 0;
				mutex_unlock(&governor_mutex);
			}
		} else
			r = -EPERM;
	} else {
		r = -EPERM;
	}

	return r;
}

char *governor_get_dvfs_info(char *p)
{
	int i;

	struct governor_profile *gvrctrl = &governor_ctrl;
	int uv = vcorefs_get_curr_vcore();

	p += sprintf(p, "curr_opp: %d\n", vcorefs_curr_opp);
	p += sprintf(p, "prev_opp: %d\n", vcorefs_prev_opp);
	p += sprintf(p, "\n");

	p += sprintf(p, "[plat_feature_en]: %d\n", is_vcorefs_feature_enable());
	p += sprintf(p, "[vcore_dvs   ]: %d\n", gvrctrl->vcore_dvs);
	p += sprintf(p, "[ddr_dfs     ]: %d\n", gvrctrl->ddr_dfs);
	p += sprintf(p, "[freq_dfs    ]: %d\n", gvrctrl->freq_dfs);
	p += sprintf(p, "[isr_debug   ]: %d\n", gvrctrl->isr_debug);
	p += sprintf(p, "[screen_on   ]: %d\n", gvrctrl->screen_on);
	p += sprintf(p, "[md_dvfs_req ]: 0x%x\n", gvrctrl->md_dvfs_req);
	p += sprintf(p, "\n");

	p += sprintf(p, "[vcore] uv : %u (0x%x)\n", uv, vcore_uv_to_pmic(uv));
	p += sprintf(p, "[ddr  ] khz: %u\n", vcorefs_get_curr_ddr());
	p += sprintf(p, "[axi  ] khz: %u\n", ckgen_meter(1));
	p += sprintf(p, "\n");

	p += sprintf(p, "[perf_bw_en]: %d\n", gvrctrl->perform_bw_enable);
	p += sprintf(p, "[LPM_thres ]: 0x%x\n", gvrctrl->perform_bw_lpm_threshold);
	p += sprintf(p, "[HPM_thres ]: 0x%x\n", gvrctrl->perform_bw_hpm_threshold);
	p += sprintf(p, "\n");

	p += sprintf(p, "[dvfs_timer]: %u\n", gvrctrl->dvfs_timer);
	p += sprintf(p, "[dvfs_cnt  ]: ");
	for (i = 0; i < NUM_RANGE; i++)
		p += sprintf(p, "[%d] %lu ", vcorefs_range[i], vcorefs_cnt[i]);
	p += sprintf(p, "\n");

	return p;
}

/*
 * EMIBW
 */
int vcorefs_enable_perform_bw(bool enable)
{
	struct governor_profile *gvrctrl = &governor_ctrl;

	mutex_lock(&governor_mutex);
	gvrctrl->perform_bw_enable = enable;
	spm_vcorefs_enable_perform_bw(enable);
	mutex_unlock(&governor_mutex);

	return 0;
}

int vcorefs_set_perform_bw_threshold(u32 lpm_threshold, u32 hpm_threshold)
{
	struct governor_profile *gvrctrl = &governor_ctrl;

	if (((hpm_threshold & (~0x7F)) != 0) || ((lpm_threshold & (~0x7F)) != 0)) {
		vcorefs_err("perform BW threshold out-of-range, LPM: %u, HPM: %u\n", lpm_threshold, hpm_threshold);
		return -1;
	}

	gvrctrl->perform_bw_lpm_threshold = lpm_threshold;
	gvrctrl->perform_bw_hpm_threshold = hpm_threshold;

	mutex_lock(&governor_mutex);
	spm_vcorefs_set_perform_bw_threshold(lpm_threshold, hpm_threshold);
	mutex_unlock(&governor_mutex);

	return 0;
}

/*
 * Sub-main function
 */
static int set_dvfs_with_opp(struct kicker_config *krconf)
{
	struct governor_profile *gvrctrl = &governor_ctrl;
	struct opp_profile *opp_ctrl_table = opp_table;
	int timer = 0;

	gvrctrl->curr_vcore_uv = vcorefs_get_curr_vcore();
	gvrctrl->curr_ddr_khz = vcorefs_get_curr_ddr();

	vcorefs_crit("opp: %d, vcore: %u(%u), fddr: %u(%u), screen_on: %u %s\n",
		     krconf->dvfs_opp,
		     opp_ctrl_table[krconf->dvfs_opp].vcore_uv, gvrctrl->curr_vcore_uv,
		     opp_ctrl_table[krconf->dvfs_opp].ddr_khz, gvrctrl->curr_ddr_khz,
		     gvrctrl->screen_on,
		     (gvrctrl->vcore_dvs || gvrctrl->ddr_dfs) ? "" : "[X]");

	if (!gvrctrl->vcore_dvs && !gvrctrl->ddr_dfs)
		return 0;

	timer = spm_set_vcore_dvfs(krconf->dvfs_opp, gvrctrl->screen_on);

	if (timer < 0) {
		vcorefs_err("FAILED: SET VCORE DVFS FAIL\n");
		return -EBUSY;
	}

	record_dvfs_timer(timer);
	gvrctrl->curr_vcore_uv = opp_ctrl_table[krconf->dvfs_opp].vcore_uv;
	gvrctrl->curr_ddr_khz = opp_ctrl_table[krconf->dvfs_opp].ddr_khz;

	return 0;
}

static int set_freq_with_opp(struct kicker_config *krconf)
{
	struct governor_profile *gvrctrl = &governor_ctrl;
	struct opp_profile *opp_ctrl_table = opp_table;
	int r = 0;

	gvrctrl->curr_axi_khz = ckgen_meter(1);

	vcorefs_crit("opp: %d, faxi: %u(%u), screen_on: %u %s\n",
		     krconf->dvfs_opp,
		     opp_ctrl_table[krconf->dvfs_opp].axi_khz, gvrctrl->curr_axi_khz,
		     gvrctrl->screen_on,
		     gvrctrl->freq_dfs ? "" : "[X]");

	if (!gvrctrl->freq_dfs || !gvrctrl->screen_on)
		return 0;

#if CCF_CONFIG
	r = clk_prepare_enable(clk_axi_sel);

	if (r) {
		vcorefs_err("*** FAILED: CLOCK PREPARE ENABLE ***\n");
		BUG_ON(r);
	}

	switch (krconf->dvfs_opp) {
	case OPP_0:
		clk_set_parent(clk_axi_sel, clk_syspll_d7);		/* 158MHz */
		break;
	case OPP_1:
		clk_set_parent(clk_axi_sel, clk_mux_ulposc_axi_ck_mux);	/* 138MHz */
		break;
	default:
		vcorefs_err("*** FAILED: OPP INDEX IS INCORRECT ***\n");
		return r;
	}

	clk_disable_unprepare(clk_axi_sel);
#endif

	gvrctrl->curr_axi_khz = opp_ctrl_table[krconf->dvfs_opp].axi_khz;

	return r;
}

static int do_dvfs_for_performance(struct kicker_config *krconf)
{
	int r;

	/* for performance: scale UP voltage -> frequency */

	r = set_dvfs_with_opp(krconf);
	if (r)
		return -2;

	r = set_freq_with_opp(krconf);
	if (r)
		return -3;

	return 0;
}

static int do_dvfs_for_low_power(struct kicker_config *krconf)
{
	int r;

	/* for low power: scale DOWN frequency -> voltage */

	r = set_freq_with_opp(krconf);
	if (r)
		return -3;

	r = set_dvfs_with_opp(krconf);
	if (r)
		return -2;

	return 0;
}

/*
 * Main function API
 */
int kick_dvfs_by_opp_index(struct kicker_config *krconf)
{
	int r = 0;

	if (krconf->dvfs_opp == OPP_0)
		r = do_dvfs_for_performance(krconf);
	else
		r = do_dvfs_for_low_power(krconf);

	if (r == 0) {
		vcorefs_prev_opp = krconf->dvfs_opp;
		vcorefs_curr_opp = krconf->dvfs_opp;
	} else if (r < 0) {
		vcorefs_err("Vcore DVFS Fail, r: %d\n", r);

		vcorefs_prev_opp = vcorefs_curr_opp;
		vcorefs_curr_opp = krconf->dvfs_opp;
	} else {
		vcorefs_err("unknown error handling, r: %d\n", r);
		BUG();
	}

	vcorefs_crit("kick_dvfs_by_opp_index done, r: %d\n", r);

	return r;
}

static int vcorefs_fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct governor_profile *gvrctrl = &governor_ctrl;
	struct kicker_config krconf;
	struct fb_event *evdata = data;
	int blank;

	if (!is_vcorefs_feature_enable() || !gvrctrl->plat_init_done)
		return 0;

	if (event != FB_EVENT_BLANK)
		return 0;

	blank = *(int *)evdata->data;

	switch (blank) {
	case FB_BLANK_UNBLANK:
		mutex_lock(&governor_mutex);
		spm_vcorefs_screen_on_setting();
		gvrctrl->screen_on = 1;

		if (vcorefs_get_curr_opp() == OPPI_PERF) {
			krconf.dvfs_opp = OPP_0;
			set_freq_with_opp(&krconf);
		}

		mutex_unlock(&governor_mutex);
		break;
	case FB_BLANK_POWERDOWN:
		mutex_lock(&governor_mutex);
		spm_vcorefs_screen_off_setting(gvrctrl->md_dvfs_req);

		if (vcorefs_get_curr_opp() == OPPI_PERF) {
			krconf.dvfs_opp = OPP_1;
			set_freq_with_opp(&krconf);
		}

		gvrctrl->screen_on = 0;
		mutex_unlock(&governor_mutex);
		break;
	default:
		break;
	}

	return 0;
}

static struct notifier_block vcorefs_fb_notif = {
		.notifier_call = vcorefs_fb_notifier_callback,
};

/*
 * AutoK related API
 */
void governor_autok_manager(void)
{
	int r;

	#if (AUTOK_EMMC == 1)
	r = emmc_autok();
	vcorefs_crit("EMMC autok done: %s\n", (r == 0) ? "Yes" : "No");
	#endif

	#if (AUTOK_SD == 1)
	r = sd_autok();
	vcorefs_crit("SD autok done: %s\n", (r == 0) ? "Yes" : "No");
	#endif

	#if (AUTOK_SDIO == 1)
	r = sdio_autok();
	vcorefs_crit("SDIO autok done: %s\n", (r == 0) ? "Yes" : "No");
	#endif
}

bool governor_autok_check(int kicker)
{
	int is_autok = true;
	struct governor_profile *gvrctrl = &governor_ctrl;

	mutex_lock(&governor_mutex);
	if (!((1 << kicker) & gvrctrl->autok_kir_group)) {
		is_autok = false;
	} else if (gvrctrl->active_autok_kir != 0 && gvrctrl->active_autok_kir != kicker) {
		vcorefs_err("Not allow kir:%d autok ( other kir:%d on-going)\n", kicker, gvrctrl->active_autok_kir);
		is_autok = false;
	} else {
		is_autok = true;
	}
	mutex_unlock(&governor_mutex);

	return is_autok;
}

bool governor_autok_lock_check(int kicker, int opp)
{
	bool lock_r = true;
	struct governor_profile *gvrctrl = &governor_ctrl;

	mutex_lock(&governor_mutex);

	if (gvrctrl->active_autok_kir == 0) {
		gvrctrl->active_autok_kir = kicker;	/* start autok */
		lock_r = true;
	} else if (kicker == gvrctrl->active_autok_kir) {
		lock_r = true;	/* continue autok */
	} else {
		BUG();
	}

	if (opp == OPPI_UNREQ) {
		gvrctrl->active_autok_kir = 0;
		lock_r = false;
	}

	mutex_unlock(&governor_mutex);
	return lock_r;
}

static int vcorefs_probe(struct platform_device *pdev);
static int vcorefs_remove(struct platform_device *pdev);

#ifdef CONFIG_OF
static const struct of_device_id alsps_of_match[] = {
	{ .compatible = "mediatek,mt6797-vcorefs" },
	{},
};
#endif

static struct platform_driver vcorefs_driver = {
	.probe      = vcorefs_probe,
	.remove     = vcorefs_remove,
	.driver     = {
		.name = "VCOREFS",
		#ifdef CONFIG_OF
		.of_match_table = alsps_of_match,
		#endif
	}
};

static int vcorefs_probe(struct platform_device *pdev)
{
#if CCF_CONFIG
	clk_axi_sel = devm_clk_get(&pdev->dev, "mux_axi");
	if (IS_ERR(clk_axi_sel)) {
		vcorefs_err("FAILED: CANNOT GET clk_axi_sel\n");
		/* BUG(); */
		return PTR_ERR(clk_axi_sel);
	}

	clk_syspll_d7 = devm_clk_get(&pdev->dev, "syspll_d7");
	if (IS_ERR(clk_syspll_d7)) {
		vcorefs_err("FAILED: CANNOT GET clk_syspll_d7\n");
		/* BUG(); */
		return PTR_ERR(clk_syspll_d7);
	}

	clk_mux_ulposc_axi_ck_mux = devm_clk_get(&pdev->dev, "mux_ulposc_axi_ck_mux");
	if (IS_ERR(clk_mux_ulposc_axi_ck_mux)) {
		vcorefs_err("FAILED: CANNOT GET clk_mux_ulposc_axi_ck_mux\n");
		/* BUG(); */
		return PTR_ERR(clk_mux_ulposc_axi_ck_mux);
	}
#endif
	return 0;
}

static int vcorefs_remove(struct platform_device *pdev)
{
	return 0;
}

/*
 * late_init is called after dyna_load_fw ready
 */
int vcorefs_late_init_dvfs(void)
{
	struct kicker_config krconf;
	struct governor_profile *gvrctrl = &governor_ctrl;

	if (is_vcorefs_feature_enable())
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO, 0, gvrctrl->screen_on, gvrctrl->md_dvfs_req);
/*	else {
		spm_go_to_vcore_dvfs(SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS, 0,
						gvrctrl->screen_on, gvrctrl->md_dvfs_req);
	}
*/
	mutex_lock(&governor_mutex);
	gvrctrl->late_init_opp = set_init_opp_index();

	if (is_vcorefs_feature_enable()) {
		krconf.kicker = KIR_LATE_INIT;
		krconf.opp = gvrctrl->late_init_opp;
		krconf.dvfs_opp = gvrctrl->late_init_opp;

		kick_dvfs_by_opp_index(&krconf);
	}

	vcorefs_curr_opp = gvrctrl->late_init_opp;
	vcorefs_prev_opp = gvrctrl->late_init_opp;
	gvrctrl->plat_init_done = 1;
	mutex_unlock(&governor_mutex);

	vcorefs_crit("[%s] plat_init_done: %d, plat_feature_en: %d, late_init_opp: %d(%d)(%d)\n", __func__,
				gvrctrl->plat_init_done, is_vcorefs_feature_enable(),
				gvrctrl->late_init_opp, vcorefs_curr_opp, vcorefs_prev_opp);

	vcorefs_drv_init(gvrctrl->plat_init_done, is_vcorefs_feature_enable(), gvrctrl->late_init_opp);

	return 0;
}

static int init_vcorefs_cmd_table(void)
{
	struct governor_profile *gvrctrl = &governor_ctrl;
	struct opp_profile *opp_ctrl_table = opp_table;
	int opp;

	mutex_lock(&governor_mutex);
	gvrctrl->curr_vcore_uv = vcorefs_get_curr_vcore();
	/* BUG_ON(gvrctrl->curr_vcore_uv == 0); */

	gvrctrl->curr_ddr_khz = vcorefs_get_curr_ddr();
	gvrctrl->curr_axi_khz = ckgen_meter(1);

	for (opp = 0; opp < NUM_OPP; opp++) {
		switch (opp) {
		case OPPI_PERF:
			opp_ctrl_table[opp].vcore_uv = vcorefs_get_vcore_by_steps(opp);
			opp_ctrl_table[opp].ddr_khz = vcorefs_get_ddr_by_steps(opp);
			opp_ctrl_table[opp].axi_khz = FAXI_S0_KHZ;
			break;
		case OPPI_LOW_PWR:
			opp_ctrl_table[opp].vcore_uv = vcorefs_get_vcore_by_steps(opp);
			opp_ctrl_table[opp].ddr_khz = vcorefs_get_ddr_by_steps(opp);
			opp_ctrl_table[opp].axi_khz = FAXI_S1_KHZ;
			break;
		default:
			break;
		}
		vcorefs_crit("opp %u: vcore_uv: %u, ddr_khz: %u, axi_khz: %u\n",
								opp,
								opp_ctrl_table[opp].vcore_uv,
								opp_ctrl_table[opp].ddr_khz,
								opp_ctrl_table[opp].axi_khz);
	}

	vcorefs_crit("curr_vcore_uv: %u, curr_ddr_khz: %u, curr_axi_khz: %u\n",
							gvrctrl->curr_vcore_uv,
							gvrctrl->curr_ddr_khz,
							gvrctrl->curr_axi_khz);

	update_vcore_pwrap_cmd(opp_ctrl_table);
	mutex_unlock(&governor_mutex);

	return 0;
}

static int __init vcorefs_module_init(void)
{
	int r;

	r = init_vcorefs_cmd_table();
	if (r) {
		vcorefs_err("FAILED TO INIT CONFIG (%d)\n", r);
		return r;
	}

	r = platform_driver_register(&vcorefs_driver);
	if (r) {
		vcorefs_err("FAILED TO REGISTER PLATFORM DRIVER (%d)\n", r);
		return r;
	}

	r = init_vcorefs_sysfs();
	if (r) {
		vcorefs_err("FAILED TO CREATE /sys/power/vcorefs (%d)\n", r);
		return r;
	}

	r = fb_register_client(&vcorefs_fb_notif);
	if (r) {
		vcorefs_err("FAILED TO REGISTER FB CLIENT (%d)\n", r);
		return r;
	}

	return r;
}

module_init(vcorefs_module_init);
