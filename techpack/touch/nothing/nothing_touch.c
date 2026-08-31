// SPDX-License-Identifier: GPL-2.0-only

#include "nothing_touch.h"

static struct nothing_touch nothing_touch_dev;
static struct nothing_touch_interface nothing_touch_intf;

struct nothing_touch *nothing_touch_dev_get(int minor)
{
	if (nothing_touch_dev.misc_dev.minor == minor)
		return &nothing_touch_dev;
	return NULL;
}
EXPORT_SYMBOL_GPL(nothing_touch_dev_get);

struct class *get_nothing_touch_class(void)
{
	return nothing_touch_dev.class;
}
EXPORT_SYMBOL_GPL(get_nothing_touch_class);

int nothing_touch_register_modedata(int touch_id,
				 struct nothing_touch_interface *data)
{
	if (!data)
		return -EINVAL;

	nothing_touch_intf = *data;
	return 0;
}
EXPORT_SYMBOL_GPL(nothing_touch_register_modedata);

int update_nothing_palm_sensor_value(int value)
{
	return value;
}
EXPORT_SYMBOL_GPL(update_nothing_palm_sensor_value);

int update_nothing_prox_sensor_value(int value)
{
	return value;
}
EXPORT_SYMBOL_GPL(update_nothing_prox_sensor_value);

static int __init nothing_touch_init(void)
{
	memset(&nothing_touch_dev, 0, sizeof(nothing_touch_dev));
	mutex_init(&nothing_touch_dev.mutex);
	mutex_init(&nothing_touch_dev.palm_mutex);
	init_waitqueue_head(&nothing_touch_dev.wait_queue);
	nothing_touch_dev.class = class_create(THIS_MODULE, "touch");
	if (IS_ERR(nothing_touch_dev.class))
		return PTR_ERR(nothing_touch_dev.class);
	return 0;
}

static void __exit nothing_touch_exit(void)
{
	if (!IS_ERR_OR_NULL(nothing_touch_dev.class))
		class_destroy(nothing_touch_dev.class);
}

module_init(nothing_touch_init);
module_exit(nothing_touch_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Nothing touch abstraction for Goodix Linux touchscreen");
