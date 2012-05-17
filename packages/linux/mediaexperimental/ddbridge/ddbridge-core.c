/*
 * ddbridge.c: Digital Devices PCIe bridge driver
 *
 * Copyright (C) 2010-2011 Digital Devices GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 only, as published by the Free Software Foundation.
 *
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 * Or, point your browser to http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/i2c.h>
#include <linux/swab.h>
#include <linux/vmalloc.h>
#include "ddbridge.h"
#include "ddbridge-regs.h"

#include "tda18271c2dd.h"
#include "stv6110x.h"
#include "stv090x.h"
#include "lnbh24.h"
#include "drxk.h"
#if 0
#include "stv0367.h"
#else
#include "stv0367dd.h"
#endif
#if 0
#include "tda18212.h"
#else
#include "tda18212dd.h"
#endif

static int adapter_alloc;
module_param(adapter_alloc, int, 0444);
MODULE_PARM_DESC(adapter_alloc, "0-one adapter per io, 1-one per tab with io, 2-one per tab, 3-one for all");

static int ts_loop = -1;
module_param(ts_loop, int, 0444);
MODULE_PARM_DESC(ts_loop, "TS in/out on port ts_loop");

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

static struct ddb *ddbs[32];

/* MSI had problems with lost interrupts, fixed but needs testing */
/* #undef CONFIG_PCI_MSI */

/******************************************************************************/

static inline void ddbwritel(struct ddb *dev, u32 val, u32 adr)
{
	writel(val, (char *) (dev->regs+(adr)));
}

static inline u32 ddbreadl(struct ddb *dev, u32 adr)
{
	return readl((char *) (dev->regs+(adr)));
}

#define ddbcpyto(_dev, _adr, _src, _count)   memcpy_toio((char *) \
					(_dev->regs + (_adr)), (_src), (_count))

#define ddbcpyfrom(_dev, _dst, _adr, _count) memcpy_fromio((_dst), (char *) \
					(_dev->regs + (_adr)), (_count))


/******************************************************************************/

static int i2c_write(struct i2c_adapter *adap, u8 adr, u8 *data, int len)
{
	struct i2c_msg msg = {.addr = adr, .flags = 0, .buf = data, .len = len};

	return (i2c_transfer(adap, &msg, 1) == 1) ? 0 : -1;
}

static int i2c_read(struct i2c_adapter *adapter, u8 adr, u8 *val)
{
	struct i2c_msg msgs[1] = {{.addr = adr,  .flags = I2C_M_RD,
				   .buf  = val,  .len   = 1 } };
	return (i2c_transfer(adapter, msgs, 1) == 1) ? 0 : -1;
}

static int i2c_read_regs(struct i2c_adapter *adapter,
			 u8 adr, u8 reg, u8 *val, u8 len)
{
	struct i2c_msg msgs[2] = {{.addr = adr,  .flags = 0,
				   .buf  = &reg, .len   = 1},
				  {.addr = adr,  .flags = I2C_M_RD,
				   .buf  = val,  .len   = len } };
	return (i2c_transfer(adapter, msgs, 2) == 2) ? 0 : -1;
}

static int i2c_read_regs16(struct i2c_adapter *adapter,
			   u8 adr, u16 reg, u8 *val, u8 len)
{
	u8 reg16[2] = { reg >> 8, reg };
	struct i2c_msg msgs[2] = {{.addr = adr,  .flags = 0,
				   .buf  = (u8 *)&reg16, .len   = 2},
				  {.addr = adr,  .flags = I2C_M_RD,
				   .buf  = val,  .len   = len } };
	return (i2c_transfer(adapter, msgs, 2) == 2) ? 0 : -1;
}

static int i2c_read_reg(struct i2c_adapter *adapter, u8 adr, u8 reg, u8 *val)
{
	struct i2c_msg msgs[2] = {{.addr = adr,  .flags = 0,
				   .buf  = &reg, .len   = 1},
				  {.addr = adr,  .flags = I2C_M_RD,
				   .buf  = val,  .len   = 1 } };
	return (i2c_transfer(adapter, msgs, 2) == 2) ? 0 : -1;
}

static int i2c_read_reg16(struct i2c_adapter *adapter, u8 adr,
			  u16 reg, u8 *val)
{
	u8 msg[2] = {reg >> 8, reg & 0xff};
	struct i2c_msg msgs[2] = {{.addr = adr, .flags = 0,
				   .buf  = msg, .len   = 2},
				  {.addr = adr, .flags = I2C_M_RD,
				   .buf  = val, .len   = 1 } };
	return (i2c_transfer(adapter, msgs, 2) == 2) ? 0 : -1;
}

static int i2c_write_reg16(struct i2c_adapter *adap, u8 adr,
			   u16 reg, u8 val)
{
	u8 msg[3] = {reg >> 8, reg & 0xff, val};

	return i2c_write(adap, adr, msg, 3);
}

static int ddb_i2c_cmd(struct ddb_i2c *i2c, u32 adr, u32 cmd)
{
	struct ddb *dev = i2c->dev;
	int stat;
	u32 val;

	i2c->done = 0;
	ddbwritel(dev, (adr << 9) | cmd, i2c->regs + I2C_COMMAND);
	stat = wait_event_timeout(i2c->wq, i2c->done == 1, HZ);
	if (stat <= 0) {
		printk(KERN_ERR "I2C timeout\n");
		{ /* MSI debugging*/
			u32 istat = ddbreadl(dev, INTERRUPT_STATUS);
			printk(KERN_ERR "IRS %08x\n", istat);
			ddbwritel(dev, istat, INTERRUPT_ACK);
		}
		return -EIO;
	}
	val = ddbreadl(dev, i2c->regs+I2C_COMMAND);
	if (val & 0x70000)
		return -EIO;
	return 0;
}

static int ddb_i2c_master_xfer(struct i2c_adapter *adapter,
			       struct i2c_msg msg[], int num)
{
	struct ddb_i2c *i2c = (struct ddb_i2c *) i2c_get_adapdata(adapter);
	struct ddb *dev = i2c->dev;
	u8 addr = 0;

	if (num)
		addr = msg[0].addr;

	if (num == 2 && msg[1].flags & I2C_M_RD &&
	    !(msg[0].flags & I2C_M_RD)) {
		memcpy_toio(dev->regs + I2C_TASKMEM_BASE + i2c->wbuf,
			    msg[0].buf, msg[0].len);
		ddbwritel(dev, msg[0].len|(msg[1].len << 16),
			  i2c->regs + I2C_TASKLENGTH);
		if (!ddb_i2c_cmd(i2c, addr, 1)) {
			memcpy_fromio(msg[1].buf,
				      dev->regs + I2C_TASKMEM_BASE + i2c->rbuf,
				      msg[1].len);
			return num;
		}
	}
	if (num == 1 && !(msg[0].flags & I2C_M_RD)) {
		ddbcpyto(dev, I2C_TASKMEM_BASE + i2c->wbuf, msg[0].buf, msg[0].len);
		ddbwritel(dev, msg[0].len, i2c->regs + I2C_TASKLENGTH);
		if (!ddb_i2c_cmd(i2c, addr, 2))
			return num;
	}
	if (num == 1 && (msg[0].flags & I2C_M_RD)) {
		ddbwritel(dev, msg[0].len << 16, i2c->regs + I2C_TASKLENGTH);
		if (!ddb_i2c_cmd(i2c, addr, 3)) {
			ddbcpyfrom(dev, msg[0].buf,
				   I2C_TASKMEM_BASE + i2c->rbuf, msg[0].len);
			return num;
		}
	}
	return -EIO;
}


static u32 ddb_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_EMUL;
}

struct i2c_algorithm ddb_i2c_algo = {
	.master_xfer   = ddb_i2c_master_xfer,
	.functionality = ddb_i2c_functionality,
};

static void ddb_i2c_release(struct ddb *dev)
{
	int i;
	struct ddb_i2c *i2c;
	struct i2c_adapter *adap;

	for (i = 0; i < dev->info->i2c_num; i++) {
		i2c = &dev->i2c[i];
		adap = &i2c->adap;
		i2c_del_adapter(adap);
	}
}

static int ddb_i2c_init(struct ddb *dev)
{
	int i, j, stat = 0;
	struct ddb_i2c *i2c;
	struct i2c_adapter *adap;

	for (i = 0; i < dev->info->i2c_num; i++) {
		i2c = &dev->i2c[i];
		i2c->dev = dev;
		i2c->nr = i;
		i2c->wbuf = i * (I2C_TASKMEM_SIZE / 4);
		i2c->rbuf = i2c->wbuf + (I2C_TASKMEM_SIZE / 8);
		i2c->regs = 0x80 + i * 0x20;
		ddbwritel(dev, I2C_SPEED_100, i2c->regs + I2C_TIMING);
		ddbwritel(dev, (i2c->rbuf << 16) | i2c->wbuf,
			  i2c->regs + I2C_TASKADDRESS);
		init_waitqueue_head(&i2c->wq);

		adap = &i2c->adap;
		i2c_set_adapdata(adap, i2c);
#ifdef I2C_ADAP_CLASS_TV_DIGITAL
		adap->class = I2C_ADAP_CLASS_TV_DIGITAL|I2C_CLASS_TV_ANALOG;
#else
#ifdef I2C_CLASS_TV_ANALOG
		adap->class = I2C_CLASS_TV_ANALOG;
#endif
#endif
		strcpy(adap->name, "ddbridge");
		adap->algo = &ddb_i2c_algo;
		adap->algo_data = (void *)i2c;
		adap->dev.parent = &dev->pdev->dev;
		stat = i2c_add_adapter(adap);
		if (stat)
			break;
	}
	if (stat)
		for (j = 0; j < i; j++) {
			i2c = &dev->i2c[j];
			adap = &i2c->adap;
			i2c_del_adapter(adap);
		}
	return stat;
}


/******************************************************************************/
/******************************************************************************/
/******************************************************************************/

static void ddb_set_dma_table(struct ddb *dev, struct ddb_dma *dma)
{
	u32 i, base;
	u64 mem;

	if (!dma)
		return;
	base = DMA_BASE_ADDRESS_TABLE + dma->nr * 0x100;
	for (i = 0; i < dma->num; i++) {
		mem = dma->pbuf[i];
		ddbwritel(dev, mem & 0xffffffff, base + i * 8);
		ddbwritel(dev, mem >> 32, base + i * 8 + 4);
	}
	dma->bufreg = (dma->div << 16) |
		((dma->num & 0x1f) << 11) |
		((dma->size >> 7) & 0x7ff);
}

static void ddb_set_dma_tables(struct ddb *dev)
{
	u32 i;

	for (i = 0; i < dev->info->port_num * 2; i++)
		ddb_set_dma_table(dev, dev->input[i].dma);
	for (i = 0; i < dev->info->port_num; i++)
		ddb_set_dma_table(dev, dev->output[i].dma);
}

static void dma_free(struct pci_dev *pdev, struct ddb_dma *dma)
{
	int i;

	if (!dma)
		return;
	for (i = 0; i < dma->num; i++) {
		if (dma->vbuf[i]) {
			pci_free_consistent(pdev, dma->size,
					    dma->vbuf[i], dma->pbuf[i]);
			dma->vbuf[i] = 0;
		}
	}
}

static void ddb_redirect_dma(struct ddb *dev,
			     struct ddb_dma *sdma,
			     struct ddb_dma *ddma)
{
	u32 i, base;
	u64 mem;

	sdma->bufreg = ddma->bufreg;
	base = DMA_BASE_ADDRESS_TABLE + sdma->nr * 0x100;
	for (i = 0; i < ddma->num; i++) {
		mem = ddma->pbuf[i];
		ddbwritel(dev, mem & 0xffffffff, base + i * 8);
		ddbwritel(dev, mem >> 32, base + i * 8 + 4);
	}
}

static void ddb_unredirect(struct ddb_port *port)
{
	struct ddb_input *ored, *ired;

	ored = port->output->redirect;
	ired = port->input[0]->redirect;

	if (!ored || !ired)
		return;
	if (ired->port->output->redirect == port->input[0]) {
		ired->port->output->redirect = ored;
		ddb_set_dma_table(port->dev, port->input[0]->dma);
		ddb_redirect_dma(ored->port->dev, ored->dma, ired->port->output->dma);
	} else
		ddb_set_dma_table(ored->port->dev, ored->dma);
	ored->redirect = ired;
	port->input[0]->redirect = 0;
	port->output->redirect = 0;
}

static int dma_alloc(struct pci_dev *pdev, struct ddb_dma *dma)
{
	int i;

	if (!dma)
		return 0;
	for (i = 0; i < dma->num; i++) {
		dma->vbuf[i] = pci_alloc_consistent(pdev, dma->size, &dma->pbuf[i]);
		if (!dma->vbuf[i])
			return -ENOMEM;
	}
	return 0;
}

static int ddb_buffers_alloc(struct ddb *dev)
{
	int i;
	struct ddb_port *port;

	for (i = 0; i < dev->info->port_num; i++) {
		port = &dev->port[i];
		switch (port->class) {
		case DDB_PORT_TUNER:
			if (dma_alloc(dev->pdev, port->input[0]->dma) < 0)
				return -1;
			if (dma_alloc(dev->pdev, port->input[1]->dma) < 0)
				return -1;
			break;
		case DDB_PORT_CI:
		case DDB_PORT_LOOP:
			if (dma_alloc(dev->pdev, port->input[0]->dma) < 0)
				return -1;
			if (dma_alloc(dev->pdev, port->output->dma) < 0)
				return -1;
			break;
		default:
			break;
		}
	}
	ddb_set_dma_tables(dev);
	return 0;
}

static void ddb_buffers_free(struct ddb *dev)
{
	int i;
	struct ddb_port *port;

	for (i = 0; i < dev->info->port_num; i++) {
		port = &dev->port[i];

		ddb_unredirect(port);
		dma_free(dev->pdev, port->input[0]->dma);
		dma_free(dev->pdev, port->input[1]->dma);
		dma_free(dev->pdev, port->output->dma);
	}
}

static void ddb_input_start(struct ddb_input *input)
{
	struct ddb *dev = input->port->dev;

	spin_lock_irq(&input->dma->lock);
	input->dma->cbuf = 0;
	input->dma->coff = 0;

	/* reset */
	ddbwritel(dev, 0, TS_INPUT_CONTROL(input->nr));
	ddbwritel(dev, 2, TS_INPUT_CONTROL(input->nr));
	ddbwritel(dev, 0, TS_INPUT_CONTROL(input->nr));

	ddbwritel(dev, input->dma->bufreg, DMA_BUFFER_SIZE(input->dma->nr));
	ddbwritel(dev, 0, DMA_BUFFER_ACK(input->dma->nr));

	ddbwritel(dev, 1, DMA_BASE_WRITE);
	ddbwritel(dev, 3, DMA_BUFFER_CONTROL(input->dma->nr));
	ddbwritel(dev, 9, TS_INPUT_CONTROL(input->nr));
	input->dma->running = 1;
	spin_unlock_irq(&input->dma->lock);
	/* printk(KERN_INFO "input_start %d\n", input->nr); */
}

static void ddb_input_stop(struct ddb_input *input)
{
	struct ddb *dev = input->port->dev;

	spin_lock_irq(&input->dma->lock);
	ddbwritel(dev, 0, TS_INPUT_CONTROL(input->nr));
	ddbwritel(dev, 0, DMA_BUFFER_CONTROL(input->dma->nr));
	input->dma->running = 0;
	spin_unlock_irq(&input->dma->lock);
}

static void ddb_output_start(struct ddb_output *output)
{
	struct ddb *dev = output->port->dev;

	spin_lock_irq(&output->dma->lock);
	output->dma->cbuf = 0;
	output->dma->coff = 0;
	ddbwritel(dev, 0, TS_OUTPUT_CONTROL(output->nr));
	ddbwritel(dev, 2, TS_OUTPUT_CONTROL(output->nr));
	ddbwritel(dev, 0, TS_OUTPUT_CONTROL(output->nr));
	ddbwritel(dev, 0x3c, TS_OUTPUT_CONTROL(output->nr));
	ddbwritel(dev, output->dma->bufreg, DMA_BUFFER_SIZE(output->dma->nr));
	ddbwritel(dev, 0, DMA_BUFFER_ACK(output->dma->nr));

	ddbwritel(dev, 1, DMA_BASE_READ);
	ddbwritel(dev, 3, DMA_BUFFER_CONTROL(output->dma->nr));
	if (output->port->input[0]->port->class == DDB_PORT_LOOP)
		ddbwritel(dev, 0x05, TS_OUTPUT_CONTROL(output->nr));
	else
		ddbwritel(dev, 0x1d, TS_OUTPUT_CONTROL(output->nr));
	output->dma->running = 1;
	spin_unlock_irq(&output->dma->lock);
	/* printk(KERN_INFO "output_start %d\n", output->nr); */
}

#if 0
static void ddb_input_start_all(struct ddb_input *input)
{
	struct ddb_input *next;

	ddb_input_start(input);
	while ((next = input->redirect) &&
	       next != input) {
		ddb_input_start(next);
		ddb_output_start(next->port->output);
	}
}
#endif

static void ddb_output_stop(struct ddb_output *output)
{
	struct ddb *dev = output->port->dev;

	spin_lock_irq(&output->dma->lock);
	ddbwritel(dev, 0, TS_OUTPUT_CONTROL(output->nr));
	ddbwritel(dev, 0, DMA_BUFFER_CONTROL(output->dma->nr));
	output->dma->running = 0;
	spin_unlock_irq(&output->dma->lock);
}

#if 0
static void ddb_input_stop_all(struct ddb_input *input)
{
	struct ddb_input *next;

	ddb_input_stop(input);
	while ((next = input->redirect) &&
	       next != input) {
		ddb_input_stop(next);
		ddb_output_stop(next->port->output);
	}
}
#endif

static u32 ddb_output_free(struct ddb_output *output)
{
	u32 idx, off, stat = output->dma->stat;
	s32 diff;

	idx = (stat >> 11) & 0x1f;
	off = (stat & 0x7ff) << 7;

	if (output->dma->cbuf != idx) {
		if ((((output->dma->cbuf + 1) % output->dma->num) == idx) &&
		    (output->dma->size - output->dma->coff <= 188))
			return 0;
		return 188;
	}
	diff = off - output->dma->coff;
	if (diff <= 0 || diff > 188)
		return 188;
	return 0;
}

static ssize_t ddb_output_write(struct ddb_output *output,
				const u8 *buf, size_t count)
{
	struct ddb *dev = output->port->dev;
	u32 idx, off, stat = output->dma->stat;
	u32 left = count, len;

	idx = (stat >> 11) & 0x1f;
	off = (stat & 0x7ff) << 7;

	while (left) {
		len = output->dma->size - output->dma->coff;
		if ((((output->dma->cbuf + 1) % output->dma->num) == idx) &&
		    (off == 0)) {
			if (len <= 188)
				break;
			len -= 188;
		}
		if (output->dma->cbuf == idx) {
			if (off > output->dma->coff) {
#if 1
				len = off - output->dma->coff;
				len -= (len % 188);
				if (len <= 188)

#endif
					break;
				len -= 188;
			}
		}
		if (len > left)
			len = left;
		if (copy_from_user(output->dma->vbuf[output->dma->cbuf] +
				   output->dma->coff,
				   buf, len))
			return -EIO;
		/* printk("cfu %d  %d %d\n", len, output->cbuf, output->coff); */
		left -= len;
		buf += len;
		output->dma->coff += len;
		if (output->dma->coff == output->dma->size) {
			output->dma->coff = 0;
			output->dma->cbuf = ((output->dma->cbuf + 1) %
					     output->dma->num);
		}
		ddbwritel(dev, (output->dma->cbuf << 11) | (output->dma->coff >> 7),
			  DMA_BUFFER_ACK(output->dma->nr));
	}
	return count - left;
}

#if 0
static u32 ddb_input_free_bytes(struct ddb_input *input)
{
	struct ddb *dev = input->port->dev;
	u32 idx, off, stat = input->dma->stat;
	u32 ctrl = ddbreadl(dev, DMA_BUFFER_CONTROL(input->dma->nr));

	idx = (stat >> 11) & 0x1f;
	off = (stat & 0x7ff) << 7;

	if (ctrl & 4)
		return 0;
	if (input->dma->cbuf != idx)
		return 1;
	return 0;
}

static s32 ddb_output_used_bufs(struct ddb_output *output)
{
	u32 idx, off, stat, ctrl;
	s32 diff;

	spin_lock_irq(&output->dma->lock);
	stat = output->dma->stat;
	ctrl = output->dma->ctrl;
	spin_unlock_irq(&output->dma->lock);

	idx = (stat >> 11) & 0x1f;
	off = (stat & 0x7ff) << 7;

	if (ctrl & 4)
		return 0;
	diff = output->dma->cbuf - idx;
	if (diff == 0 && off < output->dma->coff)
		return 0;
	if (diff <= 0)
		diff += output->dma->num;
	return diff;
}

static s32 ddb_input_free_bufs(struct ddb_input *input)
{
	u32 idx, off, stat, ctrl;
	s32 free;

	spin_lock_irq(&input->dma->lock);
	ctrl = input->dma->ctrl;
	stat = input->dma->stat;
	spin_unlock_irq(&input->dma->lock);
	if (ctrl & 4)
		return 0;
	idx = (stat >> 11) & 0x1f;
	off = (stat & 0x7ff) << 7;
	free = input->dma->cbuf - idx;
	if (free == 0 && off < input->dma->coff)
		return 0;
	if (free <= 0)
		free += input->dma->num;
	return free - 1;
}

static u32 ddb_output_ok(struct ddb_output *output)
{
	struct ddb_input *input = output->port->input[0];
	s32 diff;

	diff = ddb_input_free_bufs(input) - ddb_output_used_bufs(output);
	if (diff > 0)
		return 1;
	return 0;
}
#endif

static u32 ddb_input_avail(struct ddb_input *input)
{
	struct ddb *dev = input->port->dev;
	u32 idx, off, stat = input->dma->stat;
	u32 ctrl = ddbreadl(dev, DMA_BUFFER_CONTROL(input->dma->nr));

	idx = (stat >> 11) & 0x1f;
	off = (stat & 0x7ff) << 7;

	if (ctrl & 4) {
		printk(KERN_ERR "IA %d %d %08x\n", idx, off, ctrl);
		ddbwritel(dev, stat, DMA_BUFFER_ACK(input->dma->nr));
		return 0;
	}
	if (input->dma->cbuf != idx || off < input->dma->coff)
		return 188;
	return 0;
}

static size_t ddb_input_read(struct ddb_input *input, u8 *buf, size_t count)
{
	struct ddb *dev = input->port->dev;
	u32 left = count;
	u32 idx, off, free, stat = input->dma->stat;
	int ret;

	idx = (stat >> 11) & 0x1f;
	off = (stat & 0x7ff) << 7;

	while (left) {
		if (input->dma->cbuf == idx)
			return count - left;
		free = input->dma->size - input->dma->coff;
		if (free > left)
			free = left;
		ret = copy_to_user(buf, input->dma->vbuf[input->dma->cbuf] +
				   input->dma->coff, free);
		if (ret)
			return -EFAULT;
		input->dma->coff += free;
		if (input->dma->coff == input->dma->size) {
			input->dma->coff = 0;
			input->dma->cbuf = (input->dma->cbuf+1) %
				input->dma->num;
		}
		left -= free;
		ddbwritel(dev, (input->dma->cbuf << 11) | (input->dma->coff >> 7),
			  DMA_BUFFER_ACK(input->dma->nr));
	}
	return count;
}

/******************************************************************************/
/******************************************************************************/
/******************************************************************************/

#if 0
static struct ddb_input *fe2input(struct ddb *dev, struct dvb_frontend *fe)
{
	int i;

	for (i = 0; i < dev->info->port_num * 2; i++) {
		if (dev->input[i].fe == fe)
			return &dev->input[i];
	}
	return NULL;
}
#endif

static int locked_gate_ctrl(struct dvb_frontend *fe, int enable)
{
	struct ddb_input *input = fe->sec_priv;
	struct ddb_port *port = input->port;
	int status;

	if (enable) {
		mutex_lock(&port->i2c_gate_lock);
		status = input->dvb.gate_ctrl(fe, 1);
	} else {
		status = input->dvb.gate_ctrl(fe, 0);
		mutex_unlock(&port->i2c_gate_lock);
	}
	return status;
}

static int demod_attach_drxk(struct ddb_input *input)
{
	struct i2c_adapter *i2c = &input->port->i2c->adap;
	struct dvb_frontend *fe;
	struct drxk_config config;

	memset(&config, 0, sizeof(config));
	config.adr = 0x29 + (input->nr & 1);
	config.microcode_name = "drxk_a3.mc";

#ifdef USE_API3
	fe = input->dvb.fe = dvb_attach(drxk_attach, &config, i2c, &input->dvb.fe2);
#else
	fe = input->dvb.fe = dvb_attach(drxk_attach, &config, i2c);
#endif
	if (!input->dvb.fe) {
		printk(KERN_ERR "No DRXK found!\n");
		return -ENODEV;
	}
	fe->sec_priv = input;
	input->dvb.gate_ctrl = fe->ops.i2c_gate_ctrl;
	fe->ops.i2c_gate_ctrl = locked_gate_ctrl;
	return 0;
}

#if 0
struct stv0367_config stv0367_0 = {
	.demod_address = 0x1f,
	.xtal = 27000000,
	.if_khz = 5000,
	.if_iq_mode = FE_TER_NORMAL_IF_TUNER,
	.ts_mode = STV0367_SERIAL_PUNCT_CLOCK,
	.clk_pol = STV0367_RISINGEDGE_CLOCK,
};

struct stv0367_config stv0367_1 = {
	.demod_address = 0x1e,
	.xtal = 27000000,
	.if_khz = 5000,
	.if_iq_mode = FE_TER_NORMAL_IF_TUNER,
	.ts_mode = STV0367_SERIAL_PUNCT_CLOCK,
	.clk_pol = STV0367_RISINGEDGE_CLOCK,
};


static int demod_attach_stv0367(struct ddb_input *input)
{
	struct i2c_adapter *i2c = &input->port->i2c->adap;
	struct dvb_frontend *fe;

	fe = input->dvb.fe = dvb_attach(stv0367ter_attach,
				    (input->nr & 1) ? &stv0367_1 : &stv0367_0,
				    i2c);
	if (!input->dvb.fe) {
		printk(KERN_ERR "No stv0367 found!\n");
		return -ENODEV;
	}
	fe->sec_priv = input;
	input->dvb.gate_ctrl = fe->ops.i2c_gate_ctrl;
	fe->ops.i2c_gate_ctrl = locked_gate_ctrl;
	return 0;
}
#endif

struct stv0367_cfg stv0367dd_0 = {
	.adr = 0x1f,
	.xtal = 27000000,
};

struct stv0367_cfg stv0367dd_1 = {
	.adr = 0x1e,
	.xtal = 27000000,
};

static int demod_attach_stv0367dd(struct ddb_input *input)
{
	struct i2c_adapter *i2c = &input->port->i2c->adap;
	struct dvb_frontend *fe;

	fe = input->dvb.fe = dvb_attach(stv0367_attach, i2c,
				    (input->nr & 1) ? &stv0367dd_1 : &stv0367dd_0,
				    &input->dvb.fe2);
	if (!input->dvb.fe) {
		printk(KERN_ERR "No stv0367 found!\n");
		return -ENODEV;
	}
	fe->sec_priv = input;
	input->dvb.gate_ctrl = fe->ops.i2c_gate_ctrl;
	fe->ops.i2c_gate_ctrl = locked_gate_ctrl;
	return 0;
}

static int tuner_attach_tda18271(struct ddb_input *input)
{
	struct i2c_adapter *i2c = &input->port->i2c->adap;
	struct dvb_frontend *fe;

	if (input->dvb.fe->ops.i2c_gate_ctrl)
		input->dvb.fe->ops.i2c_gate_ctrl(input->dvb.fe, 1);
	fe = dvb_attach(tda18271c2dd_attach, input->dvb.fe, i2c, 0x60);
	if (input->dvb.fe->ops.i2c_gate_ctrl)
		input->dvb.fe->ops.i2c_gate_ctrl(input->dvb.fe, 0);
	if (!fe) {
		printk(KERN_ERR "No TDA18271 found!\n");
		return -ENODEV;
	}
	return 0;
}

static int tuner_attach_tda18212dd(struct ddb_input *input)
{
	struct i2c_adapter *i2c = &input->port->i2c->adap;
	struct dvb_frontend *fe;

	fe = dvb_attach(tda18212dd_attach, input->dvb.fe, i2c,
			(input->nr & 1) ? 0x63 : 0x60);
	if (!fe) {
		printk(KERN_ERR "No TDA18212 found!\n");
		return -ENODEV;
	}
	return 0;
}

#if 0
struct tda18212_config tda18212_0 = {
	.i2c_address = 0x60,
};

struct tda18212_config tda18212_1 = {
	.i2c_address = 0x63,
};

static int tuner_attach_tda18212(struct ddb_input *input)
{
	struct i2c_adapter *i2c = &input->port->i2c->adap;
	struct dvb_frontend *fe;
	struct tda18212_config *cfg;

	cfg = (input->nr & 1) ? &tda18212_1 : &tda18212_0;
	fe = dvb_attach(tda18212_attach, input->dvb.fe, i2c, cfg);
	if (!fe) {
		printk(KERN_ERR "No TDA18212 found!\n");
		return -ENODEV;
	}
	return 0;
}
#endif

/******************************************************************************/
/******************************************************************************/
/******************************************************************************/

static struct stv090x_config stv0900 = {
	.device         = STV0900,
	.demod_mode     = STV090x_DUAL,
	.clk_mode       = STV090x_CLK_EXT,

	.xtal           = 27000000,
	.address        = 0x69,

	.ts1_mode       = STV090x_TSMODE_SERIAL_PUNCTURED,
	.ts2_mode       = STV090x_TSMODE_SERIAL_PUNCTURED,

	.repeater_level = STV090x_RPTLEVEL_16,

	.adc1_range	= STV090x_ADC_1Vpp,
	.adc2_range	= STV090x_ADC_1Vpp,

	.diseqc_envelope_mode = true,
};

static struct stv090x_config stv0900_aa = {
	.device         = STV0900,
	.demod_mode     = STV090x_DUAL,
	.clk_mode       = STV090x_CLK_EXT,

	.xtal           = 27000000,
	.address        = 0x68,

	.ts1_mode       = STV090x_TSMODE_SERIAL_PUNCTURED,
	.ts2_mode       = STV090x_TSMODE_SERIAL_PUNCTURED,

	.repeater_level = STV090x_RPTLEVEL_16,

	.adc1_range	= STV090x_ADC_1Vpp,
	.adc2_range	= STV090x_ADC_1Vpp,

	.diseqc_envelope_mode = true,
};

static struct stv6110x_config stv6110a = {
	.addr    = 0x60,
	.refclk	 = 27000000,
	.clk_div = 1,
};

static struct stv6110x_config stv6110b = {
	.addr    = 0x63,
	.refclk	 = 27000000,
	.clk_div = 1,
};

static int demod_attach_stv0900(struct ddb_input *input, int type)
{
	struct i2c_adapter *i2c = &input->port->i2c->adap;
	struct stv090x_config *feconf = type ? &stv0900_aa : &stv0900;

	input->dvb.fe = dvb_attach(stv090x_attach, feconf, i2c,
			       (input->nr & 1) ? STV090x_DEMODULATOR_1
			       : STV090x_DEMODULATOR_0);
	if (!input->dvb.fe) {
		printk(KERN_ERR "No STV0900 found!\n");
		return -ENODEV;
	}
	if (!dvb_attach(lnbh24_attach, input->dvb.fe, i2c, 0,
			0, (input->nr & 1) ?
			(0x09 - type) : (0x0b - type))) {
		printk(KERN_ERR "No LNBH24 found!\n");
		return -ENODEV;
	}
	return 0;
}

static int tuner_attach_stv6110(struct ddb_input *input, int type)
{
	struct i2c_adapter *i2c = &input->port->i2c->adap;
	struct stv090x_config *feconf = type ? &stv0900_aa : &stv0900;
	struct stv6110x_config *tunerconf = (input->nr & 1) ?
		&stv6110b : &stv6110a;
	struct stv6110x_devctl *ctl;

	ctl = dvb_attach(stv6110x_attach, input->dvb.fe, tunerconf, i2c);
	if (!ctl) {
		printk(KERN_ERR "No STV6110X found!\n");
		return -ENODEV;
	}
	printk(KERN_INFO "attach tuner input %d adr %02x\n",
			 input->nr, tunerconf->addr);

	feconf->tuner_init          = ctl->tuner_init;
	feconf->tuner_sleep         = ctl->tuner_sleep;
	feconf->tuner_set_mode      = ctl->tuner_set_mode;
	feconf->tuner_set_frequency = ctl->tuner_set_frequency;
	feconf->tuner_get_frequency = ctl->tuner_get_frequency;
	feconf->tuner_set_bandwidth = ctl->tuner_set_bandwidth;
	feconf->tuner_get_bandwidth = ctl->tuner_get_bandwidth;
	feconf->tuner_set_bbgain    = ctl->tuner_set_bbgain;
	feconf->tuner_get_bbgain    = ctl->tuner_get_bbgain;
	feconf->tuner_set_refclk    = ctl->tuner_set_refclk;
	feconf->tuner_get_status    = ctl->tuner_get_status;

	return 0;
}

static int my_dvb_dmx_ts_card_init(struct dvb_demux *dvbdemux, char *id,
			    int (*start_feed)(struct dvb_demux_feed *),
			    int (*stop_feed)(struct dvb_demux_feed *),
			    void *priv)
{
	dvbdemux->priv = priv;

	dvbdemux->filternum = 256;
	dvbdemux->feednum = 256;
	dvbdemux->start_feed = start_feed;
	dvbdemux->stop_feed = stop_feed;
	dvbdemux->write_to_decoder = NULL;
	dvbdemux->dmx.capabilities = (DMX_TS_FILTERING |
				      DMX_SECTION_FILTERING |
				      DMX_MEMORY_BASED_FILTERING);
	return dvb_dmx_init(dvbdemux);
}

static int my_dvb_dmxdev_ts_card_init(struct dmxdev *dmxdev,
			       struct dvb_demux *dvbdemux,
			       struct dmx_frontend *hw_frontend,
			       struct dmx_frontend *mem_frontend,
			       struct dvb_adapter *dvb_adapter)
{
	int ret;

	dmxdev->filternum = 256;
	dmxdev->demux = &dvbdemux->dmx;
	dmxdev->capabilities = 0;
	ret = dvb_dmxdev_init(dmxdev, dvb_adapter);
	if (ret < 0)
		return ret;

	hw_frontend->source = DMX_FRONTEND_0;
	dvbdemux->dmx.add_frontend(&dvbdemux->dmx, hw_frontend);
	mem_frontend->source = DMX_MEMORY_FE;
	dvbdemux->dmx.add_frontend(&dvbdemux->dmx, mem_frontend);
	return dvbdemux->dmx.connect_frontend(&dvbdemux->dmx, hw_frontend);
}

static int start_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct dvb_demux *dvbdmx = dvbdmxfeed->demux;
	struct ddb_input *input = dvbdmx->priv;

	if (!input->dvb.users)
		ddb_input_start(input);

	return ++input->dvb.users;
}

static int stop_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct dvb_demux *dvbdmx = dvbdmxfeed->demux;
	struct ddb_input *input = dvbdmx->priv;

	if (--input->dvb.users)
		return input->dvb.users;

	ddb_input_stop(input);
	return 0;
}


static void dvb_input_detach(struct ddb_input *input)
{
	struct dvb_demux *dvbdemux = &input->dvb.demux;

	switch (input->dvb.attached) {
	case 6:
		if (input->dvb.fe2)
			dvb_unregister_frontend(input->dvb.fe2);
		if (input->dvb.fe)
			dvb_unregister_frontend(input->dvb.fe);
	case 5:
		dvb_frontend_detach(input->dvb.fe);
		input->dvb.fe = NULL;
	case 4:
		dvb_net_release(&input->dvb.dvbnet);
	case 3:
		dvbdemux->dmx.close(&dvbdemux->dmx);
		dvbdemux->dmx.remove_frontend(&dvbdemux->dmx,
					      &input->dvb.hw_frontend);
		dvbdemux->dmx.remove_frontend(&dvbdemux->dmx,
					      &input->dvb.mem_frontend);
		dvb_dmxdev_release(&input->dvb.dmxdev);
	case 2:
		dvb_dmx_release(&input->dvb.demux);
	case 1:
		break;
	}
	input->dvb.attached = 0;
}

static int dvb_register_adapters(struct ddb *dev)
{
	int i, ret = 0;
	struct ddb_port *port;
	struct dvb_adapter *adap;

	if (adapter_alloc == 3) {
		port = &dev->port[0];
		adap = port->input[0]->dvb.adap;
		ret = dvb_register_adapter(adap, "DDBridge", THIS_MODULE,
					   &port->dev->pdev->dev,
					   adapter_nr);
		if (ret < 0)
			return ret;
		port->input[0]->dvb.adap_registered = 1;
		for (i = 0; i < dev->info->port_num; i++) {
			port = &dev->port[i];
			port->input[0]->dvb.adap = adap;
			port->input[1]->dvb.adap = adap;
		}
		return 0;
	}

	for (i = 0; i < dev->info->port_num; i++) {
		port = &dev->port[i];
		switch (port->class) {
		case DDB_PORT_TUNER:
			adap = port->input[0]->dvb.adap;
			ret = dvb_register_adapter(adap, "DDBridge", THIS_MODULE,
						   &port->dev->pdev->dev,
						   adapter_nr);
			if (ret < 0)
				return ret;
			port->input[0]->dvb.adap_registered = 1;

			if (adapter_alloc > 0) {
				port->input[1]->dvb.adap = port->input[0]->dvb.adap;
				break;
			}
			adap = port->input[1]->dvb.adap;
			ret = dvb_register_adapter(adap, "DDBridge", THIS_MODULE,
						   &port->dev->pdev->dev,
						   adapter_nr);
			if (ret < 0)
				return ret;
			port->input[1]->dvb.adap_registered = 1;
			break;

		case DDB_PORT_CI:
		case DDB_PORT_LOOP:
			adap = port->input[0]->dvb.adap;
			ret = dvb_register_adapter(adap, "DDBridge", THIS_MODULE,
						   &port->dev->pdev->dev,
						   adapter_nr);
			if (ret < 0)
				return ret;
			port->input[0]->dvb.adap_registered = 1;
			break;
		default:
			if (adapter_alloc < 2)
				break;
			adap = port->input[0]->dvb.adap;
			ret = dvb_register_adapter(adap, "DDBridge", THIS_MODULE,
						   &port->dev->pdev->dev,
						   adapter_nr);
			if (ret < 0)
				return ret;
			port->input[0]->dvb.adap_registered = 1;
			break;
		}
	}
	return ret;
}

static void dvb_unregister_adapters(struct ddb *dev)
{
	int i;
	struct ddb_port *port;
	struct ddb_input *input;

	for (i = 0; i < dev->info->port_num; i++) {
		port = &dev->port[i];

		input = port->input[0];
		if (input->dvb.adap_registered)
			dvb_unregister_adapter(input->dvb.adap);
		input->dvb.adap_registered = 0;

		input = port->input[1];
		if (input->dvb.adap_registered)
			dvb_unregister_adapter(input->dvb.adap);
		input->dvb.adap_registered = 0;
	}
}


static int dvb_input_attach(struct ddb_input *input)
{
	int ret = 0;
	struct ddb_port *port = input->port;
	struct dvb_adapter *adap = input->dvb.adap;
	struct dvb_demux *dvbdemux = &input->dvb.demux;

	input->dvb.attached = 1;

	ret = my_dvb_dmx_ts_card_init(dvbdemux, "SW demux",
				      start_feed,
				      stop_feed, input);
	if (ret < 0)
		return ret;
	input->dvb.attached = 2;

	ret = my_dvb_dmxdev_ts_card_init(&input->dvb.dmxdev,
					 &input->dvb.demux,
					 &input->dvb.hw_frontend,
					 &input->dvb.mem_frontend, adap);
	if (ret < 0)
		return ret;
	input->dvb.attached = 3;

	ret = dvb_net_init(adap, &input->dvb.dvbnet, input->dvb.dmxdev.demux);
	if (ret < 0)
		return ret;
	input->dvb.attached = 4;

	input->dvb.fe = 0;
	switch (port->type) {
	case DDB_TUNER_DVBS_ST:
		if (demod_attach_stv0900(input, 0) < 0)
			return -ENODEV;
		if (tuner_attach_stv6110(input, 0) < 0)
			return -ENODEV;
		break;
	case DDB_TUNER_DVBS_ST_AA:
		if (demod_attach_stv0900(input, 1) < 0)
			return -ENODEV;
		if (tuner_attach_stv6110(input, 1) < 0)
			return -ENODEV;
		break;
	case DDB_TUNER_DVBCT_TR:
		if (demod_attach_drxk(input) < 0)
			return -ENODEV;
		if (tuner_attach_tda18271(input) < 0)
			return -ENODEV;
		break;
	case DDB_TUNER_DVBCT_ST:
		if (demod_attach_stv0367dd(input) < 0)
			return -ENODEV;
		if (tuner_attach_tda18212dd(input) < 0)
			return -ENODEV;
		break;
	}
	input->dvb.attached = 5;
	if (input->dvb.fe) {
		if (dvb_register_frontend(adap, input->dvb.fe) < 0)
			return -ENODEV;
	}
	if (input->dvb.fe2) {
		if (dvb_register_frontend(adap, input->dvb.fe2) < 0)
			return -ENODEV;
		input->dvb.fe2->tuner_priv = input->dvb.fe->tuner_priv;
		memcpy(&input->dvb.fe2->ops.tuner_ops,
		       &input->dvb.fe->ops.tuner_ops,
		       sizeof(struct dvb_tuner_ops));
	}
	input->dvb.attached = 6;
	return 0;
}

/****************************************************************************/
/****************************************************************************/

static ssize_t ts_write(struct file *file, const char *buf,
			size_t count, loff_t *ppos)
{
	struct dvb_device *dvbdev = file->private_data;
	struct ddb_output *output = dvbdev->priv;
	size_t left = count;
	int stat;

	while (left) {
		if (ddb_output_free(output) < 188) {
			if (file->f_flags & O_NONBLOCK)
				break;
			if (wait_event_interruptible(
				    output->dma->wq,
				    ddb_output_free(output) >= 188) < 0)
				break;
		}
		stat = ddb_output_write(output, buf, left);
		if (stat < 0)
			break;
		buf += stat;
		left -= stat;
	}
	return (left == count) ? -EAGAIN : (count - left);
}

static ssize_t ts_read(struct file *file, char *buf,
		       size_t count, loff_t *ppos)
{
	struct dvb_device *dvbdev = file->private_data;
	struct ddb_output *output = dvbdev->priv;
	struct ddb_input *input = output->port->input[0];
	int left, read;

	count -= count % 188;
	left = count;
	while (left) {
		if (ddb_input_avail(input) < 188) {
			if (file->f_flags & O_NONBLOCK)
				break;
			if (wait_event_interruptible(
				    input->dma->wq, ddb_input_avail(input) >= 188) < 0)
				break;
		}
		read = ddb_input_read(input, buf, left);
		if (read < 0)
			return read;
		left -= read;
		buf += read;
	}
	return (left == count) ? -EAGAIN : (count - left);
}

static unsigned int ts_poll(struct file *file, poll_table *wait)
{
	/*
	struct dvb_device *dvbdev = file->private_data;
	struct ddb_output *output = dvbdev->priv;
	struct ddb_input *input = output->port->input[0];
	*/
	unsigned int mask = 0;

#if 0
	if (data_avail_to_read)
		mask |= POLLIN | POLLRDNORM;
	if (data_avail_to_write)
		mask |= POLLOUT | POLLWRNORM;

	poll_wait(file, &read_queue, wait);
	poll_wait(file, &write_queue, wait);
#endif
	return mask;
}

#if 0
static int ts_release(struct inode *inode, struct file *file)
{
	struct dvb_device *dvbdev = file->private_data;
	struct ddb_output *output = dvbdev->priv;
	struct ddb_input *input = output->port->input[0];


	return dvb_generic_release(inode, file);
}

static unsigned int ts_open(struct inode *inode, struct file *file)
{
	int err;
	struct dvb_device *dvbdev = file->private_data;
	struct ddb_output *output = dvbdev->priv;
	struct ddb_input *input = output->port->input[0];

	err = dvb_generic_open(inode, file);
	if (err < 0)
		return err;

#if 0
	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		ddb_input_start(input);
	else
		ddb_output_start(output);
#endif
	return err;
}
#endif

static const struct file_operations ci_fops = {
	.owner   = THIS_MODULE,
	.read    = ts_read,
	.write   = ts_write,
	.open    = dvb_generic_open,
	.release = dvb_generic_release,
	.poll    = ts_poll,
	.mmap    = 0,
};

static struct dvb_device dvbdev_ci = {
	.priv    = 0,
	.readers = 1,
	.writers = 1,
	.users   = 2,
	.fops    = &ci_fops,
};

/****************************************************************************/
/****************************************************************************/
/****************************************************************************/

static int set_redirect(u32 i, u32 p)
{
	struct ddb *idev = ddbs[(i >> 4) & 0x1f];
	struct ddb_input *input;
	struct ddb *pdev = ddbs[(p >> 4) & 0x1f];
	struct ddb_port *port;

	if (!idev || !pdev)
		return -EINVAL;

	port = &pdev->port[p & 3];
	if (port->class != DDB_PORT_CI && port->class != DDB_PORT_LOOP)
		return -EINVAL;

	ddb_unredirect(port);
	if (i == 8)
		return 0;
	input = &idev->input[i & 7];
	if (input->port->class != DDB_PORT_TUNER)
		port->input[0]->redirect = input->redirect;
	else
		port->input[0]->redirect = input;
	input->redirect = port->input[0];
	port->output->redirect = input;

	ddb_redirect_dma(input->port->dev, input->dma, port->output->dma);
	return 0;
}

static void input_write_output(struct ddb_input *input,
			       struct ddb_output *output)
{
	ddbwritel(output->port->dev,
		  input->dma->stat, DMA_BUFFER_ACK(output->dma->nr));
}

static void output_ack_input(struct ddb_output *output,
			     struct ddb_input *input)
{
	ddbwritel(input->port->dev,
		  output->dma->stat, DMA_BUFFER_ACK(input->dma->nr));
}

static void input_write_dvb(struct ddb_input *input, struct ddb_dvb *dvb)
{
	struct ddb_dma *dma = input->dma;
	struct ddb *dev = input->port->dev;

	if (4 & ddbreadl(dev, DMA_BUFFER_CONTROL(dma->nr)))
		printk(KERN_ERR "Overflow dma %d\n", dma->nr);
	while (dma->cbuf != ((dma->stat >> 11) & 0x1f)
	       || (4 & ddbreadl(dev, DMA_BUFFER_CONTROL(dma->nr)))) {
		dvb_dmx_swfilter_packets(&dvb->demux,
					 dma->vbuf[dma->cbuf],
					 dma->size / 188);
		dma->cbuf = (dma->cbuf + 1) % dma->num;
		ddbwritel(dev, (dma->cbuf << 11),  DMA_BUFFER_ACK(dma->nr));
		dma->stat = ddbreadl(dev, DMA_BUFFER_CURRENT(dma->nr));
	}
}

static void input_tasklet(unsigned long data)
{
	struct ddb_input *input = (struct ddb_input *) data;
	struct ddb_dma *dma = input->dma;
	struct ddb *dev = input->port->dev;

	spin_lock(&dma->lock);
	if (!dma->running) {
		spin_unlock(&dma->lock);
		return;
	}
	dma->stat = ddbreadl(dev, DMA_BUFFER_CURRENT(dma->nr));

	if (input->port->class == DDB_PORT_TUNER) {
		if (input->redirect)
			input_write_output(input,
					   input->redirect->port->output);
		else
			input_write_dvb(input, &input->dvb);
	}
	if (input->port->class == DDB_PORT_CI ||
	    input->port->class == DDB_PORT_LOOP) {
		if (input->redirect) {
			if (input->redirect->port->class == DDB_PORT_TUNER)
				input_write_dvb(input, &input->redirect->dvb);
			else
				input_write_output(input,
						   input->redirect->port->output);
		} else
			wake_up(&dma->wq);
	}
	spin_unlock(&dma->lock);
}

static void output_tasklet(unsigned long data)
{
	struct ddb_output *output = (struct ddb_output *) data;
	struct ddb_dma *dma = output->dma;
	struct ddb *dev = output->port->dev;

	spin_lock(&dma->lock);
	if (!dma->running) {
		spin_unlock(&dma->lock);
		return;
	}
	dma->stat = ddbreadl(dev, DMA_BUFFER_CURRENT(dma->nr));
	dma->ctrl = ddbreadl(dev, DMA_BUFFER_CONTROL(dma->nr));
	if (output->redirect)
		output_ack_input(output, output->redirect);
	wake_up(&dma->wq);
	spin_unlock(&dma->lock);
}

#if 0
static void io_tasklet(unsigned long data)
{
	struct ddb_dma *dma = (struct ddb_dma *) data;

	spin_lock(&dma->lock);
	if (!dma->running) {
		spin_unlock(&dma->lock);
		return;
	}
	dma->stat = ddbreadl(dev, DMA_BUFFER_CURRENT(dma->nr));
	dma->ctrl = ddbreadl(dev, DMA_BUFFER_CONTROL(dma->nr));
	if (dma->nr & 8)
		handle_output((struct ddb_output *) dma->io);
	else
		handle_input((struct ddb_input *) dma->io);
	wake_up(&dma->wq);
	spin_unlock(&dma->lock);
}
#endif

/****************************************************************************/
/****************************************************************************/
/****************************************************************************/

static int wait_ci_ready(struct ddb_ci *ci)
{
	u32 count = 100;

	do {
		if (ddbreadl(ci->port->dev,
			     CI_CONTROL(ci->nr)) & CI_READY)
			break;
		msleep(1);
		if ((--count) == 0)
			return -1;
	} while (1);
	return 0;
}

static int read_attribute_mem(struct dvb_ca_en50221 *ca,
			      int slot, int address)
{
	struct ddb_ci *ci = ca->data;
	u32 val, off = (address >> 1) & (CI_BUFFER_SIZE-1);

	if (address > CI_BUFFER_SIZE)
		return -1;
	ddbwritel(ci->port->dev, CI_READ_CMD | (1 << 16) | address,
		  CI_DO_READ_ATTRIBUTES(ci->nr));
	wait_ci_ready(ci);
	val = 0xff & ddbreadl(ci->port->dev, CI_BUFFER(ci->nr) + off);
	/* printk("%04x: %02x\n", address, val); */
	return val;
}

static int write_attribute_mem(struct dvb_ca_en50221 *ca, int slot,
			       int address, u8 value)
{
	struct ddb_ci *ci = ca->data;

	ddbwritel(ci->port->dev, CI_WRITE_CMD | (value << 16) | address,
		  CI_DO_ATTRIBUTE_RW(ci->nr));
	wait_ci_ready(ci);
	return 0;
}

static int read_cam_control(struct dvb_ca_en50221 *ca,
			    int slot, u8 address)
{
	u32 count = 100;
	struct ddb_ci *ci = ca->data;
	u32 res;

	ddbwritel(ci->port->dev, CI_READ_CMD | address,
		  CI_DO_IO_RW(ci->nr));
	do {
		res = ddbreadl(ci->port->dev, CI_READDATA(ci->nr));
		if (res & CI_READY)
			break;
		msleep(1);
		if ((--count) == 0)
			return -1;
	} while (1);
	return 0xff & res;
}

static int write_cam_control(struct dvb_ca_en50221 *ca, int slot,
			     u8 address, u8 value)
{
	struct ddb_ci *ci = ca->data;

	ddbwritel(ci->port->dev, CI_WRITE_CMD | (value << 16) | address,
		  CI_DO_IO_RW(ci->nr));
	wait_ci_ready(ci);
	return 0;
}

static int slot_reset(struct dvb_ca_en50221 *ca, int slot)
{
	struct ddb_ci *ci = ca->data;

	printk(KERN_INFO "slot reset %d\n", ci->nr);
	ddbwritel(ci->port->dev, CI_POWER_ON,
		  CI_CONTROL(ci->nr));
	msleep(300);
	ddbwritel(ci->port->dev, CI_POWER_ON | CI_RESET_CAM,
		  CI_CONTROL(ci->nr));
	ddbwritel(ci->port->dev, CI_ENABLE | CI_POWER_ON | CI_RESET_CAM,
		  CI_CONTROL(ci->nr));
	udelay(20);
	ddbwritel(ci->port->dev, CI_ENABLE | CI_POWER_ON,
		  CI_CONTROL(ci->nr));
	return 0;
}

static int slot_shutdown(struct dvb_ca_en50221 *ca, int slot)
{
	struct ddb_ci *ci = ca->data;

	printk(KERN_INFO "slot shutdown\n");
	ddbwritel(ci->port->dev, 0, CI_CONTROL(ci->nr));
	return 0;
}

static int slot_ts_enable(struct dvb_ca_en50221 *ca, int slot)
{
	struct ddb_ci *ci = ca->data;
	u32 val = ddbreadl(ci->port->dev, CI_CONTROL(ci->nr));

	ddbwritel(ci->port->dev, val | CI_BYPASS_DISABLE,
		  CI_CONTROL(ci->nr));
	return 0;
}

static int poll_slot_status(struct dvb_ca_en50221 *ca, int slot, int open)
{
	struct ddb_ci *ci = ca->data;
	u32 val = ddbreadl(ci->port->dev, CI_CONTROL(ci->nr));
	int stat = 0;

	if (val & CI_CAM_DETECT)
		stat |= DVB_CA_EN50221_POLL_CAM_PRESENT;
	if (val & CI_CAM_READY)
		stat |= DVB_CA_EN50221_POLL_CAM_READY;
	return stat;
}

static struct dvb_ca_en50221 en_templ = {
	.read_attribute_mem  = read_attribute_mem,
	.write_attribute_mem = write_attribute_mem,
	.read_cam_control    = read_cam_control,
	.write_cam_control   = write_cam_control,
	.slot_reset          = slot_reset,
	.slot_shutdown       = slot_shutdown,
	.slot_ts_enable      = slot_ts_enable,
	.poll_slot_status    = poll_slot_status,
};

static void ci_attach(struct ddb_port *port)
{
	struct ddb_ci *ci = 0;

	ci = kzalloc(sizeof(*ci), GFP_KERNEL);
	if (!ci)
		return;
	memcpy(&ci->en, &en_templ, sizeof(en_templ));
	ci->en.data = ci;
	port->en = &ci->en;
	ci->port = port;
	ci->nr = port->nr - 2;
}

/****************************************************************************/
/****************************************************************************/
/****************************************************************************/


struct cxd2099_cfg cxd_cfg = {
	.bitrate =  62000,
	.adr     =  0x40,
	.polarity = 1,
	.clock_mode = 1,
};

static int ddb_ci_attach(struct ddb_port *port)
{
	if (port->type == DDB_CI_EXTERNAL_SONY) {
		port->en = cxd2099_attach(&cxd_cfg, port, &port->i2c->adap);
		if (!port->en)
			return -ENODEV;
		dvb_ca_en50221_init(port->input[0]->dvb.adap,
				    port->en, 0, 1);
	}
#if 1
	if (port->type == DDB_CI_INTERNAL) {
		ci_attach(port);
		if (!port->en)
			return -ENODEV;
		dvb_ca_en50221_init(port->input[0]->dvb.adap, port->en, 0, 1);
	}
#endif
	return 0;
}

static int ddb_port_attach(struct ddb_port *port)
{
	int ret = 0;

	switch (port->class) {
	case DDB_PORT_TUNER:
		ret = dvb_input_attach(port->input[0]);
		if (ret < 0)
			break;
		ret = dvb_input_attach(port->input[1]);
		break;
	case DDB_PORT_CI:
		ret = ddb_ci_attach(port);
		if (ret < 0)
			break;
	case DDB_PORT_LOOP:
		ddb_input_start(port->input[0]);
		ddb_output_start(port->output);
		ret = dvb_register_device(port->input[0]->dvb.adap,
					  &port->input[0]->dvb.dev,
					  &dvbdev_ci, (void *) port->output,
					  DVB_DEVICE_SEC);
		break;
	default:
		break;
	}
	if (ret < 0)
		printk(KERN_ERR "port_attach on port %d failed\n", port->nr);
	return ret;
}

static int ddb_ports_attach(struct ddb *dev)
{
	int i, ret = 0;
	struct ddb_port *port;

	ret = dvb_register_adapters(dev);
	if (ret < 0)
		return ret;

	for (i = 0; i < dev->info->port_num; i++) {
		port = &dev->port[i];
		ret = ddb_port_attach(port);
		if (ret < 0)
			break;
	}
	return ret;
}

static void ddb_ports_detach(struct ddb *dev)
{
	int i;
	struct ddb_port *port;

	for (i = 0; i < dev->info->port_num; i++) {
		port = &dev->port[i];
		switch (port->class) {
		case DDB_PORT_TUNER:
			dvb_input_detach(port->input[0]);
			dvb_input_detach(port->input[1]);
			break;
		case DDB_PORT_CI:
		case DDB_PORT_LOOP:
			if (port->input[0]->dvb.dev)
				dvb_unregister_device(port->input[0]->dvb.dev);
			ddb_input_stop(port->input[0]);
			ddb_output_stop(port->output);
			if (port->en) {
				dvb_ca_en50221_release(port->en);
				kfree(port->en);
				port->en = 0;
			}
			break;
		}
	}
	dvb_unregister_adapters(dev);
}

/****************************************************************************/
/****************************************************************************/

static int port_has_cxd(struct ddb_port *port)
{
	u8 val;
	return i2c_read_reg(&port->i2c->adap, 0x40, 0, &val) ? 0 : 1;
}

static int port_has_stv0900(struct ddb_port *port)
{
	u8 val;
	if (i2c_read_reg16(&port->i2c->adap, 0x69, 0xf100, &val) < 0)
		return 0;
	return 1;
}

static int port_has_stv0900_aa(struct ddb_port *port)
{
	u8 val;
	if (i2c_read_reg16(&port->i2c->adap, 0x68, 0xf100, &val) < 0)
		return 0;
	return 1;
}

static int port_has_drxks(struct ddb_port *port)
{
	u8 val;
	if (i2c_read(&port->i2c->adap, 0x29, &val) < 0)
		return 0;
	if (i2c_read(&port->i2c->adap, 0x2a, &val) < 0)
		return 0;
	return 1;
}

static int port_has_stv0367(struct ddb_port *port)
{
	u8 val;

	if (i2c_read_reg16(&port->i2c->adap, 0x1e, 0xf000, &val) < 0)
		return 0;
	if (val != 0x60)
		return 0;
	if (i2c_read_reg16(&port->i2c->adap, 0x1f, 0xf000, &val) < 0)
		return 0;
	if (val != 0x60)
		return 0;
	return 1;
}

static void ddb_port_probe(struct ddb_port *port)
{
	struct ddb *dev = port->dev;
	char *modname = "NO MODULE";

	port->class = DDB_PORT_NONE;

	if (port->nr > 1 && dev->info->type == DDB_OCTOPUS_CI) {
		modname = "CI internal";
		port->class = DDB_PORT_CI;
		port->type = DDB_CI_INTERNAL;
	} else if (port_has_cxd(port)) {
		modname = "CI";
		port->class = DDB_PORT_CI;
		port->type = DDB_CI_EXTERNAL_SONY;
		ddbwritel(dev, I2C_SPEED_400, port->i2c->regs + I2C_TIMING);
	} else if (port_has_stv0900(port)) {
		modname = "DUAL DVB-S2";
		port->class = DDB_PORT_TUNER;
		port->type = DDB_TUNER_DVBS_ST;
		ddbwritel(dev, I2C_SPEED_100, port->i2c->regs + I2C_TIMING);
	} else if (port_has_stv0900_aa(port)) {
		modname = "DUAL DVB-S2";
		port->class = DDB_PORT_TUNER;
		port->type = DDB_TUNER_DVBS_ST_AA;
		ddbwritel(dev, I2C_SPEED_100, port->i2c->regs + I2C_TIMING);
	} else if (port_has_drxks(port)) {
		modname = "DUAL DVB-C/T";
		port->class = DDB_PORT_TUNER;
		port->type = DDB_TUNER_DVBCT_TR;
		ddbwritel(dev, I2C_SPEED_400, port->i2c->regs + I2C_TIMING);
	} else if (port_has_stv0367(port)) {
		modname = "DUAL DVB-C/T";
		port->class = DDB_PORT_TUNER;
		port->type = DDB_TUNER_DVBCT_ST;
		ddbwritel(dev, I2C_SPEED_100, port->i2c->regs + I2C_TIMING);
	} else if (port->nr == ts_loop) {
		modname = "TS LOOP";
		port->class = DDB_PORT_LOOP;
	}
	printk(KERN_INFO "Port %d (TAB %d): %s\n", port->nr, port->nr+1, modname);
}

static void ddb_dma_init(struct ddb_dma *dma, int nr, void *io)
{
	unsigned long priv = (unsigned long) io;

	dma->io = io;
	dma->nr = nr;
	spin_lock_init(&dma->lock);
	init_waitqueue_head(&dma->wq);
	if (nr & 8) {
		tasklet_init(&dma->tasklet, output_tasklet, priv);
		dma->num = OUTPUT_DMA_BUFS;
		dma->size = OUTPUT_DMA_SIZE;
		dma->div = OUTPUT_DMA_IRQ_DIV;
	} else {
		tasklet_init(&dma->tasklet, input_tasklet, priv);
		dma->num = INPUT_DMA_BUFS;
		dma->size = INPUT_DMA_SIZE;
		dma->div = INPUT_DMA_IRQ_DIV;
	}
}

static void ddb_input_init(struct ddb_port *port, int nr, int pnr)
{
	struct ddb *dev = port->dev;
	struct ddb_input *input = &dev->input[nr];

	port->input[pnr] = input;
	input->nr = nr;
	input->port = port;
	input->dma = &dev->dma[nr];
	ddb_dma_init(input->dma, nr, (void *) input);
	ddbwritel(dev, 0, TS_INPUT_CONTROL(nr));
	ddbwritel(dev, 2, TS_INPUT_CONTROL(nr));
	ddbwritel(dev, 0, TS_INPUT_CONTROL(nr));
	ddbwritel(dev, 0, DMA_BUFFER_ACK(input->dma->nr));
	input->dvb.adap = &dev->adap[input->nr];
}

static void ddb_output_init(struct ddb_port *port, int nr)
{
	struct ddb *dev = port->dev;
	struct ddb_output *output = &dev->output[nr];
	port->output = output;
	output->nr = nr;
	output->port = port;
	output->dma = &dev->dma[nr + 8];
	ddb_dma_init(output->dma, nr + 8, (void *) output);
	ddbwritel(dev, 0, TS_OUTPUT_CONTROL(nr));
	ddbwritel(dev, 2, TS_OUTPUT_CONTROL(nr));
	ddbwritel(dev, 0, TS_OUTPUT_CONTROL(nr));
}

static void ddb_ports_init(struct ddb *dev)
{
	int i;
	struct ddb_port *port;

	for (i = 0; i < dev->info->port_num; i++) {
		port = &dev->port[i];
		port->dev = dev;
		port->nr = i;
		port->i2c = &dev->i2c[i];

		mutex_init(&port->i2c_gate_lock);
		ddb_port_probe(port);
		if (i >= 2 && dev->info->type == DDB_OCTOPUS_CI) {
			ddb_input_init(port, 2 + i, 0);
			ddb_input_init(port, 4 + i, 1);
		} else {
			ddb_input_init(port, 2 * i, 0);
			ddb_input_init(port, 2 * i + 1, 1);
		}
		ddb_output_init(port, i);
	}
}

static void ddb_ports_release(struct ddb *dev)
{
	int i;
	struct ddb_port *port;

	for (i = 0; i < dev->info->port_num; i++) {
		port = &dev->port[i];
		port->dev = dev;
		if (port->input[0])
			tasklet_kill(&port->input[0]->dma->tasklet);
		if (port->input[1])
			tasklet_kill(&port->input[1]->dma->tasklet);
		if (port->output)
			tasklet_kill(&port->output->dma->tasklet);
	}
}

/****************************************************************************/
/****************************************************************************/
/****************************************************************************/

static void irq_handle_i2c(struct ddb *dev, int n)
{
	struct ddb_i2c *i2c = &dev->i2c[n];

	i2c->done = 1;
	wake_up(&i2c->wq);
}

static irqreturn_t irq_handler(int irq, void *dev_id)
{
	struct ddb *dev = (struct ddb *) dev_id;
	u32 s = ddbreadl(dev, INTERRUPT_STATUS);

	if (!s)
		return IRQ_NONE;

	do {
		ddbwritel(dev, s, INTERRUPT_ACK);

		if (s & 0x0000000f)
			dev->i2c_irq++;
		if (s & 0x000fff00)
			dev->ts_irq++;

		if (s & 0x00000001)
			irq_handle_i2c(dev, 0);
		if (s & 0x00000002)
			irq_handle_i2c(dev, 1);
		if (s & 0x00000004)
			irq_handle_i2c(dev, 2);
		if (s & 0x00000008)
			irq_handle_i2c(dev, 3);

		if (s & 0x00000100)
			tasklet_schedule(&dev->dma[0].tasklet);
		if (s & 0x00000200)
			tasklet_schedule(&dev->dma[1].tasklet);
		if (s & 0x00000400)
			tasklet_schedule(&dev->dma[2].tasklet);
		if (s & 0x00000800)
			tasklet_schedule(&dev->dma[3].tasklet);
		if (s & 0x00001000)
			tasklet_schedule(&dev->dma[4].tasklet);
		if (s & 0x00002000)
			tasklet_schedule(&dev->dma[5].tasklet);
		if (s & 0x00004000)
			tasklet_schedule(&dev->dma[6].tasklet);
		if (s & 0x00008000)
			tasklet_schedule(&dev->dma[7].tasklet);
		if (s & 0x00010000)
			tasklet_schedule(&dev->dma[8].tasklet);
		if (s & 0x00020000)
			tasklet_schedule(&dev->dma[9].tasklet);
		if (s & 0x00040000)
			tasklet_schedule(&dev->dma[10].tasklet);
		if (s & 0x00080000)
			tasklet_schedule(&dev->dma[11].tasklet);

		/* if (s & 0x000f0000)	printk("%08x\n", istat); */
	} while ((s = ddbreadl(dev, INTERRUPT_STATUS)));

	return IRQ_HANDLED;
}

/******************************************************************************/
/******************************************************************************/
/******************************************************************************/

static int flashio(struct ddb *dev, u8 *wbuf, u32 wlen, u8 *rbuf, u32 rlen)
{
	u32 data, shift;

	if (wlen > 4)
		ddbwritel(dev, 1, SPI_CONTROL);
	while (wlen > 4) {
		/* FIXME: check for big-endian */
		data = swab32(*(u32 *)wbuf);
		wbuf += 4;
		wlen -= 4;
		ddbwritel(dev, data, SPI_DATA);
		while (ddbreadl(dev, SPI_CONTROL) & 0x0004)
			;
	}

	if (rlen)
		ddbwritel(dev, 0x0001 | ((wlen << (8 + 3)) & 0x1f00), SPI_CONTROL);
	else
		ddbwritel(dev, 0x0003 | ((wlen << (8 + 3)) & 0x1f00), SPI_CONTROL);

	data = 0;
	shift = ((4 - wlen) * 8);
	while (wlen) {
		data <<= 8;
		data |= *wbuf;
		wlen--;
		wbuf++;
	}
	if (shift)
		data <<= shift;
	ddbwritel(dev, data, SPI_DATA);
	while (ddbreadl(dev, SPI_CONTROL) & 0x0004)
		;

	if (!rlen) {
		ddbwritel(dev, 0, SPI_CONTROL);
		return 0;
	}
	if (rlen > 4)
		ddbwritel(dev, 1, SPI_CONTROL);

	while (rlen > 4) {
		ddbwritel(dev, 0xffffffff, SPI_DATA);
		while (ddbreadl(dev, SPI_CONTROL) & 0x0004)
			;
		data = ddbreadl(dev, SPI_DATA);
		*(u32 *) rbuf = swab32(data);
		rbuf += 4;
		rlen -= 4;
	}
	ddbwritel(dev, 0x0003 | ((rlen << (8 + 3)) & 0x1F00), SPI_CONTROL);
	ddbwritel(dev, 0xffffffff, SPI_DATA);
	while (ddbreadl(dev, SPI_CONTROL) & 0x0004)
		;

	data = ddbreadl(dev, SPI_DATA);
	ddbwritel(dev, 0, SPI_CONTROL);

	if (rlen < 4)
		data <<= ((4 - rlen) * 8);

	while (rlen > 0) {
		*rbuf = ((data >> 24) & 0xff);
		data <<= 8;
		rbuf++;
		rlen--;
	}
	return 0;
}

#define DDB_MAGIC 'd'

struct ddb_flashio {
	__u8 *write_buf;
	__u32 write_len;
	__u8 *read_buf;
	__u32 read_len;
};

struct ddb_gpio {
	__u32 mask;
	__u32 data;
};


#define IOCTL_DDB_FLASHIO  _IOWR(DDB_MAGIC, 0x00, struct ddb_flashio)
#define IOCTL_DDB_GPIO_IN  _IOWR(DDB_MAGIC, 0x01, struct ddb_gpio)
#define IOCTL_DDB_GPIO_OUT _IOWR(DDB_MAGIC, 0x02, struct ddb_gpio)

#define DDB_NAME "ddbridge"

static u32 ddb_num;
static int ddb_major;
static DEFINE_MUTEX(ddb_mutex);

static int ddb_open(struct inode *inode, struct file *file)
{
	struct ddb *dev = ddbs[iminor(inode)];

	file->private_data = dev;
	return 0;
}

static long ddb_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ddb *dev = file->private_data;
	void *parg = (void *)arg;
	int res;

	switch (cmd) {
	case IOCTL_DDB_FLASHIO:
	{
		struct ddb_flashio fio;
		u8 *rbuf, *wbuf;

		if (copy_from_user(&fio, parg, sizeof(fio)))
			return -EFAULT;

		if (fio.write_len > 1028 || fio.read_len > 1028)
			return -EINVAL;
		if (fio.write_len + fio.read_len > 1028)
			return -EINVAL;

		wbuf = &dev->iobuf[0];
		rbuf = wbuf + fio.write_len;

		if (copy_from_user(wbuf, fio.write_buf, fio.write_len))
			return -EFAULT;
		res = flashio(dev, wbuf, fio.write_len, rbuf, fio.read_len);
		if (res)
			return res;
		if (copy_to_user(fio.read_buf, rbuf, fio.read_len))
			return -EFAULT;
		break;
	}
	case IOCTL_DDB_GPIO_OUT:
	{
		struct ddb_gpio gpio;
		if (copy_from_user(&gpio, parg, sizeof(gpio)))
			break;
		ddbwritel(dev, gpio.mask, GPIO_DIRECTION);
		ddbwritel(dev, gpio.data, GPIO_OUTPUT);
		res = 0;
		break;
	}
	default:
		return -ENOTTY;
	}
	return 0;
}

static const struct file_operations ddb_fops = {
	.unlocked_ioctl = ddb_ioctl,
	.open           = ddb_open,
};

static char *ddb_devnode(struct device *device, mode_t *mode)
{
	struct ddb *dev = dev_get_drvdata(device);

	return kasprintf(GFP_KERNEL, "ddbridge/card%d", dev->nr);
}

static ssize_t ports_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct ddb *dev = dev_get_drvdata(device);

	return sprintf(buf, "%d\n", dev->info->port_num);
}

static ssize_t ts_irq_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct ddb *dev = dev_get_drvdata(device);

	return sprintf(buf, "%d\n", dev->ts_irq);
}

static ssize_t i2c_irq_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct ddb *dev = dev_get_drvdata(device);

	return sprintf(buf, "%d\n", dev->i2c_irq);
}

static char *class_name[] = {
	"NONE", "CI", "TUNER", "LOOP"
};

static char *type_name[] = {
	"NONE", "DVBS_ST", "DVBS_ST_AA", "DVBCT_TR", "DVBCT_ST", "INTERNAL", "CXD2099",
};

static ssize_t fan_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct ddb *dev = dev_get_drvdata(device);
	u32 val;

	val = ddbreadl(dev, GPIO_OUTPUT) & 1;
	return sprintf(buf, "%d\n", val);
}

static ssize_t fan_store(struct device *device, struct device_attribute *d,
			 const char *buf, size_t count)
{
	struct ddb *dev = dev_get_drvdata(device);
	unsigned val;

	if (sscanf(buf, "%u\n", &val) != 1)
		return -EINVAL;
	ddbwritel(dev, 1, GPIO_DIRECTION);
	ddbwritel(dev, val & 1, GPIO_OUTPUT);
	return count;
}

static ssize_t temp_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct ddb *dev = dev_get_drvdata(device);
	int temp;
	u8 tmp[2];

	if (!dev->info->temp_num)
		return sprintf(buf, "no sensor\n");
	if (i2c_read_regs(&dev->i2c[0].adap, 0x48, 0, tmp, 2) < 0)
		return sprintf(buf, "read_error\n");
	temp = (tmp[0] << 3) | (tmp[1] >> 5);
	temp *= 125;
	return sprintf(buf, "%d\n", temp);
}

static ssize_t mod_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct ddb *dev = dev_get_drvdata(device);
	int num = attr->attr.name[3] - 0x30;

	return sprintf(buf, "%s:%s\n",
		       class_name[dev->port[num].class],
		       type_name[dev->port[num].type]);
}

static ssize_t led_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct ddb *dev = dev_get_drvdata(device);
	int num = attr->attr.name[3] - 0x30;

	return sprintf(buf, "%d\n", dev->leds & (1 << num) ? 1 : 0);
}


static void ddb_set_led(struct ddb *dev, int num, int val)
{
	if (!dev->info->led_num)
		return;
	switch (dev->port[num].class) {
	case DDB_PORT_TUNER:
		switch (dev->port[num].type) {
		case DDB_TUNER_DVBS_ST:
			printk(KERN_INFO "LED %d %d\n", num, val);
			i2c_write_reg16(&dev->i2c[num].adap,
					0x69, 0xf14c, val ? 2 : 0);
			break;
		case DDB_TUNER_DVBCT_ST:
			printk(KERN_INFO "LED %d %d\n", num, val);
			i2c_write_reg16(&dev->i2c[num].adap,
					0x1f, 0xf00e, 0);
			i2c_write_reg16(&dev->i2c[num].adap,
					0x1f, 0xf00f, val ? 1 : 0);
			break;
		}
		break;
	default:
		break;
	}
}

static ssize_t led_store(struct device *device, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct ddb *dev = dev_get_drvdata(device);
	int num = attr->attr.name[3] - 0x30;
	unsigned val;

	if (sscanf(buf, "%u\n", &val) != 1)
		return -EINVAL;
	if (val)
		dev->leds |= (1 << num);
	else
		dev->leds &= ~(1 << num);
	ddb_set_led(dev, num, val);
	return count;
}

static ssize_t snr_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct ddb *dev = dev_get_drvdata(device);
	char snr[32];
	int num = attr->attr.name[3] - 0x30;

	/* serial number at 0x100-0x11f */
	if (i2c_read_regs16(&dev->i2c[num].adap, 0x57, 0x100, snr, 32) < 0)
		return sprintf(buf, "NO SNR\n");
	snr[31] = 0; /* in case it is not terminated on EEPROM */
	return sprintf(buf, "%s\n", snr);
}


static ssize_t snr_store(struct device *device, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct ddb *dev = dev_get_drvdata(device);
	int num = attr->attr.name[3] - 0x30;
	u8 snr[34] = { 0x01, 0x00 };

	if (count > 31)
		return -EINVAL;
	memcpy(snr + 2, buf, count);
	i2c_write(&dev->i2c[num].adap, 0x57, snr, 34);
	return count;
}

static ssize_t redirect_show(struct device *device, struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t redirect_store(struct device *device, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned int i, p;
	int res;

	if (sscanf(buf, "%x %x\n", &i, &p) != 2)
		return -EINVAL;
	printk(KERN_INFO "redirect: %02x, %02x\n", i, p);
	res = set_redirect(i, p);
	if (res < 0)
		return res;
	return count;
}

#define __ATTR_MRO(_name, _show) {				\
	.attr	= { .name = __stringify(_name), .mode = 0444 },	\
	.show	= _show,					\
}

struct device_attribute ddb_attrs[] = {
	__ATTR_RO(ports),
	__ATTR_RO(ts_irq),
	__ATTR_RO(i2c_irq),
	__ATTR_MRO(mod0, mod_show),
	__ATTR_MRO(mod1, mod_show),
	__ATTR_MRO(mod2, mod_show),
	__ATTR_MRO(mod3, mod_show),
	__ATTR_RO(temp),
	__ATTR(fan, 0666, fan_show, fan_store),
	__ATTR(led0, 0666, led_show, led_store),
	__ATTR(led1, 0666, led_show, led_store),
	__ATTR(led2, 0666, led_show, led_store),
	__ATTR(led3, 0666, led_show, led_store),
	__ATTR(snr0, 0666, snr_show, snr_store),
	__ATTR(snr1, 0666, snr_show, snr_store),
	__ATTR(snr2, 0666, snr_show, snr_store),
	__ATTR(snr3, 0666, snr_show, snr_store),
	__ATTR(redirect, 0666, redirect_show, redirect_store),
	__ATTR_NULL
};

static struct class ddb_class = {
	.name		= "ddbridge",
	.owner          = THIS_MODULE,
	.dev_attrs	= ddb_attrs,
	.devnode        = ddb_devnode,
};

static int ddb_class_create(void)
{
	ddb_major = register_chrdev(0, DDB_NAME, &ddb_fops);
	if (ddb_major < 0)
		return ddb_major;
	if (class_register(&ddb_class) < 0)
		return -1;
	return 0;
}

static void ddb_class_destroy(void)
{
	class_unregister(&ddb_class);
	unregister_chrdev(ddb_major, DDB_NAME);
}

static int ddb_device_create(struct ddb *dev)
{
	mutex_lock(&ddb_mutex);
	dev->nr = ddb_num++;
	ddbs[dev->nr] = dev;
	mutex_unlock(&ddb_mutex);
	dev->ddb_dev = device_create(&ddb_class, &dev->pdev->dev,
				     MKDEV(ddb_major, dev->nr),
				     dev, "ddbridge%d", dev->nr);
	if (IS_ERR(dev->ddb_dev))
		return -1;
	return 0;
}

static void ddb_device_destroy(struct ddb *dev)
{
	if (IS_ERR(dev->ddb_dev))
		return;
	device_destroy(&ddb_class, MKDEV(ddb_major, dev->nr));
}


/****************************************************************************/
/****************************************************************************/
/****************************************************************************/

static void ddb_unmap(struct ddb *dev)
{
	if (dev->regs)
		iounmap(dev->regs);
	vfree(dev);
}


static void __devexit ddb_remove(struct pci_dev *pdev)
{
	struct ddb *dev = (struct ddb *) pci_get_drvdata(pdev);

	ddb_ports_detach(dev);
	ddb_i2c_release(dev);

	ddbwritel(dev, 0, INTERRUPT_ENABLE);
	free_irq(dev->pdev->irq, dev);
#ifdef CONFIG_PCI_MSI
	if (dev->msi)
		pci_disable_msi(dev->pdev);
#endif
	ddb_ports_release(dev);
	ddb_buffers_free(dev);
	ddb_device_destroy(dev);

	ddb_unmap(dev);
	pci_set_drvdata(pdev, 0);
	pci_disable_device(pdev);
}

static int __devinit ddb_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	struct ddb *dev;
	int stat = 0;
	int irq_flag = IRQF_SHARED;

	if (pci_enable_device(pdev) < 0)
		return -ENODEV;

	dev = vzalloc(sizeof(struct ddb));
	if (dev == NULL)
		return -ENOMEM;

	dev->pdev = pdev;
	pci_set_drvdata(pdev, dev);
	dev->info = (struct ddb_info *) id->driver_data;
	printk(KERN_INFO "DDBridge driver detected: %s\n", dev->info->name);

	dev->regs = ioremap(pci_resource_start(dev->pdev, 0),
			    pci_resource_len(dev->pdev, 0));
	if (!dev->regs) {
		stat = -ENOMEM;
		goto fail;
	}
	printk(KERN_INFO "HW %08x REG %08x\n",
	       ddbreadl(dev, 0), ddbreadl(dev, 4));

#ifdef CONFIG_PCI_MSI
	if (pci_msi_enabled())
		stat = pci_enable_msi(dev->pdev);
	if (stat) {
		printk(KERN_INFO ": MSI not available.\n");
	} else {
		irq_flag = 0;
		dev->msi = 1;
	}
#endif
	stat = request_irq(dev->pdev->irq, irq_handler,
			   irq_flag, "DDBridge", (void *) dev);
	if (stat < 0)
		goto fail1;
	ddbwritel(dev, 0, DMA_BASE_WRITE);
	ddbwritel(dev, 0, DMA_BASE_READ);
	ddbwritel(dev, 0xffffffff, INTERRUPT_ACK);
	ddbwritel(dev, 0x000fff0f, INTERRUPT_ENABLE);
	ddbwritel(dev, 0, MSI1_ENABLE);

	if (ddb_i2c_init(dev) < 0)
		goto fail1;
	ddb_ports_init(dev);
	if (ddb_buffers_alloc(dev) < 0) {
		printk(KERN_INFO ": Could not allocate buffer memory\n");
		goto fail2;
	}
	if (ddb_ports_attach(dev) < 0)
		goto fail3;

	ddb_device_create(dev);

	if (dev->info->fan_num)	{
		ddbwritel(dev, 1, GPIO_DIRECTION);
		ddbwritel(dev, 1, GPIO_OUTPUT);
	}
	return 0;

fail3:
	ddb_ports_detach(dev);
	printk(KERN_ERR "fail3\n");
	ddb_ports_release(dev);
fail2:
	printk(KERN_ERR "fail2\n");
	ddb_buffers_free(dev);
	ddb_i2c_release(dev);
fail1:
	printk(KERN_ERR "fail1\n");
	free_irq(dev->pdev->irq, dev);
#ifdef CONFIG_PCI_MSI
	if (dev->msi)
		pci_disable_msi(dev->pdev);
#endif
fail:
	printk(KERN_ERR "fail\n");
	ddb_unmap(dev);
	pci_set_drvdata(pdev, 0);
	pci_disable_device(pdev);
	return -1;
}

/******************************************************************************/
/******************************************************************************/
/******************************************************************************/

static struct ddb_info ddb_none = {
	.type     = DDB_NONE,
	.name     = "Digital Devices PCIe bridge",
};

static struct ddb_info ddb_octopus = {
	.type     = DDB_OCTOPUS,
	.name     = "Digital Devices Octopus DVB adapter",
	.port_num = 4,
	.i2c_num  = 4,
};

static struct ddb_info ddb_octopus_le = {
	.type     = DDB_OCTOPUS,
	.name     = "Digital Devices Octopus LE DVB adapter",
	.port_num = 2,
	.i2c_num  = 2,
};

static struct ddb_info ddb_octopus_oem = {
	.type     = DDB_OCTOPUS,
	.name     = "Digital Devices Octopus OEM",
	.port_num = 4,
	.i2c_num  = 4,
	.led_num  = 1,
	.fan_num  = 1,
	.temp_num = 1,
};

static struct ddb_info ddb_octopus_mini = {
	.type     = DDB_OCTOPUS,
	.name     = "Digital Devices Octopus Mini",
	.port_num = 4,
	.i2c_num  = 4,
};

static struct ddb_info ddb_v6 = {
	.type     = DDB_OCTOPUS,
	.name     = "Digital Devices Cine S2 V6 DVB adapter",
	.port_num = 3,
	.i2c_num  = 3,
};

static struct ddb_info ddb_dvbct = {
	.type     = DDB_OCTOPUS,
	.name     = "Digital Devices DVBCT V6.1 DVB adapter",
	.port_num = 3,
	.i2c_num  = 3,
};

static struct ddb_info ddb_satixS2v3 = {
	.type     = DDB_OCTOPUS,
	.name     = "Mystique SaTiX-S2 V3 DVB adapter",
	.port_num = 3,
	.i2c_num  = 3,
};

static struct ddb_info ddb_ci = {
	.type     = DDB_OCTOPUS_CI,
	.name     = "Digital Devices Octopus CI",
	.port_num = 4,
	.i2c_num  = 2,
};


#define DDVID 0xdd01 /* Digital Devices Vendor ID */

#define DDB_ID(_vend, _dev, _subvend, _subdev, _driverdata) { \
	.vendor      = _vend,    .device    = _dev, \
	.subvendor   = _subvend, .subdevice = _subdev, \
	.driver_data = (unsigned long)&_driverdata }

static const struct pci_device_id ddb_id_tbl[] __devinitdata = {
	DDB_ID(DDVID, 0x0002, DDVID, 0x0001, ddb_octopus),
	DDB_ID(DDVID, 0x0003, DDVID, 0x0001, ddb_octopus),
	DDB_ID(DDVID, 0x0003, DDVID, 0x0002, ddb_octopus_le),
	DDB_ID(DDVID, 0x0003, DDVID, 0x0003, ddb_octopus_oem),
	DDB_ID(DDVID, 0x0003, DDVID, 0x0010, ddb_octopus_mini),
	DDB_ID(DDVID, 0x0003, DDVID, 0x0020, ddb_v6),
	DDB_ID(DDVID, 0x0003, DDVID, 0x0030, ddb_dvbct),
	DDB_ID(DDVID, 0x0003, DDVID, 0xdb03, ddb_satixS2v3),
	DDB_ID(DDVID, 0x0011, DDVID, 0x0040, ddb_ci),
	/* in case sub-ids got deleted in flash */
	DDB_ID(DDVID, 0x0003, PCI_ANY_ID, PCI_ANY_ID, ddb_none),
	{0}
};
MODULE_DEVICE_TABLE(pci, ddb_id_tbl);


static struct pci_driver ddb_pci_driver = {
	.name        = "DDBridge",
	.id_table    = ddb_id_tbl,
	.probe       = ddb_probe,
	.remove      = ddb_remove,
};

static __init int module_init_ddbridge(void)
{
	int stat;

	printk(KERN_INFO "Digital Devices PCIE bridge driver, "
	       "Copyright (C) 2010-11 Digital Devices GmbH\n");
	if (ddb_class_create())
		return -1;
	stat = pci_register_driver(&ddb_pci_driver);
	if (stat < 0)
		ddb_class_destroy();
	return stat;
}

static __exit void module_exit_ddbridge(void)
{
	pci_unregister_driver(&ddb_pci_driver);
	ddb_class_destroy();
}

module_init(module_init_ddbridge);
module_exit(module_exit_ddbridge);

MODULE_DESCRIPTION("Digital Devices PCIe Bridge");
MODULE_AUTHOR("Ralph Metzler");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.8");
