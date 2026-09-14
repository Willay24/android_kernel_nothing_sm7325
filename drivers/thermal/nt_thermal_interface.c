// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kdev_t.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thermal.h>
#ifdef CONFIG_DRM_PANEL
#include <drm/drm_panel.h>
#endif

#include "nt-thermal-interface.h"

#define NT_THERMAL_DT_NODE		"nt-thermal-interface"
#define NT_THERMAL_DT_BOARD_SENSOR	"board-sensor"
#define NT_THERMAL_DT_BOARD_ZONE	"board-sensor-zone"
#define NT_THERMAL_DT_BOARD_ZONE2	"board-sensor-second-zone"

static struct nt_thermal_device {
	struct device *dev;
	struct class *class;
	struct attribute_group attrs;
} nt_thermal_dev;

static struct usb_monitor {
	struct notifier_block psy_nb;
	struct work_struct usb_state_work;
	int usb_online;
} usb_state;

struct freq_table {
	u32 frequency;
};

struct cpufreq_device {
	int id;
	unsigned int cpufreq_state;
	unsigned int max_level;
	struct freq_table *freq_table;	/* descending order */
	struct cpufreq_policy *policy;
	struct list_head node;
	struct freq_qos_request *qos_req;
};

static atomic_t screen_state = ATOMIC_INIT(0);
static int screen_last_status;
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
static const char *board_sensor_zone;
static const char *board_sensor_second_zone;
static char boost[128];
static char board_sensor_temp[128];
static char board_sensor_second_temp[128];

static LIST_HEAD(cpufreq_dev_list);
static DEFINE_MUTEX(cpufreq_list_lock);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req);

static void destroy_thermal_cpu(void);	/* forward declaration */

static int nt_get_zone_temp(const char *name, int *temp)
{
	struct thermal_zone_device *tz;
	int ret;

	if (!name || !strlen(name))
		return -EINVAL;

	tz = thermal_zone_get_zone_by_name(name);
	if (IS_ERR(tz))
		return PTR_ERR(tz);

	ret = thermal_zone_get_temp(tz, temp);
	put_device(&tz->device);
	return ret;
}

static int cpufreq_set_level(struct cpufreq_device *cdev, unsigned long state)
{
	if (state > cdev->max_level)
		return -EINVAL;
	if (cdev->cpufreq_state == state)
		return 0;

	cdev->cpufreq_state = state;
	return freq_qos_update_request(cdev->qos_req,
				       cdev->freq_table[state].frequency);
}

static void cpu_limits_set_level(unsigned int cpu, unsigned long max_freq)
{
	struct cpufreq_device *cpufreq_dev;
	unsigned int level;

	mutex_lock(&cpufreq_list_lock);
	list_for_each_entry(cpufreq_dev, &cpufreq_dev_list, node) {
		if (cpufreq_dev->id != cpu)
			continue;

		for (level = 0; level <= cpufreq_dev->max_level; level++) {
			if (max_freq >= cpufreq_dev->freq_table[level].frequency) {
				/* Back off 3 steps BELOW the ceiling.
				 * freq_table[] is descending: higher index =
				 * lower frequency (old code used level-3,
				 * which RAISED the cap by 3 steps). */
				level = min(level + 3, cpufreq_dev->max_level);
				cpufreq_set_level(cpufreq_dev, level);
				break;
			}
		}
		break;
	}
	mutex_unlock(&cpufreq_list_lock);
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

static int cpu_thermal_init(void)
{
	int cpu, ret = 0;
	struct cpufreq_policy *policy;
	struct freq_qos_request *req;

	for_each_possible_cpu(cpu) {
		unsigned int i, freq;
		struct cpufreq_device *cpufreq_dev;

		req = &per_cpu(qos_req, cpu);
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: cpufreq policy not found for cpu%d\n",
			       __func__, cpu);
			ret = -ESRCH;
			goto unwind;
		}

		i = cpufreq_table_count_valid_entries(policy);
		if (!i) {
			pr_err("%s: CPUFreq table not found or empty\n", __func__);
			cpufreq_cpu_put(policy);
			ret = -ENODEV;
			goto unwind;
		}

		cpufreq_dev = kzalloc(sizeof(*cpufreq_dev), GFP_KERNEL);
		if (!cpufreq_dev) {
			cpufreq_cpu_put(policy);
			ret = -ENOMEM;
			goto unwind;
		}

		cpufreq_dev->policy = policy;	/* ref released in destroy */
		cpufreq_dev->qos_req = req;
		cpufreq_dev->max_level = i - 1;
		cpufreq_dev->id = policy->cpu;

		cpufreq_dev->freq_table = kmalloc_array(i, sizeof(*cpufreq_dev->freq_table),
							GFP_KERNEL);
		if (!cpufreq_dev->freq_table) {
			cpufreq_cpu_put(policy);
			kfree(cpufreq_dev);
			ret = -ENOMEM;
			goto unwind;
		}

		for (i = 0, freq = ~0U; i <= cpufreq_dev->max_level; i++) {
			freq = find_next_max(policy->freq_table, freq);
			cpufreq_dev->freq_table[i].frequency = freq;
			if (!freq)
				pr_warn("%s: duplicate freq entries\n", __func__);
		}

		ret = freq_qos_add_request(&policy->constraints, req,
					   FREQ_QOS_MAX,
					   cpufreq_dev->freq_table[0].frequency);
		if (ret < 0) {
			pr_err("%s: Failed to add freq constraint (%d)\n",
			       __func__, ret);
			kfree(cpufreq_dev->freq_table);
			cpufreq_cpu_put(policy);
			kfree(cpufreq_dev);
			goto unwind;
		}

		mutex_lock(&cpufreq_list_lock);
		list_add(&cpufreq_dev->node, &cpufreq_dev_list);
		mutex_unlock(&cpufreq_list_lock);
	}
	return 0;

unwind:
	destroy_thermal_cpu();
	return ret;
}

static void destroy_thermal_cpu(void)
{
	struct cpufreq_device *priv, *tmp;

	mutex_lock(&cpufreq_list_lock);
	list_for_each_entry_safe(priv, tmp, &cpufreq_dev_list, node) {
		list_del(&priv->node);
		mutex_unlock(&cpufreq_list_lock);

		freq_qos_remove_request(priv->qos_req);
		if (priv->policy)
			cpufreq_cpu_put(priv->policy);
		kfree(priv->freq_table);
		kfree(priv);

		mutex_lock(&cpufreq_list_lock);
	}
	mutex_unlock(&cpufreq_list_lock);
}

/* ---- sysfs attributes (unchanged semantics) ---- */
#define NT_INT_ATTR(_name) \
static ssize_t thermal_##_name##_show(struct device *dev, \
		struct device_attribute *attr, char *buf) \
{ return sysfs_emit(buf, "%d\n", atomic_read(&_name)); } \
static ssize_t thermal_##_name##_store(struct device *dev, \
		struct device_attribute *attr, const char *buf, size_t len) \
{ int val, ret; ret = kstrtoint(buf, 10, &val); if (ret) return ret; \
  atomic_set(&_name, val); return len; } \
static DEVICE_ATTR(_name, 0664, thermal_##_name##_show, thermal_##_name##_store)

NT_INT_ATTR(balance_mode);
NT_INT_ATTR(charger_temp);
NT_INT_ATTR(flash_state);
NT_INT_ATTR(market_download_limit);
NT_INT_ATTR(modem_limit);
NT_INT_ATTR(poor_modem_limit);
NT_INT_ATTR(sconfig);
NT_INT_ATTR(temp_state);
NT_INT_ATTR(wifi_limit);

static ssize_t thermal_board_sensor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s", board_sensor ?: "invalid");
}
static DEVICE_ATTR(board_sensor, 0664, thermal_board_sensor_show, NULL);

static ssize_t thermal_board_sensor_temp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int temp, ret;

	if (board_sensor_temp[0] != '\0')
		return sysfs_emit(buf, "%s", board_sensor_temp);

	ret = nt_get_zone_temp(board_sensor_zone, &temp);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%d\n", temp);
}

static ssize_t thermal_board_sensor_temp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(board_sensor_temp, sizeof(board_sensor_temp), "%s", buf);
	return len;
}
static DEVICE_ATTR(board_sensor_temp, 0664,
		   thermal_board_sensor_temp_show,
		   thermal_board_sensor_temp_store);

static ssize_t thermal_board_sensor_second_temp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int temp, ret;

	if (board_sensor_second_temp[0] != '\0')
		return sysfs_emit(buf, "%s", board_sensor_second_temp);

	ret = nt_get_zone_temp(board_sensor_second_zone, &temp);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%d\n", temp);
}

static ssize_t thermal_board_sensor_second_temp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(board_sensor_second_temp, sizeof(board_sensor_second_temp), "%s", buf);
	return len;
}
static DEVICE_ATTR(board_sensor_second_temp, 0664,
		   thermal_board_sensor_second_temp_show,
		   thermal_board_sensor_second_temp_store);

static ssize_t thermal_boost_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s", boost);
}
static ssize_t thermal_boost_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(boost, sizeof(boost), "%s", buf);
	return len;
}
static DEVICE_ATTR(boost, 0664, thermal_boost_show, thermal_boost_store);

static ssize_t cpu_limits_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return 0;
}
static ssize_t cpu_limits_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int cpu, max;

	if (sscanf(buf, "cpu%u %u", &cpu, &max) != 2) {
		pr_err("%s: cannot parse '%s' (expected: cpuN freq)\n",
		       __func__, buf);
		return -EINVAL;
	}
	cpu_limits_set_level(cpu, max);
	return len;
}
static DEVICE_ATTR(cpu_limits, 0664, cpu_limits_show, cpu_limits_store);

static ssize_t thermal_screen_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&screen_state));
}
static DEVICE_ATTR(screen_state, 0664, thermal_screen_state_show, NULL);

static ssize_t thermal_usb_online_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", usb_state.usb_online);
}
static DEVICE_ATTR(usb_online, 0664, thermal_usb_online_show, NULL);

static struct attribute *nt_thermal_dev_attr_group[] = {
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

/*
 * class_find_by_name()/class_put() don't exist on 4.19 (they were only
 * added upstream when class refcounting landed in ~5.16/5.18), so fetch
 * the "thermal" class from an already-registered thermal zone instead.
 * The thermal core registers its class at boot and never unregisters it,
 * so the pointer can simply be borrowed — no get/put needed.
 */
static struct class *nt_get_thermal_class(void)
{
	static const char * const fallback_zones[] = {
		"quiet_therm",
		"pm8350b_tz",
		"pmb8350b_therm",
		"usb_therm",
	};
	struct thermal_zone_device *tz;
	struct class *cls = NULL;
	const char *name;
	int n = 0;

	while (!cls) {
		if (n == 0)
			name = board_sensor_zone;
		else if (n == 1)
			name = board_sensor_second_zone;
		else if (n - 2 < ARRAY_SIZE(fallback_zones))
			name = fallback_zones[n - 2];
		else
			break;
		n++;

		if (!name || !strlen(name))
			continue;

		tz = thermal_zone_get_zone_by_name(name);
		if (IS_ERR(tz))
			continue;

		cls = tz->device.class;
		put_device(&tz->device);
	}

	return cls;
}

static void create_thermal_message_node(void)
{
	int ret;
	struct class *cls;

	if (nt_thermal_dev.dev)
		return;

	cls = nt_get_thermal_class();
	if (!cls) {
		pr_err("%s: thermal class not available (no thermal zone found)\n",
		       __func__);
		return;
	}

	nt_thermal_dev.class = cls;	/* borrowed ref, no class_put */
	nt_thermal_dev.dev = device_create(cls, NULL, 0, NULL, "thermal_message");
	if (!nt_thermal_dev.dev) {
		pr_err("%s: create thermal_message device failed\n", __func__);
		nt_thermal_dev.class = NULL;
		return;
	}

	nt_thermal_dev.attrs.attrs = nt_thermal_dev_attr_group;
	ret = sysfs_create_group(&nt_thermal_dev.dev->kobj, &nt_thermal_dev.attrs);
	if (ret) {
		pr_err("%s: sysfs_create_group failed: %d\n", __func__, ret);
		device_destroy(cls, 0);
		nt_thermal_dev.dev = NULL;
		nt_thermal_dev.class = NULL;
	}
}

static void destroy_thermal_message_node(void)
{
	if (!nt_thermal_dev.dev)
		return;

	sysfs_remove_group(&nt_thermal_dev.dev->kobj, &nt_thermal_dev.attrs);
	device_destroy(nt_thermal_dev.class, 0);
	nt_thermal_dev.dev = NULL;
	nt_thermal_dev.class = NULL;
}

#ifdef CONFIG_DRM_PANEL
static struct drm_panel *active_panel;
static struct notifier_block drm_notifier;

struct drm_panel *get_panel(void)
{
	return active_panel;
}
EXPORT_SYMBOL(get_panel);

static int thermal_check_panel(struct device_node *np)
{
	int i, count;
	struct device_node *node;
	struct drm_panel *panel = NULL;

	count = of_count_phandle_with_args(np, "qcom,display-panels", NULL);
	if (count <= 0)
		return 0;

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "qcom,display-panels", i);
		if (!node)
			continue;

		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			active_panel = panel;	/* was never assigned before! */
			pr_info("%s: panel %d bound\n", __func__, i);
			return 0;
		}
	}

	/* deferred (or missing): try again on re-probe */
	return PTR_ERR_OR_ZERO(panel) ?: -EPROBE_DEFER;
}

static int nt_thermal_notify(struct notifier_block *self, unsigned long event,
			     void *data)
{
	struct drm_panel_notifier *evdata = data;
	int *blank, state;

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
		return NOTIFY_DONE;
	}

	atomic_set(&screen_state, state);
	if (screen_last_status != state && nt_thermal_dev.dev) {
		sysfs_notify(&nt_thermal_dev.dev->kobj, NULL, "screen_state");
		screen_last_status = state;
	}
	return NOTIFY_OK;
}
#endif /* CONFIG_DRM_PANEL */

static int usb_online_callback(struct notifier_block *nb, unsigned long val,
			       void *data)
{
	struct power_supply *psy = data;

	/* old code scheduled work on EVERY power-supply event */
	if (val != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;
	if (!psy || !psy->desc || strcmp(psy->desc->name, "usb"))
		return NOTIFY_OK;

	schedule_work(&usb_state.usb_state_work);
	return NOTIFY_OK;
}

static void usb_online_work(struct work_struct *work)
{
	static struct power_supply *usb_psy;
	union power_supply_propval ret = { 0 };
	int err;

	if (!usb_psy)
		usb_psy = power_supply_get_by_name("usb");
	if (!usb_psy)
		return;

	err = power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_ONLINE, &ret);
	if (err) {
		pr_err("%s: usb online read error: %d\n", __func__, err);
		return;
	}
	usb_state.usb_online = ret.intval;
	if (nt_thermal_dev.dev)
		sysfs_notify(&nt_thermal_dev.dev->kobj, NULL, "usb_online");
}

static int of_parse_thermal_message(struct device_node *np)
{
	if (!np)
		return -EINVAL;

	if (of_property_read_string(np, NT_THERMAL_DT_BOARD_SENSOR, &board_sensor))
		board_sensor = "invalid";

	of_property_read_string(np, NT_THERMAL_DT_BOARD_ZONE, &board_sensor_zone);
	of_property_read_string(np, NT_THERMAL_DT_BOARD_ZONE2, &board_sensor_second_zone);

	pr_info("%s board sensor: %s (zone: %s, second: %s)\n", __func__,
		board_sensor,
		board_sensor_zone ?: "none",
		board_sensor_second_zone ?: "none");
	return 0;
}

static int nt_thermal_probe(struct platform_device *pdev)
{
	int ret;

#ifdef CONFIG_DRM_PANEL
	ret = thermal_check_panel(pdev->dev.of_node);
	if (ret == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (ret)
		pr_warn("%s: panel check failed (%d), continuing without notifier\n",
			__func__, ret);
#endif

	ret = cpu_thermal_init();
	if (ret)
		pr_err("%s: cpu_thermal_init failed: %d\n", __func__, ret);

	ret = of_parse_thermal_message(pdev->dev.of_node);
	if (ret)
		pr_err("%s: cannot parse thermal message node: %d\n",
		       __func__, ret);

	create_thermal_message_node();

#ifdef CONFIG_DRM_PANEL
	if (active_panel) {
		drm_notifier.notifier_call = nt_thermal_notify;
		ret = drm_panel_notifier_register(active_panel, &drm_notifier);
		if (ret)
			pr_err("%s: DRM panel notifier register failed: %d\n",
			       __func__, ret);
		else
			pr_info("%s: DRM panel notifier registered\n", __func__);
	}
#endif

	INIT_WORK(&usb_state.usb_state_work, usb_online_work);
	usb_state.psy_nb.notifier_call = usb_online_callback;
	ret = power_supply_reg_notifier(&usb_state.psy_nb);
	if (ret < 0)
		pr_err("%s: usb notifier registration failed: %d\n",
		       __func__, ret);

	return 0;
}

static int nt_thermal_remove(struct platform_device *pdev)
{
#ifdef CONFIG_DRM_PANEL
	if (active_panel && drm_notifier.notifier_call)
		drm_panel_notifier_unregister(active_panel, &drm_notifier);
#endif
	power_supply_unreg_notifier(&usb_state.psy_nb);
	cancel_work_sync(&usb_state.usb_state_work);
	destroy_thermal_message_node();
	destroy_thermal_cpu();
	return 0;
}

static const struct of_device_id nt_thermal_of_match[] = {
	{ .compatible = "nothing,nt-thermal-interface" },
	{ }
};
MODULE_DEVICE_TABLE(of, nt_thermal_of_match);

static struct platform_driver nt_thermal_driver = {
	.probe = nt_thermal_probe,
	.remove = nt_thermal_remove,
	.driver = {
		.name = "nt-thermal-interface",
		.of_match_table = nt_thermal_of_match,
	},
};
module_platform_driver(nt_thermal_driver);

MODULE_AUTHOR("Nothing kernel");
MODULE_DESCRIPTION("Nothing thermal control interface");
MODULE_LICENSE("GPL v2");