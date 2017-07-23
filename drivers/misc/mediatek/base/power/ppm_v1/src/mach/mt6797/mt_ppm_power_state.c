#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "mt_cpufreq.h"
#include "mt_ppm_platform.h"
#include "mt_ppm_internal.h"

/*==============================================================*/
/* Macros							*/
/*==============================================================*/
#define PWR_STATE_TBLS(name, type) \
	.cluster_limit = &cluster_limit_##name,	\
	.pwr_sorted_tbl = &pwr_sorted_tbl_##name##_##type,	\
	.perf_sorted_tbl = &perf_sorted_tbl_##name##_##type,	\
	.transfer_by_pwr = &transfer_by_pwr_##name,	\
	.transfer_by_perf = &transfer_by_perf_##name,

#define LIMIT(fmin, fmax, cmin, cmax) {	\
	.min_cpufreq_idx = fmin,	\
	.max_cpufreq_idx = fmax,	\
	.min_cpu_core = cmin,	\
	.max_cpu_core = cmax,	\
}

#define STATE_LIMIT(name)	\
const struct ppm_state_cluster_limit_data cluster_limit_##name = {	\
	.state_limit = state_limit_##name,		\
	.size = ARRAY_SIZE(state_limit_##name),	\
}

#define SORT_TBL_ELEMENT(idx, val, advise) {	\
	.index = idx,	\
	.value = val,	\
	.advise_index = advise,	\
}

#define STATE_PWR_TBL(name, type)	\
const struct ppm_state_sorted_pwr_tbl_data pwr_sorted_tbl_##name##_##type = {	\
	.sorted_tbl = state_pwr_tbl_##name##_##type,		\
	.size = ARRAY_SIZE(state_pwr_tbl_##name##_##type),	\
}

#define STATE_PERF_TBL(name, type)	\
const struct ppm_state_sorted_pwr_tbl_data perf_sorted_tbl_##name##_##type = {	\
	.sorted_tbl = state_perf_tbl_##name##_##type,		\
	.size = ARRAY_SIZE(state_perf_tbl_##name##_##type),	\
}

#define TRANS_DATA(state, mask, rule, delta, hold, bond, f_hold, tlp) {	\
	.next_state = PPM_POWER_STATE_##state,	\
	.mode_mask = mask,	\
	.transition_rule = rule,	\
	.loading_delta = delta,	\
	.loading_hold_time = hold,	\
	.loading_hold_cnt = 0,	\
	.loading_bond = bond,	\
	.freq_hold_time = f_hold,	\
	.freq_hold_cnt = 0,		\
	.tlp_bond = tlp,	\
}

#define STATE_TRANSFER_DATA_PWR(name)	\
struct ppm_state_transfer_data transfer_by_pwr_##name = {	\
	.transition_data = state_pwr_transfer_##name,		\
	.size = ARRAY_SIZE(state_pwr_transfer_##name),	\
}

#define STATE_TRANSFER_DATA_PERF(name)	\
struct ppm_state_transfer_data transfer_by_perf_##name = {	\
	.transition_data = state_perf_transfer_##name,		\
	.size = ARRAY_SIZE(state_perf_transfer_##name),	\
}

/*==============================================================*/
/* Local Functions						*/
/*==============================================================*/
static bool ppm_trans_rule_LL_ONLY_to_L_ONLY(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings);
static bool ppm_trans_rule_LL_ONLY_to_4LL_L(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings);
static bool ppm_trans_rule_L_ONLY_to_LL_ONLY(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings);
static bool ppm_trans_rule_L_ONLY_to_4L_LL(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings);
static bool ppm_trans_rule_4LL_L_to_LL_ONLY(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings);
static bool ppm_trans_rule_4L_LL_to_L_ONLY(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings);

/*==============================================================*/
/* Local Variables						*/
/*==============================================================*/
/* cluster limit for each power state */
static const struct ppm_cluster_limit state_limit_LL_ONLY[] = {
	[0] = LIMIT(15, 0, 1, 4),
	[1] = LIMIT(15, 0, 0, 0),
	[2] = LIMIT(15, 0, 0, 0),
};
STATE_LIMIT(LL_ONLY);

static const struct ppm_cluster_limit state_limit_L_ONLY[] = {
	[0] = LIMIT(15, 0, 0, 0),
	[1] = LIMIT(8, 0, 1, 4),
	[2] = LIMIT(15, 0, 0, 0),
};
STATE_LIMIT(L_ONLY);

static const struct ppm_cluster_limit state_limit_4LL_L[] = {
	[0] = LIMIT(15, 0, 1, 4),
	[1] = LIMIT(15, 0, 0, 4),
	[2] = LIMIT(15, 0, 0, 2),
};
STATE_LIMIT(4LL_L);

static const struct ppm_cluster_limit state_limit_4L_LL[] = {
	[0] = LIMIT(15, 0, 0, 4),
	[1] = LIMIT(15, 0, 1, 4),
	[2] = LIMIT(15, 0, 0, 2),
};
STATE_LIMIT(4L_LL);

/* state transfer data  by power/performance for each state */
static struct ppm_state_transfer state_pwr_transfer_LL_ONLY[] = {
	TRANS_DATA(NONE, 0, NULL, 0, 0, 0, 0, 0),
};
STATE_TRANSFER_DATA_PWR(LL_ONLY);

static struct ppm_state_transfer state_perf_transfer_LL_ONLY[] = {
	TRANS_DATA(
		4LL_L,
		PPM_MODE_MASK_ALL_MODE,
		ppm_trans_rule_LL_ONLY_to_4LL_L,
		PPM_DEFAULT_DELTA,
		PPM_DEFAULT_HOLD_TIME,
		PPM_LOADING_UPPER,
		0,
		PPM_TLP_CRITERIA
		),
	TRANS_DATA(
		L_ONLY,
		PPM_MODE_MASK_JUST_MAKE_ONLY | PPM_MODE_MASK_PERFORMANCE_ONLY,
		ppm_trans_rule_LL_ONLY_to_L_ONLY,
		PPM_DEFAULT_DELTA,
		PPM_DEFAULT_HOLD_TIME,
		PPM_LOADING_UPPER,
		/* PPM_DEFAULT_FREQ_HOLD_TIME, */
		8,
		PPM_TLP_CRITERIA
		),
};
STATE_TRANSFER_DATA_PERF(LL_ONLY);

static struct ppm_state_transfer state_pwr_transfer_L_ONLY[] = {
	TRANS_DATA(
		LL_ONLY,
		PPM_MODE_MASK_ALL_MODE,
		ppm_trans_rule_L_ONLY_to_LL_ONLY,
		0,
		0,
		0,
		PPM_DEFAULT_FREQ_HOLD_TIME,
		0
		),
};
STATE_TRANSFER_DATA_PWR(L_ONLY);

static struct ppm_state_transfer state_perf_transfer_L_ONLY[] = {
	TRANS_DATA(
		4L_LL,
		PPM_MODE_MASK_ALL_MODE,
		ppm_trans_rule_L_ONLY_to_4L_LL,
		PPM_DEFAULT_DELTA,
		PPM_DEFAULT_HOLD_TIME,
		PPM_LOADING_UPPER,
		0,
		0
		),
};
STATE_TRANSFER_DATA_PERF(L_ONLY);

static struct ppm_state_transfer state_pwr_transfer_4LL_L[] = {
	TRANS_DATA(
		LL_ONLY,
		PPM_MODE_MASK_ALL_MODE,
		ppm_trans_rule_4LL_L_to_LL_ONLY,
		PPM_DEFAULT_DELTA,
		PPM_DEFAULT_HOLD_TIME,
		PPM_LOADING_UPPER,
		0,
		0
		),
};
STATE_TRANSFER_DATA_PWR(4LL_L);

static struct ppm_state_transfer state_perf_transfer_4LL_L[] = {
	TRANS_DATA(NONE, 0, NULL, 0, 0, 0, 0, 0),
};
STATE_TRANSFER_DATA_PERF(4LL_L);

static struct ppm_state_transfer state_pwr_transfer_4L_LL[] = {
	TRANS_DATA(
		L_ONLY,
		PPM_MODE_MASK_ALL_MODE,
		ppm_trans_rule_4L_LL_to_L_ONLY,
		PPM_DEFAULT_DELTA,
		PPM_DEFAULT_HOLD_TIME,
		PPM_LOADING_UPPER,
		0,
		0
		),
};
STATE_TRANSFER_DATA_PWR(4L_LL);

static struct ppm_state_transfer state_perf_transfer_4L_LL[] = {
	TRANS_DATA(NONE, 0, NULL, 0, 0, 0, 0, 0),
};
STATE_TRANSFER_DATA_PERF(4L_LL);

/*==============================================================*/
/* Power Table auto-generated by script begin			*/
/* Note: DO NOT modify it manually!!				*/
/*==============================================================*/
#include "mt_ppm_power_table.h"
/*==============================================================*/
/* Power Table auto-generated by script	end			*/
/*==============================================================*/

/* default use FY table, may change to SB if needed */
const struct ppm_power_tbl_data power_table_FY = {
	.power_tbl = cpu_tlp_power_tbl_FY,
	.nr_power_tbl = ARRAY_SIZE(cpu_tlp_power_tbl_FY),
};

const struct ppm_power_tbl_data power_table_SB = {
	.power_tbl = cpu_tlp_power_tbl_SB,
	.nr_power_tbl = ARRAY_SIZE(cpu_tlp_power_tbl_SB),
};

/* PPM power state static data */
struct ppm_power_state_data pwr_state_info_FY[NR_PPM_POWER_STATE] = {
	[0] = {
		.name = __stringify(LL_ONLY),
		.state = PPM_POWER_STATE_LL_ONLY,
		PWR_STATE_TBLS(LL_ONLY, FY)
	},
	[1] = {
		.name = __stringify(L_ONLY),
		.state = PPM_POWER_STATE_L_ONLY,
		PWR_STATE_TBLS(L_ONLY, FY)
	},
	[2] = {
		.name = __stringify(4LL_L),
		.state = PPM_POWER_STATE_4LL_L,
		PWR_STATE_TBLS(4LL_L, FY)
	},
	[3] = {
		.name = __stringify(4L_LL),
		.state = PPM_POWER_STATE_4L_LL,
		PWR_STATE_TBLS(4L_LL, FY)
	},
};

struct ppm_power_state_data pwr_state_info_SB[NR_PPM_POWER_STATE] = {
	[0] = {
		.name = __stringify(LL_ONLY),
		.state = PPM_POWER_STATE_LL_ONLY,
		PWR_STATE_TBLS(LL_ONLY, SB)
	},
	[1] = {
		.name = __stringify(L_ONLY),
		.state = PPM_POWER_STATE_L_ONLY,
		PWR_STATE_TBLS(L_ONLY, SB)
	},
	[2] = {
		.name = __stringify(4LL_L),
		.state = PPM_POWER_STATE_4LL_L,
		PWR_STATE_TBLS(4LL_L, SB)
	},
	[3] = {
		.name = __stringify(4L_LL),
		.state = PPM_POWER_STATE_4L_LL,
		PWR_STATE_TBLS(4L_LL, SB)
	},
};

const unsigned int pwr_idx_search_prio[NR_PPM_POWER_STATE][NR_PPM_POWER_STATE] = {
	[PPM_POWER_STATE_LL_ONLY] = {PPM_POWER_STATE_NONE,},
	[PPM_POWER_STATE_L_ONLY] = {PPM_POWER_STATE_LL_ONLY, PPM_POWER_STATE_NONE,},
	[PPM_POWER_STATE_4LL_L] = {PPM_POWER_STATE_LL_ONLY, PPM_POWER_STATE_NONE,},
	[PPM_POWER_STATE_4L_LL] = {PPM_POWER_STATE_L_ONLY, PPM_POWER_STATE_LL_ONLY, PPM_POWER_STATE_NONE,},
};

const unsigned int perf_idx_search_prio[NR_PPM_POWER_STATE][NR_PPM_POWER_STATE] = {
	[PPM_POWER_STATE_LL_ONLY] = {PPM_POWER_STATE_L_ONLY, PPM_POWER_STATE_4LL_L, PPM_POWER_STATE_NONE,},
	[PPM_POWER_STATE_L_ONLY] = {PPM_POWER_STATE_4L_LL, PPM_POWER_STATE_NONE,},
	[PPM_POWER_STATE_4LL_L] = {PPM_POWER_STATE_NONE,},
	[PPM_POWER_STATE_4L_LL] = {PPM_POWER_STATE_NONE,},
};

/*==============================================================*/
/* Local Function Implementation				*/
/*==============================================================*/
/* transition rules */
static bool ppm_trans_rule_LL_ONLY_to_L_ONLY(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings)
{
	unsigned int cur_freq_LL;
	unsigned int heavy_task_LL;

	/* keep in LL_ONLY state if root cluster is fixed at cluster 0 */
	if (ppm_main_info.fixed_root_cluster == PPM_CLUSTER_LL)
		return false;

	/* check heavy task (waiting for scheduler's API) */
#if 0
	heavy_task_LL = sched_get_nr_heavy_task(PPM_CLUSTER_LL);
#else
	heavy_task_LL = 0;
#endif
	if (heavy_task_LL && data.ppm_cur_tlp <= settings->tlp_bond)
		return true;

	/* check loading */
	if (data.ppm_cur_loads > (settings->loading_bond - settings->loading_delta)
		&& data.ppm_cur_tlp <= settings->tlp_bond) {
		settings->loading_hold_cnt++;
		if (settings->loading_hold_cnt >= settings->loading_hold_time)
			return true;
	} else
		settings->loading_hold_cnt = 0;

	/* check freq */
	cur_freq_LL = mt_cpufreq_get_cur_phy_freq(MT_CPU_DVFS_LL);	/* FIXME */
	if (ppm_hica_is_log_enabled())
		ppm_info("LL cur freq = %d\n", cur_freq_LL);

	if (cur_freq_LL >= get_cluster_max_cpufreq(PPM_CLUSTER_LL)) {
		settings->freq_hold_cnt++;
		if (settings->freq_hold_cnt >= settings->freq_hold_time)
			return true;
	} else
		settings->freq_hold_cnt = 0;

	return false;
}

static bool ppm_trans_rule_LL_ONLY_to_4LL_L(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings)
{
	/* check heavy task (waiting for scheduler's API) */
#if 0
	unsigned int heavy_task_LL = sched_get_nr_heavy_task(PPM_CLUSTER_LL);
#else
	unsigned int heavy_task_LL = 0;
#endif

	if (heavy_task_LL && data.ppm_cur_tlp > settings->tlp_bond)
		return true;

	/* check loading only */
	if (data.ppm_cur_loads > (settings->loading_bond - settings->loading_delta)
		&& data.ppm_cur_tlp > settings->tlp_bond) {
		settings->loading_hold_cnt++;
		if (settings->loading_hold_cnt >= settings->loading_hold_time)
			return true;
	} else
		settings->loading_hold_cnt = 0;

	return false;
}

static bool ppm_trans_rule_L_ONLY_to_LL_ONLY(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings)
{
	unsigned int cur_freq_L;
	unsigned int heavy_task_L;

	/* keep in L_ONLY state if root cluster is fixed at L */
	if (ppm_main_info.fixed_root_cluster == PPM_CLUSTER_L)
		return false;

	/* check heavy task (waiting for scheduler's API) */
#if 0
	heavy_task_L = sched_get_nr_heavy_task(PPM_CLUSTER_L);
#else
	heavy_task_L = 0;
#endif
	/* stay if L has heavy task, should be transferred to 4L+LL */
	if (heavy_task_L)
		return false;

	cur_freq_L = mt_cpufreq_get_cur_phy_freq(MT_CPU_DVFS_L); /* FIXME */
	if (ppm_hica_is_log_enabled())
		ppm_info("L cur freq = %d\n", cur_freq_L);

	if (cur_freq_L < get_cluster_max_cpufreq(PPM_CLUSTER_LL)) {
		settings->freq_hold_cnt++;
		if (settings->freq_hold_cnt >= settings->freq_hold_time)
			return true;
	} else
		settings->freq_hold_cnt = 0;

	return false;
}

static bool ppm_trans_rule_L_ONLY_to_4L_LL(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings)
{
	/* check heavy task (waiting for scheduler's API) */
#if 0
	unsigned int heavy_task_L = sched_get_nr_heavy_task(PPM_CLUSTER_L);
#else
	unsigned int heavy_task_L = 0;
#endif

	if (heavy_task_L)
		return true;

	/* check loading */
	if (data.ppm_cur_loads > (settings->loading_bond - settings->loading_delta)) {
		settings->loading_hold_cnt++;
		if (settings->loading_hold_cnt >= settings->loading_hold_time)
			return true;
	} else
		settings->loading_hold_cnt = 0;

	return false;
}

static bool ppm_trans_rule_4LL_L_to_LL_ONLY(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings)
{
	int i;

	/* stay if LL/L/B has heavy task */
	for_each_ppm_clusters(i) {
#if 0
		if (sched_get_nr_heavy_task(i))
#else
		if (0)
#endif
			return false;
	}

	/* check loading */
	if (data.ppm_cur_loads <= (settings->loading_bond - settings->loading_delta)) {
		settings->loading_hold_cnt++;
		if (settings->loading_hold_cnt >= settings->loading_hold_time)
			return true;
	} else
		settings->loading_hold_cnt = 0;

	return false;
}

static bool ppm_trans_rule_4L_LL_to_L_ONLY(
	struct ppm_hica_algo_data data, struct ppm_state_transfer *settings)
{
	int i;

	/* stay if LL/L/B has heavy task */
	for_each_ppm_clusters(i) {
#if 0
		if (sched_get_nr_heavy_task(i))
#else
		if (0)
#endif
			return false;
	}

	/* check loading */
	if (data.ppm_cur_loads <= (settings->loading_bond - settings->loading_delta)) {
		settings->loading_hold_cnt++;
		if (settings->loading_hold_cnt >= settings->loading_hold_time)
			return true;
	} else
		settings->loading_hold_cnt = 0;

	return false;
}


/*==============================================================*/
/* Global Function Implementation				*/
/*==============================================================*/
struct ppm_power_state_data *ppm_get_power_state_info(void)
{
	return (ppm_main_info.dvfs_tbl_type == DVFS_TABLE_TYPE_FY)
		? pwr_state_info_FY : pwr_state_info_SB;
}

const struct ppm_power_tbl_data ppm_get_power_table(void)
{
	return (ppm_main_info.dvfs_tbl_type == DVFS_TABLE_TYPE_FY)
		? power_table_FY : power_table_SB;
}

const char *ppm_get_power_state_name(enum ppm_power_state state)
{
	if (state >= NR_PPM_POWER_STATE)
		return "NONE";

	/* the state name is the same between FY and SB */
	return pwr_state_info_FY[state].name;
}

enum ppm_power_state ppm_change_state_with_fix_root_cluster(enum ppm_power_state cur_state, int cluster)
{
	enum ppm_power_state new_state = cur_state;

	switch (cluster) {
	case PPM_CLUSTER_LL:
		if (cur_state == PPM_POWER_STATE_L_ONLY)
			new_state = PPM_POWER_STATE_LL_ONLY;
		else if (cur_state == PPM_POWER_STATE_4L_LL)
			new_state = PPM_POWER_STATE_4LL_L;
		break;
	case PPM_CLUSTER_L:
		if (cur_state == PPM_POWER_STATE_LL_ONLY)
			new_state = PPM_POWER_STATE_L_ONLY;
		else if (cur_state == PPM_POWER_STATE_4LL_L)
			new_state = PPM_POWER_STATE_4L_LL;
		break;
	/* We do not support to fix root cluster at B */
	case PPM_CLUSTER_B:
	default:
		break;
	}

	return new_state;
}

enum ppm_power_state ppm_find_next_state(enum ppm_power_state state,
			unsigned int *level, enum power_state_search_policy policy)
{
	const unsigned int *tbl;
	enum ppm_power_state new_state;
	int keep_going;

	ppm_ver("@%s: state = %s, lv = %d\n", __func__, ppm_get_power_state_name(state), *level);

	if (state >= NR_PPM_POWER_STATE || *level >= NR_PPM_POWER_STATE)
		return PPM_POWER_STATE_NONE;

	tbl = (policy == PERFORMANCE) ? perf_idx_search_prio[state] : pwr_idx_search_prio[state];

	do {
		keep_going = 0;

		new_state = tbl[*level];

		ppm_ver("@%s: new_state = %s, lv = %d\n", __func__, ppm_get_power_state_name(new_state), *level);

		if (new_state == PPM_POWER_STATE_NONE)
			break;

		/* check fix root cluster setting */
		switch (ppm_main_info.fixed_root_cluster) {
		case PPM_CLUSTER_LL:
			if (new_state == PPM_POWER_STATE_L_ONLY || new_state == PPM_POWER_STATE_4L_LL) {
				(*level)++;
				keep_going = 1;
			}
			break;
		case PPM_CLUSTER_L:
			if (new_state == PPM_POWER_STATE_LL_ONLY || new_state == PPM_POWER_STATE_4LL_L) {
				(*level)++;
				keep_going = 1;
			}
			break;
		default:
			break;
		}
	} while (keep_going);

	return new_state;
}

int ppm_find_pwr_idx(struct ppm_cluster_status *cluster_status)
{
	int i, j;
	struct ppm_power_tbl_data power_table = ppm_get_power_table();
	int core[NR_PPM_CLUSTERS];
	int opp[NR_PPM_CLUSTERS];
	char buf[128];
	char *ptr = buf;

	/* copy core/opp info */
	for_each_ppm_clusters(i) {
		core[i] = cluster_status[i].core_num;
		opp[i] = (!core[i]) ? -1 : cluster_status[i].freq_idx;

		ptr += sprintf(ptr, "(%d)(%d)(%d) ", opp[i], core[i], cluster_status[i].volt);
	}

	ppm_ver("@%s: %s\n", __func__, buf);

	/* set LL and L to max core/freq if big core is on */
	if (core[PPM_CLUSTER_B] > 0) {
		core[PPM_CLUSTER_LL] = core[PPM_CLUSTER_L] = 4;
		opp[PPM_CLUSTER_LL] = opp[PPM_CLUSTER_L] = 0;
	} else {
		/* sync opp to little one due to shared bulk */
		if (opp[PPM_CLUSTER_LL] != -1 && opp[PPM_CLUSTER_L] != -1)
			opp[PPM_CLUSTER_LL] = opp[PPM_CLUSTER_L] = MIN(opp[PPM_CLUSTER_LL], opp[PPM_CLUSTER_L]);

		/* modify L's core if current state is 4LL+L or 4L+LL */
		if (ppm_main_info.cur_power_state == PPM_POWER_STATE_4LL_L)
			opp[PPM_CLUSTER_LL] = 4;
		else if (ppm_main_info.cur_power_state == PPM_POWER_STATE_4L_LL)
			opp[PPM_CLUSTER_L] = 4;
	}

	for_each_pwr_tbl_entry(i, power_table) {
		for_each_ppm_clusters(j) {
			if (power_table.power_tbl[i].cluster_cfg[j].core_num != core[j]
				|| power_table.power_tbl[i].cluster_cfg[j].opp_lv != opp[j])
				break;
		}

		if (j == NR_PPM_CLUSTERS) {
			ppm_ver("[index][power] = [%d][%d]\n",
					i, power_table.power_tbl[i].power_idx);
			return power_table.power_tbl[i].power_idx;
		}
	}

	ppm_ver("@%s: power_idx not found!\n", __func__);

	/* return -1 if not found */
	return -1;
}

enum ppm_power_state ppm_judge_state_by_user_limit(enum ppm_power_state cur_state,
			struct ppm_userlimit_data user_limit)
{
	enum ppm_power_state new_state = PPM_POWER_STATE_NONE;
	int LL_core_min = user_limit.limit[PPM_CLUSTER_LL].min_core_num;
	int LL_core_max = user_limit.limit[PPM_CLUSTER_LL].max_core_num;
	int L_core_min = user_limit.limit[PPM_CLUSTER_L].min_core_num;
	int L_core_max = user_limit.limit[PPM_CLUSTER_L].max_core_num;
	int LL_freq_min = user_limit.limit[PPM_CLUSTER_LL].min_freq_idx;
	int L_freq_max = user_limit.limit[PPM_CLUSTER_L].max_freq_idx;
	int sum = LL_core_min + L_core_min;
	int B_core_min = user_limit.limit[PPM_CLUSTER_B].min_core_num;
	int B_core_max = user_limit.limit[PPM_CLUSTER_B].max_core_num;
	int root_cluster = ppm_main_info.fixed_root_cluster;

	ppm_ver("Judge: input --> [%s] (%d)(%d)(%d)(%d)(%d)(%d) [(%d)(%d)]\n",
		ppm_get_power_state_name(cur_state), LL_core_min, LL_core_max,
		 L_core_min, L_core_max, B_core_min, B_core_max, LL_freq_min, L_freq_max);

	LL_core_max = (LL_core_max == -1) ? get_cluster_max_cpu_core(PPM_CLUSTER_LL) : LL_core_max;
	L_core_max = (L_core_max == -1) ? get_cluster_max_cpu_core(PPM_CLUSTER_L) : L_core_max;

	/* need to check freq limit for cluster move/merge */
	if (cur_state == PPM_POWER_STATE_LL_ONLY || cur_state == PPM_POWER_STATE_L_ONLY) {
		struct ppm_power_state_data *state_info = ppm_get_power_state_info();

		LL_freq_min = (LL_freq_min == -1)
			? state_info[cur_state].cluster_limit->state_limit[PPM_CLUSTER_LL].min_cpufreq_idx
			: LL_freq_min;
		L_freq_max = (L_freq_max == -1)
			? state_info[cur_state].cluster_limit->state_limit[PPM_CLUSTER_L].max_cpufreq_idx
			: L_freq_max;
		/* idx -> freq */
		LL_freq_min = ppm_main_info.cluster_info[PPM_CLUSTER_LL].dvfs_tbl[LL_freq_min].frequency;
		L_freq_max = ppm_main_info.cluster_info[PPM_CLUSTER_L].dvfs_tbl[L_freq_max].frequency;
	}

	/* min_core <= 0: don't care */
	/* min_core > 0: turn on this cluster */
	/* max_core == 0: turn off this cluster */
	switch (cur_state) {
	case PPM_POWER_STATE_LL_ONLY:
		/* not force Big core on */
		if (B_core_min <= 0) {
			new_state = (LL_core_max == 0) ? PPM_POWER_STATE_L_ONLY
				: (L_core_min <= 0 || L_core_max == 0) ? cur_state
				/* should not go to L only due to root cluster is fixed at LL */
				: (L_core_min > 0 && root_cluster == PPM_CLUSTER_LL) ? PPM_POWER_STATE_4LL_L
				: (LL_core_min <= 0) ? PPM_POWER_STATE_L_ONLY
				/* merge to L cluster */
				: (sum <= L_core_max && L_freq_max >= LL_freq_min) ? PPM_POWER_STATE_L_ONLY
				: PPM_POWER_STATE_4LL_L;
		} else {
			new_state = (LL_core_max == 0 && L_core_max == 0) ? PPM_POWER_STATE_NONE
				: (LL_core_max == 0 || (LL_core_min <= 0 && L_core_min > 0)) ? PPM_POWER_STATE_4L_LL
				: PPM_POWER_STATE_4LL_L;
		}
		break;
	case PPM_POWER_STATE_L_ONLY:
		/* not force Big core on */
		if (B_core_min <= 0) {
			new_state = (L_core_max == 0) ? PPM_POWER_STATE_LL_ONLY
				: (LL_core_min <= 0 || LL_core_max == 0) ? cur_state
				/* keep current if for only LL min is set */
				: (LL_core_min > 0 && L_core_min == -1 && L_freq_max >= LL_freq_min) ? cur_state
				/* merge to L cluster */
				: (sum <= L_core_max && L_freq_max >= LL_freq_min) ? cur_state
				: PPM_POWER_STATE_4L_LL;
		} else {
			new_state = (LL_core_max == 0 && L_core_max == 0) ? PPM_POWER_STATE_NONE
				: (L_core_max == 0) ? PPM_POWER_STATE_4LL_L
				: PPM_POWER_STATE_4L_LL;
		}
		break;
	case PPM_POWER_STATE_4LL_L:
		/* force Big core off */
		if (B_core_max == 0) {
			new_state = (L_core_max == 0) ? PPM_POWER_STATE_LL_ONLY
				: (LL_core_max == 0) ? PPM_POWER_STATE_L_ONLY
				: (LL_core_min <= 0 && L_core_min > 0) ? PPM_POWER_STATE_4L_LL
				: cur_state;
		} else {
			new_state = (LL_core_max == 0 && L_core_max == 0) ? PPM_POWER_STATE_NONE
				: (LL_core_max == 0 || (LL_core_min <= 0 && L_core_min > 0)) ? PPM_POWER_STATE_4L_LL
				: cur_state;
		}
		break;
	case PPM_POWER_STATE_4L_LL:
		/* force Big core off */
		if (B_core_max == 0) {
			new_state = (LL_core_max == 0) ? PPM_POWER_STATE_L_ONLY
				: (L_core_max == 0) ? PPM_POWER_STATE_LL_ONLY
				: cur_state;
		} else {
			new_state = (LL_core_max == 0 && L_core_max == 0) ? PPM_POWER_STATE_NONE
				: (L_core_max == 0) ? PPM_POWER_STATE_4LL_L
				: cur_state;
		}
		break;
	default:
		break;
	}

	/* check root cluster is fixed or not */
	switch (root_cluster) {
	case PPM_CLUSTER_LL:
		new_state = (new_state == PPM_POWER_STATE_L_ONLY) ? PPM_POWER_STATE_NONE
			: (new_state == PPM_POWER_STATE_4L_LL) ? PPM_POWER_STATE_4LL_L
			: new_state;
		break;
	case PPM_CLUSTER_L:
		new_state = (new_state == PPM_POWER_STATE_LL_ONLY) ? PPM_POWER_STATE_NONE
			: (new_state == PPM_POWER_STATE_4LL_L) ? PPM_POWER_STATE_4L_LL
			: new_state;
		break;
	default:
		break;
	}

	ppm_ver("Judge: output --> [%s]\n", ppm_get_power_state_name(new_state));

	return new_state;
}

/* modify request to fit cur_state */
void ppm_limit_check_for_user_limit(enum ppm_power_state cur_state, struct ppm_policy_req *req,
			struct ppm_userlimit_data user_limit)
{
	if (req && cur_state == PPM_POWER_STATE_L_ONLY) {
		unsigned int LL_min_core = req->limit[PPM_CLUSTER_LL].min_cpu_core;
		unsigned int L_min_core = req->limit[PPM_CLUSTER_L].min_cpu_core;
		unsigned int sum = LL_min_core + L_min_core;
		unsigned int LL_min_freq, L_min_freq, L_max_freq;

		LL_min_freq = ppm_main_info.cluster_info[PPM_CLUSTER_LL]
				.dvfs_tbl[req->limit[PPM_CLUSTER_LL].min_cpufreq_idx].frequency;
		L_min_freq = ppm_main_info.cluster_info[PPM_CLUSTER_L]
				.dvfs_tbl[req->limit[PPM_CLUSTER_L].min_cpufreq_idx].frequency;
		L_max_freq = ppm_main_info.cluster_info[PPM_CLUSTER_L]
				.dvfs_tbl[req->limit[PPM_CLUSTER_L].max_cpufreq_idx].frequency;

		if (LL_min_core > 0 && L_max_freq >= LL_min_freq) {
			/* user do not set L min so we just move LL core to L */
			if (user_limit.limit[PPM_CLUSTER_L].min_core_num <= 0) {
				req->limit[PPM_CLUSTER_LL].min_cpu_core = 0;
				req->limit[PPM_CLUSTER_L].min_cpu_core = LL_min_core;
				ppm_ver("Judge: move LL min core to L = %d\n", LL_min_core);
			} else if (sum <= get_cluster_max_cpu_core(PPM_CLUSTER_L)) {
				req->limit[PPM_CLUSTER_LL].min_cpu_core = 0;
				req->limit[PPM_CLUSTER_L].min_cpu_core = sum;
				ppm_ver("Judge: merge LL and L min core = %d\n", sum);
			} else {
				ppm_ver("Judge: cannot merge to L! LL min = %d, L min = %d\n",
					LL_min_core, L_min_core);
				/* check LL max core */
				if (req->limit[PPM_CLUSTER_LL].max_cpu_core < LL_min_core)
					req->limit[PPM_CLUSTER_LL].max_cpu_core = LL_min_core;

				return;
			}

			if (LL_min_freq > L_min_freq) {
				req->limit[PPM_CLUSTER_L].min_cpufreq_idx =
					ppm_main_freq_to_idx(PPM_CLUSTER_L, LL_min_freq, CPUFREQ_RELATION_L);
				ppm_ver("Judge: change L min freq idx to %d due to LL min freq = %d\n",
					req->limit[PPM_CLUSTER_L].min_cpufreq_idx, LL_min_freq);
			}
		}
	}
}

static unsigned int max_power;

/* return value is the remaining power budget for SW DLPT */
unsigned int ppm_set_ocp(unsigned int limited_power, unsigned int percentage)
{
	struct ppm_power_tbl_data power_table = ppm_get_power_table();
	int i, ret = 0;
	unsigned int remaining_power = 0;

	/* if max_power < limited_power, set (limited_power - max_power) to HW OCP */
	if (!max_power) {
		/* get max power budget for SW DLPT */
		for_each_pwr_tbl_entry(i, power_table) {
			if (power_table.power_tbl[i].cluster_cfg[PPM_CLUSTER_B].core_num == 0) {
				max_power = (power_table.power_tbl[i].power_idx);
				break;
			}
		}
		ppm_info("@%s: max_power = %d\n", __func__, max_power);
	}

	if (limited_power <= max_power) {
		/* disable HW OCP (waiting for API) */
		/* BigOCPDisable(ALL, OCP_mW); */
		return limited_power;
	}

	/* pass remaining power to HW OCP and re-enable it (waiting for API) */
	remaining_power = (percentage)
		? ((limited_power - max_power) * 100 + (percentage - 1)) / percentage
		: (limited_power - max_power);
#if 0
	BigOCPDisable();
	ret = BigOCPEnable(ALL, remaining_power, 0, 0);
#endif
	if (ret) {
		ppm_err("@%s: Enable OCP failed, ret = %d\n", __func__, ret);
		return limited_power;
	}

	ppm_ver("set budget = %d to OCP done!\n", remaining_power);

	return max_power;
}

