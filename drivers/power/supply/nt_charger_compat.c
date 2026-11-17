// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Project EVA / ZeroTwo
 * Universal Fast Charging & Charger Platform Compatibility layer.
 *
 * Provides architecture-agnostic and vendor-agnostic platform device stubs
 * (such as smart_charging / odm:smart_charging) required by OEM charger
 * frameworks (e.g. Transsion MT6789, MediaTek, and ColorOS/OPlus ported
 * frameworks). Safely co-exists with the in-kernel Nothing charging controls
 * already exposed by qti_battery_charger under /sys/class/qcom-battery/.
 *
 * Based on the universal charger compatibility work by RapliVx
 * <raffikun1203@gmail.com>.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/of.h>

static int smartchg_enable = 1;
static int smartchg_limit = 100;
static int smartchg_bypass = 0;

static ssize_t smart_charging_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", smartchg_enable);
}

static ssize_t smart_charging_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) == 0)
		smartchg_enable = val;
	return count;
}
static DEVICE_ATTR_RW(smart_charging);

static ssize_t smart_charging_limit_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", smartchg_limit);
}

static ssize_t smart_charging_limit_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) == 0)
		smartchg_limit = val;
	return count;
}
static DEVICE_ATTR_RW(smart_charging_limit);

static ssize_t bypass_charging_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", smartchg_bypass);
}

static ssize_t bypass_charging_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) == 0)
		smartchg_bypass = val;
	return count;
}
static DEVICE_ATTR_RW(bypass_charging);

static ssize_t smart_charging_status_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "normal\n");
}
static DEVICE_ATTR_RO(smart_charging_status);

static struct attribute *smartchg_attrs[] = {
	&dev_attr_smart_charging.attr,
	&dev_attr_smart_charging_limit.attr,
	&dev_attr_bypass_charging.attr,
	&dev_attr_smart_charging_status.attr,
	NULL,
};

static const struct attribute_group smartchg_attr_group = {
	.attrs = smartchg_attrs,
};

static struct platform_device *pdev_odm_smartchg;
static struct platform_device *pdev_smartchg;

static int __init nt_charger_compat_init(void)
{
	struct device *odm_parent = NULL;
	struct device *existing = NULL;

	pr_info("nt_charger_compat: Initializing universal charger compatibility\n");

	/*
	 * 1. Register odm:smart_charging if not already present in the
	 *    hardware tree (OEM frameworks look this platform device up by
	 *    name and then attach to its sysfs attributes).
	 */
	existing = bus_find_device_by_name(&platform_bus_type, NULL,
					   "odm:smart_charging");
	if (existing) {
		pr_info("nt_charger_compat: odm:smart_charging already present in hardware tree\n");
		put_device(existing);
	} else {
		odm_parent = bus_find_device_by_name(&platform_bus_type, NULL,
						     "odm");

		pdev_odm_smartchg = platform_device_alloc("odm:smart_charging", -1);
		if (pdev_odm_smartchg) {
			if (odm_parent)
				pdev_odm_smartchg->dev.parent = odm_parent;

			if (platform_device_add(pdev_odm_smartchg) == 0) {
				if (sysfs_create_group(&pdev_odm_smartchg->dev.kobj,
						       &smartchg_attr_group))
					pr_warn("nt_charger_compat: failed to create sysfs group on odm:smart_charging\n");
				pr_info("nt_charger_compat: Registered universal odm:smart_charging stub device\n");
			} else {
				platform_device_put(pdev_odm_smartchg);
				pdev_odm_smartchg = NULL;
			}
		}

		if (odm_parent)
			put_device(odm_parent);
	}

	/*
	 * 2. Also register a plain smart_charging platform device if not
	 *    present, for frameworks that look the node up without the odm
	 *    prefix.
	 */
	existing = bus_find_device_by_name(&platform_bus_type, NULL,
					   "smart_charging");
	if (existing) {
		put_device(existing);
	} else {
		pdev_smartchg = platform_device_register_simple("smart_charging",
								-1, NULL, 0);
		if (!IS_ERR(pdev_smartchg)) {
			if (sysfs_create_group(&pdev_smartchg->dev.kobj,
					       &smartchg_attr_group))
				pr_warn("nt_charger_compat: failed to create sysfs group on smart_charging\n");
			pr_info("nt_charger_compat: Registered universal smart_charging platform device\n");
		} else {
			pdev_smartchg = NULL;
		}
	}

	return 0;
}

static void __exit nt_charger_compat_exit(void)
{
	if (pdev_odm_smartchg) {
		sysfs_remove_group(&pdev_odm_smartchg->dev.kobj,
				   &smartchg_attr_group);
		platform_device_unregister(pdev_odm_smartchg);
		pdev_odm_smartchg = NULL;
	}
	if (pdev_smartchg) {
		sysfs_remove_group(&pdev_smartchg->dev.kobj,
				   &smartchg_attr_group);
		platform_device_unregister(pdev_smartchg);
		pdev_smartchg = NULL;
	}
}

subsys_initcall(nt_charger_compat_init);
module_exit(nt_charger_compat_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Willay24, based on work by RapliVx <raffikun1203@gmail.com>");
MODULE_DESCRIPTION("Nothing Phone (1) universal charger compatibility layer");
