// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/kernfs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/power_supply.h>
#include <linux/pm_qos.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>
#ifdef CONFIG_DRM_PANEL
#include <drm/drm_panel.h>
#endif

#include <net/netlink.h>
#include <net/genetlink.h>

#include "thermal_core.h"
#include "../base/base.h"

#define NT_SETUP_RETRY_MS	1000
#define NT_SETUP_MAX_TRIES	60
#define NT_DEVT			MKDEV(0, 'H')

/*
 * Number of OPP steps the cap written to cpu_limits is relaxed by.
 * The previous revision hard-coded 3 (CPU may run 3 OPPs above the cap
 * userspace asked for).  0 = honour the cap exactly.
 */
static unsigned int cpu_limit_margin;
module_param(cpu_limit_margin, uint, 0644);
MODULE_PARM_DESC(cpu_limit_margin,
		 "OPP steps added above the cap written to cpu_limits (default 0)");

static struct device_node *thermal_np;

#ifdef CONFIG_DRM_PANEL
static int nothing_thermal_notify(struct notifier_block *self,
				  unsigned long event, void *data);

/*
 * The panel is handed to us by dsi_display_bind() through
 * nothing_thermal_register_panel(); we no longer look it up ourselves.
 */
static struct drm_panel *active_panel;
static struct notifier_block drm_notifier = {
	.notifier_call = nothing_thermal_notify,
};
static bool panel_notifier_registered;
static DEFINE_MUTEX(panel_reg_lock);
#endif

static struct nothing_thermal_device {
	struct device *dev;
	struct class *class;
} nothing_thermal_dev;

static struct usb_monitor {
	struct notifier_block psy_nb;
	struct work_struct usb_state_work;
	int usb_online;
} usb_state;

static struct power_supply *usb_psy;
static bool usb_nb_registered;

struct cpufreq_device {
	struct list_head node;
	cpumask_t cpus;			/* related CPUs of the policy */
	unsigned int cpufreq_state;
	unsigned int max_level;
	struct freq_qos_request qos_req;
	u32 freq[];			/* kHz, descending */
};

static int screen_state = 1;
static int screen_last_status = 1;

static atomic_t balance_mode = ATOMIC_INIT(0);
static atomic_t charger_temp = ATOMIC_INIT(-1);
static atomic_t flash_state = ATOMIC_INIT(0);
static atomic_t market_download_limit = ATOMIC_INIT(0);
static atomic_t modem_limit = ATOMIC_INIT(0);
static atomic_t poor_modem_limit = ATOMIC_INIT(0);
static atomic_t temp_state = ATOMIC_INIT(0);
static atomic_t sconfig = ATOMIC_INIT(10);
static atomic_t wifi_limit = ATOMIC_INIT(0);

static const char *board_sensor;
static char boost[128];
static char board_sensor_temp[128];
static char board_sensor_second_temp[128];
static DEFINE_MUTEX(nt_str_lock);

static LIST_HEAD(cpufreq_dev_list);
static DEFINE_MUTEX(cpufreq_list_lock);

/* ------------------------------------------------------------------ */
/* CPU frequency caps                                                  */
/* ------------------------------------------------------------------ */

static int cpufreq_set_level(struct cpufreq_device *cdev, unsigned int state)
{
	int ret;

	/* Request state should be less than max_level */
	if (state > cdev->max_level)
		return -EINVAL;

	/* Check if the old cooling action is same as new cooling action */
	if (cdev->cpufreq_state == state)
		return 0;

	ret = freq_qos_update_request(&cdev->qos_req, cdev->freq[state]);
	if (ret < 0)
		return ret;

	cdev->cpufreq_state = state;
	return 0;
}

static int cpu_limits_set_level(unsigned int cpu, unsigned int max_freq)
{
	struct cpufreq_device *cdev;
	int ret = -ENODEV;

	mutex_lock(&cpufreq_list_lock);
	list_for_each_entry(cdev, &cpufreq_dev_list, node) {
		unsigned int level;

		if (!cpumask_test_cpu(cpu, &cdev->cpus))
			continue;

		/*
		 * freq[] is descending: pick the first OPP not above the
		 * requested cap.  A cap below the lowest OPP ends the loop at
		 * max_level, i.e. clamps to the lowest OPP.
		 */
		for (level = 0; level < cdev->max_level; level++)
			if (max_freq >= cdev->freq[level])
				break;

		if (level > cpu_limit_margin)
			level -= cpu_limit_margin;
		else
			level = 0;

		ret = cpufreq_set_level(cdev, level);
		break;
	}
	mutex_unlock(&cpufreq_list_lock);

	return ret;
}

static unsigned int find_next_max(struct cpufreq_frequency_table *table,
				  unsigned int prev_max)
{
	struct cpufreq_frequency_table *pos;
	unsigned int max = 0;

	cpufreq_for_each_valid_entry(pos, table) {
		if (pos->frequency > max && pos->frequency < prev_max)
			max = pos->frequency;
	}

	return max;
}

static void nothing_thermal_cpu_destroy(void)
{
	struct cpufreq_device *priv, *tmp;

	mutex_lock(&cpufreq_list_lock);
	list_for_each_entry_safe(priv, tmp, &cpufreq_dev_list, node) {
		freq_qos_remove_request(&priv->qos_req);
		list_del(&priv->node);
		kfree(priv);
	}
	mutex_unlock(&cpufreq_list_lock);
}

/* One QoS request per cpufreq policy. Returns -EAGAIN when not ready yet. */
static int cpu_thermal_init(void)
{
	cpumask_t done;
	int cpu, ret;

	cpumask_clear(&done);

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct cpufreq_device *cdev;
		unsigned int i, n, freq;

		if (cpumask_test_cpu(cpu, &done))
			continue;

		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_warn("%s: cpufreq policy not found for cpu%d\n",
				__func__, cpu);
			ret = -EAGAIN;
			goto err;
		}

		n = cpufreq_table_count_valid_entries(policy);
		if (!n) {
			pr_warn("%s: CPUFreq table of cpu%d not available yet\n",
				__func__, cpu);
			cpufreq_cpu_put(policy);
			ret = -EAGAIN;
			goto err;
		}

		cdev = kzalloc(struct_size(cdev, freq, n), GFP_KERNEL);
		if (!cdev) {
			cpufreq_cpu_put(policy);
			ret = -ENOMEM;
			goto err;
		}

		/* max_level is an index, not a counter */
		cdev->max_level = n - 1;
		cpumask_copy(&cdev->cpus, policy->related_cpus);

		/* Fill freq-table in descending order of frequencies */
		for (i = 0, freq = UINT_MAX; i < n; i++) {
			freq = find_next_max(policy->freq_table, freq);
			cdev->freq[i] = freq;

			/* Warn for duplicate entries */
			if (!freq)
				pr_warn("%s: table has duplicate entries\n",
					__func__);
		}

		ret = freq_qos_add_request(&policy->constraints,
					   &cdev->qos_req, FREQ_QOS_MAX,
					   cdev->freq[0]);
		if (ret < 0) {
			pr_err("%s: Failed to add freq constraint (%d)\n",
			       __func__, ret);
			kfree(cdev);
			cpufreq_cpu_put(policy);
			goto err;
		}

		cpumask_or(&done, &done, policy->related_cpus);
		cpufreq_cpu_put(policy);

		mutex_lock(&cpufreq_list_lock);
		list_add_tail(&cdev->node, &cpufreq_dev_list);
		mutex_unlock(&cpufreq_list_lock);
	}

	return 0;

err:
	nothing_thermal_cpu_destroy();
	return ret;
}

/* ------------------------------------------------------------------ */
/* sysfs attributes                                                    */
/* ------------------------------------------------------------------ */

#define NT_INT_ATTR(_name, _var)					\
static ssize_t thermal_##_name##_show(struct device *dev,		\
				      struct device_attribute *attr,	\
				      char *buf)			\
{									\
	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&_var));	\
}									\
static ssize_t thermal_##_name##_store(struct device *dev,		\
				       struct device_attribute *attr,	\
				       const char *buf, size_t len)	\
{									\
	int val;							\
									\
	if (sscanf(buf, "%d", &val) != 1)				\
		return -EINVAL;						\
	atomic_set(&_var, val);						\
	return len;							\
}									\
static DEVICE_ATTR(_name, 0664, thermal_##_name##_show,		\
		   thermal_##_name##_store)

#define NT_STR_ATTR(_name, _buf)					\
static ssize_t thermal_##_name##_show(struct device *dev,		\
				      struct device_attribute *attr,	\
				      char *buf)			\
{									\
	ssize_t n;							\
									\
	mutex_lock(&nt_str_lock);					\
	n = scnprintf(buf, PAGE_SIZE, "%s", _buf);			\
	mutex_unlock(&nt_str_lock);					\
	return n;							\
}									\
static ssize_t thermal_##_name##_store(struct device *dev,		\
				       struct device_attribute *attr,	\
				       const char *buf, size_t len)	\
{									\
	mutex_lock(&nt_str_lock);					\
	strscpy(_buf, buf, sizeof(_buf));				\
	mutex_unlock(&nt_str_lock);					\
	return len;							\
}									\
static DEVICE_ATTR(_name, 0664, thermal_##_name##_show,		\
		   thermal_##_name##_store)

NT_INT_ATTR(balance_mode, balance_mode);
NT_INT_ATTR(charger_temp, charger_temp);
NT_INT_ATTR(flash_state, flash_state);
NT_INT_ATTR(market_download_limit, market_download_limit);
NT_INT_ATTR(modem_limit, modem_limit);
NT_INT_ATTR(poor_modem_limit, poor_modem_limit);
NT_INT_ATTR(sconfig, sconfig);
NT_INT_ATTR(temp_state, temp_state);
NT_INT_ATTR(wifi_limit, wifi_limit);

NT_STR_ATTR(boost, boost);
NT_STR_ATTR(board_sensor_temp, board_sensor_temp);
NT_STR_ATTR(board_sensor_second_temp, board_sensor_second_temp);

static ssize_t thermal_board_sensor_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s",
			 board_sensor ? board_sensor : "invalid");
}
static DEVICE_ATTR(board_sensor, 0444, thermal_board_sensor_show, NULL);

static ssize_t cpu_limits_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t cpu_limits_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	unsigned int cpu, max;
	int ret;

	if (sscanf(buf, "cpu%u %u", &cpu, &max) != 2) {
		pr_err("input param error, can not parse param\n");
		return -EINVAL;
	}

	ret = cpu_limits_set_level(cpu, max);
	if (ret)
		pr_warn_ratelimited("%s: cpu%u cap %u kHz not applied (%d)\n",
				    __func__, cpu, max, ret);

	/* keep the historical "write always succeeds" contract */
	return len;
}
static DEVICE_ATTR(cpu_limits, 0664, cpu_limits_show, cpu_limits_store);

static ssize_t thermal_screen_state_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(screen_state));
}
static DEVICE_ATTR(screen_state, 0444, thermal_screen_state_show, NULL);

static ssize_t thermal_usb_online_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 READ_ONCE(usb_state.usb_online));
}
static DEVICE_ATTR(usb_online, 0444, thermal_usb_online_show, NULL);

static struct attribute *nothing_thermal_dev_attrs[] = {
	&dev_attr_balance_mode.attr,
	&dev_attr_board_sensor.attr,
	&dev_attr_board_sensor_temp.attr,
	&dev_attr_board_sensor_second_temp.attr,
	&dev_attr_boost.attr,
	&dev_attr_charger_temp.attr,
	&dev_attr_cpu_limits.attr,
	&dev_attr_flash_state.attr,
	&dev_attr_market_download_limit.attr,
	&dev_attr_modem_limit.attr,
	&dev_attr_poor_modem_limit.attr,
	&dev_attr_sconfig.attr,
	&dev_attr_screen_state.attr,
	&dev_attr_temp_state.attr,
	&dev_attr_usb_online.attr,
	&dev_attr_wifi_limit.attr,
	NULL
};

static const struct attribute_group nothing_thermal_group = {
	.attrs = nothing_thermal_dev_attrs,
};

/*
 * The node lives in the existing "thermal" class so the path stays
 * /sys/class/thermal/thermal_message.  thermal_class is private to
 * thermal_core.c, so it is looked up through kernfs like before.
 */
static int nothing_create_thermal_message_node(void)
{
	struct kernfs_node *sysfs_sd, *class_sd, *thermal_sd;
	struct subsys_private *cp;
	struct class *cls;
	struct device *dev;
	int ret;

	if (nothing_thermal_dev.dev)
		return 0;

	sysfs_sd = kernel_kobj->sd->parent;
	if (!sysfs_sd)
		return -ENODEV;

	class_sd = kernfs_find_and_get(sysfs_sd, "class");
	if (!class_sd)
		return -ENODEV;

	thermal_sd = kernfs_find_and_get(class_sd, "thermal");
	kernfs_put(class_sd);
	if (!thermal_sd)
		return -ENODEV;

	if (!thermal_sd->priv) {
		kernfs_put(thermal_sd);
		return -ENODEV;
	}

	cp = to_subsys_private((struct kobject *)thermal_sd->priv);
	cls = cp->class;
	kernfs_put(thermal_sd);
	if (!cls)
		return -ENODEV;

	dev = device_create(cls, NULL, NT_DEVT, NULL, "thermal_message");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		pr_err("%s create device dev err: %d\n", __func__, ret);
		return ret;
	}

	ret = sysfs_create_group(&dev->kobj, &nothing_thermal_group);
	if (ret) {
		pr_err("%s ERROR: Cannot create sysfs structure!:%d\n",
		       __func__, ret);
		device_destroy(cls, NT_DEVT);
		return ret;
	}

	nothing_thermal_dev.class = cls;
	nothing_thermal_dev.dev = dev;
	return 0;
}

static void nothing_destroy_thermal_message_node(void)
{
	if (!nothing_thermal_dev.dev)
		return;

	sysfs_remove_group(&nothing_thermal_dev.dev->kobj,
			   &nothing_thermal_group);
	device_destroy(nothing_thermal_dev.class, NT_DEVT);
	nothing_thermal_dev.dev = NULL;
	nothing_thermal_dev.class = NULL;
}

/* ------------------------------------------------------------------ */
/* Display panel / screen state                                        */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_DRM_PANEL
static int nothing_thermal_notify(struct notifier_block *self,
				  unsigned long event, void *data)
{
	struct drm_panel_notifier *evdata = data;
	int state, *blank;

	if (event != DRM_PANEL_EVENT_BLANK || !evdata || !evdata->data)
		return NOTIFY_DONE;

	blank = evdata->data;

	switch (*blank) {
	case DRM_PANEL_BLANK_UNBLANK:
		state = 1;
		break;
	case DRM_PANEL_BLANK_LP:
	case DRM_PANEL_BLANK_POWERDOWN:
		state = 0;
		break;
	default:
		return NOTIFY_OK;
	}

	WRITE_ONCE(screen_state, state);
	if (screen_last_status != state) {
		screen_last_status = state;
		if (nothing_thermal_dev.dev)
			sysfs_notify(&nothing_thermal_dev.dev->kobj, NULL,
				     "screen_state");
	}

	return NOTIFY_OK;
}

/*
 * Called from dsi_display_bind() once the panel is set up.  Safe to call
 * more than once (e.g. several DSI displays): only the first successful
 * registration sticks.
 */
int nothing_thermal_register_panel(struct drm_panel *panel)
{
	int ret = 0;

	mutex_lock(&panel_reg_lock);
	if (panel_notifier_registered)
		goto out;

	ret = drm_panel_notifier_register(panel, &drm_notifier);
	if (ret) {
		pr_err("%s: Failed to register DRM panel notifier, rc=%d\n",
		       __func__, ret);
		goto out;
	}

	active_panel = panel;
	panel_notifier_registered = true;
	pr_info("%s: Registered DRM panel notifier successfully\n", __func__);
out:
	mutex_unlock(&panel_reg_lock);
	return ret;
}
EXPORT_SYMBOL(nothing_thermal_register_panel);
#endif /* CONFIG_DRM_PANEL */

/* ------------------------------------------------------------------ */
/* USB online                                                          */
/* ------------------------------------------------------------------ */

static int usb_online_callback(struct notifier_block *nb, unsigned long val,
			       void *data)
{
	struct power_supply *psy = data;

	if (psy && psy->desc && !strcmp(psy->desc->name, "usb"))
		schedule_work(&usb_state.usb_state_work);

	return NOTIFY_OK;
}

static void usb_online_work(struct work_struct *work)
{
	union power_supply_propval val = { 0, };
	int err;

	if (!usb_psy) {
		usb_psy = power_supply_get_by_name("usb");
		if (!usb_psy)
			return;
	}

	err = power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_ONLINE,
					&val);
	if (err) {
		pr_err_ratelimited("%s: usb online read error: %d\n",
				   __func__, err);
		return;
	}

	if (READ_ONCE(usb_state.usb_online) == val.intval)
		return;

	WRITE_ONCE(usb_state.usb_online, val.intval);
	if (nothing_thermal_dev.dev)
		sysfs_notify(&nothing_thermal_dev.dev->kobj, NULL,
			     "usb_online");
}

/* ------------------------------------------------------------------ */
/* Setup / retry                                                       */
/* ------------------------------------------------------------------ */

static bool node_ready, cpu_ready;
static int setup_tries;

static int of_parse_thermal_message(struct device_node *np)
{
	if (of_property_read_string(np, "board-sensor", &board_sensor))
		return -EINVAL;

	pr_info("%s board sensor: %s\n", __func__, board_sensor);
	return 0;
}

/* Returns -EAGAIN while something is still waiting for another driver. */
static int nothing_thermal_try_setup(void)
{
	int ret, pending = 0;

	if (!node_ready) {
		ret = nothing_create_thermal_message_node();
		if (!ret)
			node_ready = true;
		else
			pending = -EAGAIN;
	}

	if (!cpu_ready) {
		ret = cpu_thermal_init();
		if (!ret)
			cpu_ready = true;
		else if (ret == -EAGAIN)
			pending = -EAGAIN;
		else {
			pr_err("%s: cpu limits unavailable (%d)\n", __func__,
			       ret);
			cpu_ready = true;	/* permanent, stop retrying */
		}
	}

	return pending;
}

static void nothing_thermal_setup_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(setup_work, nothing_thermal_setup_work);

static void nothing_thermal_setup_work(struct work_struct *work)
{
	if (nothing_thermal_try_setup() != -EAGAIN)
		return;

	if (++setup_tries < NT_SETUP_MAX_TRIES)
		schedule_delayed_work(&setup_work,
				      msecs_to_jiffies(NT_SETUP_RETRY_MS));
	else
		pr_err("%s: giving up after %d tries (node=%d cpu=%d)\n",
		       __func__, setup_tries, node_ready, cpu_ready);
}

static int __init nothing_thermal_interface_init(void)
{
	int ret;

	thermal_np = of_find_node_by_name(NULL, "nothing-thermal-interface");
	if (!thermal_np) {
		pr_err("%s: Cannot find nothing-thermal-interface node\n",
		       __func__);
		return 0;
	}

	ret = of_parse_thermal_message(thermal_np);
	if (ret)
		pr_err("%s: Can not parse thermal message node: %d\n",
		       __func__, ret);

	INIT_WORK(&usb_state.usb_state_work, usb_online_work);
	usb_state.psy_nb.notifier_call = usb_online_callback;
	ret = power_supply_reg_notifier(&usb_state.psy_nb);
	if (ret < 0)
		pr_err("%s: usb online notifier registration failed err: %d\n",
		       __func__, ret);
	else
		usb_nb_registered = true;

	if (nothing_thermal_try_setup() == -EAGAIN)
		schedule_delayed_work(&setup_work,
				      msecs_to_jiffies(NT_SETUP_RETRY_MS));

	return 0;
}
late_initcall(nothing_thermal_interface_init);

static void __exit nothing_thermal_interface_exit(void)
{
	cancel_delayed_work_sync(&setup_work);

	if (usb_nb_registered) {
		power_supply_unreg_notifier(&usb_state.psy_nb);
		cancel_work_sync(&usb_state.usb_state_work);
		usb_nb_registered = false;
	}
	if (usb_psy) {
		power_supply_put(usb_psy);
		usb_psy = NULL;
	}

#ifdef CONFIG_DRM_PANEL
	mutex_lock(&panel_reg_lock);
	if (panel_notifier_registered && active_panel) {
		drm_panel_notifier_unregister(active_panel, &drm_notifier);
		panel_notifier_registered = false;
		active_panel = NULL;
	}
	mutex_unlock(&panel_reg_lock);
#endif
	nothing_destroy_thermal_message_node();
	nothing_thermal_cpu_destroy();
	of_node_put(thermal_np);
}
module_exit(nothing_thermal_interface_exit);

MODULE_AUTHOR("Nothing kernel");
MODULE_DESCRIPTION("Nothing thermal control interface");
MODULE_LICENSE("GPL v2");
