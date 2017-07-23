#ifndef __GF_SPI_TEE_H
#define __GF_SPI_TEE_H

#include <linux/types.h>
#include <linux/netlink.h>
#include "gf_spi_tee.h"


/**********************feature defination**********************/
#undef GF_FASYNC    /* If support fasync mechanism */
#define GF_NETLINK	/* If support netlink mechanism */

/* #undef GF_SPI_REE_OPERATION */ /* If support SPI read in REE environment */
#define GF_SPI_REE_OPERATION /* If support SPI read in REE environment */

/**********************IO Magic**********************/
#define GF_IOC_MAGIC	'g'

enum gf_key_event {
	GF_KEY_NONE = 0,
	GF_KEY_HOME,
	GF_KEY_POWER,
	GF_KEY_CAPTURE,
};

struct gf_key {
	enum gf_key_event key;
	uint32_t value;   /* key down = 1, key up = 0 */
};

enum gf_netlink_cmd {
	GF_NETLINK_TEST = 0,
	GF_NETLINK_IRQ = 1,
	GF_NETLINK_SCREEN_OFF,
	GF_NETLINK_SCREEN_ON
};

/* define commands */
#define GF_IOC_INIT			_IO(GF_IOC_MAGIC, 0)
#define GF_IOC_EXIT			_IO(GF_IOC_MAGIC, 1)
#define GF_IOC_RESET			_IO(GF_IOC_MAGIC, 2)

#define GF_IOC_ENABLE_IRQ		_IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ		_IO(GF_IOC_MAGIC, 4)

#define GF_IOC_ENABLE_SPI_CLK		_IO(GF_IOC_MAGIC, 5)
#define GF_IOC_DISABLE_SPI_CLK		_IO(GF_IOC_MAGIC, 6)

#define GF_IOC_ENABLE_POWER		_IO(GF_IOC_MAGIC, 7)
#define GF_IOC_DISABLE_POWER		_IO(GF_IOC_MAGIC, 8)

#define GF_IOC_INPUT_KEY_EVENT		_IOW(GF_IOC_MAGIC, 9, struct gf_key)

/* fp sensor has change to sleep mode while screen off */
#define GF_IOC_ENTER_SLEEP_MODE		_IO(GF_IOC_MAGIC, 10)

#define  GF_IOC_MAXNR    11  /* THIS MACRO IS NOT USED NOW... */

struct gf_dev {
	dev_t devt;
	struct device *dev;
	spinlock_t	spi_lock;
	struct spi_device *spi;
	struct list_head device_entry;

	struct input_dev *input;

	/* buffer is NULL unless this device is open (users > 0) */
	unsigned users;
	u8 *buffer;
	struct mutex buf_lock;
	u8 buf_status;
	u8 device_available;	/* changed during fingerprint chip sleep and wakeup phase */

#ifdef GF_FASYNC
	/* fasync support used */
	struct fasync_struct *async;
#endif

#ifdef GF_NETLINK
	/* for netlink use */
	struct sock *nl_sk;
	int pid;
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#else
	struct notifier_block notifier;
#endif

	u8 probe_finish;
	u8 irq_count;

	/* bit24-bit32 of signal count */
	/* bit16-bit23 of event type, 1: key down; 2: key up; 3: fp data ready; 4: home key */
	/* bit0-bit15 of event type, buffer status register */
	u32 event_type;
	u8 sig_count;
	u8 is_sleep_mode;

	u32 cs_gpio;
	u32 reset_gpio;
	u32 irq_gpio;
	u32 irq_num;

#ifdef CONFIG_OF
	struct pinctrl *pinctrl_gpios;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_miso_spi, *pins_miso_pullhigh, *pins_miso_pulllow;
	struct pinctrl_state *pins_reset_high, *pins_reset_low;
#endif
};

#endif	/* __GF_SPI_TEE_H */
