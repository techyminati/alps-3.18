#ifndef __MT_FREQHOPPING_H__
#define __MT_FREQHOPPING_H__

#define DISABLE_FREQ_HOPPING	/* Disable all FHCTL Common Interface for chip Bring-up */

#define FHTAG "[FH]"
#define VERBOSE_DEBUG 0
#define DEBUG_MSG_ENABLE 0

#if VERBOSE_DEBUG
#define FH_MSG(fmt, args...) \
	printk(FHTAG""fmt" <- %s(): L<%d>  PID<%s><%d>\n", \
	##args , __func__, __LINE__, current->comm, current->pid)

#define FH_MSG_DEBUG   FH_MSG
#else

#define FH_MSG(fmt, args...) pr_debug(fmt, ##args)

#define FH_MSG_DEBUG(fmt, args...)\
	do {    \
		if (DEBUG_MSG_ENABLE)           \
			printk(FHTAG""fmt"\n", ##args); \
	} while (0)
#endif



enum FH_FH_STATUS {
	FH_FH_UNINIT = 0,
	FH_FH_DISABLE,
	FH_FH_ENABLE_SSC,
};

enum FH_PLL_STATUS {
	FH_PLL_DISABLE = 0,
	FH_PLL_ENABLE = 1
};


enum FH_CMD {
	FH_CMD_ENABLE = 1,
	FH_CMD_DISABLE,
	FH_CMD_ENABLE_USR_DEFINED,
	FH_CMD_DISABLE_USR_DEFINED,
	FH_CMD_INTERNAL_MAX_CMD
};



/* [PD] Need porting for platform. */
enum FH_PLL_ID {
	/* MCU FHCTL */
	MCU_FH_PLL0 = 0,	/* CAXPLL0 */
	MCU_FH_PLL1 = 1,	/* CAXPLL1 */
	MCU_FH_PLL2 = 2,	/* CAXPLL2 */
	MCU_FH_PLL3 = 3,	/* CAXPLL3 */
	/* FHCTL */
	FH_PLL0 = 4,		/* VDECPLL */
	FH_PLL1 = 5,		/* MPLL */
	FH_PLL2 = 6,		/* MAINPLL */
	FH_PLL3 = 7,		/* MEMPLL */
	FH_PLL4 = 8,		/* MSDCPLL */
	FH_PLL5 = 9,		/* MFGPLL */
	FH_PLL6 = 10,		/* IMGPLL */
	FH_PLL7 = 11,		/* TVDPLL */
	FH_PLL8 = 12,		/* CODECPLL */

	FH_PLL_NUM,
};

#define isFHCTL(id) ((id >= FH_PLL0)?true:false)


/* keep track the status of each PLL */
/* TODO: do we need another "uint mode" for Dynamic FH */
typedef struct {
	unsigned int curr_freq;	/* Useless variable, just a legacy */
	unsigned int fh_status;
	unsigned int pll_status;
	unsigned int setting_id;
	unsigned int setting_idx_pattern;
	unsigned int user_defined;
} fh_pll_t;


struct freqhopping_ssc {
	unsigned int freq;	/* useless variable, just a legacy. */
	unsigned int idx_pattern;
	unsigned int dt;
	unsigned int df;
	unsigned int upbnd;
	unsigned int lowbnd;
	unsigned int dds;
};

struct freqhopping_ioctl {
	unsigned int pll_id;
	struct freqhopping_ssc ssc_setting;	/* used only when user-define */
	int result;
};

/* FHCTL HAL driver Interface */
int freqhopping_config(unsigned int pll_id, unsigned long def_set_idx, unsigned int enable);
void mt_freqhopping_init(void);
void mt_freqhopping_pll_init(void);
int mt_h2l_mempll(void);
int mt_l2h_mempll(void);
int mt_h2l_dvfs_mempll(void);
int mt_l2h_dvfs_mempll(void);
int mt_is_support_DFS_mode(void);
void mt_fh_popod_save(void);
void mt_fh_popod_restore(void);
int mt_fh_dram_overclock(int clk);
int mt_fh_get_dramc(void);
int mt_freqhopping_devctl(unsigned int cmd, void *args);


/* FHCTL Common driver Interface */
int mt_dfs_mmpll(unsigned int dds);
int mt_dfs_armpll(unsigned int current_freq, unsigned int dds);

#endif				/* !__MT_FREQHOPPING_H__ */
