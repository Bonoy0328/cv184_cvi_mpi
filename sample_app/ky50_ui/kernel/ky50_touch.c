// SPDX-License-Identifier: GPL-2.0
/* KY50 ST7102 TDDI, verified info map: 0x55, 16-bit BE register address,
 * 480x854, five contacts. Protocol reference: Espressif esp_lcd_touch_st7123
 * (Apache-2.0), register definitions and seven-byte contact records.
 * Polling avoids depending on an unverified IRQ polarity. No firmware writes,
 * reset or power changes; the display application owns the shared reset order.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/workqueue.h>
#include <linux/slab.h>

struct ky50_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	unsigned int contacts, errors;
	bool down;
};
static struct i2c_client *created_client;

static int read_regs(struct i2c_client *c, u16 reg, u8 *p, u16 len)
{
	u8 addr[] = {reg >> 8, reg & 255};
	struct i2c_msg m[] = {{.addr=c->addr,.len=2,.buf=addr},
		{.addr=c->addr,.flags=I2C_M_RD,.len=len,.buf=p}};
	int rc = i2c_transfer(c->adapter, m, 2);
	return rc == 2 ? 0 : (rc < 0 ? rc : -EIO);
}

static void release_points(struct ky50_ts *ts)
{
	unsigned int i;
	for (i=0; i<ts->contacts; ++i) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(ts->input);
	input_mt_report_pointer_emulation(ts->input, true);
	input_sync(ts->input); ts->down = false;
}

static void poll_touch(struct work_struct *work)
{
	struct ky50_ts *ts = container_of(to_delayed_work(work), struct ky50_ts, work);
	u8 frame[4 + 7 * 10] = {};
	unsigned int i, n = 0, first_x=0, first_y=0;
	int rc = read_regs(ts->client, 0x10, frame, 4 + 7 * ts->contacts);
	if (rc) {
		if (++ts->errors == 3) { release_points(ts); dev_warn(&ts->client->dev,"touch read errors: %d\n",rc); }
		goto again;
	}
	ts->errors = 0;
	/* No coordinate update is not a release; retain a stationary contact. */
	if (!(frame[0] & 8)) goto again;
	for (i=0; i<ts->contacts; ++i) {
		u8 *p = frame + 4 + i * 7;
		unsigned int x = ((p[0] & 0x3f) << 8) | p[1];
		unsigned int y = (p[2] << 8) | p[3];
		bool valid = (p[0] & 0x80) && x < 480 && y < 854;
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, valid);
		if (valid) {
			input_report_abs(ts->input, ABS_MT_POSITION_X, x);
			input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
			if (!n) { first_x=x; first_y=y; } ++n;
		}
	}
	input_mt_sync_frame(ts->input);
	input_mt_report_pointer_emulation(ts->input, true);
	input_sync(ts->input);
	if (!!n != ts->down)
		dev_info(&ts->client->dev,"%s x=%u y=%u contacts=%u\n", n ? "DOWN":"UP", first_x,first_y,n);
	ts->down = !!n;
again:
	schedule_delayed_work(&ts->work, msecs_to_jiffies(10));
}

static int ky50_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct ky50_ts *ts;
	struct input_dev *in;
	u8 info[16];
	int rc;
	(void)id;
	rc = read_regs(client, 0, info, sizeof(info));
	if (rc) return rc;
	if (((info[5]<<8)|info[6]) != 480 || ((info[7]<<8)|info[8]) != 854 ||
	    !info[9] || info[9] > 10) return -ENODEV;
	ts = devm_kzalloc(&client->dev,sizeof(*ts),GFP_KERNEL);
	in = devm_input_allocate_device(&client->dev);
	if (!ts || !in) return -ENOMEM;
	ts->client=client; ts->input=in; ts->contacts=info[9];
	in->name="KY50 ST7102 Touchscreen"; in->id.bustype=BUS_I2C;
	input_set_abs_params(in,ABS_X,0,479,0,0);
	input_set_abs_params(in,ABS_Y,0,853,0,0);
	input_set_abs_params(in,ABS_MT_POSITION_X,0,479,0,0);
	input_set_abs_params(in,ABS_MT_POSITION_Y,0,853,0,0);
	input_set_capability(in,EV_KEY,BTN_TOUCH);
	rc=input_mt_init_slots(in,ts->contacts,INPUT_MT_DIRECT);
	if (rc) return rc;
	rc=input_register_device(in);
	if (rc) return rc;
	i2c_set_clientdata(client,ts);
	INIT_DELAYED_WORK(&ts->work,poll_touch);
	schedule_delayed_work(&ts->work,msecs_to_jiffies(10));
	dev_info(&client->dev,"verified 480x854 contacts=%u fw=%02x rev=%02x.%02x.%02x.%02x\n",
		 info[9],info[0],info[12],info[13],info[14],info[15]);
	return 0;
}
static int ky50_remove(struct i2c_client *client)
{
	struct ky50_ts *ts=i2c_get_clientdata(client);
	cancel_delayed_work_sync(&ts->work); release_points(ts); return 0;
}
static const struct i2c_device_id ids[] = {{"ky50_tddi",0},{}};
MODULE_DEVICE_TABLE(i2c,ids);
static struct i2c_driver driver = {.driver={.name="ky50_tddi"},
	.probe=ky50_probe,.remove=ky50_remove,.id_table=ids};
static int __init ky50_init(void)
{
	struct i2c_adapter *ad;
	struct i2c_board_info info = {I2C_BOARD_INFO("ky50_tddi",0x55)};
	int rc=i2c_add_driver(&driver);
	if (rc) return rc;
	ad=i2c_get_adapter(1);
	if (!ad) { rc=-ENODEV; goto fail; }
	created_client=i2c_new_client_device(ad,&info); i2c_put_adapter(ad);
	if (IS_ERR(created_client)) { rc=PTR_ERR(created_client); created_client=NULL; goto fail; }
	if (!i2c_get_clientdata(created_client)) {
		i2c_unregister_device(created_client); created_client=NULL; rc=-ENODEV; goto fail;
	}
	return 0;
fail: i2c_del_driver(&driver); return rc;
}
static void __exit ky50_exit(void)
{
	if (created_client) i2c_unregister_device(created_client);
	i2c_del_driver(&driver);
}
module_init(ky50_init);
module_exit(ky50_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KY50 verified TDDI touch input");
