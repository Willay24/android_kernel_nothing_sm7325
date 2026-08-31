// SPDX-License-Identifier: GPL-2.0-only

#ifndef __NOTHING_TOUCH_H
#define __NOTHING_TOUCH_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

#define NOTHING_TOUCH_MODE_COUNT 20
#define NOTHING_TOUCH_VALUE_TYPE_SIZE 6
#define NOTHING_TOUCH_MAX_BUF 256

enum nothing_touch_mode_type {
	Touch_Game_Mode = 0,
	Touch_Active_MODE = 1,
	Touch_UP_THRESHOLD = 2,
	Touch_Tolerance = 3,
	Touch_Aim_Sensitivity = 4,
	Touch_Tap_Stability = 5,
	Touch_Expert_Mode = 6,
	Touch_Edge_Filter = 7,
	Touch_Panel_Orientation = 8,
	Touch_Report_Rate = 9,
	Touch_Fod_Enable = 10,
	Touch_Aod_Enable = 11,
	Touch_Resist_RF = 12,
	Touch_Idle_Time = 13,
	Touch_Doubletap_Mode = 14,
	Touch_Grip_Mode = 15,
	Touch_Power_Status = 19,
	Touch_Mode_NUM = 20,
};

enum nothing_touch_value_type {
	SET_CUR_VALUE = 0,
	GET_CUR_VALUE,
	GET_DEF_VALUE,
	GET_MIN_VALUE,
	GET_MAX_VALUE,
	GET_MODE_VALUE,
	RESET_MODE,
	SET_LONG_VALUE,
};

struct nothing_touch_interface {
	int thp_cmd_buf[NOTHING_TOUCH_MAX_BUF];
	int thp_cmd_size;
	int touch_mode[NOTHING_TOUCH_MODE_COUNT][NOTHING_TOUCH_VALUE_TYPE_SIZE];
	int (*setModeValue)(int mode, int value);
	int (*setModeLongValue)(int mode, int value_len, int *value);
	int (*getModeValue)(int mode, int value_type);
	int (*getModeAll)(int mode, int *modevalue);
	int (*resetMode)(int mode);
	int (*prox_sensor_read)(void);
	int (*prox_sensor_write)(int on);
	int (*palm_sensor_read)(void);
	int (*palm_sensor_write)(int on);
	int (*get_touch_rx_num)(void);
	int (*get_touch_tx_num)(void);
	int (*get_touch_x_resolution)(void);
	int (*get_touch_y_resolution)(void);
	int (*enable_touch_raw)(int en);
	int (*enable_clicktouch_raw)(int count);
	int (*enable_touch_delta)(bool en);
	u8 (*panel_vendor_read)(void);
	u8 (*panel_color_read)(void);
	u8 (*panel_display_read)(void);
	char (*touch_vendor_read)(void);
	bool is_enable_touchraw;
	int thp_downthreshold;
	int thp_upthreshold;
	int thp_movethreshold;
	int thp_noisefilter;
	int thp_islandthreshold;
	int thp_smooth;
	int thp_dump_raw;
	bool is_enable_touchdelta;
};

struct nothing_touch {
	struct miscdevice misc_dev;
	struct device *dev;
	struct class *class;
	struct mutex mutex;
	struct mutex palm_mutex;
	wait_queue_head_t wait_queue;
};

struct nothing_touch_pdata {
	struct nothing_touch *device;
	struct nothing_touch_interface *touch_data[2];
	int suspend_state;
};

struct nothing_touch *nothing_touch_dev_get(int minor);
struct class *get_nothing_touch_class(void);
int nothing_touch_register_modedata(int touch_id,
				 struct nothing_touch_interface *data);
int update_nothing_palm_sensor_value(int value);
int update_nothing_prox_sensor_value(int value);

#endif /* __NOTHING_TOUCH_H */
