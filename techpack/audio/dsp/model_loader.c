// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2020 Mi */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <sound/control.h>
#include <sound/soc.h>
#include <dsp/model_loader.h>
#include <dsp/q6afe-v2.h>
#include <dsp/msm_audio_ion.h>

#define VOICE_SND_MODEL_MAX_SIZE SZ_16M

static int voice_snd_model_buf_free(struct afe_audio_client *client,
				    struct voice_sound_model *sm)
{
	int rc = 0;

	mutex_lock(&client->cmd_lock);

	if (sm->mem_map_handle) {
		rc = afe_cmd_memory_unmap(sm->mem_map_handle);
		if (rc)
			pr_err("%s: memory unmap failed: %d\n", __func__, rc);
	}

	if (!IS_ERR_OR_NULL(sm->dma_buf))
		msm_audio_ion_free(sm->dma_buf);

	sm->dma_buf = NULL;
	sm->data = NULL;
	sm->size = 0;
	sm->mem_map_handle = 0;
	sm->phys = 0;

	mutex_unlock(&client->cmd_lock);

	return rc;
}

static int voice_snd_model_buf_alloc(struct afe_audio_client *client,
				     struct voice_sound_model *sm, size_t len)
{
	size_t mapped_len;
	int rc;

	if (!client || !sm || !len)
		return -EINVAL;

	if (sm->dma_buf)
		return -EBUSY;

	sm->size = PAGE_ALIGN(len);
	mapped_len = sm->size;

	mutex_lock(&client->cmd_lock);
	rc = msm_audio_ion_alloc(&sm->dma_buf, sm->size, &sm->phys,
				 &mapped_len, &sm->data);
	if (rc) {
		pr_err("%s: audio ION allocation failed: %d, size %zu\n",
		       __func__, rc, sm->size);
		goto unlock;
	}

	rc = afe_memory_map(sm->phys, mapped_len, client);
	if (rc) {
		pr_err("%s: failed to map model memory to DSP: %d\n",
		       __func__, rc);
		goto unlock;
	}

	sm->mem_map_handle = client->mem_map_handle;
	sm->size = mapped_len;

unlock:
	mutex_unlock(&client->cmd_lock);
	if (rc)
		voice_snd_model_buf_free(client, sm);

	return rc;
}

static int voice_send_snd_model(struct model_info *model,
				struct afe_audio_client *client,
				struct voice_sound_model *sm)
{
	int rc;

	if (!model || !model->data)
		return -EFAULT;

	if (model->len > sm->size)
		return -E2BIG;

	if (copy_from_user(sm->data, model->data, model->len)) {
		pr_err("%s: failed to copy model from user buffer\n", __func__);
		return -EFAULT;
	}

	rc = afe_send_data(sm->phys, sm->mem_map_handle, model->len);
	if (rc < 0)
		pr_err("%s: failed to send model buffer: %d\n", __func__, rc);

	return rc;
}

static int qdsp_voice_model_data_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	memset(ucontrol->value.bytes.data, 0, sizeof(struct model_info));
	return 0;
}

static int qdsp_voice_model_data_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct afe_audio_client *client;
	struct voice_sound_model sm = { 0 };
	struct model_info info;
	int rc;
	int free_rc;

	memcpy(&info, ucontrol->value.bytes.data, sizeof(info));
	if (!info.data || !info.len)
		return -EINVAL;
	if (info.len > VOICE_SND_MODEL_MAX_SIZE)
		return -E2BIG;

	client = q6afe_audio_client_alloc(NULL);
	if (!client)
		return -ENOMEM;

	rc = voice_snd_model_buf_alloc(client, &sm, info.len);
	if (!rc)
		rc = voice_send_snd_model(&info, client, &sm);

	free_rc = voice_snd_model_buf_free(client, &sm);
	if (!rc)
		rc = free_rc;

	q6afe_audio_client_free(client);
	return rc;
}

static const struct snd_kcontrol_new qdsp_send_data_mixer_controls[] = {
	SND_SOC_BYTES_EXT("Qdsp voice model Data", sizeof(struct model_info),
		qdsp_voice_model_data_get, qdsp_voice_model_data_put),
};

unsigned int send_data_add_component_controls(void *component)
{
	const unsigned int num_controls =
		ARRAY_SIZE(qdsp_send_data_mixer_controls);
	int rc;

	if (!component) {
		pr_err("%s: component pointer is NULL\n", __func__);
		return 0;
	}

	rc = snd_soc_add_component_controls(component,
		qdsp_send_data_mixer_controls, num_controls);
	if (rc < 0) {
		pr_err("%s: failed to add model loader control: %d\n",
		       __func__, rc);
		return 0;
	}

	return num_controls;
}
EXPORT_SYMBOL(send_data_add_component_controls);
