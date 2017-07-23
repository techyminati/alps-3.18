#ifdef pr_fmt
#undef pr_fmt
#endif

#define pr_fmt(fmt) "["KBUILD_MODNAME"]" fmt

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include "mt_sd.h"
#include "msdc_tune.h"
#include "board.h"
#include <linux/delay.h>


#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#ifndef CONFIG_MTK_LEGACY
#include <linux/regulator/consumer.h>
#endif

/**************************************************************/
/* Section 1: Device Tree                                     */
/**************************************************************/

const struct of_device_id msdc_of_ids[] = {
	{   .compatible = "mediatek,mt6797-mmc", },
	{   .compatible = "mediatek,MSDC2", },
	{   .compatible = "mediatek,MSDC3", },
	{ },
};

unsigned int msdc_host_enable[HOST_MAX_NUM] = {
	1,
	1,
/* FIXME: if msdc2 msdc3 modify device tree standard, msdc_host_enable can be removed */
#if defined(CFG_DEV_MSDC2)
	1,
#else
	0,
#endif
#if defined(CFG_DEV_MSDC3)
	1,
#else
	0,
#endif
};

/* GPIO node */
struct device_node *gpio_node;
struct device_node *io_cfg_b_node;
struct device_node *io_cfg_r_node;

/* debug node */
struct device_node *infracfg_ao_node;
struct device_node *infracfg_node;
struct device_node *pericfg_node;
struct device_node *emi_node;
struct device_node *toprgu_node;
struct device_node *apmixed_node;
struct device_node *topckgen_node;

/* eint node */
struct device_node *eint_node;

void __iomem *gpio_base;
void __iomem *io_cfg_b_base;
void __iomem *io_cfg_r_base;

void __iomem *infracfg_ao_reg_base;
void __iomem *infracfg_reg_base;
void __iomem *pericfg_reg_base;
void __iomem *emi_reg_base;
void __iomem *toprgu_reg_base;
void __iomem *apmixed_reg_base;
void __iomem *topckgen_reg_base;

/*
 * parse pinctl settings
 * Driver strength
 */
static int msdc_get_pinctl_settings(struct msdc_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct device_node *np = mmc->parent->of_node;
	struct device_node *pinctl_node;
	struct device_node *pins_cmd_node;
	struct device_node *pins_dat_node;
	struct device_node *pins_clk_node;
	struct device_node *pins_rst_node;
	struct device_node *pins_ds_node;
	struct device_node *pinctl_sdr104_node;
	struct device_node *pinctl_sdr50_node;
	struct device_node *pinctl_ddr50_node;

	pinctl_node = of_parse_phandle(np, "pinctl", 0);
	pins_cmd_node = of_get_child_by_name(pinctl_node, "pins_cmd");
	of_property_read_u8(pins_cmd_node, "drive-strength",
			&host->hw->cmd_drv);

	pins_dat_node = of_get_child_by_name(pinctl_node, "pins_dat");
	of_property_read_u8(pins_dat_node, "drive-strength",
			&host->hw->dat_drv);

	pins_clk_node = of_get_child_by_name(pinctl_node, "pins_clk");
	of_property_read_u8(pins_clk_node, "drive-strength",
			&host->hw->clk_drv);

	pins_rst_node = of_get_child_by_name(pinctl_node, "pins_rst");
	of_property_read_u8(pins_rst_node, "drive-strength",
			&host->hw->rst_drv);

	pins_ds_node = of_get_child_by_name(pinctl_node, "pins_ds");
	of_property_read_u8(pins_ds_node, "drive-strength", &host->hw->ds_drv);

	/* SDR104 for SDcard */
	pinctl_sdr104_node = of_parse_phandle(np, "pinctl_sdr104", 0);
	pins_cmd_node = of_get_child_by_name(pinctl_sdr104_node, "pins_cmd");
	of_property_read_u8(pins_cmd_node, "drive-strength",
			&host->hw->cmd_drv_sd_18);

	pins_dat_node = of_get_child_by_name(pinctl_sdr104_node, "pins_dat");
	of_property_read_u8(pins_dat_node, "drive-strength",
			&host->hw->dat_drv_sd_18);

	pins_clk_node = of_get_child_by_name(pinctl_sdr104_node, "pins_clk");
	of_property_read_u8(pins_clk_node, "drive-strength",
			&host->hw->clk_drv_sd_18);
	/* SDR50 for SDcard */
	pinctl_sdr50_node = of_parse_phandle(np, "pinctl_sdr50", 0);
	pins_cmd_node = of_get_child_by_name(pinctl_sdr50_node, "pins_cmd");
	of_property_read_u8(pins_cmd_node, "drive-strength",
			&host->hw->cmd_drv_sd_18_sdr50);

	pins_dat_node = of_get_child_by_name(pinctl_sdr50_node, "pins_dat");
	of_property_read_u8(pins_dat_node, "drive-strength",
			&host->hw->dat_drv_sd_18_sdr50);

	pins_clk_node = of_get_child_by_name(pinctl_sdr50_node, "pins_clk");
	of_property_read_u8(pins_clk_node, "drive-strength",
			&host->hw->clk_drv_sd_18_sdr50);

	/* DDR50 for SDcard */
	pinctl_ddr50_node = of_parse_phandle(np, "pinctl_ddr50", 0);
	pins_cmd_node = of_get_child_by_name(pinctl_ddr50_node, "pins_cmd");
	of_property_read_u8(pins_cmd_node, "drive-strength",
			&host->hw->cmd_drv_sd_18_ddr50);

	pins_dat_node = of_get_child_by_name(pinctl_ddr50_node, "pins_dat");
	of_property_read_u8(pins_dat_node, "drive-strength",
			&host->hw->dat_drv_sd_18_ddr50);

	pins_clk_node = of_get_child_by_name(pinctl_ddr50_node, "pins_clk");
	of_property_read_u8(pins_clk_node, "drive-strength",
			&host->hw->clk_drv_sd_18_ddr50);

	return 0;
}
/* Get msdc register settings
 * 1. internal data delay for tuning, FIXME: can be removed when use data tune?
 * 2. sample edge
 * 3. ett settings, FIXME: can be removed when use autok
 */
static int msdc_get_rigister_settings(struct msdc_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct device_node *np = mmc->parent->of_node;
	struct device_node *register_setting_node = NULL;

	/*parse hw property settings*/
	register_setting_node = of_parse_phandle(np, "register_setting", 0);
	if (register_setting_node) {
		of_property_read_u8(register_setting_node, "dat0rddly",
				&host->hw->dat0rddly);
		of_property_read_u8(register_setting_node, "dat1rddly",
				&host->hw->dat1rddly);
		of_property_read_u8(register_setting_node, "dat2rddly",
				&host->hw->dat2rddly);
		of_property_read_u8(register_setting_node, "dat3rddly",
				&host->hw->dat3rddly);
		of_property_read_u8(register_setting_node, "dat4rddly",
				&host->hw->dat4rddly);
		of_property_read_u8(register_setting_node, "dat5rddly",
				&host->hw->dat5rddly);
		of_property_read_u8(register_setting_node, "dat6rddly",
				&host->hw->dat6rddly);
		of_property_read_u8(register_setting_node, "dat7rddly",
				&host->hw->dat7rddly);

		of_property_read_u8(register_setting_node, "datwrddly",
				&host->hw->datwrddly);
		of_property_read_u8(register_setting_node, "cmdrrddly",
				&host->hw->cmdrrddly);
		of_property_read_u8(register_setting_node, "cmdrddly",
				&host->hw->cmdrddly);

		of_property_read_u8(register_setting_node, "cmd_edge",
				&host->hw->cmd_edge);
		of_property_read_u8(register_setting_node, "rdata_edge",
				&host->hw->rdata_edge);
		of_property_read_u8(register_setting_node, "wdata_edge",
				&host->hw->wdata_edge);
	} else {
		pr_err("[msdc%d] register_setting is not found in DT.\n",
				host->id);
		return 1;
	}
	/* parse ett */
	if (of_property_read_u32(register_setting_node, "ett-hs200-cells",
				&host->hw->ett_hs200_count))
		pr_err("[msdc] ett-hs200-cells is not found in DT.\n");
	host->hw->ett_hs200_settings =
		kzalloc(sizeof(struct msdc_ett_settings) *
				host->hw->ett_hs200_count, GFP_KERNEL);

	if (MSDC_EMMC == host->hw->host_function &&
		!of_property_read_u32_array(register_setting_node,
		"ett-hs200-customer", host->hw->ett_hs200_settings,
		host->hw->ett_hs200_count * 3)) {
		pr_err("[msdc%d] hs200 ett for customer is found in DT.\n",
				host->id);
	} else if (MSDC_EMMC == host->hw->host_function
		&& !of_property_read_u32_array(register_setting_node,
		"ett-hs200-default", host->hw->ett_hs200_settings,
		host->hw->ett_hs200_count * 3)) {
		pr_err("[msdc%d] hs200 ett for default is found in DT.\n",
				host->id);
	} else if (MSDC_EMMC == host->hw->host_function) {
		pr_err("[msdc%d]error: hs200 ett is not found in DT.\n",
				host->id);
	}

	if (of_property_read_u32(register_setting_node, "ett-hs400-cells",
				&host->hw->ett_hs400_count))
		pr_err("[msdc] ett-hs400-cells is not found in DT.\n");
	host->hw->ett_hs400_settings =
		kzalloc(sizeof(struct msdc_ett_settings) *
				host->hw->ett_hs400_count, GFP_KERNEL);

	if (MSDC_EMMC == host->hw->host_function &&
		!of_property_read_u32_array(register_setting_node,
		"ett-hs400-customer", host->hw->ett_hs400_settings,
		host->hw->ett_hs400_count * 3)) {
		pr_err("[msdc%d] hs400 ett for customer is found in DT.\n",
				host->id);
	} else if (MSDC_EMMC == host->hw->host_function &&
		!of_property_read_u32_array(register_setting_node,
		"ett-hs400-default", host->hw->ett_hs400_settings,
		host->hw->ett_hs400_count * 3)) {
		pr_err("[msdc%d] hs400 ett for default is found in DT.\n",
				host->id);
	} else if (MSDC_EMMC == host->hw->host_function) {
		pr_err("[msdc%d]error: hs400 ett is not found in DT.\n",
				host->id);
	}
	return 0;
}
/*
 *	msdc_of_parse() - parse host's device-tree node
 *	@host: host whose node should be parsed.
 *
 */
int msdc_of_parse(struct mmc_host *mmc)
{
	struct device_node *np;
	struct msdc_host *host = mmc_priv(mmc);
	int ret, len, debug = 0;

	if (!mmc->parent || !mmc->parent->of_node)
		return 1;

	np = mmc->parent->of_node;
	host->mmc = mmc;  /* msdc_check_init_done() need */
	host->hw = kzalloc(sizeof(struct msdc_hw), GFP_KERNEL);

	/*basic settings*/
	if (0 == strcmp(np->name, "msdc0"))
		host->id = 0;
	else if (0 == strcmp(np->name, "msdc1"))
		host->id = 1;
	else if (0 == strcmp(np->name, "msdc2"))
		host->id = 2;
	else
		host->id = 3;

	/* iomap register */
	host->base = of_iomap(np, 0);
	if (!host->base) {
		pr_err("[msdc%d] of_iomap failed\n", host->id);
		return -ENOMEM;
	}
	pr_err("[msdc%d] host->base is %x\n", host->id, host->base);

	/* get irq #  */
	host->irq = irq_of_parse_and_map(np, 0);
	if (host->irq < 0) {
		pr_err("[msdc%d]  get irq failed, host->irq = %d\n",
			host->id, host->irq);
		BUG_ON(1);
	}
	pr_err("[msdc%d] get irq # %d\n", host->id, host->irq);

	/* get clk_src */
	if (of_property_read_u8(np, "clk_src", &host->hw->clk_src)) {
		pr_err("[msdc%d] error: clk_src isn't found in device tree.\n",
				host->id);
		BUG_ON(1);
	}

	/* get msdc flag(caps)*/
	if (of_find_property(np, "msdc-sys-suspend", &len))
		host->hw->flags |= MSDC_SYS_SUSPEND;

	/* Returns 0 on success, -EINVAL if the property does not exist,
	* -ENODATA if property does not have a value, and -EOVERFLOW if the
	* property data isn't large enough.*/

	if (of_property_read_u8(np, "host_function", &host->hw->host_function))
		pr_err("[msdc%d] host_function isn't found in device tree\n",
				host->id);

	if (of_find_property(np, "bootable", &len))
		host->hw->boot = 1;

	/*get cd_level*/
	if (of_property_read_u8(np, "cd_level", &host->hw->cd_level))
		pr_err("[msdc%d] cd_level isn't found in device tree\n",
				host->id);

	/*get cd_gpio*/
	of_property_read_u32_index(np, "cd-gpios", 1, &cd_gpio);
	msdc_get_rigister_settings(host);
	msdc_get_pinctl_settings(host);

	return 0;
out:
	return ret;
}

int msdc_dt_init(struct platform_device *pdev, struct mmc_host *mmc,
		unsigned int *cd_irq)
{
	/* regulator node */
#if !defined(FPGA_PLATFORM) && !defined(CONFIG_MTK_LEGACY)
	struct device_node *msdc_backup_node;
#endif
	unsigned int host_id = 0;
	int i, ret;
	struct msdc_host *host = mmc_priv(mmc);
	static const char const *msdc_names[] = {"msdc0", "msdc1", "msdc2", "msdc3"};

	for (i = 0; i < HOST_MAX_NUM; i++) {
		if (0 == strcmp(pdev->dev.of_node->name, msdc_names[i])) {
			if (msdc_host_enable[i] == 0)
				return 1;
			/* FIXME: msdc0 msdc1 prase parameter from device tree
			 * msdc2 msdc3 should be modified later
			 */
			if (i == 2) {
#ifdef CFG_DEV_MSDC2
				host->hw = &msdc2_hw;
				pr_err("platform_data hw:0x%p, is msdc2_hw\n",
					host->hw);
#endif
			}
			pdev->id = i;
			host_id = i;
			break;
		}
	}
	if (i == HOST_MAX_NUM) {
		pr_err("%s: Can not find msdc host\n", __func__);
		return 1;
	}

	pr_err("msdc%d of msdc DT probe %s!\n",
			host_id, pdev->dev.of_node->name);

	ret = mmc_of_parse(mmc);
	if (ret) {
		pr_err("%s: mmc of parse error!!\n", __func__);
		return ret;
	}
	ret = msdc_of_parse(mmc);
	if (ret) {
		pr_err("%s: msdc of parse error!!\n", __func__);
		return ret;
	}

	if (gpio_node == NULL) {
		gpio_node = of_find_compatible_node(NULL, NULL,
			"mediatek,gpio");
		gpio_base = of_iomap(gpio_node, 0);
		pr_debug("of_iomap for gpio base @ 0x%p\n", gpio_base);
	}

	if (io_cfg_r_node == NULL) {
		io_cfg_r_node = of_find_compatible_node(NULL, NULL,
			"mediatek,iocfg_r");
		io_cfg_r_base = of_iomap(io_cfg_r_node, 0);
		pr_debug("of_iomap for io_cfg_r base @ 0x%p\n", io_cfg_r_base);
	}
	if (io_cfg_b_node == NULL) {
		io_cfg_b_node = of_find_compatible_node(NULL, NULL,
			"mediatek,iocfg_b");
		io_cfg_b_base = of_iomap(io_cfg_b_node, 0);
		pr_debug("of_iomap for io_cfg_b base @ 0x%p\n", io_cfg_b_base);
	}

#ifndef FPGA_PLATFORM
	/*FIXME, correct node match string */
	if (infracfg_ao_node == NULL) {
		infracfg_ao_node = of_find_compatible_node(NULL, NULL,
			"mediatek,INFRACFG_AO");
		infracfg_ao_reg_base = of_iomap(infracfg_ao_node, 0);
		pr_debug("of_iomap for infracfg_ao base @ 0x%p\n",
			infracfg_ao_reg_base);
	}

	if (infracfg_node == NULL) {
		infracfg_node = of_find_compatible_node(NULL, NULL,
			"mediatek,INFRACFG");
		infracfg_reg_base = of_iomap(infracfg_node, 0);
		pr_debug("of_iomap for infracfg base @ 0x%p\n",
			infracfg_reg_base);
	}

	if (pericfg_node == NULL) {
		pericfg_node = of_find_compatible_node(NULL, NULL,
			"mediatek,PERICFG");
		pericfg_reg_base = of_iomap(pericfg_node, 0);
		pr_debug("of_iomap for pericfg base @ 0x%p\n",
			pericfg_reg_base);
	}

	if (emi_node == NULL) {
		emi_node = of_find_compatible_node(NULL, NULL,
			"mediatek,EMI");
		emi_reg_base = of_iomap(emi_node, 0);
		pr_debug("of_iomap for emi base @ 0x%p\n",
			emi_reg_base);
	}

	if (toprgu_node == NULL) {
		toprgu_node = of_find_compatible_node(NULL, NULL,
			"mediatek,TOPRGU");
		toprgu_reg_base = of_iomap(toprgu_node, 0);
		pr_debug("of_iomap for toprgu base @ 0x%p\n",
			toprgu_reg_base);
	}

	if (apmixed_node == NULL) {
		apmixed_node = of_find_compatible_node(NULL, NULL,
			"mediatek,APMIXED");
		apmixed_reg_base = of_iomap(apmixed_node, 0);
		pr_debug("of_iomap for APMIXED base @ 0x%p\n",
			apmixed_reg_base);
	}

	if (topckgen_node == NULL) {
		topckgen_node = of_find_compatible_node(NULL, NULL,
			"mediatek,TOPCKGEN");
		topckgen_reg_base = of_iomap(topckgen_node, 0);
		pr_debug("of_iomap for TOPCKGEN base @ 0x%p\n",
			topckgen_reg_base);
	}

#ifndef CONFIG_MTK_LEGACY
	/* backup original dev.of_node */
	msdc_backup_node = pdev->dev.of_node;
	/* get regulator supply node */
	pdev->dev.of_node = of_find_compatible_node(NULL, NULL,
		"mediatek,mt_pmic_regulator_supply");
	msdc_get_regulators(&(pdev->dev));
	/* restore original dev.of_node */
	pdev->dev.of_node = msdc_backup_node;
#endif
#endif
	/* Get SD card detect eint irq */
	if ((host->hw->host_function == MSDC_SD) && (eint_node == NULL)) {
		eint_node = of_find_compatible_node(NULL, NULL,
			"mediatek, MSDC1_INS-eint");
		if (eint_node) {
			*cd_irq = irq_of_parse_and_map(eint_node, 0);
			if (*cd_irq == 0)
				pr_err("Parse map for sd detect eint fail\n");
			else
				pr_err("msdc1 eint get irq # %d\n", *cd_irq);
		} else {
			pr_err("can't find MSDC1_INS-eint compatible node\n");
		}
	}

	return 0;
}

u32 g_msdc0_io;
u32 g_msdc0_flash;
u32 g_msdc1_io;
u32 g_msdc1_flash;
u32 g_msdc2_io;
u32 g_msdc2_flash;
u32 g_msdc3_io;
u32 g_msdc3_flash;

#ifndef FPGA_PLATFORM
/**************************************************************/
/* Section 2: Power                                           */
/**************************************************************/
#ifdef CONFIG_MTK_LEGACY
#include <mach/mt_pm_ldo.h>
#endif
#include <mt-plat/upmu_common.h>
#ifndef CONFIG_MTK_LEGACY
struct regulator *reg_vemc;
struct regulator *reg_vmc;
struct regulator *reg_vmch;

enum MSDC_LDO_POWER {
	POWER_LDO_VMCH,
	POWER_LDO_VMC,
	POWER_LDO_VEMC,
};

void msdc_get_regulators(struct device *dev)
{
	if (reg_vemc == NULL)
		reg_vemc = regulator_get(dev, "vemc");
	if (reg_vmc == NULL)
		reg_vmc = regulator_get(dev, "vmc");
	if (reg_vmch == NULL)
		reg_vmch = regulator_get(dev, "vmch");
}

bool msdc_hwPowerOn(unsigned int powerId, int powerVolt, char *mode_name)
{
#if 0
	struct regulator *reg = NULL;

	switch (powerId) {
	case POWER_LDO_VMCH:
		reg = reg_vmch;
		break;
	case POWER_LDO_VMC:
		reg = reg_vmc;
		break;
	case POWER_LDO_VEMC:
		reg = reg_vemc;
		break;
	default:
		pr_err("[MSDC] Invalid power id\n");
		return FALSE;
	}
	if (reg == NULL) {
		pr_err("[MSDC] regulator pointer is NULL\n");
		return FALSE;
	}
	/* New API voltage use micro V */
	regulator_set_voltage(reg, powerVolt, powerVolt);
	regulator_enable(reg);
	pr_err("msdc_hwPoweron:%d: name:%s", powerId, mode_name);
	return TRUE;
#endif
}

bool msdc_hwPowerDown(unsigned int powerId, char *mode_name)
{
#if 0
	struct regulator *reg = NULL;

	switch (powerId) {
	case POWER_LDO_VMCH:
		reg = reg_vmch;
		break;
	case POWER_LDO_VMC:
		reg = reg_vmc;
		break;
	case POWER_LDO_VEMC:
		reg = reg_vemc;
		break;
	default:
		pr_err("[MSDC] Invalid power id\n");
		return FALSE;
	}
	if (reg == NULL) {
		pr_err("[MSDC] regulator pointer is NULL\n");
		return FALSE;
	}
	/* New API voltage use micro V */
	regulator_disable(reg);
	pr_err("msdc_hwPowerOff:%d: name:%s", powerId, mode_name);

	return TRUE;
#endif
}

#else /*for CONFIG_MTK_LEGACY defined*/
#define	POWER_LDO_VMCH		MT6351_POWER_LDO_VMCH
#define	POWER_LDO_VMC		MT6351_POWER_LDO_VMC
#define	POWER_LDO_VEMC		MT6351_POWER_LDO_VEMC

#define msdc_hwPowerOn(powerId, powerVolt, name) \
	hwPowerOn(powerId, powerVolt, name)

#define msdc_hwPowerDown(powerId, name) \
	hwPowerDown(powerId, name)
#endif

#ifndef CONFIG_MTK_LEGACY
u32 msdc_ldo_power(u32 on, unsigned int powerId, int voltage_mv,
	u32 *status)
#else
u32 msdc_ldo_power(u32 on, MT65XX_POWER powerId, int voltage_mv,
	u32 *status)
#endif
{
	int voltage_uv = voltage_mv * 1000;

	if (on) { /* want to power on */
		if (*status == 0) {  /* can power on */
			pr_err("msdc LDO<%d> power on<%d>\n", powerId,
				voltage_uv);
			msdc_hwPowerOn(powerId, voltage_uv, "msdc");
			*status = voltage_uv;
		} else if (*status == voltage_uv) {
			pr_err("msdc LDO<%d><%d> power on again!\n", powerId,
				voltage_uv);
		} else { /* for sd3.0 later */
			pr_err("msdc LDO<%d> change<%d> to <%d>\n", powerId,
				*status, voltage_uv);
			msdc_hwPowerDown(powerId, "msdc");
			msdc_hwPowerOn(powerId, voltage_uv, "msdc");
			/*hwPowerSetVoltage(powerId, voltage_uv, "msdc");*/
			*status = voltage_uv;
		}
	} else {  /* want to power off */
		if (*status != 0) {  /* has been powerred on */
			pr_err("msdc LDO<%d> power off\n", powerId);
			msdc_hwPowerDown(powerId, "msdc");
			*status = 0;
		} else {
			pr_err("LDO<%d> not power on\n", powerId);
		}
	}

	return 0;
}

void msdc_dump_ldo_sts(struct msdc_host *host)
{
#if 0
#ifdef MTK_MSDC_BRINGUP_DEBUG
	u32 ldo_en = 0, ldo_vol = 0;
	u32 id = host->id;

	switch (id) {
	case 0:
		pmic_read_interface_nolock(REG_VEMC_EN, &ldo_en, MASK_VEMC_EN,
			SHIFT_VEMC_EN);
		pmic_read_interface_nolock(REG_VEMC_VOSEL, &ldo_vol,
			MASK_VEMC_VOSEL, SHIFT_VEMC_VOSEL);
		pr_err("VEMC_EN=0x%x, be:1b'1,VEMC_VOL=0x%x, be:1b'0(3.0V),1b'1(3.3V)\n",
			ldo_en, ldo_vol);
		break;
	case 1:
		pmic_read_interface_nolock(REG_VMC_EN, &ldo_en, MASK_VMC_EN,
			SHIFT_VMC_EN);
		pmic_read_interface_nolock(REG_VMC_VOSEL, &ldo_vol,
			MASK_VMC_VOSEL, SHIFT_VMC_VOSEL);
		pr_err("VMC_EN=0x%x, be:bit1=1,VMC_VOL=0x%x, be:3b'101(2.8V),3b'011(1.8V)\n",
			ldo_en, ldo_vol);

		pmic_read_interface_nolock(REG_VMCH_EN, &ldo_en, MASK_VMCH_EN,
			SHIFT_VMCH_EN);
		pmic_read_interface_nolock(REG_VMCH_VOSEL, &ldo_vol,
			MASK_VMCH_VOSEL, SHIFT_VMCH_VOSEL);
		pr_err("VMCH_EN=0x%x, be:bit1=1, VMCH_VOL=0x%x, be:1b'0(3V),1b'1(3.3V)\n",
			ldo_en, ldo_vol);
		break;
	default:
		break;
	}
#endif
#endif
}

void msdc_sd_power_switch(struct msdc_host *host, u32 on)
{
#if 0
	unsigned int reg_val;
	switch (host->id) {
	case 1:
		msdc_ldo_power(on, POWER_LDO_VMC, VOL_1800, &g_msdc1_io);
		/* MT6351 have no 2.9v setting, So calibration 100mv here */
		if (on) {
			/* VMC calibration. */
			pmic_read_interface(REG_VMC_VOSEL_CAL,
				&reg_val,
				MASK_VMC_VOSEL_CAL,
				SHIFT_VMC_VOSEL_CAL);

			/* -100mv. since 2.9V is 2.8V + 100mV. */
			reg_val = reg_val + 5;

			pmic_config_interface(REG_VMC_VOSEL_CAL,
				reg_val,
				MASK_VMC_VOSEL_CAL,
				SHIFT_VMC_VOSEL_CAL);
		}

		msdc_set_tdsel(host, 0, 1);
		msdc_set_rdsel(host, 0, 1);
		msdc_set_driving(host, host->hw, 1);
		break;
	case 2:
		msdc_ldo_power(on, POWER_LDO_VMC, VOL_1800, &g_msdc2_io);
		msdc_set_tdsel(host, 0, 1);
		msdc_set_rdsel(host, 0, 1);
		msdc_set_driving(host, host->hw, 1);
		break;
	default:
		break;
	}
#endif
}

void msdc_sdio_power(struct msdc_host *host, u32 on)
{
#if 0
	switch (host->id) {
#if defined(CFG_DEV_MSDC2)
	case 2:
		g_msdc2_flash = g_msdc2_io;
		break;
#endif
	default:
		/* if host_id is 3, it uses default 1.8v setting,
		which always turns on */
		break;
	}
#endif
}

void msdc_emmc_power(struct msdc_host *host, u32 on)
{
#if 0
	unsigned long tmo = 0;
	void __iomem *base = host->base;
	unsigned int sa_timeout;

	/* if MMC_CAP_WAIT_WHILE_BUSY not set, mmc core layer will wait for
	 sa_timeout */
	if (host->mmc && host->mmc->card && (on == 0) &&
	    host->mmc->caps & MMC_CAP_WAIT_WHILE_BUSY) {
		/* max timeout: 1000ms */
		sa_timeout = host->mmc->card->ext_csd.sa_timeout;
		if ((DIV_ROUND_UP(sa_timeout, 10000)) < 1000)
			tmo = DIV_ROUND_UP(sa_timeout, 10000000/HZ) + HZ/100;
		else
			tmo = HZ;
		tmo += jiffies;

		while ((MSDC_READ32(MSDC_PS) & 0x10000) != 0x10000) {
			if (time_after(jiffies, tmo)) {
				pr_err("Dat0 keep low before power off,sa_timeout = 0x%x",
					sa_timeout);
				emmc_sleep_failed = 1;
				break;
			}
		}
	}

	/* Set to 3.0V - 100mV
	 * 4'b0000: 0 mV
	 * 4'b0001: -20 mV
	 * 4'b0010: -40 mV
	 * 4'b0011: -60 mV
	 * 4'b0100: -80 mV
	 * 4'b0101: -100 mV
	*/
	msdc_ldo_power(on, POWER_LDO_VEMC, VOL_3000, &g_msdc0_flash);
	/* cal 3.0V - 100mv */
	if (on && CARD_VOL_ACTUAL != VOL_3000) {
		pmic_config_interface(REG_VEMC_VOSEL_CAL,
			VEMC_VOSEL_CAL_mV(CARD_VOL_ACTUAL - VOL_3000),
			MASK_VEMC_VOSEL_CAL,
			SHIFT_VEMC_VOSEL_CAL);
	}

	msdc_dump_ldo_sts(host);
#endif
}

void msdc_sd_power(struct msdc_host *host, u32 on)
{
#if 0
	u32 card_on = on;
	switch (host->id) {
	case 1:
		msdc_set_driving(host, host->hw, 0);
		msdc_set_tdsel(host, 0, 0);
		msdc_set_rdsel(host, 0, 0);
		if (host->hw->flags & MSDC_SD_NEED_POWER)
			card_on = 1;
		msdc_ldo_power(card_on, POWER_LDO_VMCH, VOL_2900,
			&g_msdc1_flash);
		msdc_ldo_power(on, POWER_LDO_VMC, VOL_2900, &g_msdc1_io);

		break;
	case 2:
		msdc_set_driving(host, host->hw, 0);
		msdc_set_tdsel(host, 0, 0);
		msdc_set_rdsel(host, 0, 0);
		msdc_ldo_power(on, POWER_LDO_VMCH, VOL_3000, &g_msdc2_flash);
		msdc_ldo_power(on, POWER_LDO_VMC, VOL_3000, &g_msdc2_io);
		break;
	default:
		break;
	}

	msdc_dump_ldo_sts(host);
#endif
}

/**************************************************************/
/* Section 3: Clock                                           */
/**************************************************************/
/*  msdc clock source
 *  Clock manager will set a reference value in CCF
 *  Normal code doesn't change clock source
 *  Lower frequence will change clock source
 */
/* msdc50_0_hclk reference value is 273M */
u32 hclks_msdc50_0_hclk[] = {PLLCLK_50M, PLLCLK_273M, PLLCLK_182M, PLLCLK_78M};
/* msdc0 clock source reference value is 400M */
u32 hclks_msdc50_0[] = {PLLCLK_50M, PLLCLK_400M, PLLCLK_364M, PLLCLK_156M,
			PLLCLK_182M, PLLCLK_156M, PLLCLK_200M, PLLCLK_312M,
			PLLCLK_416M};
/* msdc1 clock source reference value is 200M */
u32 hclks_msdc30_1[] = {PLLCLK_50M, PLLCLK_208M, PLLCLK_200M, PLLCLK_156M,
		      PLLCLK_182M, PLLCLK_156M, PLLCLK_178M};
/* FIXME: msdc2 is not be portted */
u32 hclks_msdc30_2[] = {PLLCLK_50M};
u32 *hclks_msdc = NULL;

/* #include <mt-plat/mt_idle.h> */
struct clk *g_msdc0_pll_sel;
struct clk *g_msdc0_pll_400m;
struct clk *g_msdc0_pll_200m;

int msdc_get_ccf_clk_pointer(struct platform_device *pdev,
	struct msdc_host *host)
{
	if (pdev->id == 0) {
		host->clock_control = devm_clk_get(&pdev->dev, "msdc0-clock");
		g_msdc0_pll_sel  = devm_clk_get(&pdev->dev, "MSDC0_PLL_SEL");
		g_msdc0_pll_400m = devm_clk_get(&pdev->dev, "MSDC0_PLL_400M");
		g_msdc0_pll_200m = devm_clk_get(&pdev->dev, "MSDC0_PLL_200M");
	} else if (pdev->id == 1) {
		host->clock_control = devm_clk_get(&pdev->dev, "msdc1-clock");
	} else if (pdev->id == 2) {
		/*host->clock_control = devm_clk_get(&pdev->dev,
			"MSDC2-CLOCK");*/
	}
	if (IS_ERR(host->clock_control)) {
		pr_err("[msdc%d] can not get clock control\n", pdev->id);
		return 1;
	}
	if (clk_prepare(host->clock_control)) {
		pr_err("[msdc%d] can not prepare clock control\n", pdev->id);
		return 1;
	}
	if (host->id == 0) {
		if (IS_ERR(g_msdc0_pll_sel) ||
		    IS_ERR(g_msdc0_pll_400m) || IS_ERR(g_msdc0_pll_200m)) {
			pr_err("msdc0 get pll error,g_msdc0_pll_sel=%p, g_msdc0_pll_400m=%p,g_msdc0_pll_200m=%p\n",
				g_msdc0_pll_sel,
				g_msdc0_pll_400m, g_msdc0_pll_200m);
			return 1;
		}
		if (clk_prepare(g_msdc0_pll_sel)) {
			pr_err("msdc%d can not prepare g_msdc0_pll_sel\n",
				pdev->id);
			return 1;
		}
	}

	return 0;
}

void msdc_select_clksrc(struct msdc_host *host, int clksrc)
{
	u32 *hclks;
	struct clk *clk;
	unsigned int ret;

	hclks = msdc_get_hclks(host->id);

	pr_err("[%s]: msdc%d change clk_src from %dKHz to %d:%dKHz\n", __func__,
		host->id, (host->hclk/1000), clksrc, (hclks[clksrc]/1000));

	if (host->id != 0) {
		pr_err("[msdc%d] NOT Support switch pll souce[%s]%d\n",
			host->id, __func__, __LINE__);
		return;
	}

	if (hclks[clksrc] == PLLCLK_400M) {
		clk = g_msdc0_pll_400m;
	} else if (hclks[clksrc] == PLLCLK_200M) {
		clk = g_msdc0_pll_200m;
	} else {
		pr_err("[msdc%d] NOT Support switch pll souce[%s]%d\n",
			host->id, __func__, __LINE__);
		return;
	}

	clk_enable(g_msdc0_pll_sel);
	ret = clk_set_parent(g_msdc0_pll_sel, clk);
	if (ret)
		pr_err("[msdc%d] switch clk source ERROR...[%s]%d\n",
			host->id, __func__, __LINE__);
	clk_disable(g_msdc0_pll_sel);

	host->hclk = hclks[clksrc];
	host->hw->clk_src = clksrc;
}

void msdc_dump_clock_sts(struct msdc_host *host)
{
#ifdef MTK_MSDC_BRINGUP_DEBUG
	if (topckgen_reg_base) {
		/* CLK_CFG_3 control msdc clock source PLL */
		pr_err(" CLK_CFG_3 register address is 0x%p\n\n",
			topckgen_reg_base + MSDC_CLK_CFG_3_OFFSET);
		pr_err(" bit[9~8]=01b,     bit[15]=0b\n");
		pr_err(" bit[19~16]=0001b, bit[23]=0b\n");
		pr_err(" bit[26~24]=0010b, bit[31]=0b\n");
		pr_err(" Read value is       0x%x\n",
			MSDC_READ32(topckgen_reg_base + MSDC_CLK_CFG_3_OFFSET));
	}
	if (apmixed_reg_base) {
		/* bit0 is enables PLL, 0: disable 1: enable */
		pr_err(" MSDCPLL_CON0_OFFSET register address is 0x%p\n\n",
			apmixed_reg_base + MSDCPLL_CON0_OFFSET);
		pr_err(" bit[2~0]=111b\n");
		pr_err(" Read value is       0x%x\n",
			MSDC_READ32(apmixed_reg_base + MSDCPLL_CON0_OFFSET));

		pr_err(" MSDCPLL_CON1_OFFSET register address is 0x%p\n\n",
			apmixed_reg_base + MSDCPLL_CON1_OFFSET);
		pr_err(" bit[31]=1b\n");
		pr_err(" Read value is       0x%x\n",
			MSDC_READ32(apmixed_reg_base + MSDCPLL_CON1_OFFSET));

		pr_err(" MSDCPLL_CON2_OFFSET register address is 0x%p\n\n",
			apmixed_reg_base + MSDCPLL_CON2_OFFSET);
		pr_err(" Read value is       0x%x\n",
			MSDC_READ32(apmixed_reg_base + MSDCPLL_CON2_OFFSET));

		pr_err(" MSDCPLL_PWR_CON0 register address is 0x%p\n\n",
			apmixed_reg_base + MSDCPLL_PWR_CON0_OFFSET);
		pr_err(" bit[31]=0b, bit[1]=0b, bit[0]=1b\n");
		pr_err(" Read value is       0x%x\n",
			MSDC_READ32(apmixed_reg_base + MSDCPLL_PWR_CON0_OFFSET));
	}
#endif
}

/**************************************************************/
/* Section 4: GPIO and Pad                                    */
/**************************************************************/
void msdc_dump_padctl_by_id(u32 id)
{
	switch (id) {
	case 0:
		pr_err("MSDC0 MODE14[0x%p] =0x%8x\tshould:0x111111??\n",
			MSDC0_GPIO_MODE14, MSDC_READ32(MSDC0_GPIO_MODE14));
		pr_err("MSDC0 MODE15[0x%p] =0x%8x\tshould:0x??111111\n",
			MSDC0_GPIO_MODE15, MSDC_READ32(MSDC0_GPIO_MODE15));
		pr_err("MSDC0 SMT   [0x%p] =0x%8x\tshould:0x???????f\n",
			MSDC0_GPIO_SMT_ADDR, MSDC_READ32(MSDC0_GPIO_SMT_ADDR));
		pr_err("MSDC0 IES   [0x%p] =0x%8x\tshould:0x?????fff\n",
			MSDC0_GPIO_IES_ADDR, MSDC_READ32(MSDC0_GPIO_IES_ADDR));
		pr_err("PUPD/R0/R1: dat/cmd:0/1/0, clk/dst: 1/0/1");
		pr_err("PUPD/R0/R1: rstb: 0/0/1");
		pr_err("MSDC0 PUPD [0x%p] =0x%8x\tshould: 011000000000 ??????b\n",
			MSDC0_GPIO_PUPD_ADDR,
			MSDC_READ32(MSDC0_GPIO_PUPD_ADDR));
		pr_err("MSDC0 RO [0x%p]   =0x%8x\tshould: 000111111111 ??????b\n",
			MSDC0_GPIO_R0_ADDR,
			MSDC_READ32(MSDC0_GPIO_R0_ADDR));
		pr_err("MSDC0 R1 [0x%p]   =0x%8x\tshould: 111000000000 ??????b\n",
			MSDC0_GPIO_R1_ADDR,
			MSDC_READ32(MSDC0_GPIO_R1_ADDR));
		pr_err("MSDC0 SR    [0x%p] =0x%8x\n",
			MSDC0_GPIO_SR_ADDR,
			MSDC_READ32(MSDC0_GPIO_SR_ADDR));
		pr_err("MSDC0 TDSEL [0x%p] =0x%8x\tshould:0x????0000\n",
			MSDC0_GPIO_TDSEL_ADDR,
			MSDC_READ32(MSDC0_GPIO_TDSEL_ADDR));
		pr_err("MSDC0 RDSEL [0x%p] =0x%8x\tshould:0x???00000\n",
			MSDC0_GPIO_RDSEL_ADDR,
			MSDC_READ32(MSDC0_GPIO_RDSEL_ADDR));
		pr_err("MSDC0 DRV   [0x%p] =0x%8x\tshould: 1001001001b\n",
			MSDC0_GPIO_DRV_ADDR,
			MSDC_READ32(MSDC0_GPIO_DRV_ADDR));
		break;
	case 1:
		pr_err("MSDC1 MODE16 [0x%p] =0x%8x\n",
			MSDC1_GPIO_MODE16, MSDC_READ32(MSDC1_GPIO_MODE16));
		pr_err("MSDC1 SMT    [0x%p] =0x%8x\n",
			MSDC1_GPIO_SMT_ADDR, MSDC_READ32(MSDC1_GPIO_SMT_ADDR));
		pr_err("MSDC1 IES    [0x%p] =0x%8x\n",
			MSDC1_GPIO_IES_ADDR, MSDC_READ32(MSDC1_GPIO_IES_ADDR));
		pr_err("MSDC1 PUPD   [0x%p] =0x%8x\n",
			MSDC1_GPIO_PUPD_ADDR,
			MSDC_READ32(MSDC1_GPIO_PUPD_ADDR));
		pr_err("MSDC1 RO     [0x%p] =0x%8x\n",
			MSDC1_GPIO_R0_ADDR,
			MSDC_READ32(MSDC1_GPIO_R0_ADDR));
		pr_err("MSDC1 R1     [0x%p] =0x%8x\n",
			MSDC1_GPIO_R1_ADDR,
			MSDC_READ32(MSDC1_GPIO_R1_ADDR));

		pr_err("MSDC1 SR     [0x%p] =0x%8x\n",
			MSDC1_GPIO_SR_ADDR, MSDC_READ32(MSDC1_GPIO_SR_ADDR));
		pr_err("MSDC1 TDSEL  [0x%p] =0x%8x\n",
			MSDC1_GPIO_TDSEL_ADDR,
			MSDC_READ32(MSDC1_GPIO_TDSEL_ADDR));
		/* Note1=> For Vcore=0.7V sleep mode
		 * if TDSEL0~3 don't set to [1111]
		 * the digital output function will fail
		 */
		pr_err("should 1.8v: sleep:0x?fff????, awake:0x?aaa????\n");
		pr_err("MSDC1 RDSEL  [0x%p] =0x%8x\n",
			MSDC1_GPIO_RDSEL_ADDR,
			MSDC_READ32(MSDC1_GPIO_RDSEL_ADDR));
		pr_err("1.8V: ??0000??, 2.9v: 0x??c30c??\n");
		pr_err("MSDC1 DRV    [0x%p] =0x%8x\n",
			MSDC1_GPIO_DRV_ADDR, MSDC_READ32(MSDC1_GPIO_DRV_ADDR));
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}

void msdc_set_pin_mode(struct msdc_host *host)
{
	switch (host->id) {
	case 0:
		MSDC_SET_FIELD(MSDC0_GPIO_MODE14, MSDC0_MODE_DAT5_MASK, 0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE14, MSDC0_MODE_DAT4_MASK, 0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE14, MSDC0_MODE_DAT3_MASK, 0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE14, MSDC0_MODE_DAT2_MASK, 0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE14, MSDC0_MODE_DAT1_MASK, 0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE14, MSDC0_MODE_DAT0_MASK, 0x1);

		MSDC_SET_FIELD(MSDC0_GPIO_MODE15, MSDC0_MODE_RSTB_MASK, 0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE15, MSDC0_MODE_DSL_MASK,  0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE15, MSDC0_MODE_CLK_MASK,  0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE15, MSDC0_MODE_CMD_MASK,  0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE15, MSDC0_MODE_DAT7_MASK, 0x1);
		MSDC_SET_FIELD(MSDC0_GPIO_MODE15, MSDC0_MODE_DAT6_MASK, 0x1);
		break;
	case 1:
		MSDC_SET_FIELD(MSDC1_GPIO_MODE16, MSDC1_MODE_CMD_MASK,  0x1);
		MSDC_SET_FIELD(MSDC1_GPIO_MODE16, MSDC1_MODE_DAT0_MASK, 0x1);
		MSDC_SET_FIELD(MSDC1_GPIO_MODE16, MSDC1_MODE_DAT1_MASK, 0x1);
		MSDC_SET_FIELD(MSDC1_GPIO_MODE16, MSDC1_MODE_DAT2_MASK, 0x1);
		MSDC_SET_FIELD(MSDC1_GPIO_MODE16, MSDC1_MODE_DAT3_MASK, 0x1);
		MSDC_SET_FIELD(MSDC1_GPIO_MODE16, MSDC1_MODE_CLK_MASK,  0x1);
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;

	}
}
void msdc_set_smt_by_id(u32 id, int set_smt)
{
	switch (id) {
	case 0:
		MSDC_SET_FIELD(MSDC0_GPIO_SMT_ADDR, MSDC0_SMT_ALL_MASK,
			(set_smt ? 0xf : 0));
		break;
	case 1:
		MSDC_SET_FIELD(MSDC1_GPIO_SMT_ADDR, MSDC1_SMT_ALL_MASK,
			(set_smt ? 0x7 : 0));
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}
/* set pin ies:
 * 0: Disable
 * 1: Enable
 */
void msdc_set_ies_by_id(u32 id, int set_ies)
{
	switch (id) {
	case 0:
		MSDC_SET_FIELD(MSDC0_GPIO_IES_ADDR, MSDC0_IES_ALL_MASK,
			(set_ies ? 0xfff : 0));
		break;
	case 1:
		MSDC_SET_FIELD(MSDC1_GPIO_IES_ADDR, MSDC1_IES_ALL_MASK,
			(set_ies ? 0x3f : 0));
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}
/* msdc pin config
 * MSDC0
 * PUPD/RO/R1
 * 0/0/0: High-Z
 * 0/0/1: Pull-up with 50Kohm
 * 0/1/0: Pull-up with 10Kohm
 * 0/1/1: Pull-up with 10Kohm//50Kohm
 * 1/0/0: High-Z
 * 1/0/1: Pull-down with 50Kohm
 * 1/1/0: Pull-down with 10Kohm
 * 1/1/1: Pull-down with 10Kohm//50Kohm
 */
void msdc_pin_config_by_id(u32 id, u32 mode)
{
	switch (id) {
	case 0:
		/*Attention: don't pull CLK high; Don't toggle RST to prevent
		  from entering boot mode */
		if (MSDC_PIN_PULL_NONE == mode) {
			/* 0/0/0: High-Z */
			MSDC_SET_FIELD(MSDC0_GPIO_PUPD_ADDR,
					MSDC0_PUPD_ALL_MASK, 0x0);
			MSDC_SET_FIELD(MSDC0_GPIO_R0_ADDR,
					MSDC0_R0_ALL_MASK, 0x0);
			MSDC_SET_FIELD(MSDC0_GPIO_R1_ADDR,
					MSDC0_R1_ALL_MASK, 0x0);
		} else if (MSDC_PIN_PULL_DOWN == mode) {
			/* all:1/0/1: Pull-down with 50Kohm*/
			MSDC_SET_FIELD(MSDC0_GPIO_PUPD_ADDR,
					MSDC0_PUPD_ALL_MASK, 0xfff);
			MSDC_SET_FIELD(MSDC0_GPIO_R0_ADDR,
					MSDC0_R0_ALL_MASK, 0x0);
			MSDC_SET_FIELD(MSDC0_GPIO_R1_ADDR,
					MSDC0_R1_ALL_MASK, 0xfff);
		} else if (MSDC_PIN_PULL_UP == mode) {
			/* clk/dsl: 1/0/1: Pull-down with 50Kohm */
			MSDC_SET_FIELD(MSDC0_GPIO_PUPD_ADDR,
					MSDC0_PUPD_CLK_DSL_MASK, 0x3);
			MSDC_SET_FIELD(MSDC0_GPIO_R0_ADDR,
					MSDC0_R0_CLK_DSL_MASK, 0x0);
			MSDC_SET_FIELD(MSDC0_GPIO_R1_ADDR,
					MSDC0_R1_CLK_DSL_MASK, 0x3);
			/* cmd/dat: 0/1/0: Pull-up with 10Kohm*/
			MSDC_SET_FIELD(MSDC0_GPIO_PUPD_ADDR,
					MSDC0_PUPD_CMD_DAT_MASK, 0x0);
			MSDC_SET_FIELD(MSDC0_GPIO_R0_ADDR,
					MSDC0_R0_CMD_DAT_MASK, 0x1ff);
			MSDC_SET_FIELD(MSDC0_GPIO_R1_ADDR,
					MSDC0_R1_CMD_DAT_MASK, 0x0);
			/* rstb: 0/0/1: Pull-up with 50Kohm*/
			MSDC_SET_FIELD(MSDC0_GPIO_PUPD_ADDR,
					MSDC0_PUPD_RSTB_MASK, 0x0);
			MSDC_SET_FIELD(MSDC0_GPIO_R0_ADDR,
					MSDC0_R0_RSTB_MASK, 0x0);
			MSDC_SET_FIELD(MSDC0_GPIO_R1_ADDR,
					MSDC0_R1_RSTB_MASK, 0x1);
		}
		break;
	case 1:
		if (MSDC_PIN_PULL_NONE == mode) {
			/* high-Z */
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD_ADDR,
				MSDC1_PUPD_ALL_MASK, 0x0);
			MSDC_SET_FIELD(MSDC1_GPIO_R0_ADDR,
				MSDC1_R0_ALL_MASK, 0x0);
			MSDC_SET_FIELD(MSDC1_GPIO_R1_ADDR,
				MSDC1_R1_ALL_MASK, 0x0);
		} else if (MSDC_PIN_PULL_DOWN == mode) {
			/* ALL: 1/0/1: Pull-down with 50Kohm */
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD_ADDR,
				MSDC1_PUPD_ALL_MASK, 0x3f);
			MSDC_SET_FIELD(MSDC1_GPIO_R0_ADDR,
				MSDC1_R0_ALL_MASK, 0x0);
			MSDC_SET_FIELD(MSDC1_GPIO_R1_ADDR,
				MSDC1_R1_ALL_MASK, 0x3f);
		} else if (MSDC_PIN_PULL_UP == mode) {
			/* cmd/dat:0/0/1: Pull-up with 50Kohm */
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD_ADDR,
				MSDC1_PUPD_CMD_DAT_MASK, 0x0);
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD_ADDR,
				MSDC1_R0_CMD_DAT_MASK, 0x0);
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD_ADDR,
				MSDC1_R1_CMD_DAT_MASK, 0x1f);
			/* clk: 1/0/1: Pull-down with 50Kohm */
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD_ADDR,
				MSDC1_PUPD_CLK_MASK, 0x1);
			MSDC_SET_FIELD(MSDC1_GPIO_R0_ADDR,
				MSDC1_R0_CLK_MASK, 0x0);
			MSDC_SET_FIELD(MSDC1_GPIO_R1_ADDR,
				MSDC1_R1_CLK_MASK, 0x1);

		}
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}

}
void msdc_set_sr_by_id(u32 id, int clk, int cmd, int dat)
{
	switch (id) {
	case 0:
		/* clk */
		MSDC_SET_FIELD(MSDC0_GPIO_SR_ADDR, MSDC0_SR_CLK_MASK,
			(clk != 0));
		/* cmd dsl rstb */
		MSDC_SET_FIELD(MSDC0_GPIO_SR_ADDR, MSDC0_SR_CMD_DSL_RSTB_MASK,
			(cmd != 0));
		/* dat */
		MSDC_SET_FIELD(MSDC0_GPIO_SR_ADDR, MSDC0_SR_DAT3_0_MASK,
			(dat != 0));
		MSDC_SET_FIELD(MSDC0_GPIO_SR_ADDR, MSDC0_SR_DAT7_4_MASK,
			(dat != 0));

		break;
	case 1:
		MSDC_SET_FIELD(MSDC1_GPIO_SR_ADDR, MSDC1_SR_CMD_MASK,
			(cmd != 0));
		MSDC_SET_FIELD(MSDC1_GPIO_SR_ADDR, MSDC1_SR_CLK_MASK,
			(clk != 0));
		MSDC_SET_FIELD(MSDC1_GPIO_SR_ADDR, MSDC1_SR_DAT_MASK,
			(dat != 0));
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}

}
void msdc_set_tdsel_by_id(u32 id, bool sleep, bool sd_18)
{
	switch (id) {
	case 0:
		MSDC_SET_FIELD(MSDC0_GPIO_TDSEL_ADDR, MSDC0_TDSEL_ALL_MASK,
			(sleep ? 0xfff : 0x0));
		break;
	case 1:
		MSDC_SET_FIELD(MSDC1_GPIO_TDSEL_ADDR, MSDC1_TDSEL_ALL_MASK,
			(sleep ? 0xfff : 0xaaa));
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}

void msdc_set_rdsel_by_id(u32 id, bool sleep, bool sd_18)
{
	switch (id) {
	case 0:
		MSDC_SET_FIELD(MSDC0_GPIO_RDSEL_ADDR, MSDC0_RDSEL_ALL_MASK, 0);
		break;

	case 1:
		MSDC_SET_FIELD(MSDC1_GPIO_RDSEL_ADDR, MSDC1_RDSEL_ALL_MASK,
			(sd_18 ? 0 : 0xc30c));
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}

void msdc_set_tdsel_dbg_by_id(u32 id, u32 value)
{
	switch (id) {
	case 0:
		MSDC_SET_FIELD(MSDC0_GPIO_TDSEL_ADDR, MSDC0_TDSEL_ALL_MASK,
				value);
		break;
	case 1:
		MSDC_SET_FIELD(MSDC1_GPIO_TDSEL_ADDR, MSDC1_TDSEL_ALL_MASK,
				value);
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}

void msdc_set_rdsel_dbg_by_id(u32 id, u32 value)
{
	switch (id) {
	case 0:
		MSDC_SET_FIELD(MSDC0_GPIO_RDSEL_ADDR, MSDC0_RDSEL_ALL_MASK,
				value);
		break;
	case 1:
		MSDC_SET_FIELD(MSDC1_GPIO_RDSEL_ADDR, MSDC1_RDSEL_ALL_MASK,
				value);
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}

void msdc_get_tdsel_dbg_by_id(u32 id, u32 *value)
{
	switch (id) {
	case 0:
		MSDC_GET_FIELD(MSDC0_GPIO_TDSEL_ADDR, MSDC0_TDSEL_ALL_MASK,
				*value);
		break;
	case 1:
		MSDC_GET_FIELD(MSDC1_GPIO_TDSEL_ADDR, MSDC1_TDSEL_ALL_MASK,
				*value);
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}

void msdc_get_rdsel_dbg_by_id(u32 id, u32 *value)
{
	switch (id) {
	case 0:
		MSDC_GET_FIELD(MSDC0_GPIO_RDSEL_ADDR, MSDC0_RDSEL_ALL_MASK,
				*value);
		break;
	case 1:
		MSDC_GET_FIELD(MSDC1_GPIO_RDSEL_ADDR, MSDC1_RDSEL_ALL_MASK,
				*value);
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}
/*
 * Set pin driving:
 * MSDC0,MSDC1
 * 000: 2mA
 * 001: 4mA
 * 010: 6mA
 * 011: 8mA
 * 100: 10mA
 * 101: 12mA
 * 110: 14mA
 * 111: 16mA
 */
void msdc_set_driving_by_id(u32 id, struct msdc_hw *hw, bool sd_18)
{
	switch (id) {
	case 0:
		MSDC_SET_FIELD(MSDC0_GPIO_DRV_ADDR, MSDC0_DRV_CMD_DSL_RSTB_MASK,
			hw->cmd_drv);
		MSDC_SET_FIELD(MSDC0_GPIO_DRV_ADDR, MSDC0_DRV_CLK_MASK,
			hw->clk_drv);
		MSDC_SET_FIELD(MSDC0_GPIO_DRV_ADDR, MSDC0_DRV_DAT_MASK,
			hw->dat_drv);
		break;
	case 1:
		if (sd_18) {
			MSDC_SET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_CMD_MASK,
				hw->cmd_drv_sd_18);
			MSDC_SET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_CLK_MASK,
				hw->clk_drv_sd_18);
			MSDC_SET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_DAT_MASK,
				hw->dat_drv_sd_18);
		} else {
			MSDC_SET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_CMD_MASK,
				hw->cmd_drv);
			MSDC_SET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_CLK_MASK,
				hw->clk_drv);
			MSDC_SET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_DAT_MASK,
				hw->dat_drv);
		}
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}
void msdc_set_driving(struct msdc_host *host, struct msdc_hw *hw, bool sd_18)
{
	msdc_set_driving_by_id(host->id, hw, sd_18);
}
void msdc_get_driving_by_id(u32 id, struct msdc_hw *hw)
{
	switch (id) {
	case 0:
		MSDC_GET_FIELD(MSDC0_GPIO_DRV_ADDR, MSDC0_DRV_CMD_DSL_RSTB_MASK,
			hw->cmd_drv);
		MSDC_GET_FIELD(MSDC0_GPIO_DRV_ADDR, MSDC0_DRV_CLK_MASK,
			hw->clk_drv);
		MSDC_GET_FIELD(MSDC0_GPIO_DRV_ADDR, MSDC0_DRV_DAT_MASK,
			hw->dat_drv);
		hw->ds_drv = hw->cmd_drv;
		hw->rst_drv = hw->cmd_drv;
		break;
	case 1:
		MSDC_GET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_CMD_MASK,
			hw->cmd_drv);
		MSDC_GET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_CLK_MASK,
			hw->clk_drv);
		MSDC_GET_FIELD(MSDC1_GPIO_DRV_ADDR, MSDC1_DRV_DAT_MASK,
			hw->dat_drv);
		break;
	default:
		pr_err("[%s] invalid host->id!\n", __func__);
		break;
	}
}
/**************************************************************/
/* Section 5: MISC                                            */
/**************************************************************/

void dump_axi_bus_info(void)
{
	pr_err("=============== AXI BUS INFO =============");
	return; /*weiping check*/

	if (infracfg_ao_reg_base && infracfg_reg_base && pericfg_reg_base) {
		pr_err("reg[0x10001224]=0x%x",
			MSDC_READ32(infracfg_ao_reg_base + 0x224));
		pr_err("reg[0x10201000]=0x%x",
			MSDC_READ32(infracfg_reg_base + 0x000));
		pr_err("reg[0x10201018]=0x%x",
			MSDC_READ32(infracfg_reg_base + 0x018));
		pr_err("reg[0x1000320c]=0x%x",
			MSDC_READ32(pericfg_reg_base + 0x20c));
		pr_err("reg[0x10003210]=0x%x",
			MSDC_READ32(pericfg_reg_base + 0x210));
		pr_err("reg[0x10003214]=0x%x",
			MSDC_READ32(pericfg_reg_base + 0x214));
	} else {
		pr_err("infracfg_ao_reg_base = %p, infracfg_reg_base = %p, pericfg_reg_base = %p\n",
			infracfg_ao_reg_base,
			infracfg_reg_base,
			pericfg_reg_base);
	}
}

void dump_emi_info(void)
{
	unsigned int i = 0;
	unsigned int addr = 0;

	return; /*weiping check */

	pr_err("=============== EMI INFO =============");
	if (!emi_reg_base) {
		pr_err("emi_reg_base = %p\n", emi_reg_base);
		return;
	}

	pr_err("before, reg[0x102034e8]=0x%x",
		MSDC_READ32(emi_reg_base + 0x4e8));
	pr_err("before, reg[0x10203400]=0x%x",
		MSDC_READ32(emi_reg_base + 0x400));
	MSDC_WRITE32(emi_reg_base + 0x4e8, 0x2000000);
	MSDC_WRITE32(emi_reg_base + 0x400, 0xff0001);
	pr_err("after, reg[0x102034e8]=0x%x",
		MSDC_READ32(emi_reg_base + 0x4e8));
	pr_err("after, reg[0x10203400]=0x%x",
		MSDC_READ32(emi_reg_base + 0x400));

	for (i = 0; i < 5; i++) {
		for (addr = 0; addr < 0x78; addr += 4) {
			pr_err("reg[0x%x]=0x%x", (0x10203500 + addr),
				MSDC_READ32((emi_reg_base + 0x500 + addr)));
			if (addr % 0x10 == 0)
				mdelay(1);
		}
	}
}

void msdc_polling_axi_status(int line, int dead)
{
	int i = 0;

	if (!pericfg_reg_base) {
		pr_err("pericfg_reg_base = %p\n", pericfg_reg_base);
		return;
	}

	while (MSDC_READ32(pericfg_reg_base + 0x214) & 0xc) {
		if (++i < 300) {
			mdelay(10);
			continue;
		}
		pr_err("[%s]: check peri-bus: 0x%x at %d\n", __func__,
			MSDC_READ32(pericfg_reg_base + 0x214), line);

		pr_err("############## AXI bus hang! start ##################");
		dump_emi_info();
		mdelay(10);

		dump_axi_bus_info();
		mdelay(10);

		/*dump_audio_info();
		mdelay(10);*/

		msdc_dump_info(0);
		mdelay(10);

		msdc_dump_gpd_bd(0);
		pr_err("############## AXI bus hang! end ####################");
		if (dead == 0)
			break;
		i = 0;
	}
}
#endif /*if !defined(FPGA_PLATFORM)*/
