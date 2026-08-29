/* SPDX-License-Identifier: GPL-2.0 */
/*
 * init/execprog_bootmode.h
 *
 * Generic, device-agnostic boot mode check used by execprog.c and
 * security/selinux/selinuxfs.c.
 */

#ifndef _EXECPROG_BOOTMODE_H
#define _EXECPROG_BOOTMODE_H

#include <linux/string.h>

extern char *saved_command_line;

static inline bool is_normal_boot(void)
{
	const char *cmdline = saved_command_line;

	if (!cmdline)
		return true;

	if (strstr(cmdline, "androidboot.mode=charger"))
		return false;
	if (strstr(cmdline, "androidboot.mode=recovery"))
		return false;
	if (strstr(cmdline, "androidboot.mode=ffbm"))
		return false;

	return true;
}

#endif /* _EXECPROG_BOOTMODE_H */
