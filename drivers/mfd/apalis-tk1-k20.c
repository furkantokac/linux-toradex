/*
 * Copyright 2016-2017 Toradex AG
 * Dominik Sliwa <dominik.sliwa@toradex.com>
 *
 * based on a driver for MC13xxx by:
 * Copyright 2009-2010 Pengutronix
 * Uwe Kleine-Koenig <u.kleine-koenig@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/apalis-tk1-k20.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>

#include "apalis-tk1-k20-ezp.h"
#define APALIS_TK1_K20_MAX_MSG 4

static unsigned int fw_ignore = 0;
module_param(fw_ignore , uint, 0);
MODULE_PARM_DESC(fw_ignore, "Assume that K20 is running valid fw version. "
		 "Don't verify, don't erase, don't update");

static unsigned int force_fw_reload = 0;
module_param(force_fw_reload , uint, 0);
MODULE_PARM_DESC(force_fw_reload, "Update K20 fw even when the same version"
		" is already flashed.");

static const struct spi_device_id apalis_tk1_k20_device_ids[] = {
	{
		.name = "apalis-tk1-k20",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(spi, apalis_tk1_k20_device_ids);

static const struct of_device_id apalis_tk1_k20_dt_ids[] = {
	{
		.compatible = "toradex,apalis-tk1-k20",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, apalis_tk1_k20_dt_ids);

static const struct regmap_config apalis_tk1_k20_regmap_spi_config = {
	.reg_bits = 8,
	.pad_bits = 0,
	.val_bits = 8,

	.max_register = APALIS_TK1_K20_LAST_REG,

	.cache_type = REGCACHE_NONE,
	.use_single_rw = 0,
};

static int apalis_tk1_k20_spi_read(void *context, const void *reg,
		size_t reg_size, void *val, size_t val_size)
{
	unsigned char w[APALIS_TK1_K20_MAX_BULK] = {APALIS_TK1_K20_READ_INST,
			 *((unsigned char *) reg), val_size, 0x00, 0x00, 0x00,
			0x00};
	unsigned char r[APALIS_TK1_K20_MAX_BULK];
	unsigned char *p = val;
	int i = 0, j = 0;
	struct device *dev = context;
	struct spi_device *spi = to_spi_device(dev);
	struct spi_transfer t = {
		.tx_buf = w,
		.rx_buf = r,
		.len = 8,
		.cs_change = 0,
		.delay_usecs = 0,
	};

	struct spi_message m;
	int ret;
	spi->mode = SPI_MODE_1;

	if (reg_size != 1)
		return -ENOTSUPP;

	if (val_size == 1) {
		spi_message_init(&m);
		spi_message_add_tail(&t, &m);
		ret = spi_sync(spi, &m);

		for (i = 3; i < 7; i++ )
		{
			if (((unsigned char *)t.rx_buf)[i] == 0x55) {
				*p = ((unsigned char *)t.rx_buf)[i + 1];
				return ret;
			}
		}

		for (j = 0; j < APALIS_TK1_MAX_RETRY_CNT; j++) {
			udelay(250 * j * j);
			t.tx_buf = w;
			t.rx_buf = r;
			spi_message_init(&m);
			spi_message_add_tail(&t, &m);
			ret = spi_sync(spi, &m);
			for (i = 3; i < 7; i++ )
			{
				if (((unsigned char *)t.rx_buf)[i] == 0x55) {
					*p = ((unsigned char *)t.rx_buf)[i + 1];
					return ret;
				}
			}
		}
		ret = -EIO;

	} else if ((val_size > 1) && (val_size < APALIS_TK1_K20_MAX_BULK)) {
		t.len = 12;
		w[0] = APALIS_TK1_K20_BULK_READ_INST;
		spi_message_init(&m);
		spi_message_add_tail(&t, &m);
		ret = spi_sync(spi, &m);
		/* no need to reinit the message*/
		t.len = val_size;
		t.rx_buf = p;
		/* just use the same transfer */
		ret = spi_sync(spi, &m);

	} else {
		return -ENOTSUPP;
	}

	return ret;
}


static int apalis_tk1_k20_spi_write(void *context, const void *data,
		size_t count)
{
	struct device *dev = context;
	struct spi_device *spi = to_spi_device(dev);
	uint8_t out_data[APALIS_TK1_K20_MAX_BULK];
	int ret;

	spi->mode = SPI_MODE_1;

	if (count == 2) {
		out_data[0] = APALIS_TK1_K20_WRITE_INST;
		out_data[1] = ((uint8_t *)data)[0];
		out_data[2] = ((uint8_t *)data)[1];
		ret = spi_write(spi, out_data, 3);

	} else if ((count > 2) && (count < APALIS_TK1_K20_MAX_BULK)) {
		out_data[0] = APALIS_TK1_K20_BULK_WRITE_INST;
		out_data[1] = ((uint8_t *)data)[0];
		out_data[2] = count - 1;
		memcpy(&out_data[3], &((uint8_t *)data)[1], count - 1);
		ret = spi_write(spi, out_data, count + 2);
	} else {
		dev_err(dev, "Apalis TK1 K20 MFD invalid write count = %d\n",
				count);
		return -ENOTSUPP;
	}
	return ret;
}

static struct regmap_bus regmap_apalis_tk1_k20_bus = {
	.write = apalis_tk1_k20_spi_write,
	.read = apalis_tk1_k20_spi_read,
};

void apalis_tk1_k20_lock(struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	if (!mutex_trylock(&apalis_tk1_k20->lock)) {
		dev_dbg(apalis_tk1_k20->dev, "wait for %s from %ps\n",
				__func__, __builtin_return_address(0));

		mutex_lock(&apalis_tk1_k20->lock);
	}
	dev_dbg(apalis_tk1_k20->dev, "%s from %ps\n",
			__func__, __builtin_return_address(0));
}
EXPORT_SYMBOL(apalis_tk1_k20_lock);

void apalis_tk1_k20_unlock(struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	dev_dbg(apalis_tk1_k20->dev, "%s from %ps\n",
			__func__, __builtin_return_address(0));
	mutex_unlock(&apalis_tk1_k20->lock);
}
EXPORT_SYMBOL(apalis_tk1_k20_unlock);

int apalis_tk1_k20_reg_read(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		unsigned int offset, u32 *val)
{
	int ret;

	ret = regmap_read(apalis_tk1_k20->regmap, offset, val);
	dev_dbg(apalis_tk1_k20->dev, "[0x%02x] -> 0x%02x ret = %d\n", offset,
			*val, ret);

	return ret;
}
EXPORT_SYMBOL(apalis_tk1_k20_reg_read);

int apalis_tk1_k20_reg_write(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		unsigned int offset, u32 val)
{
	int ret;

	if (val >= BIT(24))
		return -EINVAL;

	ret = regmap_write(apalis_tk1_k20->regmap, offset, val);

	dev_dbg(apalis_tk1_k20->dev, "[0x%02x] <- 0x%02x ret = %d\n", offset, val,
			 ret);

	return ret;
}
EXPORT_SYMBOL(apalis_tk1_k20_reg_write);

int apalis_tk1_k20_reg_read_bulk(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		unsigned int offset,
		uint8_t *val, size_t size)
{
	int ret;

	if (size > APALIS_TK1_K20_MAX_BULK)
		return -EINVAL;

	ret = regmap_bulk_read(apalis_tk1_k20->regmap, offset, val, size);
	dev_dbg(apalis_tk1_k20->dev, "bulk read %d bytes from [0x%02x]\n",
			size, offset);

	return ret;
}
EXPORT_SYMBOL(apalis_tk1_k20_reg_read_bulk);

int apalis_tk1_k20_reg_write_bulk(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		unsigned int offset, uint8_t *buf, size_t size)
{
	dev_dbg(apalis_tk1_k20->dev, "bulk write %d bytes to [0x%02x]\n",
			(unsigned int)size, offset);

	if (size > APALIS_TK1_K20_MAX_BULK)
		return -EINVAL;
	return regmap_bulk_write(apalis_tk1_k20->regmap, offset, buf, size);
}
EXPORT_SYMBOL(apalis_tk1_k20_reg_write_bulk);

int apalis_tk1_k20_reg_rmw(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		unsigned int offset, u32 mask, u32 val)
{
	dev_dbg(apalis_tk1_k20->dev, "[0x%02x] <- 0x%06x (mask: 0x%06x)\n",
			offset, val, mask);

	return regmap_update_bits(apalis_tk1_k20->regmap, offset, mask, val);
}
EXPORT_SYMBOL(apalis_tk1_k20_reg_rmw);

int apalis_tk1_k20_irq_mask(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		int irq)
{
	int virq = -1;
	if (irq != APALIS_TK1_K20_CAN1_IRQ && irq != APALIS_TK1_K20_CAN0_IRQ) {
		virq = regmap_irq_get_virq(apalis_tk1_k20->irq_data, irq);

	} else {
		virq = (irq == APALIS_TK1_K20_CAN0_IRQ) ?
			apalis_tk1_k20->can0_irq:apalis_tk1_k20->can1_irq;
	}

	disable_irq_nosync(virq);

	return 0;
}
EXPORT_SYMBOL(apalis_tk1_k20_irq_mask);

int apalis_tk1_k20_irq_unmask(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		int irq)
{
	int virq = -1;
	if (irq != APALIS_TK1_K20_CAN1_IRQ && irq != APALIS_TK1_K20_CAN0_IRQ) {
		virq = regmap_irq_get_virq(apalis_tk1_k20->irq_data, irq);

	} else {
		virq = (irq == APALIS_TK1_K20_CAN0_IRQ) ?
			apalis_tk1_k20->can0_irq:apalis_tk1_k20->can1_irq;
	}

	enable_irq(virq);

	return 0;
}
EXPORT_SYMBOL(apalis_tk1_k20_irq_unmask);

int apalis_tk1_k20_irq_status(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		int irq, int *enabled, int *pending)
{
	int ret;
	unsigned int offmask = APALIS_TK1_K20_MSQREG;
	unsigned int offstat = APALIS_TK1_K20_IRQREG;
	u32 irqbit = 1 << irq;

	if (irq < 0 || irq >= ARRAY_SIZE(apalis_tk1_k20->irqs))
		return -EINVAL;

	if (enabled) {
		u32 mask;

		ret = apalis_tk1_k20_reg_read(apalis_tk1_k20, offmask, &mask);
		if (ret)
			return ret;

		*enabled = mask & irqbit;
	}

	if (pending) {
		u32 stat;

		ret = apalis_tk1_k20_reg_read(apalis_tk1_k20, offstat, &stat);
		if (ret)
			return ret;

		*pending = stat & irqbit;
	}

	return 0;
}
EXPORT_SYMBOL(apalis_tk1_k20_irq_status);

int apalis_tk1_k20_irq_request(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		int irq, irq_handler_t handler, const char *name, void *dev)
{
	int virq = -1;
	int irq_flags = IRQF_ONESHOT;
	if (irq != APALIS_TK1_K20_CAN1_IRQ && irq != APALIS_TK1_K20_CAN0_IRQ) {
		virq = regmap_irq_get_virq(apalis_tk1_k20->irq_data, irq);
		irq_flags = IRQF_ONESHOT;
	} else {
		virq = (irq == APALIS_TK1_K20_CAN0_IRQ) ?
			apalis_tk1_k20->can0_irq:apalis_tk1_k20->can1_irq;
		irq_flags = IRQF_ONESHOT | IRQF_TRIGGER_HIGH;
	}
	return devm_request_threaded_irq(apalis_tk1_k20->dev, virq,
			 NULL, handler, irq_flags, name, dev);
}
EXPORT_SYMBOL(apalis_tk1_k20_irq_request);

int apalis_tk1_k20_irq_free(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
		int irq, void *dev)
{
	int virq = -1;
	if (irq != APALIS_TK1_K20_CAN1_IRQ && irq != APALIS_TK1_K20_CAN0_IRQ) {
		virq = regmap_irq_get_virq(apalis_tk1_k20->irq_data, irq);

	} else {
		virq = (irq == APALIS_TK1_K20_CAN0_IRQ) ?
			apalis_tk1_k20->can0_irq:apalis_tk1_k20->can1_irq;
	}

	devm_free_irq(apalis_tk1_k20->dev, virq, dev);

	return 0;
}
EXPORT_SYMBOL(apalis_tk1_k20_irq_free);


static int apalis_tk1_k20_add_subdevice_pdata_id(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20, const char *name,
		void *pdata, size_t pdata_size, int id)
{
	struct mfd_cell cell = {
		.platform_data = pdata,
		.pdata_size = pdata_size,
	};

	cell.name = kmemdup(name, strlen(name) + 1, GFP_KERNEL);
	if (!cell.name)
		return -ENOMEM;

	return mfd_add_devices(apalis_tk1_k20->dev, id, &cell, 1, NULL, 0,
			       regmap_irq_get_domain(apalis_tk1_k20->irq_data));
}

static int apalis_tk1_k20_add_subdevice_pdata(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20, const char *name,
		void *pdata, size_t pdata_size)
{
	return apalis_tk1_k20_add_subdevice_pdata_id(apalis_tk1_k20, name,
			pdata, pdata_size, -1);
}

static int apalis_tk1_k20_add_subdevice(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20, const char *name)
{
	return apalis_tk1_k20_add_subdevice_pdata(apalis_tk1_k20, name, NULL,
						  0);
}

static void apalis_tk1_k20_reset_chip(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	udelay(10);
	gpio_set_value(apalis_tk1_k20->reset_gpio, 0);
	msleep(10);
	gpio_set_value(apalis_tk1_k20->reset_gpio, 1);
	udelay(10);
}

#ifdef CONFIG_APALIS_TK1_K20_EZP
static int apalis_tk1_k20_read_ezport(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20, uint8_t command,
		int addr, int count, uint8_t *buffer)
{
	uint8_t w[4 + APALIS_TK1_K20_EZP_MAX_DATA];
	uint8_t r[4 + APALIS_TK1_K20_EZP_MAX_DATA];
	uint8_t *out;
	struct spi_message m;
	struct spi_device *spi = to_spi_device(apalis_tk1_k20->dev);
	struct spi_transfer t = {
		.tx_buf = w,
		.rx_buf = r,
		.speed_hz = APALIS_TK1_K20_EZP_MAX_SPEED,
	};
	int ret;

	spi->mode = SPI_MODE_0;

	if (count > APALIS_TK1_K20_EZP_MAX_DATA)
		return -ENOSPC;

	memset(w, 0, 4 + count);

	switch (command) {
	case APALIS_TK1_K20_EZP_READ:
	case APALIS_TK1_K20_EZP_FREAD:
		t.len = 4 + count;
		w[1] = (addr & 0xFF0000) >> 16;
		w[2] = (addr & 0xFF00) >> 8;
		w[3] = (addr & 0xFC);
		out = &r[4];
		break;
	case APALIS_TK1_K20_EZP_RDSR:
	case APALIS_TK1_K20_EZP_FRDFCOOB:
		t.len = 1 + count;
		out = &r[1];
		break;
	default:
		return -EINVAL;
	}
	w[0] = command;

	gpio_set_value(apalis_tk1_k20->ezpcs_gpio, 0);
	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	ret = spi_sync(spi, &m);
	gpio_set_value(apalis_tk1_k20->ezpcs_gpio, 1);
	if (ret != 0)
		return ret;

	memcpy(buffer, out, count);

	return 0;
}

static int apalis_tk1_k20_write_ezport(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20, uint8_t command,
		int addr, int count, const uint8_t *buffer)
{
	uint8_t w[4 + APALIS_TK1_K20_EZP_MAX_DATA];
	uint8_t r[4 + APALIS_TK1_K20_EZP_MAX_DATA];
	struct spi_message m;
	struct spi_device *spi = to_spi_device(apalis_tk1_k20->dev);
	struct spi_transfer t = {
		.tx_buf = w,
		.rx_buf = r,
		.speed_hz = APALIS_TK1_K20_EZP_MAX_SPEED,
	};
	int ret;

	spi->mode = SPI_MODE_0;

	if (count > APALIS_TK1_K20_EZP_MAX_DATA)
		return -ENOSPC;

	switch (command) {
	case APALIS_TK1_K20_EZP_SP:
	case APALIS_TK1_K20_EZP_SE:
		t.len = 4 + count;
		w[1] = (addr & 0xFF0000) >> 16;
		w[2] = (addr & 0xFF00) >> 8;
		w[3] = (addr & 0xF8);
		memcpy(&w[4], buffer, count);
		break;
	case APALIS_TK1_K20_EZP_WREN:
	case APALIS_TK1_K20_EZP_WRDI:
	case APALIS_TK1_K20_EZP_BE:
	case APALIS_TK1_K20_EZP_RESET:
	case APALIS_TK1_K20_EZP_WRFCCOB:
		t.len = 1 + count;
		memcpy(&w[1], buffer, count);
		break;
	default:
		return -EINVAL;
	}
	w[0] = command;

	gpio_set_value(apalis_tk1_k20->ezpcs_gpio, 0);
	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	ret = spi_sync(spi, &m);
	gpio_set_value(apalis_tk1_k20->ezpcs_gpio, 1);

	return ret;
}

static int apalis_tk1_k20_set_wren_ezport(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	uint8_t buffer;

	if (apalis_tk1_k20_write_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_WREN,
			0, 0, NULL) < 0)
		return -EIO;
	if (apalis_tk1_k20_read_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_RDSR,
			0, 1, &buffer) < 0)
		return -EIO;
	if ((buffer & APALIS_TK1_K20_EZP_STA_WEN))
		return 0;

	/* If it failed try one last time */
	if (apalis_tk1_k20_write_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_WREN,
			0, 0, NULL) < 0)
		return -EIO;
	if (apalis_tk1_k20_read_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_RDSR,
			0, 1, &buffer) < 0)
		return -EIO;
	if ((buffer & APALIS_TK1_K20_EZP_STA_WEN))
		return 0;

	return -EIO;

}

static int apalis_tk1_k20_enter_ezport(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	uint8_t status = 0x00;
	uint8_t buffer;

	gpio_set_value(apalis_tk1_k20->ezpcs_gpio, 0);
	msleep(10);
	apalis_tk1_k20_reset_chip(apalis_tk1_k20);
	msleep(10);
	gpio_set_value(apalis_tk1_k20->ezpcs_gpio, 1);
	if (apalis_tk1_k20_read_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_RDSR,
			0, 1, &buffer) < 0)
		goto bad;
	status = buffer;
	if (apalis_tk1_k20_write_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_WREN,
			0, 0, NULL) < 0)
		goto bad;
	if (apalis_tk1_k20_read_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_RDSR,
			0, 1, &buffer) < 0)
		goto bad;
	if ((buffer & APALIS_TK1_K20_EZP_STA_WEN) && buffer != status)
		return 0;

bad:
	dev_err(apalis_tk1_k20->dev, "Error entering EZ Port mode.\n");
	return -EIO;
}

static int apalis_tk1_k20_erase_chip_ezport(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	uint8_t buffer[16];
	int i;

	if (apalis_tk1_k20_set_wren_ezport(apalis_tk1_k20))
		goto bad;
	if (apalis_tk1_k20_write_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_BE,
			0, 0, NULL) < 0)
		goto bad;

	if (apalis_tk1_k20_read_ezport(apalis_tk1_k20, APALIS_TK1_K20_EZP_RDSR,
			0, 1, buffer) < 0)
		goto bad;

	i = 0;
	while (buffer[0] & APALIS_TK1_K20_EZP_STA_WIP) {
		msleep(20);
		if ((apalis_tk1_k20_read_ezport(apalis_tk1_k20,
		     APALIS_TK1_K20_EZP_RDSR, 0, 1, buffer) < 0) || (i > 50))
			goto bad;
		i++;
	}

	return 0;

bad:
	dev_err(apalis_tk1_k20->dev, "Error erasing the chip.\n");
	return -EIO;
}

static int apalis_tk1_k20_flash_chip_ezport(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	uint8_t buffer;
	const uint8_t *fw_chunk;
	int i, j, transfer_size;

	for (i = 0; i < fw_entry->size;) {
		if (apalis_tk1_k20_set_wren_ezport(apalis_tk1_k20))
			goto bad;

		fw_chunk = fw_entry->data + i;
		transfer_size = (i + APALIS_TK1_K20_EZP_WRITE_SIZE <
				 fw_entry->size) ? APALIS_TK1_K20_EZP_WRITE_SIZE
				 : (fw_entry->size - i - 1);
		dev_dbg(apalis_tk1_k20->dev,
			 "Apalis TK1 K20 MFD transfer_size = %d addr = 0x%X\n",
				 transfer_size, i);
		if (apalis_tk1_k20_write_ezport(apalis_tk1_k20,
				APALIS_TK1_K20_EZP_SP, i,
			transfer_size, fw_chunk) < 0)
			goto bad;
		udelay(2000);
		if (apalis_tk1_k20_read_ezport(apalis_tk1_k20,
				APALIS_TK1_K20_EZP_RDSR, 0, 1, &buffer) < 0)
			goto bad;

		j = 0;
		while (buffer & APALIS_TK1_K20_EZP_STA_WIP) {
			msleep(10);
			if ((apalis_tk1_k20_read_ezport(apalis_tk1_k20,
					APALIS_TK1_K20_EZP_RDSR, 0, 1,
					&buffer) < 0) || (j > 10000))
				goto bad;
			j++;
		}
		i += APALIS_TK1_K20_EZP_WRITE_SIZE;
	}

	return 0;

bad:
	dev_err(apalis_tk1_k20->dev, "Error writing to the chip.\n");
	return -EIO;
}

static uint8_t apalis_tk1_k20_fw_ezport_status(void)
{
	return fw_entry->data[APALIS_TK1_K20_FW_FOPT_ADDR] &
			APALIS_TK1_K20_FOPT_EZP_ENA;
}

static uint8_t apalis_tk1_k20_get_fw_revision(void)
{
	if (fw_entry)
		if (fw_entry->size > APALIS_TK1_K20_FW_VER_ADDR)
			return fw_entry->data[APALIS_TK1_K20_FW_VER_ADDR];
	return 0;
}
#endif /* CONFIG_APALIS_TK1_K20_EZP */


#ifdef CONFIG_OF
static int apalis_tk1_k20_probe_flags_dt(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	struct device_node *np = apalis_tk1_k20->dev->of_node;

	if (!np)
		return -ENODEV;

	if (of_property_read_bool(np, "toradex,apalis-tk1-k20-uses-adc"))
		apalis_tk1_k20->flags |= APALIS_TK1_K20_USES_ADC;

	if (of_property_read_bool(np, "toradex,apalis-tk1-k20-uses-tsc"))
		apalis_tk1_k20->flags |= APALIS_TK1_K20_USES_TSC;

	if (of_property_read_bool(np, "toradex,apalis-tk1-k20-uses-can"))
		apalis_tk1_k20->flags |= APALIS_TK1_K20_USES_CAN;

	if (of_property_read_bool(np, "toradex,apalis-tk1-k20-uses-gpio"))
		apalis_tk1_k20->flags |= APALIS_TK1_K20_USES_GPIO;

	return 0;
}

static int apalis_tk1_k20_probe_gpios_dt(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	struct device_node *np = apalis_tk1_k20->dev->of_node;

	if (!np)
		return -ENODEV;

	apalis_tk1_k20->reset_gpio = of_get_named_gpio(np, "rst-gpio", 0);
	if (apalis_tk1_k20->reset_gpio < 0)
		return apalis_tk1_k20->reset_gpio;

	gpio_request(apalis_tk1_k20->reset_gpio, "apalis-tk1-k20-reset");
	gpio_direction_output(apalis_tk1_k20->reset_gpio, 1);

	apalis_tk1_k20->ezpcs_gpio = of_get_named_gpio(np, "ezport-cs-gpio", 0);
	if (apalis_tk1_k20->ezpcs_gpio < 0)
		return apalis_tk1_k20->ezpcs_gpio;

	gpio_request(apalis_tk1_k20->ezpcs_gpio, "apalis-tk1-k20-ezpcs");
	gpio_direction_output(apalis_tk1_k20->ezpcs_gpio, 1);

	apalis_tk1_k20->int2_gpio = of_get_named_gpio(np, "int2-gpio", 0);
	if (apalis_tk1_k20->int2_gpio < 0)
		return apalis_tk1_k20->int2_gpio;

	gpio_request(apalis_tk1_k20->int2_gpio, "apalis-tk1-k20-int2");
	gpio_direction_output(apalis_tk1_k20->int2_gpio, 1);

	return 0;
}
#else
static inline int apalis_tk1_k20_probe_flags_dt(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	return -ENODEV;
}
static inline int apalis_tk1_k20_probe_gpios_dt(
		struct apalis_tk1_k20_regmap *apalis_tk1_k20)
{
	return -ENODEV;
}
#endif

int apalis_tk1_k20_fw_update(struct apalis_tk1_k20_regmap *apalis_tk1_k20,
			     uint32_t revision)
{
	int erase_only = 0;

	if ((request_firmware(&fw_entry, "apalis-tk1-k20.bin",
			      apalis_tk1_k20->dev)
	     < 0)
	    && (revision != APALIS_TK1_K20_FW_VER)) {
		dev_err(apalis_tk1_k20->dev,
			"Unsupported firmware version %d.%d and no local"
			" firmware file available.\n",
			(revision & 0xF0 >> 8), (revision & 0x0F));
		return -ENOTSUPP;
	}

	if ((fw_entry == NULL) && (revision != APALIS_TK1_K20_FW_VER)) {
		dev_err(apalis_tk1_k20->dev,
			"Unsupported firmware version %d.%d and no local"
			" firmware file available.\n",
			(revision & 0xF0 >> 8), (revision & 0x0F));
		return -ENOTSUPP;
	}

	if (fw_entry != NULL) {
		if (fw_entry->size == 1)
			erase_only = 1;
	}

	if ((apalis_tk1_k20_get_fw_revision() != APALIS_TK1_K20_FW_VER)
	    && (revision != APALIS_TK1_K20_FW_VER) && !erase_only
	    && (fw_entry != NULL)) {
		dev_err(apalis_tk1_k20->dev,
			"Unsupported firmware version in both the device "
			"as well as the local firmware file.\n");
		release_firmware(fw_entry);
		return -ENOTSUPP;
	}

	if ((revision != APALIS_TK1_K20_FW_VER) && !erase_only
	    && (!apalis_tk1_k20_fw_ezport_status()) && (fw_entry != NULL)) {
		dev_err(apalis_tk1_k20->dev,
			"Unsupported firmware version in the device and the "
			"local firmware file disables the EZ Port.\n");
		release_firmware(fw_entry);
		return -ENOTSUPP;
	}

	if (((revision != APALIS_TK1_K20_FW_VER) || erase_only
	     || force_fw_reload)
	    && (fw_entry != NULL)) {
		int i = 0;
		while (apalis_tk1_k20_enter_ezport(apalis_tk1_k20) < 0
		       && i++ < 5) {
			msleep(50);
		}
		if (i >= 5) {
			dev_err(apalis_tk1_k20->dev,
				"Problem entering EZ port mode.\n");
			release_firmware(fw_entry);
			return -EIO;
		}
		if (apalis_tk1_k20_erase_chip_ezport(apalis_tk1_k20) < 0) {
			dev_err(apalis_tk1_k20->dev,
				"Problem erasing the chip. Deferring...\n");
			release_firmware(fw_entry);
			return -EPROBE_DEFER;
		}
		if (erase_only) {
			dev_err(apalis_tk1_k20->dev, "Chip fully erased.\n");
			release_firmware(fw_entry);
			return -EIO;
		}
		if (apalis_tk1_k20_flash_chip_ezport(apalis_tk1_k20) < 0) {
			dev_err(apalis_tk1_k20->dev,
				"Problem flashing new firmware. Deferring...\n");
			release_firmware(fw_entry);
			return -EPROBE_DEFER;
		}
	}

	if (fw_entry != NULL)
		release_firmware(fw_entry);

	return 1;
}

int apalis_tk1_k20_dev_init(struct device *dev)
{
	struct apalis_tk1_k20_platform_data *pdata = dev_get_platdata(dev);
	struct apalis_tk1_k20_regmap *apalis_tk1_k20 = dev_get_drvdata(dev);
	uint32_t revision = 0x00;
	int ret, i;

	apalis_tk1_k20->dev = dev;

	ret = apalis_tk1_k20_probe_gpios_dt(apalis_tk1_k20);
	if ((ret < 0) && pdata) {
		if (pdata) {
			apalis_tk1_k20->ezpcs_gpio = pdata->ezpcs_gpio;
			apalis_tk1_k20->reset_gpio = pdata->reset_gpio;
			apalis_tk1_k20->int2_gpio = pdata->int2_gpio;
		} else {
			dev_err(dev, "Error claiming GPIOs\n");
			ret = -EINVAL;
			goto bad;
		}
	}
	apalis_tk1_k20_reset_chip(apalis_tk1_k20);
	msleep(10);
	ret = apalis_tk1_k20_reg_read(apalis_tk1_k20, APALIS_TK1_K20_REVREG,
			&revision);

#ifdef CONFIG_APALIS_TK1_K20_EZP

	if (fw_ignore == 0) {
		ret = apalis_tk1_k20_fw_update(apalis_tk1_k20, revision);

		if (ret < 0)
			goto bad;
	}
	if (ret) {
		msleep(10);
		apalis_tk1_k20_reset_chip(apalis_tk1_k20);
		msleep(10);

		ret = apalis_tk1_k20_reg_read(apalis_tk1_k20, APALIS_TK1_K20_REVREG,
				&revision);
	}
#endif /* CONFIG_APALIS_TK1_K20_EZP */

	if (ret) {
		dev_err(apalis_tk1_k20->dev, "Device is not answering.\n");
		goto bad;
	}

	if ((revision != APALIS_TK1_K20_FW_VER) && (fw_ignore == 0)) {
		dev_err(apalis_tk1_k20->dev,
			"Unsupported firmware version %d.%d.\n",
			((revision & 0xF0) >> 4), (revision & 0x0F));
		ret = -ENOTSUPP;
		goto bad;
	}

	if (fw_ignore == 1) {
		dev_err(apalis_tk1_k20->dev, "fw_ignore == 1. Detected "
				"firmware %d.%d. Driver expected %d.%d\n",
				((revision & 0xF0) >> 4), (revision & 0x0F),
				((APALIS_TK1_K20_FW_VER & 0xF0) >> 4),
				(APALIS_TK1_K20_FW_VER & 0x0F));
	}

	for (i = 0; i < ARRAY_SIZE(apalis_tk1_k20->irqs); i++) {
		apalis_tk1_k20->irqs[i].reg_offset = i /
						     APALIS_TK1_K20_IRQ_PER_REG;
		apalis_tk1_k20->irqs[i].mask = BIT(i %
						   APALIS_TK1_K20_IRQ_PER_REG);
	}

	apalis_tk1_k20->irq_chip.name		= dev_name(dev);
	apalis_tk1_k20->irq_chip.status_base	= APALIS_TK1_K20_IRQREG;
	apalis_tk1_k20->irq_chip.mask_base	= APALIS_TK1_K20_MSQREG;
	apalis_tk1_k20->irq_chip.ack_base	= 0;
	apalis_tk1_k20->irq_chip.irq_reg_stride	= 0;
	apalis_tk1_k20->irq_chip.num_regs	= APALIS_TK1_K20_IRQ_REG_CNT;
	apalis_tk1_k20->irq_chip.irqs		= apalis_tk1_k20->irqs;
	apalis_tk1_k20->irq_chip.num_irqs = ARRAY_SIZE(apalis_tk1_k20->irqs);

	ret = regmap_add_irq_chip(apalis_tk1_k20->regmap, apalis_tk1_k20->irq,
			IRQF_ONESHOT | IRQF_TRIGGER_FALLING |
			IRQF_TRIGGER_RISING, 0, &apalis_tk1_k20->irq_chip,
			&apalis_tk1_k20->irq_data);
	if (ret)
		goto bad;

	mutex_init(&apalis_tk1_k20->lock);

	if (apalis_tk1_k20_probe_flags_dt(apalis_tk1_k20) < 0 && pdata)
		apalis_tk1_k20->flags = pdata->flags;

	if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_CAN) {
		apalis_tk1_k20->can0_irq = irq_of_parse_and_map(
					apalis_tk1_k20->dev->of_node, 1);
		apalis_tk1_k20->can1_irq = irq_of_parse_and_map(
					apalis_tk1_k20->dev->of_node, 2);
		if (apalis_tk1_k20->can0_irq == 0 ||
				apalis_tk1_k20->can1_irq == 0) {
			apalis_tk1_k20->flags &= ~APALIS_TK1_K20_USES_CAN;
			dev_err(apalis_tk1_k20->dev,
					"Missing CAN interrupts.\n");
		}
	}

	if (pdata) {
		if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_TSC)
			apalis_tk1_k20_add_subdevice_pdata(apalis_tk1_k20,
					"apalis-tk1-k20-ts",
					&pdata->touch, sizeof(pdata->touch));

		if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_ADC)
			apalis_tk1_k20_add_subdevice_pdata(apalis_tk1_k20,
					"apalis-tk1-k20-adc",
					&pdata->adc, sizeof(pdata->adc));

		if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_CAN) {
			/* We have 2 CAN devices inside K20 */
			pdata->can0.id = 0;
			pdata->can1.id = 1;
			apalis_tk1_k20_add_subdevice_pdata(apalis_tk1_k20,
					"apalis-tk1-k20-can",
					&pdata->can0, sizeof(pdata->can0));
			apalis_tk1_k20_add_subdevice_pdata(apalis_tk1_k20,
					"apalis-tk1-k20-can",
					&pdata->can1, sizeof(pdata->can1));
		}
		if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_GPIO)
			apalis_tk1_k20_add_subdevice_pdata(apalis_tk1_k20,
					"apalis-tk1-k20-gpio",
					&pdata->gpio, sizeof(pdata->gpio));
	} else {
		if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_TSC)
			apalis_tk1_k20_add_subdevice(apalis_tk1_k20,
					"apalis-tk1-k20-ts");

		if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_ADC)
			apalis_tk1_k20_add_subdevice(apalis_tk1_k20,
					"apalis-tk1-k20-adc");

		if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_CAN) {
			/* We have 2 CAN devices inside K20 */
			apalis_tk1_k20_add_subdevice_pdata_id(apalis_tk1_k20,
					"apalis-tk1-k20-can",
					NULL, 0, 0);
			apalis_tk1_k20_add_subdevice_pdata_id(apalis_tk1_k20,
					"apalis-tk1-k20-can",
					NULL, 0, 1);
		}

		if (apalis_tk1_k20->flags & APALIS_TK1_K20_USES_GPIO)
			apalis_tk1_k20_add_subdevice(apalis_tk1_k20,
					"apalis-tk1-k20-gpio");
	}

	dev_info(apalis_tk1_k20->dev, "Apalis TK1 K20 MFD driver. "
			"Firmware version %d.%d.\n", FW_MAJOR, FW_MINOR);

	return 0;

bad:
	if (apalis_tk1_k20->ezpcs_gpio >= 0)
		gpio_free(apalis_tk1_k20->ezpcs_gpio);
	if (apalis_tk1_k20->reset_gpio >= 0)
		gpio_free(apalis_tk1_k20->reset_gpio);
	if (apalis_tk1_k20->int2_gpio >= 0)
		gpio_free(apalis_tk1_k20->int2_gpio);

	return ret;
}


static int apalis_tk1_k20_spi_probe(struct spi_device *spi)
{
	struct apalis_tk1_k20_regmap *apalis_tk1_k20;
	int ret;

	apalis_tk1_k20 = devm_kzalloc(&spi->dev, sizeof(*apalis_tk1_k20),
			GFP_KERNEL);
	if (!apalis_tk1_k20)
		return -ENOMEM;

	dev_set_drvdata(&spi->dev, apalis_tk1_k20);

	spi->mode = SPI_MODE_1;

	apalis_tk1_k20->irq = spi->irq;

	spi->max_speed_hz = (spi->max_speed_hz >= APALIS_TK1_K20_MAX_SPI_SPEED)
				? APALIS_TK1_K20_MAX_SPI_SPEED : spi->max_speed_hz;

	ret = spi_setup(spi);
	if (ret)
		return ret;

	apalis_tk1_k20->regmap = devm_regmap_init(&spi->dev,
			&regmap_apalis_tk1_k20_bus, &spi->dev,
			&apalis_tk1_k20_regmap_spi_config);
	if (IS_ERR(apalis_tk1_k20->regmap)) {
		ret = PTR_ERR(apalis_tk1_k20->regmap);
		dev_err(&spi->dev, "Failed to initialize regmap: %d\n", ret);
		return ret;
	}

	return apalis_tk1_k20_dev_init(&spi->dev);
}

static int apalis_tk1_k20_spi_remove(struct spi_device *spi)
{
	struct apalis_tk1_k20_regmap *apalis_tk1_k20 =
			dev_get_drvdata(&spi->dev);

	if (apalis_tk1_k20->ezpcs_gpio >= 0)
		gpio_free(apalis_tk1_k20->ezpcs_gpio);
	if (apalis_tk1_k20->reset_gpio >= 0)
		gpio_free(apalis_tk1_k20->reset_gpio);
	if (apalis_tk1_k20->int2_gpio >= 0)
		gpio_free(apalis_tk1_k20->int2_gpio);

	kfree(spi->controller_data);
	spi->controller_data = NULL;

	mfd_remove_devices(&spi->dev);
	regmap_del_irq_chip(apalis_tk1_k20->irq, apalis_tk1_k20->irq_data);
	mutex_destroy(&apalis_tk1_k20->lock);

	return 0;
}

static struct spi_driver apalis_tk1_k20_spi_driver = {
	.id_table = apalis_tk1_k20_device_ids,
	.driver = {
		.name = "apalis-tk1-k20",
		.of_match_table = apalis_tk1_k20_dt_ids,
	},
	.probe = apalis_tk1_k20_spi_probe,
	.remove = apalis_tk1_k20_spi_remove,
};

static int __init apalis_tk1_k20_init(void)
{
	return spi_register_driver(&apalis_tk1_k20_spi_driver);
}
subsys_initcall(apalis_tk1_k20_init);

static void __exit apalis_tk1_k20_exit(void)
{
	spi_unregister_driver(&apalis_tk1_k20_spi_driver);
}
module_exit(apalis_tk1_k20_exit);

MODULE_DESCRIPTION("MFD driver for Kinetis MK20DN512 MCU on Apalis TK1");
MODULE_AUTHOR("Dominik Sliwa <dominik.sliwa@toradex.com>");
MODULE_LICENSE("GPL v2");
