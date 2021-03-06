/*
* Simple driver
* Copyright (C)
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
*/
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/semaphore.h>
#include "lp8556.h"
#include "hisi_fb.h"
#include "../hisi_fb_panel.h"

static struct lp8556_backlight_information lp8556_bl_info;

static struct gpio_desc lp8556_hw_en_on_cmds[] = {
	{DTYPE_GPIO_REQUEST, WAIT_TYPE_US, 0,
		GPIO_LP8556_EN_NAME, &lp8556_bl_info.lp8556_hw_en_gpio, 0},
	{DTYPE_GPIO_OUTPUT, WAIT_TYPE_MS, 5,
		GPIO_LP8556_EN_NAME, &lp8556_bl_info.lp8556_hw_en_gpio, 1},
};

static struct gpio_desc lp8556_hw_en_disable_cmds[] =
{
	{DTYPE_GPIO_OUTPUT, WAIT_TYPE_US, 0,
		GPIO_LP8556_EN_NAME, &lp8556_bl_info.lp8556_hw_en_gpio, 0},
	{DTYPE_GPIO_INPUT, WAIT_TYPE_US, 10,
		GPIO_LP8556_EN_NAME, &lp8556_bl_info.lp8556_hw_en_gpio, 0},
};

static struct gpio_desc lp8556_hw_en_free_cmds[] =
{
	{DTYPE_GPIO_FREE, WAIT_TYPE_US, 50,
		GPIO_LP8556_EN_NAME, &lp8556_bl_info.lp8556_hw_en_gpio, 0},
};

static char *lp8556_dts_string[LP8556_RW_REG_MAX] = {
	"lp8556_device_control",
	"lp8556_eprom_cfg0",
	"lp8556_eprom_cfg1",
	"lp8556_eprom_cfg2",
	"lp8556_eprom_cfg3",
	"lp8556_eprom_cfg4",
	"lp8556_eprom_cfg5",
	"lp8556_eprom_cfg6",
	"lp8556_eprom_cfg7",
	"lp8556_eprom_cfg9",
	"lp8556_eprom_cfgA",
	"lp8556_eprom_cfgE",
	"lp8556_eprom_cfg9E",
	"lp8556_led_enable",
};

static unsigned int lp8556_reg_addr[LP8556_RW_REG_MAX] = {
	LP8556_DEVICE_CONTROL,
	LP8556_EPROM_CFG0,
	LP8556_EPROM_CFG1,
	LP8556_EPROM_CFG2,
	LP8556_EPROM_CFG3,
	LP8556_EPROM_CFG4,
	LP8556_EPROM_CFG5,
	LP8556_EPROM_CFG6,
	LP8556_EPROM_CFG7,
	LP8556_EPROM_CFG9,
	LP8556_EPROM_CFGA,
	LP8556_EPROM_CFGE,
	LP8556_EPROM_CFG9E,
	LP8556_LED_ENABLE,
};

struct class *lp8556_class = NULL;
struct lp8556_chip_data *lp8556_g_chip = NULL;
static bool lp8556_init_status = false;

/*
** for debug, S_IRUGO
** /sys/module/hisifb/parameters
*/
unsigned lp8556_msg_level = 7;
module_param_named(debug_lp8556_msg_level, lp8556_msg_level, int, 0640);
MODULE_PARM_DESC(debug_lp8556_msg_level, "backlight lp8556 msg level");

static int lp8556_parse_dts(struct device_node *np)
{
	int ret = 0;
	int i = 0;

	for (i = 0;i < LP8556_RW_REG_MAX;i++ ) {
		ret |= of_property_read_u32(np, lp8556_dts_string[i], &lp8556_bl_info.lp8556_reg[i]);
		if (ret < 0) {
			LP8556_ERR("get lp8556 dts config failed\n");
		} else {
			LP8556_DEBUG("get %s from dts value = 0x%x\n", lp8556_dts_string[i],lp8556_bl_info.lp8556_reg[i]);
		}
	}

	ret |= of_property_read_u32(np, "lp8556_hw_en_gpio", &lp8556_bl_info.lp8556_hw_en_gpio);
	ret |= of_property_read_u32(np, "bl_on_kernel_mdelay", &lp8556_bl_info.bl_on_kernel_mdelay);
	if (ret < 0) {
		LP8556_ERR("get lp8556 dts config failed\n");
		return ret;
	} else {
		LP8556_DEBUG("get %s from dts value = 0x%d\n", "lp8556_hw_en_gpio", lp8556_bl_info.lp8556_hw_en_gpio);
		lp8556_bl_info.lp8556_hw_en_gpio = 68;
	}

	return ret;
}

static int lp8556_config_write(struct lp8556_chip_data *pchip,
			unsigned int reg[],unsigned int val[],unsigned int size)
{
	int ret = 0;
	unsigned int i = 0;

	for(i = 0;i < size;i++) {
		ret = regmap_write(pchip->regmap, reg[i], val[i]);
		if (ret < 0) {
			LP8556_ERR("write lp8556 backlight config register 0x%x failed\n",reg[i]);
			goto exit;
		} else {
			LP8556_INFO("register 0x%x value = 0x%x\n", reg[i], val[i]);
		}
	}

exit:
	return ret;
}

static int lp8556_config_read(struct lp8556_chip_data *pchip,
			unsigned int reg[],unsigned int val[],unsigned int size)
{
	int ret = 0;
	unsigned int i = 0;

	for(i = 0;i < size;i++) {
		ret = regmap_read(pchip->regmap, reg[i],&val[i]);
		if (ret < 0) {
			LP8556_ERR("read lp8556 backlight config register 0x%x failed",reg[i]);
			goto exit;
		} else {
			LP8556_INFO("read 0x%x value = 0x%x\n", reg[i], val[i]);
		}
	}

exit:
	return ret;
}

/* initialize chip */
static int lp8556_chip_init(struct lp8556_chip_data *pchip)
{
	int ret = -1;
	struct device_node *np = NULL;

	LP8556_INFO("in!\n");

	memset(&lp8556_bl_info, 0, sizeof(struct lp8556_backlight_information));

	np = of_find_compatible_node(NULL, NULL, DTS_COMP_LP8556);
	if (!np) {
		LP8556_ERR("NOT FOUND device node %s!\n", DTS_COMP_LP8556);
		goto out;
	}

	ret = lp8556_parse_dts(np);
	if (ret < 0) {
		LP8556_ERR("parse lp8556 dts failed");
		goto out;
	}

	ret = lp8556_config_write(pchip, lp8556_reg_addr, lp8556_bl_info.lp8556_reg, LP8556_RW_REG_MAX);
	if (ret < 0) {
		LP8556_ERR("lp8556 config register failed");
		goto out;
	}

	ret = lp8556_config_read(pchip, lp8556_reg_addr, lp8556_bl_info.lp8556_reg, LP8556_RW_REG_MAX);
	if (ret < 0) {
		LP8556_ERR("lp8556 config read failed");
		goto out;
	}

	LP8556_INFO("ok!\n");

	return ret;

out:
	dev_err(pchip->dev, "i2c failed to access register\n");
	return ret;
}

/**
 * lp8556_set_backlight_init(): initial ic working mode
 *
 * @bl_level: value for backlight ,range from 0 to ~
 *
 * A value of zero will be returned on success, a negative errno will
 * be returned in error cases.
 */
ssize_t lp8556_set_backlight_init(uint32_t bl_level)
{
	int ret = 0;

	if (g_fake_lcd_flag) {
		LP8556_INFO("is fake lcd!\n");
		return ret;
	}

	if (down_trylock(&(lp8556_g_chip->test_sem))) {
		LP8556_INFO("Now in test mode\n");
		return 0;
	}

	if (false == lp8556_init_status && bl_level > 0) {
		mdelay(lp8556_bl_info.bl_on_kernel_mdelay);
		gpio_cmds_tx(lp8556_hw_en_on_cmds, ARRAY_SIZE(lp8556_hw_en_on_cmds));

		/* chip initialize */
		ret = lp8556_chip_init(lp8556_g_chip);
		if (ret < 0) {
			LP8556_ERR("lp8556_chip_init fail!\n");
			goto out;
		}

		lp8556_init_status = true;
	} else if (true == lp8556_init_status && 0 == bl_level) {
		gpio_cmds_tx(lp8556_hw_en_disable_cmds, ARRAY_SIZE(lp8556_hw_en_disable_cmds));
		gpio_cmds_tx(lp8556_hw_en_free_cmds, ARRAY_SIZE(lp8556_hw_en_free_cmds));

		lp8556_init_status = false;
	} else {
		LP8556_DEBUG("lp8556_chip_init %u, 0: already off; else : already init!\n", bl_level);
	}

out:
	up(&(lp8556_g_chip->test_sem));
	return ret;
}
/* EXPORT_SYMBOL(lp8556_set_backlight_init); */

static ssize_t lp8556_reg_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t lp8556_reg_store(struct device *dev,
					struct device_attribute *devAttr,
					const char *buf, size_t size)
{
	return 0;
}

static DEVICE_ATTR(reg, (S_IRUGO|S_IWUSR), lp8556_reg_show, lp8556_reg_store);

/* pointers to created device attributes */
static struct attribute *lp8556_attributes[] = {
	&dev_attr_reg.attr,
	NULL,
};

static const struct attribute_group lp8556_group = {
	.attrs = lp8556_attributes,
};

static const struct regmap_config lp8556_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
};
static int lp8556_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = client->adapter;
	struct lp8556_chip_data *pchip = NULL;
	int ret = -1;

	LP8556_INFO("in!\n");

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "i2c functionality check fail.\n");
		return -EOPNOTSUPP;
	}

	pchip = devm_kzalloc(&client->dev,
				sizeof(struct lp8556_chip_data), GFP_KERNEL);
	if (!pchip)
		return -ENOMEM;

#ifdef CONFIG_REGMAP_I2C
	pchip->regmap = devm_regmap_init_i2c(client, &lp8556_regmap);
	if (IS_ERR(pchip->regmap)) {
		ret = PTR_ERR(pchip->regmap);
		dev_err(&client->dev, "fail : allocate register map: %d\n", ret);
		goto err_out;
	}
#endif

	lp8556_g_chip = pchip;
	pchip->client = client;
	i2c_set_clientdata(client, pchip);

	sema_init(&(pchip->test_sem), 1);

	pchip->dev = device_create(lp8556_class, NULL, 0, "%s", client->name);
	if (IS_ERR(pchip->dev)) {
		/* Not fatal */
		LP8556_ERR("Unable to create device; errno = %ld\n", PTR_ERR(pchip->dev));
		pchip->dev = NULL;
	} else {
		dev_set_drvdata(pchip->dev, pchip);
		ret = sysfs_create_group(&pchip->dev->kobj, &lp8556_group);
		if (ret)
			goto err_sysfs;
	}

	return ret;

err_sysfs:
	LP8556_DEBUG("sysfs error!\n");
	device_destroy(lp8556_class, 0);
err_out:
	devm_kfree(&client->dev, pchip);
	return ret;
}

static int lp8556_remove(struct i2c_client *client)
{
	sysfs_remove_group(&client->dev.kobj, &lp8556_group);

	return 0;
}

static const struct i2c_device_id lp8556_id[] = {
	{LP8556_NAME, 0},
	{},
};

static const struct of_device_id lp8556_of_id_table[] = {
	{.compatible = "ti,lp8556"},
	{},
};

MODULE_DEVICE_TABLE(i2c, lp8556_id);
static struct i2c_driver lp8556_i2c_driver = {
		.driver = {
			.name = "lp8556",
			.owner = THIS_MODULE,
			.of_match_table = lp8556_of_id_table,
		},
		.probe = lp8556_probe,
		.remove = lp8556_remove,
		.id_table = lp8556_id,
};

static int __init lp8556_module_init(void)
{
	int ret = -1;

	LP8556_INFO("in!\n");

	lp8556_class = class_create(THIS_MODULE, "lp8556");
	if (IS_ERR(lp8556_class)) {
		LP8556_ERR("Unable to create lp8556 class; errno = %ld\n", PTR_ERR(lp8556_class));
		lp8556_class = NULL;
	}

	ret = i2c_add_driver(&lp8556_i2c_driver);
	if (ret)
		LP8556_ERR("Unable to register lp8556 driver\n");

	LP8556_INFO("ok!\n");

	return ret;
}
late_initcall(lp8556_module_init);

MODULE_DESCRIPTION("Texas Instruments Backlight driver for LP8556");
MODULE_AUTHOR("Daniel Jeong <daniel.jeong@ti.com>");
MODULE_AUTHOR("G.Shark Jeong <gshark.jeong@gmail.com>");
MODULE_LICENSE("GPL v2");
