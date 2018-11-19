/*
 * Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 * Based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 *
 * Copyright (c) 2011, The Linux Foundation. All rights reserved.
 *
 * Copyright (C) 2016-2018, Sultanxda <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/power/oem_external_fg.h>

#define DRIVER_VERSION			"1.1.0-DUMPLING_MSM"

/* BQ27541 standard data commands */
#define BQ27541_REG_CNTL		0x00
#define BQ27541_REG_AR			0x02
#define BQ27541_REG_ARTTE		0x04
#define BQ27541_REG_TEMP		0x06
#define BQ27541_REG_VOLT		0x08
#define BQ27541_REG_FLAGS		0x0A
#define BQ27541_REG_NAC			0x0C
#define BQ27541_REG_FAC			0x0e
#define BQ27541_REG_RM			0x10
#define BQ27541_REG_FCC			0x12
#define BQ27541_REG_AI			0x14
#define BQ27541_REG_TTE			0x16
#define BQ27541_REG_TTF			0x18
#define BQ27541_REG_SI			0x1a
#define BQ27541_REG_STTE		0x1c
#define BQ27541_REG_MLI			0x1e
#define BQ27541_REG_MLTTE		0x20
#define BQ27541_REG_AE			0x22
#define BQ27541_REG_AP			0x24
#define BQ27541_REG_TTECP		0x26
#define BQ27541_REG_SOH			0x28
#define BQ27541_REG_CC			0x2a
#define BQ27541_REG_SOC			0x2c
#define BQ27541_REG_NIC			0x2e
#define BQ27541_REG_ICR			0x30
#define BQ27541_REG_LOGIDX		0x32
#define BQ27541_REG_LOGBUF		0x34

#define BQ27541_FLAG_DSC		BIT(0)
#define BQ27541_FLAG_FC			BIT(9)

#define BQ27541_CS_DLOGEN		BIT(15)
#define BQ27541_CS_SS		    BIT(13)

/* BQ27411 standard data commands */
#define BQ27411_REG_TEMP                0x02
#define BQ27411_REG_VOLT                0x04
#define BQ27411_REG_RM                  0x0c
#define BQ27411_REG_AI                  0x10
#define BQ27411_REG_SOC                 0x1c

/* Control subcommands */
#define BQ27541_SUBCMD_CNTL_STATUS  0x0000
#define BQ27541_SUBCMD_DEVICE_TYPE  0x0001
#define BQ27541_SUBCMD_FW_VER  0x0002
#define BQ27541_SUBCMD_HW_VER  0x0003
#define BQ27541_SUBCMD_DF_CSUM  0x0004
#define BQ27541_SUBCMD_PREV_MACW   0x0007
#define BQ27541_SUBCMD_CHEM_ID   0x0008
#define BQ27541_SUBCMD_BD_OFFSET   0x0009
#define BQ27541_SUBCMD_INT_OFFSET  0x000a
#define BQ27541_SUBCMD_CC_VER   0x000b
#define BQ27541_SUBCMD_OCV  0x000c
#define BQ27541_SUBCMD_BAT_INS   0x000d
#define BQ27541_SUBCMD_BAT_REM   0x000e
#define BQ27541_SUBCMD_SET_HIB   0x0011
#define BQ27541_SUBCMD_CLR_HIB   0x0012
#define BQ27541_SUBCMD_SET_SLP   0x0013
#define BQ27541_SUBCMD_CLR_SLP   0x0014
#define BQ27541_SUBCMD_FCT_RES   0x0015
#define BQ27541_SUBCMD_ENABLE_DLOG  0x0018
#define BQ27541_SUBCMD_DISABLE_DLOG 0x0019
#define BQ27541_SUBCMD_SEALED   0x0020
#define BQ27541_SUBCMD_ENABLE_IT    0x0021
#define BQ27541_SUBCMD_DISABLE_IT   0x0023
#define BQ27541_SUBCMD_CAL_MODE  0x0040
#define BQ27541_SUBCMD_RESET   0x0041
#define ZERO_DEGREES_CELSIUS_IN_TENTH_KELVIN   2731
#define BQ27541_INIT_DELAY   (msecs_to_jiffies(300))

#define BQ27541_CHG_CALIB_CNT   2 /* Num of calibration cycles after charging */

/* Back up and use old data if raw measurement fails */
struct bq27541_old_data {
	atomic_t cap;
	atomic_t curr;
	atomic_t is_charging;
	atomic_t mvolts;
	atomic_t soc;
	atomic_t temp;
};

struct bq27541_device_info {
	struct device			*dev;
	struct i2c_client		*client;
	struct delayed_work		hw_config;
	struct bq27541_old_data		old_data;
	struct mutex			i2c_read_lock;
	bool				disable_reading;
};

static struct bq27541_device_info *bq27541_di;

extern int get_charging_status(void);

static int bq27541_read_i2c(u8 reg, int *rt_value,
	struct bq27541_device_info *di)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[1];
	unsigned char data[2];
	int err;

	if (!client->adapter)
		return -ENODEV;

	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = 1;
	msg->buf = data;

	data[0] = reg;
	err = i2c_transfer(client->adapter, msg, 1);

	if (err >= 0) {
		msg->len = 2;
		msg->flags = I2C_M_RD;

		err = i2c_transfer(client->adapter, msg, 1);
		if (err >= 0) {
			*rt_value = get_unaligned_le16(data);
			return 0;
		}
	}

	return err;
}

static int bq27541_read(u8 reg, int *rt_value, struct bq27541_device_info *di)
{
	int ret;

	mutex_lock(&di->i2c_read_lock);
	if (di->disable_reading)
		ret = -EBUSY;
	else
		ret = bq27541_read_i2c(reg, rt_value, di);
	mutex_unlock(&di->i2c_read_lock);

	return ret;
}

/* Return the battery temperature in tenths of degrees Celsius */
static int bq27541_get_battery_temperature(void)
{
	struct bq27541_device_info *di = bq27541_di;
	int ret, temp;

	ret = bq27541_read(BQ27411_REG_TEMP, &temp, di);
	if (ret) {
		dev_dbg(di->dev, "error reading temperature, ret: %d\n", ret);
		return atomic_read(&di->old_data.temp);
	}

	temp -= ZERO_DEGREES_CELSIUS_IN_TENTH_KELVIN;
	atomic_set(&di->old_data.temp, temp);
	return temp;
}

static int bq27541_get_battery_mvolts(void)
{
	struct bq27541_device_info *di = bq27541_di;
	int ret, volt;

	ret = bq27541_read(BQ27411_REG_VOLT, &volt, di);
	if (ret) {
		dev_dbg(di->dev, "error reading voltage, ret: %d\n", ret);
		return atomic_read(&di->old_data.mvolts);
	}

	volt *= 1000;
	atomic_set(&di->old_data.mvolts, volt);
	return volt;
}

static int bq27541_get_average_current(void)
{
	struct bq27541_device_info *di = bq27541_di;
	int ret, curr;

	ret = bq27541_read(BQ27411_REG_AI, &curr, di);
	if (ret) {
		dev_dbg(di->dev, "error reading current, ret: %d\n", ret);
		return atomic_read(&di->old_data.curr);
	}

	/* Negative current */
	if (curr & 0x8000)
		curr = -((~(curr - 1)) & 0xFFFF);
	curr *= -1;

	atomic_set(&di->old_data.curr, curr);
	return curr;
}

static int bq27541_get_battery_soc(void)
{
	struct bq27541_device_info *di = bq27541_di;
	int ret, old_soc, soc;

	old_soc = atomic_read(&di->old_data.soc);

	ret = bq27541_read(BQ27411_REG_SOC, &soc, di);
	if (ret) {
		dev_dbg(di->dev, "error reading SOC, ret: %d\n", ret);
		return old_soc;
	}

	/* Double check before reporting 0% SOC */
	if (old_soc && !soc) {
		atomic_set(&di->old_data.soc, 0);
		return old_soc;
	}

	/* Initialize old data */
	if (!old_soc) {
		atomic_set(&di->old_data.soc, soc);
		old_soc = soc;
	}

	if (soc > old_soc) {
		/*
		 * Don't raise SOC while discharging, unless this is
		 * a calibration cycle.
		 */
		int chg_status = get_charging_status();
		if (chg_status == POWER_SUPPLY_STATUS_DISCHARGING) {
			if (atomic_read(&di->old_data.is_charging))
				atomic_dec(&di->old_data.is_charging);
			else
				soc = old_soc;
		} else {
			atomic_set(&di->old_data.is_charging,
				BQ27541_CHG_CALIB_CNT);
		}
	} else if (soc < old_soc) {
		/*
		 * Don't force SOC to scale down by 1% during first
		 * BQ27541_CHG_CALIB_CNT discharge heartbeats after
		 * charging. This will allow SOC to quickly drop to
		 * its true value if needed.
		 */
		if (atomic_read(&di->old_data.is_charging))
			atomic_dec(&di->old_data.is_charging);
		else
			/* Scale down 1% at a time */
			soc = old_soc - 1;
	}

	atomic_set(&di->old_data.soc, soc);
	return soc;
}

static int bq27541_get_batt_remaining_capacity(void)
{
	struct bq27541_device_info *di = bq27541_di;
	int ret, cap;

	ret = bq27541_read(BQ27411_REG_RM, &cap, di);
	if (ret) {
		dev_dbg(di->dev, "error reading cap, ret: %d\n", ret);
		return atomic_read(&di->old_data.cap);
	}

	atomic_set(&di->old_data.cap, cap);
	return cap;
}

static void bq27541_set_allow_reading(bool enable)
{
	struct bq27541_device_info *di = bq27541_di;

	mutex_lock(&di->i2c_read_lock);
	di->disable_reading = !enable;
	mutex_unlock(&di->i2c_read_lock);
}

static struct external_battery_gauge bq27541_batt_gauge = {
	.get_battery_mvolts		= bq27541_get_battery_mvolts,
	.get_battery_temperature	= bq27541_get_battery_temperature,
	.get_battery_soc		= bq27541_get_battery_soc,
	.get_average_current		= bq27541_get_average_current,
	.get_batt_remaining_capacity	= bq27541_get_batt_remaining_capacity,
	.set_allow_reading		= bq27541_set_allow_reading,
};

/* I2C-specific code */
static int bq27541_i2c_txsubcmd(u8 reg, unsigned short subcmd,
	struct bq27541_device_info *di)
{
	struct i2c_msg msg;
	unsigned char data[3];
	int ret;

	if (!di->client)
		return -ENODEV;

	memset(data, 0, sizeof(data));
	data[0] = reg;
	data[1] = subcmd & 0x00FF;
	data[2] = (subcmd & 0xFF00) >> 8;

	msg.addr = di->client->addr;
	msg.flags = 0;
	msg.len = 3;
	msg.buf = data;

	ret = i2c_transfer(di->client->adapter, &msg, 1);
	if (ret < 0)
		return -EIO;

	return 0;
}

static void bq27541_cntl_cmd(struct bq27541_device_info *di,
				int subcmd)
{
	bq27541_i2c_txsubcmd(BQ27541_REG_CNTL, subcmd, di);
}

static int bq27541_chip_config(struct bq27541_device_info *di)
{
	int flags, ret;

	bq27541_cntl_cmd(di, BQ27541_SUBCMD_CNTL_STATUS);
	udelay(66);
	ret = bq27541_read(BQ27541_REG_CNTL, &flags, di);
	if (ret) {
		dev_err(di->dev, "error reading register %02x, ret: %d\n",
			 BQ27541_REG_CNTL, ret);
		return ret;
	}
	udelay(66);

	bq27541_cntl_cmd(di, BQ27541_SUBCMD_ENABLE_IT);
	udelay(66);

	if (!(flags & BQ27541_CS_DLOGEN)) {
		bq27541_cntl_cmd(di, BQ27541_SUBCMD_ENABLE_DLOG);
		udelay(66);
	}

	return 0;
}

static void bq27541_hw_config(struct work_struct *work)
{
	int ret, flags = 0, type = 0, fw_ver = 0;
	struct bq27541_device_info *di;

	di = container_of(work, struct bq27541_device_info, hw_config.work);
	ret = bq27541_chip_config(di);
	if (ret) {
		dev_err(di->dev, "Failed to configure device, retrying\n");
		schedule_delayed_work(&di->hw_config, BQ27541_INIT_DELAY);
		return;
	}

	external_battery_gauge_register(&bq27541_batt_gauge);
	bq27541_information_register(&bq27541_batt_gauge);

	bq27541_cntl_cmd(di, BQ27541_SUBCMD_CNTL_STATUS);
	udelay(66);
	bq27541_read(BQ27541_REG_CNTL, &flags, di);
	bq27541_cntl_cmd(di, BQ27541_SUBCMD_DEVICE_TYPE);
	udelay(66);
	bq27541_read(BQ27541_REG_CNTL, &type, di);
	bq27541_cntl_cmd(di, BQ27541_SUBCMD_FW_VER);
	udelay(66);
	bq27541_read(BQ27541_REG_CNTL, &fw_ver, di);

	dev_info(di->dev, "DEVICE_TYPE is 0x%02X, FIRMWARE_VERSION is 0x%02X\n",
			type, fw_ver);
	dev_info(di->dev, "Completed configuration 0x%02X\n", flags);
}

static int bq27541_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct bq27541_device_info *di;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		return -ENOMEM;
	}

	i2c_set_clientdata(client, di);
	di->dev = &client->dev;
	di->client = client;
	atomic_set(&di->old_data.is_charging, BQ27541_CHG_CALIB_CNT);

	mutex_init(&di->i2c_read_lock);

	bq27541_di = di;

	/*
	 * 300ms delay is needed after bq27541 is powered up
	 * and before any successful I2C transaction
	 */
	INIT_DELAYED_WORK(&di->hw_config, bq27541_hw_config);
	schedule_delayed_work(&di->hw_config, BQ27541_INIT_DELAY);

	return 0;
}

static const struct of_device_id bq27541_match[] = {
	{ .compatible = "ti,bq27541-battery" },
	{ },
};

static const struct i2c_device_id bq27541_id[] = {
	{ "bq27541", 1 },
	{ },
};

static struct i2c_driver bq27541_battery_driver = {
	.driver		= {
			.name = "bq27541-battery",
			.owner = THIS_MODULE,
			.of_match_table = bq27541_match,
	},
	.probe		= bq27541_battery_probe,
	.id_table	= bq27541_id,
};

static int __init bq27541_battery_init(void)
{
	int ret;

	ret = i2c_add_driver(&bq27541_battery_driver);
	if (ret)
		pr_err("Unable to register BQ27541 driver, ret: %d\n", ret);

	return ret;
}
device_initcall(bq27541_battery_init);
