// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Copyright (C) 2023 Allwinner Technology Co., Ltd.
 *
 * Etek ET7304 Type-C Chip Driver
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/usb/tcpm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/version.h>
#include <linux/usb/role.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 130)
#include <linux/usb/tcpci.h>
#else
#include "tcpci.h"
#endif

struct tcpci {
	struct device *dev;
	struct tcpm_port *port;
	struct regmap *regmap;
	bool controls_vbus;
	struct tcpc_dev tcpc;
	struct tcpci_data *data;
};

#define ET7304_VID		0x6DCF
#define ET7304_PID		0x1711
#define ET7304_RTCTRL8		0x9B
/* Autoidle timeout = (tout * 2 + 1) * 6.4ms */
#define ET7304_RTCTRL8_SET(ck300, ship_off, auto_idle, tout) \
			    (((ck300) << 7) | ((ship_off) << 5) | \
			    ((auto_idle) << 3) | ((tout) & 0x07))
#define ET7304_RTCTRL13	0xA0
#define ET7304_RTCTRL14	0xA1
#define ET7304_RTCTRL15	0xA2
#define ET7304_RTCTRL16	0xA3

#define TCPC_CMD_RESETTRANSMITBUFFER		0xDD
#define ET7304_TCPM_DEBOUNCE_MS		500 /* ms */

struct et7304_chip {
	struct tcpci_data data;
	struct tcpci *tcpci;
	struct device *dev;
	struct regulator *vbus;
	struct usb_role_switch *role_sw;
	struct delayed_work wq_detcable;
	unsigned long debounce_jiffies;
	bool vbus_on;
	int gpio_int_n_irq;
	int gpio_int_n;
};

static const char * const typec_cc_status_name[] = {
	[TYPEC_CC_OPEN]		= "Open",
	[TYPEC_CC_RA]		= "Ra",
	[TYPEC_CC_RD]		= "Rd",
	[TYPEC_CC_RP_DEF]	= "Rp-def",
	[TYPEC_CC_RP_1_5]	= "Rp-1.5",
	[TYPEC_CC_RP_3_0]	= "Rp-3.0",
};

static const char * const usb_role_name[] = {
	[USB_ROLE_NONE]		= "NONE",
	[USB_ROLE_HOST]		= "HOST",
	[USB_ROLE_DEVICE]	= "DEVICE",
};

#define tcpci_cc_is_sink(cc) \
	((cc) == TYPEC_CC_RP_DEF || (cc) == TYPEC_CC_RP_1_5 || \
	 (cc) == TYPEC_CC_RP_3_0)

#define tcpci_port_is_sink(cc1, cc2) \
	(tcpci_cc_is_sink(cc1) || tcpci_cc_is_sink(cc2))

#define tcpci_cc_is_source(cc) ((cc) == TYPEC_CC_RD)

#define tcpci_port_is_source(cc1, cc2) \
	((tcpci_cc_is_source(cc1) && !tcpci_cc_is_source(cc2)) || \
	 (tcpci_cc_is_source(cc2) && !tcpci_cc_is_source(cc1)))

static int et7304_read16(struct et7304_chip *chip, unsigned int reg, u16 *val)
{
	return regmap_raw_read(chip->data.regmap, reg, val, sizeof(u16));
}

static int et7304_write16(struct et7304_chip *chip, unsigned int reg, u16 val)
{
	return regmap_raw_write(chip->data.regmap, reg, &val, sizeof(u16));
}

static int et7304_read8(struct et7304_chip *chip, unsigned int reg, u8 *val)
{
	return regmap_raw_read(chip->data.regmap, reg, val, sizeof(u8));
}

static int et7304_write8(struct et7304_chip *chip, unsigned int reg, u8 val)
{
	return regmap_raw_write(chip->data.regmap, reg, &val, sizeof(u8));
}

static int tcpci_read16(struct tcpci *tcpci, unsigned int reg, u16 *val)
{
	return regmap_raw_read(tcpci->regmap, reg, val, sizeof(u16));
}

static int tcpci_write16(struct tcpci *tcpci, unsigned int reg, u16 val)
{
	return regmap_raw_write(tcpci->regmap, reg, &val, sizeof(u16));
}

static const struct regmap_config et7304_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xFF, /* 0x80 .. 0xFF are vendor defined */
};

static struct et7304_chip *tdata_to_et7304(struct tcpci_data *tdata)
{
	return container_of(tdata, struct et7304_chip, data);
}

static int et7304_init(struct tcpci *tcpci, struct tcpci_data *tdata)
{
	int ret;
	struct et7304_chip *chip = tdata_to_et7304(tdata);

	/* CK 300K from 320K, shipping off, auto_idle enable, tout = 32ms */
	ret = et7304_write8(chip, ET7304_RTCTRL8,
			     ET7304_RTCTRL8_SET(0, 1, 1, 2));
	/* tTCPCfilter : (26.7 * val) us */
	ret |= et7304_write8(chip, ET7304_RTCTRL14, 0x0F);
	/*  tDRP : (51.2 + 6.4 * val) ms */
	ret |= et7304_write8(chip, ET7304_RTCTRL15, 0x04);
	/* dcSRC.DRP : 33% */
	ret |= et7304_write16(chip, ET7304_RTCTRL16, 330);

	if (ret < 0)
		dev_err(chip->dev, "fail to init registers(%d)\n", ret);

	return ret;
}

static int et7304_set_vbus(struct tcpci *tcpci, struct tcpci_data *tdata,
			    bool on, bool charge)
{
	struct et7304_chip *chip = tdata_to_et7304(tdata);
	int ret = 0;

	if (chip->vbus_on == on) {
		dev_dbg(chip->dev, "vbus is already %s", on ? "On" : "Off");
		goto done;
	}

	if (on) {
		ret = regulator_enable(chip->vbus);
	} else {
		ret = regulator_disable(chip->vbus);
	}
	if (ret < 0) {
		dev_err(chip->dev, "cannot %s vbus regulator, ret=%d",
			on ? "enable" : "disable", ret);
		goto done;
	}

	chip->vbus_on = on;

done:
	return ret;
}

static int et7304_set_vconn(struct tcpci *tcpci, struct tcpci_data *tdata,
			    bool enable)
{
	struct et7304_chip *chip = tdata_to_et7304(tdata);

	return et7304_write8(chip, ET7304_RTCTRL8,
			      ET7304_RTCTRL8_SET(0, 1, !enable, 2));
}

static int et7304_start_drp_toggling(struct tcpci *tcpci,
				     struct tcpci_data *tdata,
				     enum typec_cc_status cc)
{
	struct et7304_chip *chip = tdata_to_et7304(tdata);
	int ret;
	unsigned int reg = 0;

	switch (cc) {
	default:
	case TYPEC_CC_RP_DEF:
		reg |= (TCPC_ROLE_CTRL_RP_VAL_DEF <<
			TCPC_ROLE_CTRL_RP_VAL_SHIFT);
		break;
	case TYPEC_CC_RP_1_5:
		reg |= (TCPC_ROLE_CTRL_RP_VAL_1_5 <<
			TCPC_ROLE_CTRL_RP_VAL_SHIFT);
		break;
	case TYPEC_CC_RP_3_0:
		reg |= (TCPC_ROLE_CTRL_RP_VAL_3_0 <<
			TCPC_ROLE_CTRL_RP_VAL_SHIFT);
		break;
	}

	if (cc == TYPEC_CC_RD)
		reg |= (TCPC_ROLE_CTRL_CC_RD << TCPC_ROLE_CTRL_CC1_SHIFT) |
			   (TCPC_ROLE_CTRL_CC_RD << TCPC_ROLE_CTRL_CC2_SHIFT);
	else
		reg |= (TCPC_ROLE_CTRL_CC_RP << TCPC_ROLE_CTRL_CC1_SHIFT) |
			   (TCPC_ROLE_CTRL_CC_RP << TCPC_ROLE_CTRL_CC2_SHIFT);

	ret = et7304_write8(chip, TCPC_ROLE_CTRL, reg);
	if (ret < 0)
		return ret;
	usleep_range(500, 1000);

	return 0;
}

static irqreturn_t et7304_irq(int irq, void *dev_id)
{
	struct et7304_chip *chip = dev_id;
#if 0
	int ret;
	u16 alert;
	u8 status;

	ret = et7304_read16(chip, TCPC_ALERT, &alert);
	if (ret < 0)
		goto out;

	if (alert & TCPC_ALERT_CC_STATUS) {
		ret = et7304_read8(chip, TCPC_CC_STATUS, &status);
		if (ret < 0)
			goto out;
		/* Clear cc change event triggered by starting toggling */
		if (status & TCPC_CC_STATUS_TOGGLING)
			et7304_write8(chip, TCPC_ALERT, TCPC_ALERT_CC_STATUS);
	}
#else
	queue_delayed_work(system_power_efficient_wq, &chip->wq_detcable,
		chip->debounce_jiffies);
#endif
out:
	return tcpci_irq(chip->tcpci);
}

static int et7304_sw_reset(struct et7304_chip *chip)
{
	int ret;

	ret = et7304_write8(chip, ET7304_RTCTRL13, 0x01);
	if (ret < 0)
		return ret;

	usleep_range(1000, 2000);
	return 0;
}

static int et7304_check_revision(struct i2c_client *i2c)
{
	int ret;

	ret = i2c_smbus_read_word_data(i2c, TCPC_VENDOR_ID);
	if (ret < 0) {
		dev_err(&i2c->dev, "fail to read Vendor id(%d)\n", ret);
		return ret;
	}

	if (ret != ET7304_VID) {
		dev_err(&i2c->dev, "vid is not correct, 0x%04x\n", ret);
		return -ENODEV;
	}

	ret = i2c_smbus_read_word_data(i2c, TCPC_PRODUCT_ID);
	if (ret < 0) {
		dev_err(&i2c->dev, "fail to read Product id(%d)\n", ret);
		return ret;
	}

	if (ret != ET7304_PID) {
		dev_err(&i2c->dev, "pid is not correct, 0x%04x\n", ret);
		return -ENODEV;
	}

	return 0;
}

static int et7304_init_gpio(struct et7304_chip *chip)
{
	struct device *dev = chip->dev;
	struct device_node *np = dev->of_node;
	int ret = 0;

	ret = of_get_named_gpio(np, "et7304,intr_gpio", 0);
	if (ret < 0) {
		dev_err(dev, "no intr_gpio info, ret = %d\n", ret);
		return ret;
	}
	chip->gpio_int_n = ret;

	ret = devm_gpio_request(chip->dev, chip->gpio_int_n, "et7304,intr_gpio");
	if (ret < 0) {
		dev_err(dev, "failed to request GPIO%d (ret = %d)\n", chip->gpio_int_n, ret);
		return ret;
	}

	ret = gpio_direction_input(chip->gpio_int_n);
	if (ret < 0) {
		dev_err(dev, "failed to set GPIO%d as input pin(ret = %d)\n", chip->gpio_int_n, ret);
		return ret;
	}

	ret = gpio_to_irq(chip->gpio_int_n);
	if (ret < 0) {
		dev_err(dev,
			"cannot request IRQ for GPIO Int_N, ret=%d", ret);
		return ret;
	}
	chip->gpio_int_n_irq = ret;

	return 0;
}

static void et7304_detect_cable(struct work_struct *work)
{
	struct et7304_chip *chip = container_of(to_delayed_work(work), struct et7304_chip,
						 wq_detcable);
	enum typec_cc_status cc1, cc2;

	chip->tcpci->tcpc.get_cc(&chip->tcpci->tcpc, &cc1, &cc2);

	dev_info(chip->dev, "CC1: %d - %s, CC2: %d - %s\n",
		 cc1, typec_cc_status_name[cc1], cc2, typec_cc_status_name[cc2]);

	/**
	 * FIXME:
	 * 1. Support double Rp to Vbus cable as sink and device.
	 * 2. Update symbol list for 'usb_role_switch_get_role' function.
	 */
	if (!tcpci_port_is_sink(cc1, cc2) && !tcpci_port_is_source(cc1, cc2)) {
		dev_dbg(chip->dev, "Setting Role [%s]\n", usb_role_name[USB_ROLE_NONE]);
		usb_role_switch_set_role(chip->role_sw, USB_ROLE_NONE);

	} else if (tcpci_cc_is_sink(cc1) && tcpci_cc_is_sink(cc2)) {
		dev_dbg(chip->dev, "Setting Role [%s]\n", usb_role_name[USB_ROLE_DEVICE]);
		usb_role_switch_set_role(chip->role_sw, USB_ROLE_DEVICE);
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
static int et7304_probe(struct i2c_client *client,
			 const struct i2c_device_id *i2c_id)
#else
static int et7304_probe(struct i2c_client *client)
#endif
{
	int ret;
	struct et7304_chip *chip;

	ret = et7304_check_revision(client);
	if (ret < 0) {
		dev_err(&client->dev, "check vid/pid fail(%d)\n", ret);
		return ret;
	}

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->data.regmap = devm_regmap_init_i2c(client,
						 &et7304_regmap_config);
	if (IS_ERR(chip->data.regmap))
		return PTR_ERR(chip->data.regmap);

	chip->dev = &client->dev;
	i2c_set_clientdata(client, chip);

	chip->vbus = devm_regulator_get_optional(chip->dev, "vbus");
	if (IS_ERR(chip->vbus)) {
		ret = PTR_ERR(chip->vbus);
		chip->vbus = NULL;
		if (ret != -ENODEV)
			return ret;
	}

	ret = et7304_sw_reset(chip);
	if (ret < 0)
		return ret;

	/* Disable chip interrupts before requesting irq */
	ret = et7304_write16(chip, TCPC_ALERT_MASK, 0);
	if (ret < 0)
		return ret;

	if (chip->vbus)
		chip->data.set_vbus = et7304_set_vbus;

	chip->data.init = et7304_init;
	chip->data.set_vconn = et7304_set_vconn;
	chip->data.start_drp_toggling = et7304_start_drp_toggling;

	chip->debounce_jiffies = msecs_to_jiffies(ET7304_TCPM_DEBOUNCE_MS);
	INIT_DELAYED_WORK(&chip->wq_detcable, et7304_detect_cable);

	if (client->irq) {
		chip->gpio_int_n_irq = client->irq;
	} else {
		ret = et7304_init_gpio(chip);
		if (ret < 0)
			return ret;
		client->irq = chip->gpio_int_n_irq;
	}

	chip->tcpci = tcpci_register_port(chip->dev, &chip->data);
	if (IS_ERR(chip->tcpci)) {
		dev_err(chip->dev, "fail to register tcpci port, %ld\n", PTR_ERR(chip->tcpci));
		return PTR_ERR(chip->tcpci);
	}

	chip->role_sw = usb_role_switch_get(chip->dev);
	if (IS_ERR(chip->role_sw)) {
		ret = PTR_ERR(chip->role_sw);
		dev_err(chip->dev, "fail to get usb role switch, %ld\n", PTR_ERR(chip->role_sw));
		return ret;
	}

	ret = devm_request_threaded_irq(chip->dev, client->irq, NULL,
					et7304_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_LOW,
					client->name, chip);

	if (ret < 0)
		return ret;

	if (!device_property_read_bool(chip->dev, "wakeup-source")) {
		dev_info(chip->dev, "wakeup source is disabled!\n");
	} else {
		enable_irq_wake(client->irq);
	}

	dev_err(&client->dev, "%s successful\n", __func__);

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
static int et7304_remove(struct i2c_client *client)
#else
static void et7304_remove(struct i2c_client *client)
#endif
{
	struct et7304_chip *chip = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&chip->wq_detcable);
	usb_role_switch_put(chip->role_sw);
	tcpci_unregister_port(chip->tcpci);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
	return 0;
#endif
}

static const struct i2c_device_id et7304_id[] = {
	{ "et7304", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, et7304_id);

#ifdef CONFIG_OF
static const struct of_device_id et7304_of_match[] = {
	{ .compatible = "etek,et7304", },
	{},
};
MODULE_DEVICE_TABLE(of, et7304_of_match);
#endif

static struct i2c_driver et7304_i2c_driver = {
	.driver = {
		.name = "et7304",
		.of_match_table = of_match_ptr(et7304_of_match),
	},
	.probe = et7304_probe,
	.remove = et7304_remove,
	.id_table = et7304_id,
};
module_i2c_driver(et7304_i2c_driver);

MODULE_AUTHOR("jianghao@allwinnertech.com");
MODULE_DESCRIPTION("ET7304 USB Type-C Port Controller Interface Driver");
MODULE_LICENSE("GPL");
