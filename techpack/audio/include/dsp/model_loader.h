/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2020 Mi */

#ifndef __MODEL_LOADER_H__
#define __MODEL_LOADER_H__

#include <linux/dma-buf.h>
#include <linux/types.h>

struct model_info {
	void __user *data;
	u32 len;
};

struct voice_sound_model {
	dma_addr_t phys;
	void *data;
	size_t size;
	struct dma_buf *dma_buf;
	u32 mem_map_handle;
};

unsigned int send_data_add_component_controls(void *component);

#endif /* __MODEL_LOADER_H__ */
