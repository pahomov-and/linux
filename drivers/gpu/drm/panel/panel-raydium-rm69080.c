// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright © 2020 ADUK GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Portions of this file (derived from panel-simple.c) are:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * This is the driver for controlling the DSI panel on the RM69080 MIPI
 * controller (1 data lane).
 * This driver was checked by Raspberry Pi 4 and AMOLED in the display of
 * the company Kingtech model pv13900als20c has a resolution of 400x400 pixels.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pm.h>


// #include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#ifdef CONFIG_STACKPROTECTOR
unsigned long __stack_chk_guard = 0xDEADBEEF;
#endif

#define DSI_DRIVER_NAME "kingtech-pv13900als20c"

struct cmd_dsi {
	u8 cmd;
	u8 param;
};

#define SLEEP_CMD 0
#define COUNT_CMD(x) (size_t)(sizeof((x))/sizeof(x[0]))

static const struct cmd_dsi rm69080_400x400_mod[] = {
		{0xFE, 0x05},
		{0x05, 0x00},
		{0xFE, 0x07},
		{0x07, 0x4F},
		{0xFE, 0x0A},
		{0x1C, 0x1B},
		{0xFE, 0x00},
		{0x35, 0x00},

		{0x51, 0xF0},		// Brightness control 0~255
		{0x38, 0x00},		// Idle mode Off (60HZ)
//		{0x39, 0x00},		// Enter Idle mode (15HZ)

		{0x11, 0x00},		// Sleep out
		{SLEEP_CMD, 0x96},	// Delay 150 ms
		{SLEEP_CMD, 0x96},
		{0x29, 0x00},		// Display on
};

struct rm69080 {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;
	struct i2c_client *i2c;
	bool prepared;
	bool enabled;
};

static const struct drm_display_mode rm69080_modes[] = {
	{
		.clock = 11094,
		.hdisplay = 400,
		.hsync_start = 400 + 10,
		.hsync_end = 400 + 10 + 10,
		.htotal = 10 + 400 + 10 + 10,
		.vdisplay = 400,
		.vsync_start = 400 + 10,
		.vsync_end = 400 + 10 + 10,
		.vtotal = 10 + 400 + 10 + 10,
		// .vrefresh = 60,
		// .width_mm = 35,
		// .height_mm = 35,
	},
};

static struct rm69080 *panel_to_rm69080(struct drm_panel *panel)
{
	return container_of(panel, struct rm69080, base);
}

static int set_cmd_rm69080(struct mipi_dsi_device *dsi,
	const struct cmd_dsi *cmd_list, size_t count)
{
	size_t i;
	int ret = 0;

	for (i = 0; i < count; i++) {
		const struct cmd_dsi *entry = &cmd_list[i];

		if (entry->cmd != SLEEP_CMD) {
			u8 buffer[2] = { entry->cmd, entry->param };

			ret = mipi_dsi_generic_write(dsi,
				&buffer, sizeof(buffer));

			if (ret < 0) {
				dev_err(&dsi->dev, "mipi_dsi_generic_write() failed\n");
				return ret;
			}
		} else {
			dev_info(&dsi->dev, "sleep: %d ms\n", entry->param);
			msleep(entry->param);
		}
	}
	return ret;
}

static int rm69080_disable(struct drm_panel *panel)
{
	struct rm69080 *ctx = panel_to_rm69080(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;

	dev_info(&dsi->dev, "%s:%s\n", DSI_DRIVER_NAME, __func__);

	if (!ctx->enabled)
		return 0;

	ctx->enabled = false;

	return 0;
}

static int rm69080_unprepare(struct drm_panel *panel)
{
	struct rm69080 *ctx = panel_to_rm69080(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	dev_info(&dsi->dev, "%s:%s\n", DSI_DRIVER_NAME, __func__);

	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret)
		dev_info(&dsi->dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret)
		dev_info(&dsi->dev, "failed to enter sleep mode: %d\n", ret);

	ctx->prepared = false;

	return 0;
}

static int rm69080_prepare(struct drm_panel *panel)
{
	struct rm69080 *ctx = panel_to_rm69080(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	dev_info(&dsi->dev, "%s:%s\n", DSI_DRIVER_NAME, __func__);

	set_cmd_rm69080(dsi, &rm69080_400x400_mod[0],
		COUNT_CMD(rm69080_400x400_mod));

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret) {
		dev_err(&dsi->dev, "mipi_dsi_dcs_exit_sleep_mode() failed: %d\n", ret);
		return ret;
	}

	// msleep(400);
	msleep(40);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret) {
		dev_err(&dsi->dev, "mipi_dsi_dcs_exit_sleep_mode() failed: %d\n", ret);
		return ret;
	}

	// msleep(2000);
	msleep(20);

	ctx->prepared = true;

	return 0;
}

static int rm69080_enable(struct drm_panel *panel)
{
	struct rm69080 *ctx = panel_to_rm69080(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;

	dev_info(&dsi->dev, "%s:%s\n", DSI_DRIVER_NAME, __func__);

	if (ctx->enabled)
		return 0;

	ctx->enabled = true;

	return 0;
}

static int rm69080_get_modes(struct drm_panel *panel,
				     struct drm_connector *connector)
{
	struct rm69080 *ctx = panel_to_rm69080(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;

	// struct drm_connector *connector = panel->connector;
	// struct drm_device *drm = &panel->drm;
	struct drm_display_mode *mode;
	const struct drm_display_mode *m = &rm69080_modes[0];

	dev_info(&dsi->dev, "%s:%s\n", DSI_DRIVER_NAME, __func__);

	mode = drm_mode_duplicate(connector->dev, m);
	if (!mode)
		dev_err(&dsi->dev, "mipi_dsi_attach() failed\n");

	dev_info(&dsi->dev, "add mode %ux%u@%u\n",
			m->hdisplay, m->vdisplay, drm_mode_vrefresh(m));

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER;// | DRM_MODE_TYPE_PREFERRED;

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	drm_mode_probed_add(connector, mode);
	return 1;
}


static const struct drm_panel_funcs rm69080_funcs = {
	.disable = rm69080_disable,
	.unprepare = rm69080_unprepare,
	.prepare = rm69080_prepare,
	.enable = rm69080_enable,
	.get_modes = rm69080_get_modes,
};

static int rm69080_probe(struct mipi_dsi_device *dsi)
{
	int ret;
	struct rm69080 *ctx;
	struct device *dev = &dsi->dev;
	struct device_node *endpoint;

	dev_info(&dsi->dev, "%s:%s\n", DSI_DRIVER_NAME, __func__);

	dsi->mode_flags = (MIPI_DSI_MODE_VIDEO |
			   MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			   MIPI_DSI_MODE_LPM);
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 1;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		dev_err(dev, "devm_kzalloc() failed!\n");
		return -ENOMEM;
	}

	endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
	ctx->dsi = dsi;

	drm_panel_init(&ctx->base, dev, &rm69080_funcs, DRM_MODE_CONNECTOR_DSI);
	// ctx->base.dev = dev;
	// ctx->base.funcs = &rm69080_funcs;

	// dev_set_drvdata(dev, ctx);

	ret = drm_panel_add(&ctx->base);
	if (ret) {
		dev_err(dev, "drm_panel_add() failed!\n");
		return ret;
	}

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		dev_err(dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->base);
	}

	return 0;
}

static int rm69080_remove(struct mipi_dsi_device *dsi)
{
	struct rm69080 *ctx = mipi_dsi_get_drvdata(dsi);

	dev_info(&dsi->dev, "%s:%s\n", DSI_DRIVER_NAME, __func__);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->base);

	return 0;
}

static const struct of_device_id rm69080_of_match[] = {
	{ .compatible = "kingtech,pv13900als20c", },
	{ .compatible = "raydium,rm69080", },
	{ }
};

MODULE_DEVICE_TABLE(of, rm69080_of_match);

static struct mipi_dsi_driver rm69080_driver = {
	.driver = {
		.name = DSI_DRIVER_NAME,
		.of_match_table = rm69080_of_match,
	},
	.probe = rm69080_probe,
	.remove = rm69080_remove,
};

module_mipi_dsi_driver(rm69080_driver);

MODULE_AUTHOR("Andrey Pahomov <pahomov.and@gmail.com>");
MODULE_DESCRIPTION("DRM Driver for Raydium RM69080 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
