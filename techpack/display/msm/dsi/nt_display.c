// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/string.h>

#include "sde_connector.h"
#include "dsi_display.h"
#include "dsi_panel.h"

static const char * const nt_sfm_cmd_map[DSI_CMD_SFM_MAX] = {
	"set-frame-command",
	"disable-skip-frame-command",
	"get-reg2f-value-command",
	"get-reg6d-value-command",
	"switch-te-not-follow-source-command",
	"switch-te-follow-source-command",
	"switch-30hz-dimming-command",
	"switch-10hz-dimming-command",
	"switch-1hz-dimming-command",
	"switch-60hz-no-dimming-command",
	"switch-30hz-no-dimming-command",
	"switch-24hz-no-dimming-command",
	"switch-10hz-no-dimming-command",
	"switch-1hz-no-dimming-command",
};

extern struct dsi_panel *nt_panel;

void nt_display_free_switch_cmds(struct dsi_panel *panel)
{
	int i;

	for (i = 0; i < DSI_CMD_SFM_MAX; i++) {
		kfree(panel->nt_cmd_sets[i].cmds);
		panel->nt_cmd_sets[i].cmds = NULL;
		panel->nt_cmd_sets[i].count = 0;
	}
}

int nt_display_parse_switch_cmds(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	int i, missing = 0;

	for (i = 0; i < DSI_CMD_SFM_MAX; i++) {
		const char *str = NULL;

		if (utils->read_string(utils->data, nt_sfm_cmd_map[i], &str) ||
		    !str) {
			DSI_DEBUG("[%s] %s not found\n", panel->name,
				  nt_sfm_cmd_map[i]);
			missing++;
			continue;
		}

		panel->nt_cmd_sets[i].cmds = kstrdup(str, GFP_KERNEL);
		if (!panel->nt_cmd_sets[i].cmds) {
			nt_display_free_switch_cmds(panel);
			return -ENOMEM;
		}
		panel->nt_cmd_sets[i].count = strlen(str);
	}

	return missing ? -ENODATA : 0;
}

static int nt_send(struct sde_connector *conn, enum dsi_cmd_sfm_set_type type)
{
	struct dsi_panel *panel = nt_panel;

	if (!panel || !panel->nt_cmd_sets[type].cmds)
		return -ENODATA;

	return nt_tx_cmd(conn, panel->nt_cmd_sets[type].cmds,
			 panel->nt_cmd_sets[type].count);
}

int nt_set_refresh_rate(struct sde_connector *conn, int index)
{
	int rc = 0;
	bool latch = true;

	if (!conn || !nt_panel)
		return -EINVAL;

	switch (index) {
	case 0:
		rc = nt_send(conn, DSI_CMD_30HZ_DIMMING);
		break;
	case 1:
		rc = nt_send(conn, DSI_CMD_10HZ_DIMMING);
		break;
	case 2:
		rc = nt_send(conn, DSI_CMD_1HZ_DIMMING);
		break;
	case 7:
		rc = nt_send(conn, DSI_CMD_TE_FOLLOW_SOURCE);
		if (!rc)
			rc = nt_send(conn, DSI_CMD_60HZ_NODIMMING);
		break;
	case 8:
		rc = nt_send(conn, DSI_CMD_TE_NOTFOLLOW_SOURCE);
		if (!rc)
			rc = nt_send(conn, DSI_CMD_30HZ_NODIMMING);
		break;
	case 9:
		rc = nt_send(conn, DSI_CMD_24HZ_NODIMMING);
		break;
	case 10:
		rc = nt_send(conn, DSI_CMD_TE_NOTFOLLOW_SOURCE);
		if (!rc)
			rc = nt_send(conn, DSI_CMD_10HZ_NODIMMING);
		break;
	case 11:
		rc = nt_send(conn, DSI_CMD_1HZ_NODIMMING);
		break;
	case 16:
		rc = nt_send(conn, DSI_CMD_DISABLE_SKIP_FRAME_MODE);
		latch = false;
		break;
	default:
		SDE_ERROR("invalid refresh rate index: %d\n", index);
		return -EINVAL;
	}

	if (!rc && latch)
		rc = nt_send(conn, DSI_CMD_SET_FRAME);

	return rc;
}

int nt_get_refresh_rate(struct sde_connector *conn)
{
	struct dsi_panel *panel = nt_panel;
	int reg_2f, i, rc;
	int reg_6d[2] = {0, 0};

	if (!conn || !panel ||
	    !panel->nt_cmd_sets[DSI_CMD_GET_REG2F].cmds ||
	    !panel->nt_cmd_sets[DSI_CMD_GET_REG6D].cmds)
		return -EINVAL;

	rc = nt_rx_cmd(conn, panel->nt_cmd_sets[DSI_CMD_GET_REG2F].cmds,
		       panel->nt_cmd_sets[DSI_CMD_GET_REG2F].count);
	if (rc) {
		SDE_ERROR("read reg 0x2f failed, rc=%d\n", rc);
		return rc;
	}
	reg_2f = conn->cmd_rx_buf[0];

	if (reg_2f == 0x00 || reg_2f == 0x01)
		return 16;

	if (reg_2f != 0x30) {
		SDE_ERROR("unknown reg 0x2f value: 0x%x\n", reg_2f);
		return -EINVAL;
	}

	rc = nt_rx_cmd(conn, panel->nt_cmd_sets[DSI_CMD_GET_REG6D].cmds,
		       panel->nt_cmd_sets[DSI_CMD_GET_REG6D].count);
	if (rc) {
		SDE_ERROR("read reg 0x6d failed, rc=%d\n", rc);
		return rc;
	}
	for (i = 0; i < conn->rx_len && i < ARRAY_SIZE(reg_6d); i++)
		reg_6d[i] = conn->cmd_rx_buf[i];

	if (reg_6d[1]) {
		switch (reg_6d[0]) {
		case 0x02:
			return 0;
		case 0x04:
			return 1;
		case 0x05:
			return 2;
		}
	} else {
		switch (reg_6d[0]) {
		case 0x00:
			return 7;
		case 0x06:
			return 8;
		case 0x03:
			return 9;
		case 0x07:
			return 10;
		case 0x05:
			return 11;
		}
	}

	SDE_ERROR("unknown reg 0x6d value: 0x%x 0x%x\n", reg_6d[0], reg_6d[1]);
	return -EINVAL;
}
