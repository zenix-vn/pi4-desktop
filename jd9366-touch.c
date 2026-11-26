// SPDX-License-Identifier: GPL-2.0+
/*
 * Cam ung cua panel OSPTEK YDP1010BT006-V1 (JD9366 TDDI) cho Raspberry Pi 4.
 * I2C 0x68, khong dung chan INT (tren adapter INT bi chia ap con ~0.9 V).
 *
 * Giao thuc (OSPTEK, CC-BY-4.0), dia chi thanh ghi 32-bit big-endian:
 *   - ID:       ghi 40 00 80 76, doc 2 byte -> 0x9032
 *   - backdoor: ghi 40 00 80 04 A5, roi 40 00 80 81 00
 *   - toa do:   ghi 20 02 11 20, doc 3 + 5*N byte; byte 0 = 0 la khong cham, 0xFF la khung hong;
 *               moi diem {x_hi, x_lo, y_hi, y_lo, ?}, toa do = hi * 255 + lo
 *
 * Khac ban vi du Pi 5 cua OSPTEK, rut tu kinh nghiem tren ESP32-P4 (P4-Desktop/main/touch.c):
 *   - doi chip cam ung khoi dong (ID that) va gui lenh backdoor;
 *   - doc NGAN (mac dinh 1 diem = 8 byte): doc 80 byte giua luc chip dang quet hinh lam man nhap nhay;
 *   - khung toan 0xFF hoac loi I2C: giu nguyen trang thai, khong bao "nha tay" gia;
 *   - neu co te-gpios (chan TE cua panel), chi doc ngay sau xung TE = trong khoang trong doc,
 *     giong ban ESP32 dong bo theo vsync. Khong co thi hoi vong theo thoi gian.
 *   - ho tro touchscreen-swapped-x-y / touchscreen-inverted-x/y trong device tree.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/workqueue.h>

#define JD_NAME     "jd9366-touch"
#define JD_CHIP_ID  0x9032
#define JD_W        800
#define JD_H        1280
#define JD_MAX_PTS  10

static unsigned int max_points = 1;
module_param(max_points, uint, 0444);
MODULE_PARM_DESC(max_points, "So diem cham doc moi lan (1..10). Nhieu diem = doc lau hon, de nhap nhay");

struct jd9366_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct gpio_desc *te;
	struct delayed_work work;
	unsigned int npts;
	unsigned int frame;
	unsigned int tries;
	bool ready;
	bool pressed;
};

static int jd_xfer(struct i2c_client *c, const u8 *w, int wlen, u8 *r, int rlen)
{
	struct i2c_msg msg[2] = {
		{ .addr = c->addr, .flags = 0, .len = wlen, .buf = (u8 *)w },
		{ .addr = c->addr, .flags = I2C_M_RD, .len = rlen, .buf = r },
	};
	int n = rlen ? 2 : 1;
	int ret = i2c_transfer(c->adapter, msg, n);

	return ret == n ? 0 : (ret < 0 ? ret : -EIO);
}

/*
 * Mot lan thu doc ID + gui backdoor. Phan cam ung cua TDDI chi song khi panel da chay (sau khi DRM
 * bat man hinh), luc dau NACK hoac tra 0xFFFF, nen goi lai moi chu ky cho toi khi thanh cong.
 */
static bool jd_chip_init(struct jd9366_ts *ts)
{
	static const u8 id_cmd[4] = { 0x40, 0x00, 0x80, 0x76 };
	static const u8 bd1[5] = { 0x40, 0x00, 0x80, 0x04, 0xA5 };
	static const u8 bd2[5] = { 0x40, 0x00, 0x80, 0x81, 0x00 };
	struct device *dev = &ts->client->dev;
	u16 id;
	u8 r[2];

	if (jd_xfer(ts->client, id_cmd, sizeof(id_cmd), NULL, 0) ||
	    i2c_master_recv(ts->client, r, 2) != 2)
		goto fail;
	id = r[1] << 8 | r[0];
	if (id == 0xFFFF || id == 0x0000)
		goto fail;
	if (id != JD_CHIP_ID)
		dev_warn(dev, "ID 0x%04x (mong doi 0x%04x), van tiep tuc\n", id, JD_CHIP_ID);

	if (jd_xfer(ts->client, bd1, sizeof(bd1), NULL, 0) ||
	    jd_xfer(ts->client, bd2, sizeof(bd2), NULL, 0))
		goto fail;
	msleep(10);
	dev_info(dev, "chip cam ung ID 0x%04x da san sang\n", id);
	return true;

fail:
	if (++ts->tries == 200)  /* ~10 s */
		dev_warn(dev, "chua doc duoc ID cam ung (I2C 0x%02x), van dang thu...\n", ts->client->addr);
	return false;
}

/* Doc mot khung. 1 = co du lieu moi (da bao cao), 0 = bo qua khung nay. */
static int jd_poll_once(struct jd9366_ts *ts)
{
	static const u8 cmd[4] = { 0x20, 0x02, 0x11, 0x20 };
	u8 buf[3 + 5 * JD_MAX_PTS];
	int len = 3 + 5 * ts->npts;
	bool any = false;
	int i, ret;

	if (!ts->ready) {
		ts->ready = jd_chip_init(ts);
		return 0;
	}

	/* Ghi va doc tach roi (STOP o giua) nhu ban ESP32 va ban Pi 5 cua hang */
	ret = jd_xfer(ts->client, cmd, sizeof(cmd), NULL, 0);
	if (!ret)
		ret = i2c_master_recv(ts->client, buf, len) == len ? 0 : -EIO;
	if (ret || buf[0] == 0xFF)
		return 0;

	for (i = 0; i < ts->npts; i++) {
		const u8 *p = &buf[3 + i * 5];
		bool on = buf[0] != 0 && p[0] <= 100 && p[2] <= 100;

		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, on);
		if (on) {
			unsigned int x = min(p[0] * 255 + p[1], JD_W - 1);
			unsigned int y = min(p[2] * 255 + p[3], JD_H - 1);

			touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
			any = true;
		}
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	ts->pressed = any;
	return 1;
}

/* Che do TE: moi xung TE (dau khoang trong doc) danh thuc thread nay */
static irqreturn_t jd_te_thread(int irq, void *data)
{
	struct jd9366_ts *ts = data;

	/* Khong cham: 1/3 khung (~20 lan/s). Dang cham: moi khung (~60 lan/s). */
	if (!ts->pressed && (++ts->frame % 3))
		return IRQ_HANDLED;
	jd_poll_once(ts);
	return IRQ_HANDLED;
}

/* Che do hoi vong (khong noi TE) */
static void jd_work(struct work_struct *work)
{
	struct jd9366_ts *ts = container_of(to_delayed_work(work), struct jd9366_ts, work);

	jd_poll_once(ts);
	schedule_delayed_work(&ts->work, msecs_to_jiffies(ts->pressed ? 16 : 50));
}

static void jd_cancel_work(void *data)
{
	struct jd9366_ts *ts = data;

	cancel_delayed_work_sync(&ts->work);
}

static int jd9366_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct jd9366_ts *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	ts->npts = clamp(max_points, 1u, (unsigned int)JD_MAX_PTS);
	i2c_set_clientdata(client, ts);

	ts->te = devm_gpiod_get_optional(dev, "te", GPIOD_IN);
	if (IS_ERR(ts->te))
		return dev_err_probe(dev, PTR_ERR(ts->te), "te-gpios loi\n");

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "JD9366 Touchscreen";
	ts->input->phys = "i2c/jd9366";
	ts->input->id.bustype = BUS_I2C;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, JD_W - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, JD_H - 1, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->prop);

	ret = input_mt_init_slots(ts->input, ts->npts, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;
	ret = input_register_device(ts->input);
	if (ret)
		return ret;

	if (ts->te) {
		int irq = gpiod_to_irq(ts->te);

		if (irq < 0)
			return dev_err_probe(dev, irq, "te-gpios khong co IRQ\n");
		ret = devm_request_threaded_irq(dev, irq, NULL, jd_te_thread,
						IRQF_TRIGGER_RISING | IRQF_ONESHOT, JD_NAME, ts);
		if (ret)
			return dev_err_probe(dev, ret, "xin IRQ TE loi\n");
		dev_info(dev, "san sang: dong bo theo TE, %u diem\n", ts->npts);
	} else {
		INIT_DELAYED_WORK(&ts->work, jd_work);
		ret = devm_add_action_or_reset(dev, jd_cancel_work, ts);
		if (ret)
			return ret;
		schedule_delayed_work(&ts->work, msecs_to_jiffies(50));
		dev_info(dev, "san sang: hoi vong (khong co TE), %u diem\n", ts->npts);
	}
	return 0;
}

static const struct of_device_id jd9366_of_match[] = {
	{ .compatible = "osptek,ydp1010bt006-touch" },
	{ }
};
MODULE_DEVICE_TABLE(of, jd9366_of_match);

static const struct i2c_device_id jd9366_id[] = {
	{ JD_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, jd9366_id);

static struct i2c_driver jd9366_driver = {
	.driver = {
		.name = JD_NAME,
		.of_match_table = jd9366_of_match,
	},
	.id_table = jd9366_id,
	.probe = jd9366_probe,
};
module_i2c_driver(jd9366_driver);

MODULE_DESCRIPTION("Cam ung JD9366 (OSPTEK YDP1010BT006-V1) cho Raspberry Pi 4");
MODULE_LICENSE("GPL");
