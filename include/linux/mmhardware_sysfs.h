/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MMHARDWARE_SYSFS_H__
#define __MMHARDWARE_SYSFS_H__

#define MM_HARDWARE_SYSFS_ROOT_FOLDER		"mm_hardware"
#define MM_HARDWARE_SYSFS_ADSP_FOLDER		"adsp"
#define MM_HARDWARE_SYSFS_CODEC_FOLDER		"codec"
#define MM_HARDWARE_SYSFS_SPK_L_FOLDER		"speaker_l"
#define MM_HARDWARE_SYSFS_SPK_R_FOLDER		"speaker_r"
#define MM_HARDWARE_SYSFS_HAPTIC_FOLDER		"haptic"
#define MM_HARDWARE_SYSFS_AS_FOLDER		"audioswitch"

enum hardware_id {
	MM_HW_NONE	= 0x0,
	MM_HW_ADSP	= 0x1,	/* audio DSP (Q6 ADSP, audio reaches userspace) */
	MM_HW_CODEC	= 0x2,	/* audio codec (WCD938x on Spacewar) */
	MM_HW_PA_1	= 0x4,	/* speaker left amp */
	MM_HW_PA_2	= 0x8,	/* speaker right amp */
	MM_HW_HAPTIC_1	= 0x10,	/* haptic motor driver */
	MM_HW_AS	= 0x20,	/* USB-C analog audio switch (FSA4480) */
};

struct mm_info {
	struct kobj_attribute k_attr;
	enum hardware_id mm_id;
	int on_register;
};

#define __MMHW(_id, _mm_name) {				\
	.k_attr = {						\
			.attr  = {				\
					.name = __stringify(_mm_name),	\
					.mode = 0644,			\
			},					\
			.show  = mm_register_show,		\
			.store = mm_register_store,		\
	},							\
	.mm_id = _id,						\
	.on_register = 0,					\
}

/* create sysfs node */
#define MM_INFO(_id, _mm_name)		\
	struct mm_info _mm_name##_info = __MMHW(_id, _mm_name)

int mmhardware_initialize_sysfs(void);
void mmhardware_cleanup_sysfs(void);
int register_kobj_under_mmsysfs(enum hardware_id mm_id, const char *name);

#endif /* __MMHARDWARE_SYSFS_H__ */
