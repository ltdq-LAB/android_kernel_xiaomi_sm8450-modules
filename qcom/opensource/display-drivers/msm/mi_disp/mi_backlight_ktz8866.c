/*
 * KTZ Semiconductor KTZ8866 LED Driver
 *
 * Copyright (C) 2013 Ideas on board SPRL
 *
 * Contact: Zhang Teng <zhangteng3@xiaomi.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt)	"ktz8866:[%s:%d] " fmt, __func__, __LINE__

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include "mi_backlight_ktz8866.h"
#include "mi_panel_id.h"

#define u8	unsigned int
static struct list_head ktz8866_dev_list;
static struct mutex ktz8866_dev_list_mutex;
static bool ktz8866_driver_registered;
#define M80_NORMAL_MAX_DBV 1737
#define M81_NORMAL_MAX_DBV 1700

static const struct regmap_config ktz8866_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int ktz8866_write(struct ktz8866_led *ktz, u8 reg, u8 data)
{
	int ret;

	if(NULL != ktz) {
		ret = regmap_write(ktz->regmap, reg, data);
		if(ret < 0)
			pr_err("ktz write failed to access reg = %d, data = %d \n", reg, data);
	} else {
		pr_err("write missing ktz8866 \n ");
		ret = -EINVAL;
	}
	return ret;
}

static int ktz8866_read(struct ktz8866_led *ktz, u8 reg, u8 *data)
{
	int ret;

	if(NULL != ktz) {
		ret = regmap_read(ktz->regmap, reg, data);
		if(ret < 0)
			pr_err("ktz read failed to access reg = %d, data = %d", reg, *data);
	} else {
		pr_err("read missing ktz8866 \n ");
		ret = -EINVAL;
	}
	return ret;
}

static int ktz_update_status(struct ktz8866_led *ktz, unsigned int level,
		unsigned int normal_max_dbv)
{
	int exponential_bl = level;
	int brightness = 0;
	u8 v[2];
	int ret = 0;

	if(!ktz) {
		pr_err("ktz8866 not exit, return !! \n ");
		return -EINVAL;
	}
	if(exponential_bl <= BL_LEVEL_MAX) {
		exponential_bl = (exponential_bl * normal_max_dbv) / 2047;
		}
	else if(exponential_bl <= BL_LEVEL_MAX_HBM) {
		exponential_bl = ((exponential_bl - 2048) *
				(2047 - normal_max_dbv)) / 2047 + normal_max_dbv;
		}
	else {
		pr_info("ktz8866 backlight out of 4095 too large!!!\n");
		return -EINVAL;
	}
	brightness = mi_bl_level_remap[exponential_bl];
	if (brightness < 0 || brightness > BL_LEVEL_MAX || brightness == ktz->level)
		return ret;

	if (!ktz->ktz8866_status && brightness > 0) {
		ret = ktz8866_write(ktz, KTZ8866_DISP_BL_ENABLE, 0x7f);
		if (ret)
			return ret;
		ktz->ktz8866_status = 1;
		pr_info("ktz8866 backlight enable,dimming close");
	} else if (brightness == 0) {
		ret = ktz8866_write(ktz, KTZ8866_DISP_BL_ENABLE, 0x1f);
		if (ret)
			return ret;
		ktz->ktz8866_status = 0;
		usleep_range((10 * 1000),(10 * 1000) + 10);
		pr_info( "ktz8866 backlight disable,dimming close");
	}
	v[0] = (brightness >> 3) & 0xff;
	v[1] = brightness & 0x7;
	pr_info("ktz8866 get level: %d, exponential_bl: %d, set reg: %d ,MSB: 0x%02x, LSB: 0x%02x \n",
		level, exponential_bl, brightness, v[0], v[1]);
	ret = ktz8866_write(ktz, KTZ8866_DISP_BB_LSB, v[1]);
	if (ret)
		return ret;
	ret = ktz8866_write(ktz, KTZ8866_DISP_BB_MSB, v[0]);
	if (ret)
		return ret;
	ktz->level = brightness;
	return 0;
}

static int ktz_get_brightness(struct ktz8866_led *ktz, u8 reg, u8 *data)
{
	return ktz8866_read(ktz, reg, data);
}

static const struct ktz_ops ops = {
	.update_status	= ktz_update_status,
	.get_brightness	= ktz_get_brightness,
};

int ktz8866_backlight_update_status(struct dsi_panel *panel,
		unsigned int level)
{
	int ret = 0;
	int op_ret;
	bool found = false;
	struct ktz8866_led *ktz;
	unsigned int normal_max_dbv = M80_NORMAL_MAX_DBV;
	u8 read = 0;
	enum mi_project_panel_id panel_id;

	if (!panel)
		return -EINVAL;

	panel_id = mi_get_panel_id(panel->mi_cfg.mi_panel_id);
	if (panel_id == M81_PANEL_PA || panel_id == M81_PANEL_PB)
		normal_max_dbv = M81_NORMAL_MAX_DBV;

	mutex_lock(&ktz8866_dev_list_mutex);
	list_for_each_entry(ktz, &ktz8866_dev_list, entry) {
		found = true;
		op_ret = ktz->ops->update_status(ktz, level, normal_max_dbv);
		if (op_ret && !ret)
			ret = op_ret;
		op_ret = ktz->ops->get_brightness(ktz,
				KTZ8866_DISP_BB_MSB, &read);
		if (op_ret && !ret)
			ret = op_ret;
	}
	mutex_unlock(&ktz8866_dev_list_mutex);

	return found ? ret : -ENODEV;
}

static int ktz8866_probe(struct i2c_client *i2c,
			  const struct i2c_device_id *id)
{
	struct ktz8866_led *pdata;
	int ret = 0;

	if (!i2c_check_functionality(i2c->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&i2c->dev, "ktz8866 I2C adapter doesn't support I2C_FUNC_SMBUS_BYTE\n");
		return -EIO;
	}
	pdata = devm_kzalloc(&i2c->dev,
			     sizeof(struct ktz8866_led), GFP_KERNEL);
	if (!pdata){
		pr_err("failed: out of memory \n");
		return -ENOMEM;
	}
	pdata->regmap = devm_regmap_init_i2c(i2c, &ktz8866_i2c_regmap_config);
	if (IS_ERR(pdata->regmap)) {
		ret = PTR_ERR(pdata->regmap);
		dev_err(&i2c->dev, "init regmap failed: %d\n", ret);
		return ret;
	}
	pdata->ops = &ops;
	i2c_set_clientdata(i2c, pdata);
	mutex_lock(&ktz8866_dev_list_mutex);
	list_add_tail(&pdata->entry, &ktz8866_dev_list);
	mutex_unlock(&ktz8866_dev_list_mutex);
	dev_info(&i2c->dev, "probe sucess end\n");
	return 0;
}

static int ktz8866_remove(struct i2c_client *i2c)
{
	struct ktz8866_led *pdata = i2c_get_clientdata(i2c);

	if (!pdata)
		return 0;

	mutex_lock(&ktz8866_dev_list_mutex);
	list_del_init(&pdata->entry);
	mutex_unlock(&ktz8866_dev_list_mutex);
	i2c_set_clientdata(i2c, NULL);

	return 0;
}

static struct of_device_id ktz8866_match_table[] = {
	{ .compatible = "ktz,ktz8866",},
	{ },
};

static struct i2c_driver ktz8866_driver = {
	.driver = {
		.name = "ktz8866",
		.of_match_table = ktz8866_match_table,
	},
	.probe = ktz8866_probe,
	.remove = ktz8866_remove,
};

int mi_backlight_ktz8866_init(void)
{
	int ret;

	INIT_LIST_HEAD(&ktz8866_dev_list);
	mutex_init(&ktz8866_dev_list_mutex);
	ret = i2c_add_driver(&ktz8866_driver);
	if (!ret)
		ktz8866_driver_registered = true;

	return ret;
}

void mi_backlight_ktz8866_deinit(void)
{
	if (!ktz8866_driver_registered)
		return;

	i2c_del_driver(&ktz8866_driver);
	ktz8866_driver_registered = false;
	mutex_destroy(&ktz8866_dev_list_mutex);
}
