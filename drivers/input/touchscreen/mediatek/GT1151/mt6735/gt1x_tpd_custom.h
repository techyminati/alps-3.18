/* drivers/input/touchscreen/gt1x_tpd_custom.h
 *
 * 2010 - 2014 Goodix Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Version: 1.0
 * Revision Record:
 *      V1.0:  first release. 2014/09/28.
 */

#ifndef GT1X_TPD_CUSTOM_H__
#define GT1X_TPD_CUSTOM_H__

#include <asm/uaccess.h>
#ifdef CONFIG_MTK_BOOT
#include <mach/mt_boot.h>
#endif
#include "tpd.h"
#include "upmu_common.h"

#define TPD_I2C_NUMBER		        1
#define TPD_SUPPORT_I2C_DMA         0	/* if gt9l, better enable it if hardware platform supported*/

#if defined(CONFIG_MTK_LEGACY)
#define TPD_POWER_SOURCE_CUSTOM	MT6328_POWER_LDO_VGP1
#endif

#define GTP_GPIO_AS_INT(pin) tpd_gpio_as_int(pin)
#define GTP_GPIO_OUTPUT(pin, level) tpd_gpio_output(pin, level)

#define IIC_MAX_TRANSFER_SIZE         8
#define IIC_DMA_MAX_TRANSFER_SIZE     250
#define I2C_MASTER_CLOCK              300

#define TPD_MAX_RESET_COUNT           3

#define TPD_HAVE_CALIBRATION
#define TPD_CALIBRATION_MATRIX        {962, 0, 0, 0, 1600, 0, 0, 0}

extern void tpd_on(void);
extern void tpd_off(void);

#endif /* GT1X_TPD_CUSTOM_H__ */
