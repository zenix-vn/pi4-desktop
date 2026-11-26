// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver DRM cho panel OSPTEK YDP1010BT006-V1 10.1" 800x1280 (IC JD9366, MIPI-DSI 2 lane)
 * tren Raspberry Pi 4 (DSI1, cong 15 pin).
 *
 * Dua tren vi du Raspberry Pi 5 cua OSPTEK (github.com/osptek/display-tft-10.1inch-800x1280,
 * examples/rpi5-panel-jd9366-touch-800x1280). Khac biet:
 *   - chuoi khoi tao lay tu ban ESP32-P4 da chay that (jd9366_init_seq.h, khac gamma trang B);
 *   - chi dung API mipi_dsi_* co tu lau (khong can _multi) de build duoc tren nhieu kernel;
 *   - tham so module pclk_khz de ha xung pixel neu DSI cua Pi 4 khong on dinh.
 *
 * Timing: VFP = 140 dong la BAT BUOC. JD9366 la chip TDDI (hien thi + cam ung chung mot chip),
 * phan cam ung chi quet duoc trong khoang trong doc; VFP = 20 lam cam ung doc ra toan 0xFF.
 */

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include "jd9366_init_seq.h"

/*
 * Pi 4: VC4 tao xung lane = PLLD 3 GHz / so nguyen, nen 76 MHz (912 Mb/s/lane) bi lam tron len
 * 1000 Mb/s/lane (driver tu keo dai htotal, van ~60 Hz). Neu hinh chap chon / mat hinh, thu
 * pclk_khz=62500 -> dung 750 Mb/s/lane, man hinh chay ~49 Hz.
 */
static unsigned int pclk_khz = 76000;
module_param(pclk_khz, uint, 0444);
MODULE_PARM_DESC(pclk_khz, "Xung pixel (kHz), mac dinh 76000; thu 62500 neu DSI khong on dinh");

#define HACT 800
#define HFP  40
#define HSW  8
#define HBP  28
#define VACT 1280
#define VFP  140
#define VSW  8
#define VBP  20

struct jd9366 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	enum drm_panel_orientation orientation;
};

static inline struct jd9366 *to_jd9366(struct drm_panel *p)
{
	return container_of(p, struct jd9366, panel);
}

static int jd9366_send_init(struct jd9366 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	const u8 *p = jd9366_init_seq;
	const u8 *end = jd9366_init_seq + sizeof(jd9366_init_seq);
	u8 madctl = 0x00;
	int ret;

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_ADDRESS_MODE, &madctl, 1);
	if (ret < 0)
		return ret;

	while (p < end) {
		u8 delay = p[0], len = p[1];

		/* p[2] = lenh, p[3..] = du lieu: mipi_dsi_dcs_write_buffer gui ca khoi */
		ret = mipi_dsi_dcs_write_buffer(dsi, &p[2], len + 1);
		if (ret < 0) {
			dev_err(&dsi->dev, "lenh 0x%02x loi: %d\n", p[2], ret);
			return ret;
		}
		if (delay)
			msleep(delay);
		p += 3 + len;
	}
	return 0;
}

static int jd9366_prepare(struct drm_panel *panel)
{
	struct jd9366 *ctx = to_jd9366(panel);

	/*
	 * Adapter cua OSPTEK keo RESET len 1V8 qua 10k (khong co chan reset toi Pi). Neu co noi
	 * reset-gpios trong overlay thi reset cung; neu khong thi doi IC on dinh nhu ban ESP32.
	 */
	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		msleep(10);
		gpiod_set_value_cansleep(ctx->reset, 0);
	}
	msleep(120);

	return jd9366_send_init(ctx);
}

static int jd9366_enable(struct drm_panel *panel)
{
	return mipi_dsi_dcs_set_display_on(to_jd9366(panel)->dsi);
}

static int jd9366_disable(struct drm_panel *panel)
{
	return mipi_dsi_dcs_set_display_off(to_jd9366(panel)->dsi);
}

static int jd9366_unprepare(struct drm_panel *panel)
{
	struct jd9366 *ctx = to_jd9366(panel);
	int ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);

	msleep(120);
	if (ctx->reset)
		gpiod_set_value_cansleep(ctx->reset, 1);
	return ret < 0 ? ret : 0;
}

static int jd9366_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode = drm_mode_create(connector->dev);

	if (!mode)
		return -ENOMEM;

	mode->clock = pclk_khz;
	mode->hdisplay = HACT;
	mode->hsync_start = HACT + HFP;
	mode->hsync_end = HACT + HFP + HSW;
	mode->htotal = HACT + HFP + HSW + HBP;
	mode->vdisplay = VACT;
	mode->vsync_start = VACT + VFP;
	mode->vsync_end = VACT + VFP + VSW;
	mode->vtotal = VACT + VFP + VSW + VBP;
	mode->width_mm = 135;
	mode->height_mm = 217;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	connector->display_info.bpc = 8;
	drm_connector_set_panel_orientation(connector, to_jd9366(panel)->orientation);

	return 1;
}

static enum drm_panel_orientation jd9366_get_orientation(struct drm_panel *panel)
{
	return to_jd9366(panel)->orientation;
}

static const struct drm_panel_funcs jd9366_funcs = {
	.prepare = jd9366_prepare,
	.enable = jd9366_enable,
	.disable = jd9366_disable,
	.unprepare = jd9366_unprepare,
	.get_modes = jd9366_get_modes,
	.get_orientation = jd9366_get_orientation,
};

static int jd9366_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct jd9366 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_LPM;

	/* reset-gpios: muc logic 1 = dang reset (khai bao GPIO_ACTIVE_LOW trong overlay) */
	ctx->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "khong lay duoc reset-gpios\n");

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0)
		ctx->orientation = DRM_MODE_PANEL_ORIENTATION_NORMAL;

	drm_panel_init(&ctx->panel, dev, &jd9366_funcs, DRM_MODE_CONNECTOR_DSI);
	/* DSI host phai o LP-11 truoc khi gui lenh khoi tao */
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);
	mipi_dsi_set_drvdata(dsi, ctx);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		dev_err(dev, "mipi_dsi_attach loi: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	dev_info(dev, "JD9366 800x1280, pclk %u kHz\n", pclk_khz);
	return 0;
}

static void jd9366_remove(struct mipi_dsi_device *dsi)
{
	struct jd9366 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id jd9366_of_match[] = {
	{ .compatible = "osptek,ydp1010bt006" },
	{ }
};
MODULE_DEVICE_TABLE(of, jd9366_of_match);

static struct mipi_dsi_driver jd9366_driver = {
	.probe = jd9366_probe,
	.remove = jd9366_remove,
	.driver = {
		.name = "panel-jd9366",
		.of_match_table = jd9366_of_match,
	},
};
module_mipi_dsi_driver(jd9366_driver);

MODULE_DESCRIPTION("OSPTEK YDP1010BT006-V1 (JD9366) 800x1280 MIPI-DSI panel cho Raspberry Pi 4");
MODULE_LICENSE("GPL");
