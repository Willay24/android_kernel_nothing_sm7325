// SPDX-License-Identifier: GPL-2.0-only
/*
 * USB connector temperature short-protection driver for Nothing Phone (1)
 *
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/notifier.h>
#include <linux/pm_wakeup.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/thermal.h>

struct usb_temp_device_info {
	struct device		*dev;
	struct workqueue_struct	*usb_temp_wq;
	struct work_struct	usb_temp_check_wk;
	struct work_struct	usb_state_wk;
	struct notifier_block	psy_nb;
	struct power_supply	*usb_psy;
	struct power_supply	*batt_psy;
	struct wakeup_source	*ws;
	struct hrtimer		timer;
	struct thermal_zone_device *tzd;
	int	gpio_usb_temp;
	int	need_protect_temp;
	int	need_recover_temp;
	int	open_mosfet_temp;
	int	close_mosfet_temp;
	int	interval_switch_temp;
	int	check_interval;
	int	keep_check_cnt;
	int	no_need_usb_temp;
	bool	wake_active;
};

#define	INVALID_DELAY_TIME		0
#define	USB_TEMP_INSERT_CHG_CNT		1100
#define	USB_TEMP_START_CHK_CNT		0
#define	USB_TEMP_END_CHK_CNT		1001
#define	USB_TEMP_CNT			2
#define	FAST_MONITOR_INTERVAL		300	/* ms */
#define	NORMAL_MONITOR_INTERVAL		1000	/* ms */
#define	INVALID_DELTA_TEMP		0
#define	USB_TEMP_CHK_CNT_STEP		1
#define	USB_TEMP_DEFAULT_CHK_CNT	(-1)
#define	GPIO_HIGH			1
#define	GPIO_LOW			0
#define	INTERVAL_0			0
#define	INVALID_BATT_TEMP		(-255)
#define	TUSB_TEMP_UPPER_LIMIT		100
#define	TUSB_TEMP_LOWER_LIMIT		(-30)

/* Same custom values the glink charger uses (QTI HVDCP range) */
#define	NT_USB_TYPE_HVDCP		0x80
#define	NT_USB_TYPE_HVDCP_3		0x81
#define	NT_USB_TYPE_HVDCP_3P5		0x82

#define	USB_TEMP_ZONE_DEFAULT		"usb_therm"

#undef	pr_info
#define	pr_info	pr_debug

static int protect_enable;
static int protect_dmd_notify_enable = 1;
static int is_usb_protect_mode;

static struct usb_temp_device_info *g_di;

static bool is_factory_mode;
static int __init early_parse_factory_mode(char *cmdline)
{
	if (cmdline && !strncmp(cmdline, "ffbm", strlen("ffbm")))
		is_factory_mode = true;
	return 0;
}
early_param("androidboot.mode", early_parse_factory_mode);

static void usb_temp_wake_lock(struct usb_temp_device_info *di)
{
	if (!di->wake_active) {
		pr_info("usb_temp.wake lock\n");
		__pm_stay_awake(di->ws);
		di->wake_active = true;
	}
}

static void usb_temp_wake_unlock(struct usb_temp_device_info *di)
{
	if (di->wake_active) {
		pr_info("usb_temp.wake unlock\n");
		__pm_relax(di->ws);
		di->wake_active = false;
	}
}

/* Returns true if a charging-capable USB source is attached */
static bool usb_is_charging(struct usb_temp_device_info *di)
{
	union power_supply_propval val = {0};
	int usb_type;

	if (!di->usb_psy)
		return false;

	if (power_supply_get_property(di->usb_psy, POWER_SUPPLY_PROP_ONLINE,
				      &val) || !val.intval)
		return false;

	if (power_supply_get_property(di->usb_psy, POWER_SUPPLY_PROP_USB_TYPE,
				      &val))
		return true; /* online but type unreadable: assume charging */

	usb_type = val.intval;
	switch (usb_type) {
	case POWER_SUPPLY_USB_TYPE_SDP:
	case POWER_SUPPLY_USB_TYPE_DCP:
	case POWER_SUPPLY_USB_TYPE_CDP:
	case POWER_SUPPLY_USB_TYPE_ACA:
	case POWER_SUPPLY_USB_TYPE_C:
	case POWER_SUPPLY_USB_TYPE_PD:
	case POWER_SUPPLY_USB_TYPE_PD_DRP:
	case POWER_SUPPLY_USB_TYPE_PD_PPS:
	case POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID:
	case NT_USB_TYPE_HVDCP:
	case NT_USB_TYPE_HVDCP_3:
	case NT_USB_TYPE_HVDCP_3P5:
		return true;
	default:
		return false;
	}
}

static void charge_type_handler(struct usb_temp_device_info *di, bool charging)
{
	static bool first_time = true;

	if (!protect_enable || !di)
		return;

	if (!charging) {
		pr_debug("usb_temp.no charging source, do nothing\n");
		return;
	}

	if (hrtimer_active(&di->timer)) {
		pr_debug("usb_temp.timer already working, do nothing\n");
		return;
	}

	pr_info("usb_temp.start usb_temp check\n");
	di->keep_check_cnt = USB_TEMP_INSERT_CHG_CNT;
	if (first_time) {
		/* first boot: delay 10s for power-on plug-in */
		hrtimer_start(&di->timer,
			      ktime_set(10000 / MSEC_PER_SEC,
					(10000 % MSEC_PER_SEC) * USEC_PER_SEC),
			      HRTIMER_MODE_REL);
		first_time = false;
	} else {
		hrtimer_start(&di->timer,
			      ktime_set(INTERVAL_0 / MSEC_PER_SEC,
					(INTERVAL_0 % MSEC_PER_SEC) * USEC_PER_SEC),
			      HRTIMER_MODE_REL);
	}
}

static void usb_temp_update_state_work(struct work_struct *work)
{
	struct usb_temp_device_info *di =
		container_of(work, struct usb_temp_device_info, usb_state_wk);

	charge_type_handler(di, usb_is_charging(di));
}

static int usb_temp_psy_notifier(struct notifier_block *nb, unsigned long event,
				 void *data)
{
	struct power_supply *psy = data;
	struct usb_temp_device_info *di =
		container_of(nb, struct usb_temp_device_info, psy_nb);

	if (event != PSY_EVENT_PROP_CHANGED || !psy || !psy->desc)
		return NOTIFY_OK;
	if (strcmp(psy->desc->name, "usb"))
		return NOTIFY_OK;

	pr_debug("usb_temp.usb psy changed\n");
	queue_work(di->usb_temp_wq, &di->usb_state_wk);
	return NOTIFY_OK;
}

static int get_usb_temp_value(struct usb_temp_device_info *di)
{
	int temp_mc, ret;

	if (!di->tzd)
		return -ENODEV;

	ret = thermal_zone_get_temp(di->tzd, &temp_mc);
	if (ret) {
		pr_err("usb_temp.usb thermal zone read failed rc=%d\n", ret);
		return ret;
	}

	return DIV_ROUND_CLOSEST(temp_mc, 1000); /* mC -> C */
}

static int get_batt_temp_value(void)
{
	union power_supply_propval ret = {0};
	int rc;

	if (!g_di || !g_di->batt_psy)
		return INVALID_BATT_TEMP;

	/* qti_battery_charger already returns integer degC */
	rc = power_supply_get_property(g_di->batt_psy,
				       POWER_SUPPLY_PROP_TEMP, &ret);
	if (rc) {
		pr_err("usb_temp.get battery temp error!\n");
		return INVALID_BATT_TEMP;
	}

	pr_info("usb_temp.the battery temperature is %d\n", ret.intval);
	return ret.intval;
}

static void set_interval(struct usb_temp_device_info *di, int temp)
{
	if (!di) {
		pr_err("usb_temp.%s di is NULL!\n", __func__);
		return;
	}

	if (temp > di->interval_switch_temp) {
		di->check_interval = FAST_MONITOR_INTERVAL;
		di->keep_check_cnt = USB_TEMP_START_CHK_CNT;
		is_usb_protect_mode = 1;
		pr_info("usb_temp.cnt = %d!\n", di->keep_check_cnt);
	} else {
		if (di->keep_check_cnt > USB_TEMP_END_CHK_CNT) {
			/* 0.3s cadence for the first ~30s after insertion */
			pr_info("usb_temp.cnt = %d!\n", di->keep_check_cnt);
			di->keep_check_cnt -= USB_TEMP_CHK_CNT_STEP;
			di->check_interval = FAST_MONITOR_INTERVAL;
			is_usb_protect_mode = 0;
		} else if (di->keep_check_cnt == USB_TEMP_END_CHK_CNT) {
			pr_info("usb_temp.cnt = %d!\n", di->keep_check_cnt);
			di->keep_check_cnt = USB_TEMP_DEFAULT_CHK_CNT;
			di->check_interval = NORMAL_MONITOR_INTERVAL;
			is_usb_protect_mode = 0;
			usb_temp_wake_unlock(di);
		} else if (di->keep_check_cnt >= USB_TEMP_START_CHK_CNT) {
			pr_info("usb_temp.cnt = %d!\n", di->keep_check_cnt);
			di->keep_check_cnt += USB_TEMP_CHK_CNT_STEP;
			di->check_interval = FAST_MONITOR_INTERVAL;
			is_usb_protect_mode = 1;
		} else {
			di->check_interval = NORMAL_MONITOR_INTERVAL;
			is_usb_protect_mode = 0;
		}
	}
}

static void protection_process(struct usb_temp_device_info *di, int temp,
			       int usb_temp)
{
	int gpio_value;

	if (!di) {
		pr_err("usb_temp.%s di is NULL!\n", __func__);
		return;
	}

	gpio_value = gpio_get_value(di->gpio_usb_temp);
	if ((temp >= di->open_mosfet_temp) && (usb_temp >= di->need_protect_temp)) {
		usb_temp_wake_lock(di);
		gpio_set_value(di->gpio_usb_temp, GPIO_HIGH); /* open mosfet */
		pr_err("usb_temp.temp wrong, pull up protect gpio (was %d)\n",
		       gpio_value);
	} else if ((temp <= di->close_mosfet_temp) &&
		   (usb_temp <= di->need_recover_temp)) {
		gpio_set_value(di->gpio_usb_temp, GPIO_LOW); /* close mosfet */
		pr_info("usb_temp.temp normal, pull down protect gpio (was %d)\n",
			gpio_value);
	}
}

static void check_temperature(struct usb_temp_device_info *di)
{
	int tusb, tbatt, tdiff;

	if (!di) {
		pr_err("usb_temp.%s di is NULL!\n", __func__);
		return;
	}

	tusb = get_usb_temp_value(di);
	tbatt = get_batt_temp_value();

	pr_info("usb_temp.tusb = %d, tbatt = %d\n", tusb, tbatt);
	tdiff = tusb - tbatt;
	if (INVALID_BATT_TEMP == tbatt) {
		tdiff = INVALID_DELTA_TEMP;
		pr_err("usb_temp.get battery adc temp err, not care!!!\n");
	}

	set_interval(di, tdiff);
	protection_process(di, tdiff, tusb);
}

static void usb_temp_check_work(struct work_struct *work)
{
	struct usb_temp_device_info *di =
		container_of(work, struct usb_temp_device_info, usb_temp_check_wk);

#ifdef CONFIG_HLTHERM_RUNTEST
	pr_info("usb_temp.Disable HLTHERM protect\n");
	return;
#endif

	if (USB_TEMP_DEFAULT_CHK_CNT == di->keep_check_cnt &&
	    !usb_is_charging(di)) {
		protect_dmd_notify_enable = 1;
		gpio_set_value(di->gpio_usb_temp, GPIO_LOW); /* close mosfet */
		di->check_interval = NORMAL_MONITOR_INTERVAL;
		is_usb_protect_mode = 0;
		usb_temp_wake_unlock(di);
		pr_err("usb_temp.charger removed, stop checking\n");
		return; /* do not re-arm the timer */
	}

	check_temperature(di);
	hrtimer_start(&di->timer,
		      ktime_set(di->check_interval / MSEC_PER_SEC,
				(di->check_interval % MSEC_PER_SEC) * USEC_PER_SEC),
		      HRTIMER_MODE_REL);
}

static enum hrtimer_restart usb_temp_timer_func(struct hrtimer *timer)
{
	struct usb_temp_device_info *di =
		container_of(timer, struct usb_temp_device_info, timer);

	queue_work(di->usb_temp_wq, &di->usb_temp_check_wk);
	return HRTIMER_NORESTART;
}

static void check_ntc_error(struct usb_temp_device_info *di)
{
	int temp, sum = 0, i;

	for (i = 0; i < USB_TEMP_CNT; ++i)
		sum += get_usb_temp_value(di);

	temp = sum / USB_TEMP_CNT;
	if (temp > TUSB_TEMP_UPPER_LIMIT || temp < TUSB_TEMP_LOWER_LIMIT) {
		pr_err("usb_temp.usb ntc out of range (%d), disable protect\n",
		       temp);
		protect_enable = 0;
	} else {
		pr_info("usb_temp.enable usb short protect\n");
		protect_enable = 1;
	}
}

static int usb_temp_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct usb_temp_device_info *di;
	const char *zone_name = USB_TEMP_ZONE_DEFAULT;
	int ret, gpio_lo = -EINVAL, gpio_hi = -EINVAL;
	bool batt_present = true, need_usb_temp = true;

	/* optional legacy board detect (gpio127 low + gpio128 high = skip) */
	gpio_lo = of_get_named_gpio(np, "shamrock,board-detect-gpios", 0);
	gpio_hi = of_get_named_gpio(np, "shamrock,board-detect-gpios", 1);
	if (gpio_is_valid(gpio_lo) && gpio_is_valid(gpio_hi) &&
	    !gpio_get_value(gpio_lo) && gpio_get_value(gpio_hi))
		return 0;

	pr_info("usb_temp.enter into usb_temp probe\n");
	if (!np)
		return -EINVAL;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	di->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, di);
	g_di = di;
	di->keep_check_cnt = USB_TEMP_INSERT_CHG_CNT;
	di->check_interval = NORMAL_MONITOR_INTERVAL;
	is_usb_protect_mode = 0;

	di->usb_psy = power_supply_get_by_name("usb");
	if (!di->usb_psy) {
		pr_err("usb_temp.usb supply not found, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto free_mem;
	}
	di->batt_psy = power_supply_get_by_name("battery");
	if (!di->batt_psy) {
		pr_err("usb_temp.batt supply not found, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto put_usb;
	}

	/* thermal zone instead of qpnp vadc */
	of_property_read_string(np, "shamrock,usb-temp-zone", &zone_name);
	di->tzd = thermal_zone_get_zone_by_name(zone_name);
	if (IS_ERR(di->tzd)) {
		pr_err("usb_temp.thermal zone %s not ready, defer\n", zone_name);
		ret = -EPROBE_DEFER;
		goto put_psy;
	}

	di->gpio_usb_temp = of_get_named_gpio(np, "shamrock,gpio_usb_temp_protect", 0);
	if (!gpio_is_valid(di->gpio_usb_temp)) {
		pr_err("usb_temp.gpio_usb_temp is not valid\n");
		ret = -EINVAL;
		goto put_psy;
	}
	pr_info("usb_temp.gpio_usb_temp = %d\n", di->gpio_usb_temp);

	ret = gpio_request(di->gpio_usb_temp, "usb_temp_protect");
	if (ret) {
		pr_err("usb_temp.could not request gpio_usb_temp\n");
		ret = -EINVAL;
		goto put_psy;
	}
	gpio_direction_output(di->gpio_usb_temp, GPIO_LOW);

	ret = of_property_read_u32(np, "shamrock,no_need_usb_temp",
				   &di->no_need_usb_temp);
	if (ret)
		pr_info("usb_temp.no_need_usb_temp not set\n");

	ret = of_property_read_u32(np, "shamrock,need_protect_temp",
				   &di->need_protect_temp);
	if (ret)
		goto free_gpio;
	ret = of_property_read_u32(np, "shamrock,need_recover_temp",
				   &di->need_recover_temp);
	if (ret)
		goto free_gpio;
	ret = of_property_read_u32(np, "shamrock,open_mosfet_temp",
				   &di->open_mosfet_temp);
	if (ret)
		goto free_gpio;
	ret = of_property_read_u32(np, "shamrock,close_mosfet_temp",
				   &di->close_mosfet_temp);
	if (ret)
		goto free_gpio;
	ret = of_property_read_u32(np, "shamrock,interval_switch_temp",
				   &di->interval_switch_temp);
	if (ret)
		goto free_gpio;

	pr_info("usb_temp.thresholds protect=%d recover=%d open=%d close=%d switch=%d\n",
		di->need_protect_temp, di->need_recover_temp,
		di->open_mosfet_temp, di->close_mosfet_temp,
		di->interval_switch_temp);

	check_ntc_error(di);

	if (!power_supply_get_property(di->batt_psy, POWER_SUPPLY_PROP_PRESENT,
			&(union power_supply_propval){ .intval = 0 }))
		batt_present = true;

	if (is_factory_mode || di->no_need_usb_temp == 1)
		need_usb_temp = false;

	if (!batt_present || !need_usb_temp) {
		pr_err("usb_temp.batt missing or factory mode, disable protect\n");
		protect_enable = 0;
	}

	if (!protect_enable) {
		ret = 0;
		goto free_gpio;
	}

	di->ws = wakeup_source_register(&pdev->dev, "usb_temp_protect");
	if (!di->ws) {
		ret = -ENOMEM;
		goto free_gpio;
	}

	di->usb_temp_wq = create_singlethread_workqueue("usb_temp_protect_wq");
	if (!di->usb_temp_wq) {
		ret = -ENOMEM;
		goto free_ws;
	}

	INIT_WORK(&di->usb_temp_check_wk, usb_temp_check_work);
	INIT_WORK(&di->usb_state_wk, usb_temp_update_state_work);
	hrtimer_init(&di->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	di->timer.function = usb_temp_timer_func;

	di->psy_nb.notifier_call = usb_temp_psy_notifier;
	ret = power_supply_reg_notifier(&di->psy_nb);
	if (ret < 0) {
		pr_err("usb_temp.psy notifier register failed %d\n", ret);
		goto free_wq;
	}

	/* catch an already-attached charger */
	queue_work(di->usb_temp_wq, &di->usb_state_wk);

	pr_info("usb_temp.usb_temp probe ok!\n");
	return 0;

free_wq:
	destroy_workqueue(di->usb_temp_wq);
free_ws:
	wakeup_source_unregister(di->ws);
free_gpio:
	gpio_free(di->gpio_usb_temp);
put_psy:
	power_supply_put(di->batt_psy);
put_usb:
	power_supply_put(di->usb_psy);
free_mem:
	kfree(di);
	g_di = NULL;
	return ret;
}

static int usb_temp_remove(struct platform_device *pdev)
{
	struct usb_temp_device_info *di = dev_get_drvdata(&pdev->dev);

	if (!di)
		return 0;

	hrtimer_cancel(&di->timer);
	cancel_work_sync(&di->usb_temp_check_wk);
	cancel_work_sync(&di->usb_state_wk);
	destroy_workqueue(di->usb_temp_wq);
	power_supply_unreg_notifier(&di->psy_nb);
	if (di->wake_active)
		__pm_relax(di->ws);
	wakeup_source_unregister(di->ws);
	power_supply_put(di->batt_psy);
	power_supply_put(di->usb_psy);
	gpio_free(di->gpio_usb_temp);
	kfree(di);
	g_di = NULL;

	return 0;
}

static const struct of_device_id usb_temp_match_table[] = {
	{ .compatible = "shamrock,usb_temp_protect" },
	{ .compatible = "nothing,usb-temp-protect" },
	{ }
};
MODULE_DEVICE_TABLE(of, usb_temp_match_table);

static struct platform_driver usb_temp_driver = {
	.probe = usb_temp_probe,
	.remove = usb_temp_remove,
	.driver = {
		.name = "usb_temp_protect",
		.of_match_table = usb_temp_match_table,
	},
};
module_platform_driver(usb_temp_driver);

MODULE_DESCRIPTION("USB connector temperature protection SM7325");
MODULE_LICENSE("GPL v2");