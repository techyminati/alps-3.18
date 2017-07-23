#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kallsyms.h>
#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <asm/uaccess.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/notifier.h>
#include <linux/suspend.h>

#include <linux/platform_device.h>

#include <mach/fliper.h>
#include <mt_vcorefs_governor.h>
#include <mt_vcorefs_manager.h>

#include <mach/mt_emi_bm.h>

#define SEQ_printf(m, x...)\
	do {\
		if (m)\
			seq_printf(m, x);\
		else\
			pr_emerg(x);\
	} while (0)
#undef TAG
#define TAG "[SOC DVFS FLIPER]"


static int cg_lpm_bw_threshold, cg_hpm_bw_threshold;
static int cg_fliper_enabled;
static int fliper_debug;

/******* FLIPER SETTING *********/

int getCGHistory(void)
{
	return BM_GetBWST1();
}

int getCGConfiguration(void)
{
	return BM_GetBW1();
}

int setCG(const unsigned int value)
{
	return BM_SetBW1(value);
}

void enable_cg_fliper(int enable)
{
	if (enable) {
		vcorefs_enable_perform_bw(true);
		pr_emerg(TAG"CG fliper enabled\n");
		cg_fliper_enabled = 1;
	} else {
		vcorefs_enable_perform_bw(false);
		pr_emerg(TAG"CG fliper disabled\n");
		cg_fliper_enabled = 0;
	}
}

int cg_set_threshold(int bw1, int bw2)
{
	int lpm_threshold, hpm_threshold;
	int ddr_perf = vcorefs_get_ddr_by_steps(OPPI_PERF);
	int ddr_curr = vcorefs_get_curr_ddr();

	if (bw1 <= BW_THRESHOLD_MAX && bw1 >= BW_THRESHOLD_MIN &&
			bw2 <= BW_THRESHOLD_MAX && bw2 >= BW_THRESHOLD_MIN) {
		lpm_threshold = bw1 * THRESHOLD_SCALE / LPM_MAX_BW + 1;
		hpm_threshold = bw2 * THRESHOLD_SCALE / HPM_MAX_BW;
		if (lpm_threshold > 127 || lpm_threshold < 1 || hpm_threshold > 127 || hpm_threshold < 1) {
			pr_emerg(TAG"error set threshold out of range\n");
			return 0;
		}
		if (ddr_perf == ddr_curr) {
			setCG(0x210080 | (hpm_threshold << 8));
			pr_emerg(TAG"ddr high, Configure CG: 0x%08x\n", getCGConfiguration());
		} else {
			setCG(0x210080 | (lpm_threshold << 8));
			pr_emerg(TAG"ddr low, Configure CG: 0x%08x\n", getCGConfiguration());
		}
		vcorefs_set_perform_bw_threshold(lpm_threshold, hpm_threshold);
		pr_emerg(TAG"Set CG bdw threshold %d %d -> %d %d\n",
				cg_lpm_bw_threshold, cg_hpm_bw_threshold, bw1, bw2);
		pr_emerg(TAG"CG lpm: %d hpm: %d\n", lpm_threshold, hpm_threshold);
		cg_lpm_bw_threshold = bw1;
		cg_hpm_bw_threshold = bw2;
	} else {
		pr_emerg(TAG"Set CG bdw threshold Error: (MAX:%d, MIN:%d)\n",
			BW_THRESHOLD_MAX, BW_THRESHOLD_MIN);
	}

	return 0;
}

int cg_restore_threshold(void)
{
	cg_set_threshold(CG_DEFAULT_LPM, CG_DEFAULT_HPM);
	pr_emerg(TAG"Restore CG bdw threshold %d %d -> %d %d\n",
		cg_lpm_bw_threshold, cg_hpm_bw_threshold, CG_DEFAULT_LPM, CG_DEFAULT_HPM);
	cg_lpm_bw_threshold = CG_DEFAULT_LPM;
	cg_hpm_bw_threshold = CG_DEFAULT_HPM;
	return 0;
}

static ssize_t mt_fliper_write(struct file *filp, const char *ubuf,
		size_t cnt, loff_t *data)
{
	char buf[64];
	int ret;
	unsigned long arg1, arg2;
	char option[64], arg[10];
	int i, j;

	if (cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;
	buf[cnt] = '\0';

	pr_emerg(TAG"option buffer is %s\n", buf);

	/* get option */
	for (i = 0; i < cnt && buf[i] != ' '; i++)
		option[i] = buf[i];
	option[i] = '\0';
	pr_emerg(TAG"option string is %s\n", option);

	/* get arg1 */
	for (; i < cnt && buf[i] == ' '; i++)
		;
	for (j = 0; i < cnt && buf[i] != ' '; i++, j++)
		arg[j] = buf[i];
	arg[j] = '\0';
	pr_emerg(TAG"arg1 %s end", arg);
	if (j > 0) {
		ret = kstrtoul(arg, 0, (unsigned long *)&arg1);
		if (ret < 0) {
			pr_emerg(TAG"1 ret of kstrtoul is broke\n");
			return ret;
		}
	}

	/* get arg2 */
	for (; i < cnt && buf[i] == ' '; i++)
		;
	for (j = 0; i < cnt && buf[i] != ' '; i++, j++)
		arg[j] = buf[i];
	arg[j] = '\0';
	if (j > 0) {
		ret = kstrtoul(arg, 0, (unsigned long *)&arg2);
		if (ret < 0) {
			pr_emerg(TAG"1 ret of kstrtoul is broke\n");
			return ret;
		}
	}

	if (strncmp(option, "ENABLE_CG", 9) == 0) {
		enable_cg_fliper(arg1);
	} else if (strncmp(option, "SET_CG_THRES", 12) == 0) {
		cg_set_threshold(arg1, arg2);
	} else if (strncmp(option, "RESTORE_CG", 10) == 0) {
		cg_restore_threshold();
	}	else if (strncmp(option, "SET_CG", 6) == 0) {
		setCG(arg1);
	}  else if (strncmp(option, "POWER_MODE", 10) == 0) {
		if (!fliper_debug) {
			if (arg1 == Default) {
				enable_cg_fliper(1);
				cg_set_threshold(CG_DEFAULT_LPM, CG_DEFAULT_HPM);
				vcorefs_request_dvfs_opp(KIR_PERF, OPPI_UNREQ);
				pr_emerg(TAG"POWER_MODE: default\n");
			} else if (arg1 == Low_Power_Mode) {
				enable_cg_fliper(1);
				cg_set_threshold(CG_LOW_POWER_LPM, CG_LOW_POWER_HPM);
				vcorefs_request_dvfs_opp(KIR_PERF, OPPI_UNREQ);
				pr_emerg(TAG"POWER_MODE: LOW_POWER\n");
			} else if (arg1 == Just_Make_Mode) {
				enable_cg_fliper(1);
				cg_set_threshold(CG_JUST_MAKE_LPM, CG_JUST_MAKE_HPM);
				vcorefs_request_dvfs_opp(KIR_PERF, OPPI_UNREQ);
				pr_emerg(TAG"POWER_MODE: JUST_MAKE\n");
			} else if (arg1 == Performance_Mode) {
				enable_cg_fliper(1);
#if 0
				cg_set_threshold(CG_PERFORMANCE_LPM, CG_PERFORMANCE_HPM);
#else
				vcorefs_request_dvfs_opp(KIR_PERF, OPPI_PERF);
#endif
				pr_emerg(TAG"POWER_MODE: PERFORMANCE\n");
			}
		}
	} else if (strncmp(option, "DEBUG", 5) == 0) {
		fliper_debug = arg1;
	} else {
		pr_emerg(TAG"unknown option\n");
	}

	return cnt;
}

static int mt_fliper_show(struct seq_file *m, void *v)
{
	SEQ_printf(m, "-----------------------------------------------------\n");
	SEQ_printf(m, "CG Fliper Enabled:%d, bw threshold:%d %dMB/s\n",
			cg_fliper_enabled, cg_lpm_bw_threshold, cg_hpm_bw_threshold);
	SEQ_printf(m, "CG History: 0x%08x\n", getCGHistory());
	SEQ_printf(m, "CG Configuration: 0x%08x\n", getCGConfiguration());
	SEQ_printf(m, "-----------------------------------------------------\n");
	return 0;
}
/*** Seq operation of mtprof ****/
static int mt_fliper_open(struct inode *inode, struct file *file)
{
	return single_open(file, mt_fliper_show, inode->i_private);
}

static const struct file_operations mt_fliper_fops = {
	.open = mt_fliper_open,
	.write = mt_fliper_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

	static int
fliper_pm_callback(struct notifier_block *nb,
		unsigned long action, void *ptr)
{
	switch (action) {

	case PM_SUSPEND_PREPARE:
		break;
	case PM_HIBERNATION_PREPARE:
		break;

	case PM_POST_SUSPEND:
		pr_emerg(TAG"Resume, restore CG configuration\n");
		cg_set_threshold(cg_lpm_bw_threshold, cg_hpm_bw_threshold);
		break;

	case PM_POST_HIBERNATION:
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}


/*--------------------INIT------------------------*/

static int __init init_fliper(void)
{
	struct proc_dir_entry *pe;

	pr_emerg(TAG"init fliper driver start\n");
	pe = proc_create("fliper", 0644, NULL, &mt_fliper_fops);
	if (!pe)
		return -ENOMEM;

	cg_lpm_bw_threshold = CG_DEFAULT_LPM;
	cg_hpm_bw_threshold = CG_DEFAULT_HPM;
	cg_set_threshold(CG_DEFAULT_LPM, CG_DEFAULT_HPM);
	enable_cg_fliper(1);
	cg_fliper_enabled = 1;

	fliper_debug = 0;

	pm_notifier(fliper_pm_callback, 0);


	pr_emerg(TAG"init fliper driver done\n");

	return 0;
}
device_initcall(init_fliper);

/*MODULE_LICENSE("GPL");*/
/*MODULE_AUTHOR("MTK");*/
/*MODULE_DESCRIPTION("The fliper function");*/
