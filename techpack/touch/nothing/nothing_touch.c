// SPDX-License-Identifier: GPL-2.0-only

#include "nothing_touch.h"

static struct nothing_touch_pdata *touch_pdata;
static struct nothing_touch nothing_touch_dev;

#define RAW_SIZE (PAGE_SIZE * 12)

static int nothing_touch_dev_open(struct inode *inode, struct file *file)
{
	if (!READ_ONCE(touch_pdata))
		return -ENODEV;

	return 0;
}

static ssize_t nothing_touch_dev_read(struct file *file, char __user *buf,
				      size_t count, loff_t *pos)
{
	return 0;
}

static ssize_t nothing_touch_dev_write(struct file *file,
				       const char __user *buf, size_t count,
				       loff_t *pos)
{
	return 0;
}

static unsigned int nothing_touch_dev_poll(struct file *file, poll_table *wait)
{
	return 0;
}

static void nothing_touch_notify_cmd(struct nothing_touch_interface *touch_data,
				     int user_cmd, int *buf)
{
	switch (user_cmd) {
	case SET_CUR_VALUE:
		touch_data->thp_cmd_buf[0] = user_cmd;
		touch_data->thp_cmd_buf[1] = buf[0];
		touch_data->thp_cmd_buf[2] = buf[1];
		touch_data->thp_cmd_buf[3] = buf[2];
		touch_data->thp_cmd_size = 4;
		break;
	case SET_LONG_VALUE:
		touch_data->thp_cmd_buf[0] = user_cmd;
		touch_data->thp_cmd_buf[1] = buf[0];
		touch_data->thp_cmd_buf[2] = buf[1];
		touch_data->thp_cmd_buf[3] = buf[2];
		memcpy(&touch_data->thp_cmd_buf[4], &buf[3],
		       sizeof(int) * buf[2]);
		touch_data->thp_cmd_size = 4 + buf[2];
		break;
	case RESET_MODE:
		touch_data->thp_cmd_buf[0] = user_cmd;
		touch_data->thp_cmd_buf[1] = buf[0];
		touch_data->thp_cmd_buf[2] = buf[1];
		touch_data->thp_cmd_size = 3;
		break;
	default:
		return;
	}

	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "touch_thp_cmd");
}

static long nothing_touch_dev_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	int ret = 0;
	int buf[MAX_BUF_SIZE] = { 0, };
	void __user *argp = (void __user *)arg;
	struct nothing_touch_pdata *pdata;
	struct nothing_touch_interface *touch_data;
	int user_cmd = _IOC_NR(cmd);

	if (copy_from_user(buf, argp, sizeof(buf)))
		return -EFAULT;

	if (buf[0] < 0 || buf[0] > 1) {
		pr_err("%s invalid param\n", __func__);
		return -EINVAL;
	}

	if (buf[1] < 0 || buf[1] >= Touch_Mode_NUM) {
		pr_err("%s invalid mode:%d\n", __func__, buf[1]);
		return -EINVAL;
	}

	if (user_cmd == SET_LONG_VALUE &&
	    (buf[2] < 0 || buf[2] > MAX_BUF_SIZE - 4)) {
		pr_err("%s invalid length:%d\n", __func__, buf[2]);
		return -EINVAL;
	}

	mutex_lock(&nothing_touch_dev.mutex);

	pdata = touch_pdata;
	if (!pdata || !pdata->touch_data[buf[0]]) {
		pr_err("%s invalid memory\n", __func__);
		mutex_unlock(&nothing_touch_dev.mutex);
		return -ENODEV;
	}
	touch_data = pdata->touch_data[buf[0]];

	pr_debug("%s cmd:%d, touchId:%d, mode:%d, value:%d\n", __func__,
		 user_cmd, buf[0], buf[1], buf[2]);

	switch (user_cmd) {
	case SET_CUR_VALUE:
		if (touch_data->setModeValue)
			buf[0] = touch_data->setModeValue(buf[1], buf[2]);
		break;
	case GET_CUR_VALUE:
	case GET_DEF_VALUE:
	case GET_MIN_VALUE:
	case GET_MAX_VALUE:
		if (touch_data->getModeValue)
			buf[0] = touch_data->getModeValue(buf[1], user_cmd);
		break;
	case RESET_MODE:
		if (touch_data->resetMode)
			buf[0] = touch_data->resetMode(buf[1]);
		break;
	case GET_MODE_VALUE:
		if (touch_data->getModeAll)
			ret = touch_data->getModeAll(buf[1], buf);
		break;
	case SET_LONG_VALUE:
		if (touch_data->setModeLongValue)
			ret = touch_data->setModeLongValue(buf[1], buf[2],
							   &buf[3]);
		break;
	default:
		pr_err("%s don't support mode\n", __func__);
		ret = -EINVAL;
		break;
	}

	if (ret >= 0)
		nothing_touch_notify_cmd(touch_data, user_cmd, buf);
	else
		pr_err("%s can't get data from touch driver\n", __func__);

	mutex_unlock(&nothing_touch_dev.mutex);

	if (ret < 0)
		return ret;

	return copy_to_user(argp, buf, sizeof(buf)) ? -EFAULT : 0;
}

static int nothing_touch_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct nothing_touch_pdata *pdata = READ_ONCE(touch_pdata);
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (!pdata) {
		pr_err("%s invalid memory\n", __func__);
		return -ENODEV;
	}

	if (!size || offset >= RAW_SIZE || size > RAW_SIZE - offset) {
		pr_err("%s invalid mmap range\n", __func__);
		return -EINVAL;
	}

	if (remap_pfn_range(vma, vma->vm_start,
			    (pdata->phy_base + offset) >> PAGE_SHIFT, size,
			    vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static int nothing_touch_dev_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations nothingtouch_dev_fops = {
	.owner = THIS_MODULE,
	.open = nothing_touch_dev_open,
	.read = nothing_touch_dev_read,
	.write = nothing_touch_dev_write,
	.poll = nothing_touch_dev_poll,
	.mmap = nothing_touch_dev_mmap,
	.unlocked_ioctl = nothing_touch_dev_ioctl,
	.compat_ioctl = nothing_touch_dev_ioctl,
	.release = nothing_touch_dev_release,
	.llseek = no_llseek,
};

static struct nothing_touch nothing_touch_dev = {
	.misc_dev = {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "nothing-touch",
		.fops = &nothingtouch_dev_fops,
		.parent = NULL,
	},
	.mutex = __MUTEX_INITIALIZER(nothing_touch_dev.mutex),
};

struct nothing_touch *nothing_touch_dev_get(int minor)
{
	if (nothing_touch_dev.misc_dev.minor == minor)
		return &nothing_touch_dev;
	else
		return NULL;
}

struct class *get_nothing_touch_class(void)
{
	return nothing_touch_dev.class;
}
EXPORT_SYMBOL_GPL(get_nothing_touch_class);

struct device *get_nothing_touch_dev(void)
{
	return nothing_touch_dev.dev;
}
EXPORT_SYMBOL_GPL(get_nothing_touch_dev);

bool nothing_touch_ready(void)
{
	return READ_ONCE(touch_pdata) != NULL;
}
EXPORT_SYMBOL_GPL(nothing_touch_ready);

#define NOTHING_TOUCH_REG(_f)						\
	do {								\
		if (data->_f)						\
			touch_data->_f = data->_f;			\
	} while (0)

int nothingtouch_register_modedata(int touchId,
				   struct nothing_touch_interface *data)
{
	struct nothing_touch_interface *touch_data;

	if (!data || touchId < 0 || touchId > 1)
		return -EINVAL;

	mutex_lock(&nothing_touch_dev.mutex);

	if (!touch_pdata) {
		mutex_unlock(&nothing_touch_dev.mutex);
		return -EPROBE_DEFER;
	}

	touch_data = touch_pdata->touch_data[touchId];

	NOTHING_TOUCH_REG(setModeValue);
	NOTHING_TOUCH_REG(getModeValue);
	NOTHING_TOUCH_REG(resetMode);
	NOTHING_TOUCH_REG(getModeAll);
	NOTHING_TOUCH_REG(setModeLongValue);
	NOTHING_TOUCH_REG(palm_sensor_read);
	NOTHING_TOUCH_REG(palm_sensor_write);
	NOTHING_TOUCH_REG(prox_sensor_read);
	NOTHING_TOUCH_REG(prox_sensor_write);
	NOTHING_TOUCH_REG(enable_touch_raw);
	NOTHING_TOUCH_REG(enable_clicktouch_raw);
	NOTHING_TOUCH_REG(enable_touch_delta);
	NOTHING_TOUCH_REG(get_touch_rx_num);
	NOTHING_TOUCH_REG(get_touch_tx_num);
	NOTHING_TOUCH_REG(get_touch_x_resolution);
	NOTHING_TOUCH_REG(get_touch_y_resolution);
	NOTHING_TOUCH_REG(panel_vendor_read);
	NOTHING_TOUCH_REG(panel_color_read);
	NOTHING_TOUCH_REG(panel_display_read);
	NOTHING_TOUCH_REG(touch_vendor_read);

	mutex_unlock(&nothing_touch_dev.mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(nothingtouch_register_modedata);

int update_palm_sensor_value(int value)
{
	struct nothing_touch_pdata *pdata = READ_ONCE(touch_pdata);

	if (!pdata || !nothing_touch_dev.dev)
		return -ENODEV;

	if (xchg(&pdata->palm_value, value) != value) {
		pdata->palm_changed = true;
		sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "palm_sensor");
	}

	return 0;
}
EXPORT_SYMBOL_GPL(update_palm_sensor_value);

int update_prox_sensor_value(int value)
{
	struct nothing_touch_pdata *pdata = READ_ONCE(touch_pdata);

	if (!pdata || !nothing_touch_dev.dev)
		return -ENODEV;

	if (xchg(&pdata->prox_value, value) != value) {
		pdata->prox_changed = true;
		sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "prox_sensor");
	}

	return 0;
}
EXPORT_SYMBOL_GPL(update_prox_sensor_value);

int update_fod_press_status(int value)
{
	struct nothing_touch_pdata *pdata = READ_ONCE(touch_pdata);

	if (!pdata || !nothing_touch_dev.dev)
		return -ENODEV;

	if (xchg(&pdata->fod_press_status_value, value) != value)
		sysfs_notify(&nothing_touch_dev.dev->kobj, NULL,
			     "fod_press_status");

	return 0;
}
EXPORT_SYMBOL_GPL(update_fod_press_status);

int nothing_touch_set_suspend_state(int state)
{
	struct nothing_touch_pdata *pdata = READ_ONCE(touch_pdata);

	if (!pdata || !nothing_touch_dev.dev)
		return -ENODEV;

	WRITE_ONCE(pdata->suspend_state, state);
	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "suspend_state");

	return 0;
}
EXPORT_SYMBOL_GPL(nothing_touch_set_suspend_state);

int notify_gesture_single_tap(void)
{
	if (!READ_ONCE(touch_pdata) || !nothing_touch_dev.dev)
		return -ENODEV;

	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL,
		     "gesture_single_tap_state");

	return 0;
}
EXPORT_SYMBOL_GPL(notify_gesture_single_tap);

int notify_gesture_double_tap(void)
{
	if (!READ_ONCE(touch_pdata) || !nothing_touch_dev.dev)
		return -ENODEV;

	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL,
		     "gesture_double_tap_state");

	return 0;
}
EXPORT_SYMBOL_GPL(notify_gesture_double_tap);

void nothing_touch_record_event(u32 slot, bool down)
{
	struct nothing_touch_pdata *pdata = READ_ONCE(touch_pdata);
	struct last_touch_event *events;
	struct touch_event *event;
	u32 head;

	if (!pdata || slot >= MAX_TOUCH_ID)
		return;

	if (down ? test_and_set_bit(slot, &pdata->slot_state) :
		   !test_and_clear_bit(slot, &pdata->slot_state))
		return;

	events = pdata->last_touch_events;
	head = (u32)atomic_fetch_inc(&events->head) &
	       (LAST_TOUCH_EVENTS_MAX - 1);

	event = &events->touch_event_buf[head];
	event->slot = slot;
	event->touch_ns = ktime_get_real_ns();
	WRITE_ONCE(event->state, down ? EVENT_DOWN : EVENT_UP);
}
EXPORT_SYMBOL_GPL(nothing_touch_record_event);

static int nothing_touch_set_bump(bool on)
{
	static const int bump_modes[] = { Touch_UP_THRESHOLD, Touch_Tolerance };
	struct nothing_touch_interface *touch_data = touch_pdata->touch_data[0];
	int value;
	int i;

	if (!touch_data->setModeValue)
		return -ENODEV;

	if (!on) {
		if (touch_data->resetMode)
			touch_data->resetMode(Touch_Game_Mode);
		return 0;
	}

	touch_data->setModeValue(Touch_Game_Mode, 1);
	touch_data->setModeValue(Touch_Active_MODE, 1);

	for (i = 0; i < ARRAY_SIZE(bump_modes) && touch_data->getModeValue; i++) {
		value = touch_data->getModeValue(bump_modes[i], GET_MAX_VALUE);
		if (value > 0)
			touch_data->setModeValue(bump_modes[i], value);
	}

	touch_data->setModeValue(Touch_Edge_Filter, 0);

	return 0;
}

static ssize_t bump_sample_rate_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", touch_pdata->bump_sample_rate);
}

static ssize_t bump_sample_rate_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int input;
	int ret;

	if (kstrtoint(buf, 0, &input))
		return -EINVAL;

	mutex_lock(&nothing_touch_dev.mutex);

	if (!touch_pdata) {
		mutex_unlock(&nothing_touch_dev.mutex);
		return -ENODEV;
	}

	ret = nothing_touch_set_bump(!!input);
	if (!ret)
		touch_pdata->bump_sample_rate = !!input;

	mutex_unlock(&nothing_touch_dev.mutex);

	return ret ? ret : count;
}

static ssize_t enable_touchraw_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n",
			  touch_pdata->touch_data[0]->is_enable_touchraw);
}

static ssize_t enable_touchraw_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data;
	int input;

	if (!touch_pdata)
		return -ENODEV;

	if (kstrtoint(buf, 0, &input))
		return -EINVAL;

	touch_data = touch_pdata->touch_data[0];

	if (touch_data->enable_touch_raw)
		touch_data->enable_touch_raw(!!input);

	touch_data->is_enable_touchraw = !!input;

	return count;
}

static ssize_t enable_touchdelta_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n",
			  touch_pdata->touch_data[0]->is_enable_touchdelta);
}

static ssize_t enable_touchdelta_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data;
	int input;

	if (!touch_pdata)
		return -ENODEV;

	if (kstrtoint(buf, 0, &input))
		return -EINVAL;

	touch_data = touch_pdata->touch_data[0];

	if (touch_data->enable_touch_delta)
		touch_data->enable_touch_delta(!!input);

	touch_data->is_enable_touchdelta = !!input;

	return count;
}

static ssize_t thp_cmd_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data;
	int size;

	mutex_lock(&nothing_touch_dev.mutex);

	if (!touch_pdata) {
		mutex_unlock(&nothing_touch_dev.mutex);
		return -ENODEV;
	}

	touch_data = touch_pdata->touch_data[0];
	size = clamp(touch_data->thp_cmd_size, 0, MAX_BUF_SIZE) * sizeof(int);
	memcpy(buf, touch_data->thp_cmd_buf, size);

	mutex_unlock(&nothing_touch_dev.mutex);

	return size;
}

static ssize_t thp_cmd_status_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data;
	unsigned int input[MAX_BUF_SIZE] = { 0, };
	bool new_data = false;
	int para_cnt = 0;
	int i = 0;
	const char *p;

	for (p = buf; *p != '\0'; p++) {
		if (*p >= '0' && *p <= '9') {
			if (i >= MAX_BUF_SIZE)
				break;
			input[i] = input[i] * 10 + (*p - '0');
			if (!new_data) {
				new_data = true;
				para_cnt++;
			}
		} else if (*p == ' ') {
			if (new_data) {
				i++;
				new_data = false;
			}
		} else {
			break;
		}
	}

	mutex_lock(&nothing_touch_dev.mutex);

	if (!touch_pdata) {
		mutex_unlock(&nothing_touch_dev.mutex);
		return -ENODEV;
	}

	touch_data = touch_pdata->touch_data[0];
	memcpy(touch_data->thp_cmd_buf, input, sizeof(int) * para_cnt);
	touch_data->thp_cmd_size = para_cnt;
	sysfs_notify(&dev->kobj, NULL, "touch_thp_cmd");

	mutex_unlock(&nothing_touch_dev.mutex);

	return count;
}

#define THP_RO_ATTR(_node, _cb, _fmt)					\
static ssize_t _node##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct nothing_touch_pdata *pdata = touch_pdata;		\
									\
	if (!pdata)							\
		return -ENODEV;						\
									\
	if (!pdata->touch_data[0]->_cb)					\
		return 0;						\
									\
	return sysfs_emit(buf, _fmt, pdata->touch_data[0]->_cb());	\
}									\
static DEVICE_ATTR(_node, 0444, _node##_show, NULL)

THP_RO_ATTR(touch_thp_tx_num, get_touch_tx_num, "%d\n");
THP_RO_ATTR(touch_thp_rx_num, get_touch_rx_num, "%d\n");
THP_RO_ATTR(touch_thp_x_resolution, get_touch_x_resolution, "%d\n");
THP_RO_ATTR(touch_thp_y_resolution, get_touch_y_resolution, "%d\n");
THP_RO_ATTR(panel_vendor, panel_vendor_read, "%c");
THP_RO_ATTR(panel_color, panel_color_read, "%c");
THP_RO_ATTR(panel_display, panel_display_read, "%c");
THP_RO_ATTR(touch_vendor, touch_vendor_read, "%c");

#define THP_ATTR(_node, _field)						\
static ssize_t _node##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	if (!touch_pdata)						\
		return -ENODEV;						\
									\
	return sysfs_emit(buf, "%d\n",					\
			  touch_pdata->touch_data[0]->_field);		\
}									\
									\
static ssize_t _node##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	int input;							\
									\
	if (!touch_pdata)						\
		return -ENODEV;						\
									\
	if (kstrtoint(buf, 0, &input))					\
		return -EINVAL;						\
									\
	touch_pdata->touch_data[0]->_field = input;			\
	sysfs_notify(&dev->kobj, NULL, #_node);				\
									\
	return count;							\
}									\
static DEVICE_ATTR(_node, 0664, _node##_show, _node##_store)

THP_ATTR(touch_thp_downthd, thp_downthreshold);
THP_ATTR(touch_thp_upthd, thp_upthreshold);
THP_ATTR(touch_thp_movethd, thp_movethreshold);
THP_ATTR(touch_thp_islandthd, thp_islandthreshold);
THP_ATTR(touch_thp_smooth, thp_smooth);
THP_ATTR(touch_thp_dump, thp_dump_raw);
THP_ATTR(touch_thp_noisefilter, thp_noisefilter);

static ssize_t palm_sensor_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;

	touch_pdata->palm_changed = false;

	return sysfs_emit(buf, "%d\n", touch_pdata->palm_value);
}

static ssize_t palm_sensor_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	int input;

	if (!touch_pdata)
		return -ENODEV;

	if (kstrtoint(buf, 0, &input))
		return -EINVAL;

	if (touch_pdata->touch_data[0]->palm_sensor_write)
		touch_pdata->touch_data[0]->palm_sensor_write(!!input);
	else
		pr_err("%s has not implement\n", __func__);

	return count;
}

static ssize_t prox_sensor_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;

	touch_pdata->prox_changed = false;

	return sysfs_emit(buf, "%d\n", touch_pdata->prox_value);
}

static ssize_t prox_sensor_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	int input;

	if (!touch_pdata)
		return -ENODEV;

	if (kstrtoint(buf, 0, &input))
		return -EINVAL;

	if (touch_pdata->touch_data[0]->prox_sensor_write)
		touch_pdata->touch_data[0]->prox_sensor_write(!!input);
	else
		pr_err("%s has not implement\n", __func__);

	return count;
}

static ssize_t suspend_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", READ_ONCE(touch_pdata->suspend_state));
}

static ssize_t fod_press_status_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", touch_pdata->fod_press_status_value);
}

static ssize_t gesture_single_tap_state_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%d\n", 1);
}

static ssize_t gesture_double_tap_state_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%d\n", 1);
}

static ssize_t update_rawdata_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", "1");
}

static DEVICE_ATTR(enable_touch_raw, 0664, enable_touchraw_show, enable_touchraw_store);
static DEVICE_ATTR(enable_touch_delta, 0664, enable_touchdelta_show, enable_touchdelta_store);
static DEVICE_ATTR(touch_thp_cmd, 0664, thp_cmd_status_show, thp_cmd_status_store);
static DEVICE_ATTR(palm_sensor, 0664, palm_sensor_show, palm_sensor_store);
static DEVICE_ATTR(prox_sensor, 0664, prox_sensor_show, prox_sensor_store);
static DEVICE_ATTR(bump_sample_rate, 0664, bump_sample_rate_show, bump_sample_rate_store);
static DEVICE_ATTR(suspend_state, 0444, suspend_state_show, NULL);
static DEVICE_ATTR(fod_press_status, 0444, fod_press_status_show, NULL);
static DEVICE_ATTR(gesture_single_tap_state, 0444, gesture_single_tap_state_show, NULL);
static DEVICE_ATTR(gesture_double_tap_state, 0444, gesture_double_tap_state_show, NULL);
static DEVICE_ATTR(update_rawdata, 0444, update_rawdata_show, NULL);

static struct attribute *touch_attr_group[] = {
	&dev_attr_enable_touch_raw.attr,
	&dev_attr_enable_touch_delta.attr,
	&dev_attr_touch_thp_cmd.attr,
	&dev_attr_touch_thp_tx_num.attr,
	&dev_attr_touch_thp_rx_num.attr,
	&dev_attr_touch_thp_x_resolution.attr,
	&dev_attr_touch_thp_y_resolution.attr,
	&dev_attr_touch_thp_downthd.attr,
	&dev_attr_touch_thp_upthd.attr,
	&dev_attr_touch_thp_movethd.attr,
	&dev_attr_touch_thp_islandthd.attr,
	&dev_attr_touch_thp_smooth.attr,
	&dev_attr_touch_thp_dump.attr,
	&dev_attr_touch_thp_noisefilter.attr,
	&dev_attr_palm_sensor.attr,
	&dev_attr_prox_sensor.attr,
	&dev_attr_bump_sample_rate.attr,
	&dev_attr_panel_vendor.attr,
	&dev_attr_panel_color.attr,
	&dev_attr_panel_display.attr,
	&dev_attr_touch_vendor.attr,
	&dev_attr_update_rawdata.attr,
	&dev_attr_suspend_state.attr,
	&dev_attr_fod_press_status.attr,
	&dev_attr_gesture_single_tap_state.attr,
	&dev_attr_gesture_double_tap_state.attr,
	NULL,
};

static void *event_start(struct seq_file *m, loff_t *p)
{
	struct last_touch_event *event;
	u32 pos;

	if (!touch_pdata || !touch_pdata->last_touch_events)
		return NULL;

	event = touch_pdata->last_touch_events;
	if (*p >= LAST_TOUCH_EVENTS_MAX)
		return NULL;

	pos = ((u32)atomic_read(&event->head) + *p) &
	      (LAST_TOUCH_EVENTS_MAX - 1);

	return event->touch_event_buf + pos;
}

static void *event_next(struct seq_file *m, void *v, loff_t *p)
{
	struct last_touch_event *event;
	u32 pos;

	if (!touch_pdata || !touch_pdata->last_touch_events)
		return NULL;

	event = touch_pdata->last_touch_events;
	if (++*p >= LAST_TOUCH_EVENTS_MAX)
		return NULL;

	pos = ((u32)atomic_read(&event->head) + *p) &
	      (LAST_TOUCH_EVENTS_MAX - 1);

	return event->touch_event_buf + pos;
}

static int event_show(struct seq_file *m, void *v)
{
	struct touch_event *event_info = v;
	struct timespec64 ts;
	struct rtc_time tm;
	enum touch_state state = READ_ONCE(event_info->state);

	if (state == EVENT_INIT)
		return 0;

	ts = ns_to_timespec64(event_info->touch_ns);
	rtc_time64_to_tm(ts.tv_sec, &tm);
	seq_printf(m, "%d-%02d-%02d %02d:%02d:%02d.%09ld UTC Finger (%2u) %s\n",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		   tm.tm_min, tm.tm_sec, ts.tv_nsec, event_info->slot,
		   state == EVENT_DOWN ? "P" : "R");

	return 0;
}

static void event_stop(struct seq_file *m, void *v)
{
}

static const struct seq_operations last_touch_events_seq_ops = {
	.start = event_start,
	.next = event_next,
	.stop = event_stop,
	.show = event_show,
};

static int last_touch_events_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &last_touch_events_seq_ops);
}

static const struct proc_ops last_touch_events_ops = {
	.proc_open = last_touch_events_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,
};

static int nothing_touch_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct nothing_touch_pdata *pdata;
	unsigned long raw_page;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->touch_data[0] = devm_kzalloc(dev, sizeof(*pdata->touch_data[0]),
					    GFP_KERNEL);
	pdata->touch_data[1] = devm_kzalloc(dev, sizeof(*pdata->touch_data[1]),
					    GFP_KERNEL);
	pdata->last_touch_events = devm_kzalloc(dev,
						sizeof(*pdata->last_touch_events),
						GFP_KERNEL);
	raw_page = devm_get_free_pages(dev, GFP_KERNEL | __GFP_ZERO,
				       get_order(RAW_SIZE));
	if (!pdata->touch_data[0] || !pdata->touch_data[1] ||
	    !pdata->last_touch_events || !raw_page) {
		pr_err("%s alloc mem for touch_data\n", __func__);
		return -ENOMEM;
	}

	pdata->raw_data = (unsigned int *)raw_page;
	pdata->phy_base = virt_to_phys(pdata->raw_data);

	nothing_touch_dev.class = class_create(THIS_MODULE, "touch");
	if (IS_ERR(nothing_touch_dev.class)) {
		ret = PTR_ERR(nothing_touch_dev.class);
		nothing_touch_dev.class = NULL;
		pr_err("%s create device class err:%d\n", __func__, ret);
		return ret;
	}

	nothing_touch_dev.dev = device_create(nothing_touch_dev.class, NULL, 'T',
					      NULL, "touch_dev");
	if (IS_ERR(nothing_touch_dev.dev)) {
		ret = PTR_ERR(nothing_touch_dev.dev);
		nothing_touch_dev.dev = NULL;
		pr_err("%s create device dev err:%d\n", __func__, ret);
		goto device_create_err;
	}

	pdata->device = &nothing_touch_dev;
	dev_set_drvdata(nothing_touch_dev.dev, pdata);

	nothing_touch_dev.attrs.attrs = touch_attr_group;
	ret = sysfs_create_group(&nothing_touch_dev.dev->kobj,
				 &nothing_touch_dev.attrs);
	if (ret) {
		pr_err("%s ERROR: Cannot create sysfs structure!:%d\n",
		       __func__, ret);
		goto sys_group_err;
	}

	ret = misc_register(&nothing_touch_dev.misc_dev);
	if (ret) {
		pr_err("%s create misc device err:%d\n", __func__, ret);
		goto misc_err;
	}

	pdata->last_touch_events_proc = proc_create("last_touch_events", 0444,
						    NULL, &last_touch_events_ops);

	WRITE_ONCE(touch_pdata, pdata);

	pr_info("%s probe successful\n", __func__);

	return 0;

misc_err:
	sysfs_remove_group(&nothing_touch_dev.dev->kobj, &nothing_touch_dev.attrs);
sys_group_err:
	device_destroy(nothing_touch_dev.class, 'T');
	nothing_touch_dev.dev = NULL;
device_create_err:
	class_destroy(nothing_touch_dev.class);
	nothing_touch_dev.class = NULL;

	return ret;
}

static int nothing_touch_remove(struct platform_device *pdev)
{
	struct nothing_touch_pdata *pdata;

	mutex_lock(&nothing_touch_dev.mutex);
	pdata = touch_pdata;
	WRITE_ONCE(touch_pdata, NULL);
	mutex_unlock(&nothing_touch_dev.mutex);

	if (!pdata)
		return 0;

	if (pdata->last_touch_events_proc)
		proc_remove(pdata->last_touch_events_proc);

	misc_deregister(&nothing_touch_dev.misc_dev);
	sysfs_remove_group(&nothing_touch_dev.dev->kobj, &nothing_touch_dev.attrs);
	device_destroy(nothing_touch_dev.class, 'T');
	nothing_touch_dev.dev = NULL;
	class_destroy(nothing_touch_dev.class);
	nothing_touch_dev.class = NULL;

	return 0;
}

static const struct of_device_id nothing_touch_of_match[] = {
	{ .compatible = "nothing-touch", },
	{},
};

static struct platform_driver nothing_touch_device_driver = {
	.probe = nothing_touch_probe,
	.remove = nothing_touch_remove,
	.driver = {
		.name = "nothing-touch",
		.of_match_table = of_match_ptr(nothing_touch_of_match),
	}
};

static int __init nothing_touch_init(void)
{
	return platform_driver_register(&nothing_touch_device_driver);
}

static void __exit nothing_touch_exit(void)
{
	platform_driver_unregister(&nothing_touch_device_driver);
}

subsys_initcall(nothing_touch_init);
module_exit(nothing_touch_exit);

MODULE_LICENSE("GPL");
