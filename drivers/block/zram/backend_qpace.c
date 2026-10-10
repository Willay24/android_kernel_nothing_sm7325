// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/crypto.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qpace.h>

#include "backend_qpace.h"

struct qpace_ctx {
	struct device *dev;
	void *in_buf;
	dma_addr_t in_dma;
	void *out_buf;
	dma_addr_t out_dma;
	struct crypto_comp *tfm;
};

static int qpace_setup_params(struct zcomp_params *params)
{
	/*
	 * Hold a QPaCE reference for the lifetime of this zcomp so the
	 * hardware stays awake and the urgent contexts stay programmed.
	 * If the device is suspended/unavailable we still succeed and let
	 * create_ctx() pick the software engine.
	 */
	if (qpace_get_dma_dev() && !qpace_get())
		params->drv_data = (void *)&qpace_lz4_algorithm;

	return 0;
}

static void qpace_release_params(struct zcomp_params *params)
{
	if (params->drv_data) {
		qpace_put();
		params->drv_data = NULL;
	}
}

static int qpace_create_ctx(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct qpace_ctx *cc;
	struct device *dev;

	cc = kzalloc(sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;

	dev = qpace_get_dma_dev();
	if (dev && params->drv_data) {
		/* Hardware QPaCE accelerator path (e.g. SM8750/Hawi) */
		cc->dev = dev;
		cc->in_buf = dma_alloc_coherent(dev, PAGE_SIZE, &cc->in_dma,
						GFP_KERNEL);
		if (!cc->in_buf)
			goto err_free;

		cc->out_buf = dma_alloc_coherent(dev, PAGE_SIZE, &cc->out_dma,
						 GFP_KERNEL);
		if (!cc->out_buf)
			goto err_free_in;

		ctx->context = cc;
		return 0;
	}

	/*
	 * Software path for platforms without the QPaCE IP block
	 * (e.g. SM7475 / Snapdragon 7+ Gen 2 and older Qualcomm SoCs).
	 */
	cc->tfm = crypto_alloc_comp("lz4", 0, 0);
	if (IS_ERR(cc->tfm)) {
		int err = PTR_ERR(cc->tfm);

		kfree(cc);
		return err;
	}

	ctx->context = cc;
	return 0;

err_free_in:
	dma_free_coherent(dev, PAGE_SIZE, cc->in_buf, cc->in_dma);
err_free:
	kfree(cc);
	return -ENOMEM;
}

static void qpace_destroy_ctx(struct zcomp_ctx *ctx)
{
	struct qpace_ctx *cc = ctx->context;

	if (!cc)
		return;

	if (cc->dev) {
		if (cc->out_buf)
			dma_free_coherent(cc->dev, PAGE_SIZE, cc->out_buf,
					  cc->out_dma);
		if (cc->in_buf)
			dma_free_coherent(cc->dev, PAGE_SIZE, cc->in_buf,
					  cc->in_dma);
	}
	if (cc->tfm)
		crypto_free_comp(cc->tfm);

	kfree(cc);
	ctx->context = NULL;
}

static int qpace_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req)
{
	struct qpace_algorithm *algo = params->drv_data;
	struct qpace_ctx *cc = ctx->context;
	int ret;

	if (!cc)
		return -EINVAL;

	/* Hardware QPaCE accelerator path */
	if (cc->dev && cc->in_buf && cc->out_buf) {
		if (!algo)
			algo = (struct qpace_algorithm *)&qpace_lz4_algorithm;

		if (req->src_len > PAGE_SIZE)
			return -EINVAL;

		memcpy(cc->in_buf, req->src, req->src_len);

		ret = qpace_urgent_compress(cc->in_dma, cc->out_dma, algo);

		/* Incompressible: report src_len so zram stores it as huge */
		if (ret == -E2BIG || ret >= PAGE_SIZE) {
			req->dst_len = req->src_len;
			return 0;
		}
		if (ret < 0)
			return ret;

		if (ret > req->dst_len)
			return -EIO;

		memcpy(req->dst, cc->out_buf, ret);
		req->dst_len = ret;
		return 0;
	}

	/* Software engine path */
	if (cc->tfm) {
		unsigned int dst_len = req->dst_len;

		ret = crypto_comp_compress(cc->tfm, req->src, req->src_len,
					   req->dst, &dst_len);
		if (!ret) {
			req->dst_len = dst_len;
			return 0;
		}
		return ret;
	}

	return -EINVAL;
}

static int qpace_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			    struct zcomp_req *req)
{
	struct qpace_algorithm *algo = params->drv_data;
	struct qpace_ctx *cc = ctx->context;
	int ret;

	if (!cc)
		return -EINVAL;

	/* Hardware QPaCE accelerator path */
	if (cc->dev && cc->in_buf && cc->out_buf) {
		if (!algo)
			algo = (struct qpace_algorithm *)&qpace_lz4_algorithm;

		if (req->src_len > PAGE_SIZE)
			return -EINVAL;

		memcpy(cc->in_buf, req->src, req->src_len);

		ret = qpace_urgent_decompress(cc->in_dma, cc->out_dma,
					      req->src_len, algo);
		if (ret < 0)
			return ret;

		if (ret > req->dst_len)
			return -EIO;

		memcpy(req->dst, cc->out_buf, ret);
		req->dst_len = ret;
		return 0;
	}

	/* Software engine path */
	if (cc->tfm) {
		unsigned int dst_len = req->dst_len;

		ret = crypto_comp_decompress(cc->tfm, req->src, req->src_len,
					     req->dst, &dst_len);
		if (!ret) {
			req->dst_len = dst_len;
			return 0;
		}
		return ret;
	}

	return -EINVAL;
}

const struct zcomp_ops backend_qpace = {
	.compress	= qpace_compress,
	.decompress	= qpace_decompress,
	.create_ctx	= qpace_create_ctx,
	.destroy_ctx	= qpace_destroy_ctx,
	.setup_params	= qpace_setup_params,
	.release_params	= qpace_release_params,
	.name		= "qpace-lz4",
};
