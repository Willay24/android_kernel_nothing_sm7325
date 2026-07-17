// SPDX-License-Identifier: GPL-2.0
/*
 * TEE driver for goodix fingerprint sensor
 * Copyright (C) 2016 Goodix
 * Copyright (C) 2022 Jebaitedneko <Jebaitedneko@gmail.com>
 */

#include <drm/drm_panel.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of_gpio.h>
#include <linux/pm_wakeup.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <net/netlink.h>
#include <net/sock.h>

static struct gf_dev {
	dev_t devt;
	struct spi_device *spi;
	struct input_dev *input;
	struct wakeup_source *fp_wakelock;
	struct notifier_block notifier;
	struct drm_panel *panel;
	signed irq_gpio, rst_gpio, pwr_gpio;
	int irq, irq_enabled, pwr_req, ready, nb_reg, available, users;
} gf;

#define MAX_MSGSIZE 32
static struct sock *nl_sk;
static u32 pid;
static inline void sendnlmsg(u8 event)
{
	struct sk_buff *skb_1;
	struct nlmsghdr *nlh;

	if (!nl_sk || !pid)
		return;
	skb_1 = alloc_skb(NLMSG_SPACE(MAX_MSGSIZE), GFP_ATOMIC);
	if (!skb_1)
		return;
	nlh = nlmsg_put(skb_1, 0, 0, 0, MAX_MSGSIZE, 0);
	if (!nlh) {
		kfree_skb(skb_1);
		return;
	}
	memset(NLMSG_DATA(nlh), 0, MAX_MSGSIZE);
	*(u8 *)NLMSG_DATA(nlh) = event;
	NETLINK_CB(skb_1).portid = NETLINK_CB(skb_1).dst_group = 0;
	netlink_unicast(nl_sk, skb_1, pid, MSG_DONTWAIT);
}

static inline void nl_data_ready(struct sk_buff *skb)
{
	if (skb->len >= NLMSG_HDRLEN)
		pid = nlmsg_hdr(skb)->nlmsg_pid;
}

#define NETLINK_TEST 25
static inline void netlink_init(void)
{
	struct netlink_kernel_cfg netlink_cfg;

	memset(&netlink_cfg, 0, sizeof(netlink_cfg));
	netlink_cfg.input = nl_data_ready;
	nl_sk = netlink_kernel_create(&init_net, NETLINK_TEST, &netlink_cfg);
}

static inline void netlink_exit(void)
{
	if (nl_sk) {
		netlink_kernel_release(nl_sk);
		nl_sk = NULL;
	}
}

static inline void irq_switch(struct gf_dev *gf_dev, int status)
{
	if (!gf_dev->ready || gf_dev->irq_enabled == status)
		return;
	if (status)
		enable_irq(gf_dev->irq);
	else
		disable_irq(gf_dev->irq);
	gf_dev->irq_enabled = status;
}

#define WAKELOCK_HOLD_TIME 500
static inline irqreturn_t gf_irq(int irq, void *handle)
{
	struct gf_dev *gf_dev = handle;

	__pm_wakeup_event(gf_dev->fp_wakelock, WAKELOCK_HOLD_TIME);
	return IRQ_WAKE_THREAD;
}

static inline irqreturn_t gf_irq_thread(int irq, void *handle)
{
	sendnlmsg(1);
	return IRQ_HANDLED;
}

static inline void gpio_reset(struct gf_dev *gf_dev)
{
	if (!gf_dev->ready)
		return;
	gpio_set_value(gf_dev->rst_gpio, 0);
	usleep_range(11000, 12000);
	gpio_set_value(gf_dev->rst_gpio, 1);
	usleep_range(3000, 4000);
}

static inline void gpio_power(struct gf_dev *gf_dev, int on)
{
	if (gpio_is_valid(gf_dev->pwr_gpio))
		gpio_direction_output(gf_dev->pwr_gpio, on);
}

static inline void gf_free_gpios(struct gf_dev *gf_dev)
{
	if (gpio_is_valid(gf_dev->pwr_gpio)) {
		gpio_direction_output(gf_dev->pwr_gpio, 0);
		if (gf_dev->pwr_req)
			gpio_free(gf_dev->pwr_gpio);
		gf_dev->pwr_req = 0;
	}
	gpio_free(gf_dev->irq_gpio);
	gpio_free(gf_dev->rst_gpio);
}

static inline int gf_setup(struct gf_dev *gf_dev)
{
	struct device_node *np = gf_dev->spi->dev.of_node;
	int ret;

	gf_dev->rst_gpio = of_get_named_gpio(np, "fp-gpio-reset", 0);
	gf_dev->irq_gpio = of_get_named_gpio(np, "fp-gpio-irq", 0);
	gf_dev->pwr_gpio = of_get_named_gpio(np, "fp-gpio-power", 0);
	if (!gpio_is_valid(gf_dev->rst_gpio) || !gpio_is_valid(gf_dev->irq_gpio))
		return -EINVAL;

	ret = gpio_request(gf_dev->rst_gpio, "goodix_reset");
	if (ret)
		return ret;
	gpio_direction_output(gf_dev->rst_gpio, 0);

	ret = gpio_request(gf_dev->irq_gpio, "goodix_irq");
	if (ret)
		goto err_rst;
	gpio_direction_input(gf_dev->irq_gpio);

	if (gpio_is_valid(gf_dev->pwr_gpio)) {
		gf_dev->pwr_req = !gpio_request(gf_dev->pwr_gpio, "goodix_power");
		gpio_direction_output(gf_dev->pwr_gpio, 1);
		msleep(10);
	}

	gf_dev->irq = gpio_to_irq(gf_dev->irq_gpio);
	if (gf_dev->irq < 0) {
		ret = gf_dev->irq;
		goto err_gpio;
	}
	ret = request_threaded_irq(gf_dev->irq, gf_irq, gf_irq_thread,
				   IRQF_TRIGGER_RISING | IRQF_ONESHOT, "gf",
				   gf_dev);
	if (ret)
		goto err_gpio;
	enable_irq_wake(gf_dev->irq);
	gf_dev->irq_enabled = 1;
	gf_dev->ready = 1;
	return 0;

err_gpio:
	gf_free_gpios(gf_dev);
	return ret;
err_rst:
	gpio_free(gf_dev->rst_gpio);
	return ret;
}

static inline void gf_cleanup(struct gf_dev *gf_dev)
{
	if (!gf_dev->ready)
		return;
	disable_irq_wake(gf_dev->irq);
	free_irq(gf_dev->irq, gf_dev);
	gf_dev->irq_enabled = 0;
	gf_free_gpios(gf_dev);
	gf_dev->ready = 0;
}

static inline int gf_find_panel(struct gf_dev *gf_dev)
{
	struct device_node *np = gf_dev->spi->dev.of_node;
	struct device_node *node;
	struct drm_panel *panel;
	int i, count = of_count_phandle_with_args(np, "panel", NULL);

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			gf_dev->panel = panel;
			return 0;
		}
	}
	return -ENODEV;
}

static inline int gf_fb_notifier(struct notifier_block *nb,
				 unsigned long val, void *data)
{
	struct gf_dev *gf_dev = container_of(nb, struct gf_dev, notifier);
	struct drm_panel_notifier *evdata = data;

	if (val != DRM_PANEL_EARLY_EVENT_BLANK)
		return 0;
	if (!evdata || !evdata->data || !READ_ONCE(gf_dev->available))
		return NOTIFY_OK;
	switch (*(int *)evdata->data) {
	case DRM_PANEL_BLANK_POWERDOWN:
		sendnlmsg(2);
		break;
	case DRM_PANEL_BLANK_UNBLANK:
		sendnlmsg(3);
		break;
	}
	return NOTIFY_OK;
}

enum gf_key_event {
	GF_KEY_NONE,
	GF_KEY_HOME,
	GF_KEY_POWER,
	GF_KEY_MENU,
	GF_KEY_BACK,
	GF_KEY_CAMERA,
};

struct gf_key {
	enum gf_key_event key;
	u32 value;
};

enum gf_nav_event {
	GF_NAV_NONE,
	GF_NAV_FINGER_UP,
	GF_NAV_FINGER_DOWN,
	GF_NAV_UP,
	GF_NAV_DOWN,
	GF_NAV_LEFT,
	GF_NAV_RIGHT,
	GF_NAV_CLICK,
	GF_NAV_HEAVY,
	GF_NAV_LONG_PRESS,
	GF_NAV_DOUBLE_CLICK,
};

static const unsigned int gf_keys[] = {
	KEY_HOME, KEY_MENU, KEY_BACK, KEY_POWER, KEY_CAMERA, KEY_UP, KEY_DOWN,
	KEY_LEFT, KEY_RIGHT, KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_SEARCH, KEY_CHAT,
};

static inline void key_tap(struct input_dev *input, unsigned int code)
{
	input_report_key(input, code, 1);
	input_sync(input);
	input_report_key(input, code, 0);
	input_sync(input);
}

static inline void gf_key_input(struct gf_dev *gf_dev, struct gf_key *k)
{
	if (k->key == GF_KEY_HOME) {
		input_report_key(gf_dev->input, KEY_HOME, k->value);
		input_sync(gf_dev->input);
	} else if (k->key == GF_KEY_POWER && k->value == 1) {
		key_tap(gf_dev->input, KEY_POWER);
	} else if (k->key == GF_KEY_CAMERA && k->value == 1) {
		key_tap(gf_dev->input, KEY_CAMERA);
	}
}

static inline void gf_nav_input(struct gf_dev *gf_dev, enum gf_nav_event ev)
{
	switch (ev) {
	case GF_NAV_UP:
		key_tap(gf_dev->input, KEY_UP);
		break;
	case GF_NAV_DOWN:
		key_tap(gf_dev->input, KEY_DOWN);
		break;
	case GF_NAV_LEFT:
		key_tap(gf_dev->input, KEY_LEFT);
		break;
	case GF_NAV_RIGHT:
		key_tap(gf_dev->input, KEY_RIGHT);
		break;
	case GF_NAV_CLICK:
		key_tap(gf_dev->input, KEY_VOLUMEDOWN);
		break;
	case GF_NAV_HEAVY:
		key_tap(gf_dev->input, KEY_CHAT);
		break;
	case GF_NAV_LONG_PRESS:
		key_tap(gf_dev->input, KEY_SEARCH);
		break;
	case GF_NAV_DOUBLE_CLICK:
		key_tap(gf_dev->input, KEY_VOLUMEUP);
		break;
	default:
		break;
	}
}

#define GF_IOC_MAGIC 'g'
#define GF_IOC_INIT _IOR(GF_IOC_MAGIC, 0, uint8_t)
#define GF_IOC_RESET _IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ _IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ _IO(GF_IOC_MAGIC, 4)
#define GF_IOC_ENABLE_POWER _IO(GF_IOC_MAGIC, 7)
#define GF_IOC_DISABLE_POWER _IO(GF_IOC_MAGIC, 8)
#define GF_IOC_INPUT_KEY_EVENT _IOW(GF_IOC_MAGIC, 9, struct gf_key)
#define GF_IOC_REMOVE _IO(GF_IOC_MAGIC, 12)
#define GF_IOC_NAV_EVENT _IOW(GF_IOC_MAGIC, 14, enum gf_nav_event)

static DEFINE_MUTEX(gf_lock);
static inline long gf_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct gf_dev *gf_dev = filp->private_data;
	void __user *argp = (void __user *)arg;
	long ret = 0;

	if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -ENOTTY;
	if (!gf_dev)
		return -ENODEV;

	mutex_lock(&gf_lock);
	switch (cmd) {
	case GF_IOC_INIT: {
		u8 netlink_route = NETLINK_TEST;

		if (copy_to_user(argp, &netlink_route, sizeof(u8)))
			ret = -EFAULT;
		break;
	}
	case GF_IOC_DISABLE_IRQ:
		irq_switch(gf_dev, 0);
		break;
	case GF_IOC_ENABLE_IRQ:
		irq_switch(gf_dev, 1);
		break;
	case GF_IOC_RESET:
		gpio_reset(gf_dev);
		break;
	case GF_IOC_ENABLE_POWER:
		gpio_power(gf_dev, 1);
		break;
	case GF_IOC_DISABLE_POWER:
		gpio_power(gf_dev, 0);
		break;
	case GF_IOC_INPUT_KEY_EVENT: {
		struct gf_key key;

		if (copy_from_user(&key, argp, sizeof(key)))
			ret = -EFAULT;
		else
			gf_key_input(gf_dev, &key);
		break;
	}
	case GF_IOC_NAV_EVENT: {
		enum gf_nav_event ev;

		if (copy_from_user(&ev, argp, sizeof(ev)))
			ret = -EFAULT;
		else
			gf_nav_input(gf_dev, ev);
		break;
	}
	case GF_IOC_REMOVE:
		gf_cleanup(gf_dev);
		break;
	default:
		break;
	}
	mutex_unlock(&gf_lock);
	return ret;
}

#ifdef CONFIG_COMPAT
static inline long gf_compat_ioctl(struct file *filp, unsigned int cmd,
				   unsigned long arg)
{
	return gf_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static inline int gf_open(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev = &gf;
	int ret = 0;

	mutex_lock(&gf_lock);
	if (!gf_dev->spi) {
		ret = -ENODEV;
		goto out;
	}
	if (!gf_dev->users) {
		ret = gf_setup(gf_dev);
		if (ret)
			goto out;
		if (!gf_find_panel(gf_dev) &&
		    !drm_panel_notifier_register(gf_dev->panel, &gf_dev->notifier))
			gf_dev->nb_reg = 1;
		gf_dev->available = 1;
	}
	gf_dev->users++;
	filp->private_data = gf_dev;
	nonseekable_open(inode, filp);
out:
	mutex_unlock(&gf_lock);
	return ret;
}

static inline int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev = filp->private_data;

	mutex_lock(&gf_lock);
	filp->private_data = NULL;
	if (gf_dev && gf_dev->users && !--gf_dev->users) {
		gf_dev->available = 0;
		if (gf_dev->nb_reg) {
			drm_panel_notifier_unregister(gf_dev->panel,
						      &gf_dev->notifier);
			gf_dev->nb_reg = 0;
		}
		gf_cleanup(gf_dev);
	}
	mutex_unlock(&gf_lock);
	return 0;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gf_compat_ioctl,
#endif
	.open = gf_open,
	.release = gf_release,
};

static struct class *gf_class;
static int SPIDEV_MAJOR;
#define GF_DEV_NAME "goodix_fp"
#define GF_INPUT_NAME "qwerty"
static inline int gf_probe(struct spi_device *spi)
{
	struct gf_dev *gf_dev = &gf;
	struct device *dev;
	int i, ret;

	gf_dev->irq_gpio = gf_dev->rst_gpio = gf_dev->pwr_gpio = -EINVAL;
	gf_dev->notifier.notifier_call = gf_fb_notifier;

	gf_dev->fp_wakelock = wakeup_source_register(&spi->dev, "fp_wakelock");
	if (!gf_dev->fp_wakelock)
		return -ENOMEM;

	gf_dev->input = input_allocate_device();
	if (!gf_dev->input) {
		ret = -ENOMEM;
		goto err_ws;
	}
	for (i = 0; i < ARRAY_SIZE(gf_keys); i++)
		input_set_capability(gf_dev->input, EV_KEY, gf_keys[i]);
	gf_dev->input->name = GF_INPUT_NAME;
	ret = input_register_device(gf_dev->input);
	if (ret) {
		input_free_device(gf_dev->input);
		goto err_ws;
	}

	gf_dev->spi = spi;
	gf_dev->devt = MKDEV(SPIDEV_MAJOR, 0);
	dev = device_create(gf_class, &spi->dev, gf_dev->devt, gf_dev,
			    GF_DEV_NAME);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		gf_dev->spi = NULL;
		input_unregister_device(gf_dev->input);
		goto err_ws;
	}
	return 0;

err_ws:
	wakeup_source_unregister(gf_dev->fp_wakelock);
	gf_dev->fp_wakelock = NULL;
	return ret;
}

static inline int gf_remove(struct spi_device *spi)
{
	struct gf_dev *gf_dev = &gf;

	mutex_lock(&gf_lock);
	gf_dev->spi = NULL;
	gf_dev->available = 0;
	if (gf_dev->nb_reg) {
		drm_panel_notifier_unregister(gf_dev->panel, &gf_dev->notifier);
		gf_dev->nb_reg = 0;
	}
	gf_cleanup(gf_dev);
	mutex_unlock(&gf_lock);

	device_destroy(gf_class, gf_dev->devt);
	input_unregister_device(gf_dev->input);
	wakeup_source_unregister(gf_dev->fp_wakelock);
	gf_dev->fp_wakelock = NULL;
	return 0;
}

static const struct of_device_id gx_match_table[] = {
	{ .compatible = "goodix,fingerprint" },
	{},
};

static struct spi_driver gf_driver = {
	.driver = {
		.name = GF_DEV_NAME,
		.of_match_table = gx_match_table,
	},
	.probe = gf_probe,
	.remove = gf_remove,
};

#define CHRD_DRIVER_NAME "goodix_fp_spi"
#define CLASS_NAME "goodix_fp"
static inline int __init gf_init(void)
{
	int status = register_chrdev(0, CHRD_DRIVER_NAME, &gf_fops);

	if (status < 0)
		return status;
	SPIDEV_MAJOR = status;
	gf_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(gf_class)) {
		unregister_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME);
		return PTR_ERR(gf_class);
	}
	netlink_init();
	status = spi_register_driver(&gf_driver);
	if (status) {
		netlink_exit();
		class_destroy(gf_class);
		unregister_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME);
	}
	return status;
}
module_init(gf_init);

static inline void __exit gf_exit(void)
{
	spi_unregister_driver(&gf_driver);
	netlink_exit();
	class_destroy(gf_class);
	unregister_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME);
}
module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_AUTHOR("Jandy Gou, <gouqingsong@goodix.com>");
MODULE_AUTHOR("Jebaitedneko, <Jebaitedneko@gmail.com>");
MODULE_DESCRIPTION("goodix fingerprint sensor device driver");
MODULE_LICENSE("GPL");
