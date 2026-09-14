/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Nothing Thermal Interface — public header
 *
 * Exports for touch / panel drivers that need the active DRM panel
 * handle (e.g. Goodix/Focaltech firmware resume sequencing).
 */
#ifndef _NT_THERMAL_INTERFACE_H
#define _NT_THERMAL_INTERFACE_H

#ifdef CONFIG_DRM_PANEL
#include <drm/drm_panel.h>

struct drm_panel *get_panel(void);
#else
static inline void *get_panel(void)
{
	return NULL;
}
#endif

#endif /* _NT_THERMAL_INTERFACE_H */
