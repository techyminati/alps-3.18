#include <linux/io.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#if defined(CONFIG_MTK_AEE_FEATURE)
#include <linux/aee.h>
#include <linux/mtk_ram_console.h>
#include <linux/irqchip/arm-gic.h>
#endif
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#if defined(CONFIG_FIQ_GLUE)
#include <asm/fiq.h>
#include <asm/fiq_glue.h>
#endif
#include <mach/irqs.h>
#include <mt-plat/sync_write.h>
#include <mach/mt_secure_api.h>

void __iomem *GIC_DIST_BASE;
void __iomem *GIC_CPU_BASE;
void __iomem *INT_POL_CTL0;


