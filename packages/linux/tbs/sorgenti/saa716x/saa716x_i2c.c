#include <linux/delay.h>

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>

#include <linux/i2c.h>

#include "saa716x_mod.h"

#include "saa716x_i2c_reg.h"
#include "saa716x_msi_reg.h"
#include "saa716x_cgu_reg.h"

#include "saa716x_i2c.h"
#include "saa716x_msi.h"
#include "saa716x_spi.h"
#include "saa716x_priv.h"

#define SAA716x_I2C_TXFAIL	(I2C_ERROR_IBE		| \
				 I2C_ACK_INTER_MTNA	| \
				 I2C_FAILURE_INTER_MAF)

#define SAA716x_I2C_TXBUSY	(I2C_TRANSMIT		| \
				 I2C_TRANSMIT_PROG)

#define SAA716x_I2C_RXBUSY	(I2C_RECEIVE		| \
				 I2C_RECEIVE_CLEAR)

static const char* state[] = {
	"Idle",
	"DoneStop",
	"Busy",
	"TOscl",
	"TOarb",
	"DoneWrite",
	"DoneRead",
	"DoneWriteTO",
	"DoneReadTO",
	"NoDevice",
	"NoACK",
	"BUSErr",
	"ArbLost",
	"SEQErr",
	"STErr"
};

int saa716x_i2c_irqevent(struct saa716x_dev *saa716x, u8 bus)
{
	u32 stat, mask;
	u32 *I2C_DEV;

	BUG_ON(saa716x == NULL);
	I2C_DEV = saa716x->I2C_DEV;

	stat = SAA716x_EPRD(I2C_DEV[bus], INT_STATUS);
	mask = SAA716x_EPRD(I2C_DEV[bus], INT_ENABLE);
	saa716x->i2c[bus].i2c_stat = stat;
	dprintk(SAA716x_DEBUG, 0, "Bus(%d) I2C event: Status=<%s> --> Stat=<%02x> Mask=<%02x>",
		bus, state[stat], stat, mask);

	if (!(stat & mask))
		return -1;

	SAA716x_EPWR(I2C_DEV[bus], INT_CLR_STATUS, stat);

	if (stat & I2C_INTERRUPT_STFNF)
		dprintk(SAA716x_DEBUG, 0, "<STFNF> ");

	if (stat & I2C_INTERRUPT_MTFNF) {
		dprintk(SAA716x_DEBUG, 0, "<MTFNF> ");
	}

	if (stat & I2C_INTERRUPT_RFDA)
		dprintk(SAA716x_DEBUG, 0, "<RFDA> ");

	if (stat & I2C_INTERRUPTE_RFF)
		dprintk(SAA716x_DEBUG, 0, "<RFF> ");

	if (stat & I2C_SLAVE_INTERRUPT_STDR)
		dprintk(SAA716x_DEBUG, 0, "<STDR> ");

	if (stat & I2C_MASTER_INTERRUPT_MTDR) {
		dprintk(SAA716x_DEBUG, 0, "<MTDR> ");
	}

	if (stat & I2C_ERROR_IBE)
		dprintk(SAA716x_DEBUG, 0, "<IBE> ");

	if (stat & I2C_MODE_CHANGE_INTER_MSMC)
		dprintk(SAA716x_DEBUG, 0, "<MSMC> ");

	if (stat & I2C_SLAVE_RECEIVE_INTER_SRSD)
		dprintk(SAA716x_DEBUG, 0, "<SRSD> ");

	if (stat & I2C_SLAVE_TRANSMIT_INTER_STSD)
		dprintk(SAA716x_DEBUG, 0, "<STSD> ");

	if (stat & I2C_ACK_INTER_MTNA)
		dprintk(SAA716x_DEBUG, 0, "<MTNA> ");

	if (stat & I2C_FAILURE_INTER_MAF)
		dprintk(SAA716x_DEBUG, 0, "<MAF> ");

	if (stat & I2C_INTERRUPT_MTD)
		dprintk(SAA716x_DEBUG, 0, "<MTD> ");

	return 0;
}

static irqreturn_t saa716x_i2c_irq(int irq, void *dev_id)
{
	struct saa716x_dev *saa716x	= (struct saa716x_dev *) dev_id;

	if (unlikely(saa716x == NULL)) {
		printk("%s: saa716x=NULL", __func__);
		return IRQ_NONE;
	}
	dprintk(SAA716x_DEBUG, 1, "MSI STAT L=<%02x> H=<%02x>, CTL L=<%02x> H=<%02x>",
		SAA716x_EPRD(MSI, MSI_INT_STATUS_L),
		SAA716x_EPRD(MSI, MSI_INT_STATUS_H),
		SAA716x_EPRD(MSI, MSI_INT_ENA_L),
		SAA716x_EPRD(MSI, MSI_INT_ENA_H));

	dprintk(SAA716x_DEBUG, 1, "I2C STAT 0=<%02x> 1=<%02x>, CTL 0=<%02x> 1=<%02x>",
		SAA716x_EPRD(I2C_A, INT_STATUS),
		SAA716x_EPRD(I2C_B, INT_STATUS),
		SAA716x_EPRD(I2C_A, INT_CLR_STATUS),
		SAA716x_EPRD(I2C_B, INT_CLR_STATUS));

	return IRQ_HANDLED;
}

static void saa716x_term_xfer(struct saa716x_i2c *i2c, u32 I2C_DEV)
{
	struct saa716x_dev *saa716x = i2c->saa716x;

	SAA716x_EPWR(I2C_DEV, I2C_CONTROL, 0xc0); /* Start: SCL/SDA High */
	msleep(10);
	SAA716x_EPWR(I2C_DEV, I2C_CONTROL, 0x80);
	msleep(10);
	SAA716x_EPWR(I2C_DEV, I2C_CONTROL, 0x00);
	msleep(10);
	SAA716x_EPWR(I2C_DEV, I2C_CONTROL, 0x80);
	msleep(10);
	SAA716x_EPWR(I2C_DEV, I2C_CONTROL, 0xc0);

	return;
}

static void saa716x_i2c_hwdeinit(struct saa716x_i2c *i2c, u32 I2C_DEV)
{
	struct saa716x_dev *saa716x = i2c->saa716x;

	/* Disable all interrupts and clear status */
	SAA716x_EPWR(I2C_DEV, INT_CLR_ENABLE, 0x1fff);
	SAA716x_EPWR(I2C_DEV, INT_CLR_STATUS, 0x1fff);
}

static int saa716x_i2c_hwinit(struct saa716x_i2c *i2c, u32 I2C_DEV)
{
	struct saa716x_dev *saa716x = i2c->saa716x;
	struct i2c_adapter *adapter = &i2c->i2c_adapter;

	int i, err = 0;
	u32 reg;

	reg = SAA716x_EPRD(I2C_DEV, I2C_STATUS);
	if (!(reg & 0xd)) {
		dprintk(SAA716x_ERROR, 1, "Adapter (%02x) %s RESET failed, Exiting !",
			I2C_DEV, adapter->name);
		err = -EIO;
		goto exit;
	}

	/* Flush queue */
	SAA716x_EPWR(I2C_DEV, I2C_CONTROL, 0xcc);

	/* Disable all interrupts and clear status */
	SAA716x_EPWR(I2C_DEV, INT_CLR_ENABLE, 0x1fff);
	SAA716x_EPWR(I2C_DEV, INT_CLR_STATUS, 0x1fff);

	/* Reset I2C Core and generate a delay */
	SAA716x_EPWR(I2C_DEV, I2C_CONTROL, 0xc1);

	for (i = 0; i < 100; i++) {
		reg = SAA716x_EPRD(I2C_DEV, I2C_CONTROL);
		if (reg == 0xc0) {
			dprintk(SAA716x_ERROR, 1, "Adapter (%02x) %s RESET",
				I2C_DEV, adapter->name);
			break;
		}
		msleep(1);

		if (i == 99)
			err = -EIO;
	}

	if (err) {
		dprintk(SAA716x_ERROR, 1, "Adapter (%02x) %s RESET failed",
			I2C_DEV, adapter->name);

		saa716x_term_xfer(i2c, I2C_DEV);
		err = -EIO;
		goto exit;
	}

	/* I2C Rate Setup */
	switch (i2c->i2c_rate) {
	case SAA716x_I2C_RATE_400:

		dprintk(SAA716x_DEBUG, 1, "Initializing Adapter %s @ 400k", adapter->name);
		SAA716x_EPWR(I2C_DEV, I2C_CLOCK_DIVISOR_HIGH, 0x1a); /* 0.5 * 27MHz/400kHz */
		SAA716x_EPWR(I2C_DEV, I2C_CLOCK_DIVISOR_LOW,  0x21); /* 0.5 * 27MHz/400kHz */
		SAA716x_EPWR(I2C_DEV, I2C_SDA_HOLD, 0x14);
		break;

	case SAA716x_I2C_RATE_100:

		dprintk(SAA716x_DEBUG, 1, "Initializing Adapter %s @ 100k", adapter->name);
		SAA716x_EPWR(I2C_DEV, I2C_CLOCK_DIVISOR_HIGH, 0x68); /* 0.5 * 27MHz/100kHz */
		SAA716x_EPWR(I2C_DEV, I2C_CLOCK_DIVISOR_LOW,  0x87); /* 0.5 * 27MHz/100kHz */
		SAA716x_EPWR(I2C_DEV, I2C_SDA_HOLD, 0x60);
		break;

	default:

		dprintk(SAA716x_ERROR, 1, "Adapter %s Unknown Rate (Rate=0x%02x)",
			adapter->name,
			i2c->i2c_rate);

		break;
	}

	/* Disable all interrupts and clear status */
	SAA716x_EPWR(I2C_DEV, INT_CLR_ENABLE, 0x1fff);
	SAA716x_EPWR(I2C_DEV, INT_CLR_STATUS, 0x1fff);
#if 0
	/* Enabled interrupts:
	* Master Transaction Done (),
	* Master Arbitration Failure,
	* Master Transaction No Ack,
	* I2C Error IBE
	* Master Transaction Data Request
	* (0xc7)
	*/
	msleep(5);

	SAA716x_EPWR(I2C_DEV[i],
			INT_SET_ENABLE,
			I2C_MASTER_INTERRUPT_MTDR	| \
			I2C_ERROR_IBE		| \
			I2C_ENABLE_MTNA		| \
			I2C_ENABLE_MAF		| \
			I2C_ENABLE_MTD);

	/* Check interrupt enable status */
	reg = SAA716x_EPRD(I2C_DEV[i], INT_ENABLE);
	if (reg != 0xc7) {

		dprintk(SAA716x_ERROR, 1,
			"Adapter (%d) %s Interrupt enable failed, Exiting !",
			i,
			adapter->name);

		err = -EIO;
		goto exit;
	}
#endif
	/* Check status */
	reg = SAA716x_EPRD(I2C_DEV, I2C_STATUS);
	if (!(reg & 0xd)) {

		dprintk(SAA716x_ERROR, 1,
			"Adapter (%02x) %s has bad state, Exiting !",
			I2C_DEV,
			adapter->name);

		err = -EIO;
		goto exit;
	}
#if 0
	saa716x_add_irqvector(saa716x,
				i2c_vec[i].vector,
				i2c_vec[i].edge,
				i2c_vec[i].handler,
				SAA716x_I2C_ADAPTER(i));
#endif
	reg = SAA716x_EPRD(CGU, CGU_SCR_3);
	dprintk(SAA716x_DEBUG, 1, "Adapter (%02x) Autowake <%d> Active <%d>",
		I2C_DEV,
		(reg >> 1) & 0x01,
		reg & 0x01);

	return 0;
exit:
	return err;
}

static int saa716x_i2c_send(struct saa716x_i2c *i2c, u32 I2C_DEV, u32 data)
{
	struct saa716x_dev *saa716x = i2c->saa716x;
	int i, err = 0;
	u32 reg;

	/* Check FIFO status before TX */
	reg = SAA716x_EPRD(I2C_DEV, I2C_STATUS);
	i2c->stat_tx_prior = reg;
	if (reg & SAA716x_I2C_TXBUSY) {
		for (i = 0; i < 100; i++) {
			/* TODO! check for hotplug devices */
			msleep(10);
			reg = SAA716x_EPRD(I2C_DEV, I2C_STATUS);

			if (reg & SAA716x_I2C_TXBUSY) {
				dprintk(SAA716x_ERROR, 1, "FIFO full or Blocked");

				err = saa716x_i2c_hwinit(i2c, I2C_DEV);
				if (err < 0) {
					dprintk(SAA716x_ERROR, 1, "Error Reinit");
					err = -EIO;
					goto exit;
				}
			} else {
				break;
			}
		}
	}

	/* Write to FIFO */
	SAA716x_EPWR(I2C_DEV, TX_FIFO, data);

	/* Check for data write */
	for (i = 0; i < 1000; i++) {
		/* TODO! check for hotplug devices */
		reg = SAA716x_EPRD(I2C_DEV, I2C_STATUS);
		if (reg & I2C_TRANSMIT_CLEAR) {
			break;
		}
	}
	i2c->stat_tx_done = reg;

	if (!(reg & I2C_TRANSMIT_CLEAR)) {
		dprintk(SAA716x_ERROR, 1, "TXFIFO not empty after Timeout, tried %d loops!", i);
		err = -EIO;
		goto exit;
	}

	return err;

exit:
	dprintk(SAA716x_ERROR, 1, "I2C Send failed (Err=%d)", err);
	return err;
}

static int saa716x_i2c_recv(struct saa716x_i2c *i2c, u32 I2C_DEV, u32 *data)
{
	struct saa716x_dev *saa716x = i2c->saa716x;
	int i, err = 0;
	u32 reg;

	/* Check FIFO status before RX */
	for (i = 0; i < 1000; i++) {
		reg = SAA716x_EPRD(I2C_DEV, I2C_STATUS);
		if (!(reg & SAA716x_I2C_RXBUSY)) {
			break;
		}
	}
	if (reg & SAA716x_I2C_RXBUSY) {
		dprintk(SAA716x_INFO, 1, "FIFO empty");
		err = -EIO;
		goto exit;
	}

	/* Read from FIFO */
	*data = SAA716x_EPRD(I2C_DEV, RX_FIFO);

	return 0;
exit:
	dprintk(SAA716x_ERROR, 1, "Error Reading data, err=%d", err);
	return err;
}

static int saa716x_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num)
{
	struct saa716x_i2c *i2c		= i2c_get_adapdata(adapter);
	struct saa716x_dev *saa716x	= i2c->saa716x;

	u32 DEV = SAA716x_I2C_BUS(i2c->i2c_dev);
	int i, j, err = 0;
	int t;
	u32 data;

	mutex_lock(&i2c->i2c_lock);

	for (t = 0; t < 3; t++) {
		for (i = 0; i < num; i++) {
			/* first write START width I2C address */
			data = (msgs[i].addr << 1) | I2C_START_BIT;
			if (msgs[i].flags & I2C_M_RD)
				data |= 1;
			err = saa716x_i2c_send(i2c, DEV, data);
			if (err < 0) {
				dprintk(SAA716x_ERROR, 1, "Address write failed");
				err = -EIO;
				goto retry;
			}
			/* now read or write the data */
			for (j = 0; j < msgs[i].len; j++) {
				if (msgs[i].flags & I2C_M_RD)
					data = 0x00; /* dummy write for reading */
				else {
					data = msgs[i].buf[j];
				}
				if (i == (num - 1) && j == (msgs[i].len - 1))
					data |= I2C_STOP_BIT;
				err = saa716x_i2c_send(i2c, DEV, data);
				if (err < 0) {
					dprintk(SAA716x_ERROR, 1, "Data send failed");
					err = -EIO;
					goto retry;
				}
				if (msgs[i].flags & I2C_M_RD) {
					err = saa716x_i2c_recv(i2c, DEV, &data);
					if (err < 0) {
						dprintk(SAA716x_ERROR, 1, "Data receive failed");
						err = -EIO;
						goto retry;
					}
					msgs[i].buf[j] = data;
				}
			}
		}
		break;
retry:
		dprintk(SAA716x_INFO, 1, "Error in Transfer, try %d", t);
		err = saa716x_i2c_hwinit(i2c, DEV);
		if (err < 0) {
			dprintk(SAA716x_ERROR, 1, "Error Reinit");
			err = -EIO;
			goto bail_out;
		}
	}

	mutex_unlock(&i2c->i2c_lock);
	if (t == 3)
		return -EIO;
	else
		return num;

bail_out:
	dprintk(SAA716x_ERROR, 1, "ERROR: Bailing out <%d>", err);
	mutex_unlock(&i2c->i2c_lock);
	return err;
}

static u32 saa716x_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm saa716x_algo = {
	.master_xfer	= saa716x_i2c_xfer,
	.functionality	= saa716x_i2c_func,
};

#define I2C_HW_B_SAA716x		0x12

struct saa716x_i2cvec {
	u32			vector;
	enum saa716x_edge	edge;
	irqreturn_t (*handler)(int irq, void *dev_id);
};

static const struct saa716x_i2cvec i2c_vec[] = {
	{
		.vector		= I2CINT_0,
		.edge		= SAA716x_EDGE_RISING,
		.handler	= saa716x_i2c_irq
	}, {
		.vector 	= I2CINT_1,
		.edge		= SAA716x_EDGE_RISING,
		.handler	= saa716x_i2c_irq
	}
};

int __devinit saa716x_i2c_init(struct saa716x_dev *saa716x)
{
	struct pci_dev *pdev		= saa716x->pdev;
	struct saa716x_i2c *i2c		= saa716x->i2c;
	struct i2c_adapter *adapter	= NULL;

	int i, err = 0;

	dprintk(SAA716x_DEBUG, 1, "Initializing SAA%02x I2C Core",
		saa716x->pdev->device);

	for (i = 0; i < SAA716x_I2C_ADAPTERS; i++) {

		mutex_init(&i2c->i2c_lock);

		i2c->i2c_dev	= i;
		i2c->i2c_rate	= saa716x->config->i2c_rate[i];
		adapter		= &i2c->i2c_adapter;

		if (adapter != NULL) {

			i2c_set_adapdata(adapter, i2c);

			strcpy(adapter->name, SAA716x_I2C_ADAPTER(i));

			adapter->owner		= THIS_MODULE;
			adapter->algo		= &saa716x_algo;
			adapter->algo_data 	= saa716x;
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,35)
			adapter->id		= I2C_HW_B_SAA716x;
#endif
			adapter->timeout	= 500; /* FIXME ! */
			adapter->retries	= 3; /* FIXME ! */
			adapter->dev.parent	= &pdev->dev;

			dprintk(SAA716x_DEBUG, 1, "Initializing adapter (%d) %s",
				i,
				adapter->name);

			err = i2c_add_adapter(adapter);
			if (err < 0) {
				dprintk(SAA716x_ERROR, 1, "Adapter (%d) %s init failed", i, adapter->name);
				goto exit;
			}

			i2c->saa716x = saa716x;
			saa716x_i2c_hwinit(i2c, SAA716x_I2C_BUS(i));
		}
		i2c++;
	}

	dprintk(SAA716x_DEBUG, 1, "SAA%02x I2C Core succesfully initialized",
		saa716x->pdev->device);

	return 0;
exit:
	return err;
}
EXPORT_SYMBOL_GPL(saa716x_i2c_init);

int __devexit saa716x_i2c_exit(struct saa716x_dev *saa716x)
{
	struct saa716x_i2c *i2c		= saa716x->i2c;
	struct i2c_adapter *adapter	= NULL;
	int i, err = 0;

	dprintk(SAA716x_DEBUG, 1, "Removing SAA%02x I2C Core", saa716x->pdev->device);

	for (i = 0; i < SAA716x_I2C_ADAPTERS; i++) {

		adapter = &i2c->i2c_adapter;
#if 0
		saa716x_remove_irqvector(saa716x, i2c_vec[i].vector);
#endif
		saa716x_i2c_hwdeinit(i2c, SAA716x_I2C_BUS(i));
		dprintk(SAA716x_DEBUG, 1, "Removing adapter (%d) %s", i, adapter->name);

		err = i2c_del_adapter(adapter);
		if (err < 0) {
			dprintk(SAA716x_ERROR, 1, "Adapter (%d) %s remove failed", i, adapter->name);
			goto exit;
		}
		i2c++;
	}
	dprintk(SAA716x_DEBUG, 1, "SAA%02x I2C Core succesfully removed", saa716x->pdev->device);

	return 0;

exit:
	return err;
}
EXPORT_SYMBOL_GPL(saa716x_i2c_exit);
