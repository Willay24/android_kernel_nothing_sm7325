// SPDX-License-Identifier: GPL-2.0
/*
 * Multimedia hardware detection via sysfs for Nothing Phone (1) (Spacewar)
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/mmhardware_sysfs.h>

/* show */
static ssize_t mm_register_show(struct kobject *dev,
	struct kobj_attribute *a, char *buf)
{
	struct mm_info *mi = container_of(a, struct mm_info, k_attr);

	if (!mi->on_register)
		pr_debug("%s: 0x%x is not registered\n", __func__, mi->mm_id);

	return sprintf(buf, "%d\n", mi->on_register);
}

/* store: debug override, echo 0/1 > /sys/mm_hardware/<node> */
static ssize_t mm_register_store(struct kobject *dev,
	struct kobj_attribute *a, const char *buf, size_t count)
{
	struct mm_info *mi = container_of(a, struct mm_info, k_attr);
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	mi->on_register = !!val;
	return count;
}

MM_INFO(MM_HW_ADSP, adsp);
MM_INFO(MM_HW_CODEC, codec);
MM_INFO(MM_HW_PA_1, speaker_l);
MM_INFO(MM_HW_PA_2, speaker_r);
MM_INFO(MM_HW_HAPTIC_1, haptic);
MM_INFO(MM_HW_AS, audioswitch);

static struct attribute *mm_attrs[] = {
	&adsp_info.k_attr.attr,
	&codec_info.k_attr.attr,
	&speaker_l_info.k_attr.attr,
	&speaker_r_info.k_attr.attr,
	&haptic_info.k_attr.attr,
	&audioswitch_info.k_attr.attr,
	NULL,
};

static struct attribute_group mm_attr_group = {
	.attrs = mm_attrs,
};

static struct kobject *mm_sysfs_kobj;

int mmhardware_initialize_sysfs(void)
{
	int err;

	/* create mm_hardware under /sys */
	mm_sysfs_kobj = kobject_create_and_add(MM_HARDWARE_SYSFS_ROOT_FOLDER,
					       kernel_kobj->parent);
	if (!mm_sysfs_kobj) {
		pr_err("%s: failed to create kobj\n", __func__);
		return -ENOMEM;
	}

	err = sysfs_create_group(mm_sysfs_kobj, &mm_attr_group);
	if (err) {
		pr_err("%s: failed to create sysfs group\n", __func__);
		kobject_put(mm_sysfs_kobj);
		return err;
	}

	return 0;
}

void mmhardware_cleanup_sysfs(void)
{
	sysfs_remove_group(mm_sysfs_kobj, &mm_attr_group);
	kobject_put(mm_sysfs_kobj);
}

int register_kobj_under_mmsysfs(enum hardware_id mm_id, const char *name)
{
	int iter = 0;
	struct mm_info *mi = NULL;

	if (!name) {
		pr_err("%s: device_name is empty\n", __func__);
		return -EINVAL;
	}

	while (mm_attrs[iter]) {
		mi = container_of(mm_attrs[iter], struct mm_info, k_attr.attr);
		iter++;

		if (mi->mm_id != mm_id)
			continue;

		if (mi->on_register) {
			pr_info("%s: device(id:%d name:%s) has already registered\n",
				__func__, mi->mm_id, name);
			return -EEXIST;
		}
		mi->on_register = 1;
		return 0;
	}

	pr_err("%s: can't find hardware_id 0x%x\n", __func__, mm_id);
	return -ENOENT;
}
EXPORT_SYMBOL(register_kobj_under_mmsysfs);

subsys_initcall(mmhardware_initialize_sysfs);
module_exit(mmhardware_cleanup_sysfs);

MODULE_AUTHOR("Nothing Spacewar");
MODULE_DESCRIPTION("Multimedia hardware detection for SM7325");
MODULE_LICENSE("GPL");
