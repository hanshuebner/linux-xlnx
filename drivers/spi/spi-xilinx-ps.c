/*
 *
 * Xilinx PS SPI controller driver (master mode only)
 *
 * (c) 2008-2011 Xilinx, Inc.
 *
 * based on Blackfin On-Chip SPI Driver (spi_bfin5xx.c)
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/xilinx_devices.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/delay.h>

/*
 * Name of this driver
 */
#define XSPIPS_NAME		"xspips"

/*
 * Register offset definitions
 */
#define XSPIPS_CR_OFFSET	0x00 /* Configuration  Register, RW */
#define XSPIPS_ISR_OFFSET	0x04 /* Interrupt Status Register, RO */
#define XSPIPS_IER_OFFSET	0x08 /* Interrupt Enable Register, WO */
#define XSPIPS_IDR_OFFSET	0x0c /* Interrupt Disable Register, WO */
#define XSPIPS_IMR_OFFSET	0x10 /* Interrupt Enabled Mask Register, RO */
#define XSPIPS_ER_OFFSET	0x14 /* Enable/Disable Register, RW */
#define XSPIPS_DR_OFFSET	0x18 /* Delay Register, RW */
#define XSPIPS_TXD_OFFSET	0x1C /* Data Transmit Register, WO */
#define XSPIPS_RXD_OFFSET	0x20 /* Data Receive Register, RO */
#define XSPIPS_SICR_OFFSET	0x24 /* Slave Idle Count Register, RW */
#define XSPIPS_THLD_OFFSET	0x28 /* Transmit FIFO Watermark Register,RW */

/*
 * SPI Configuration Register bit Masks
 *
 * This register contains various control bits that affect the operation
 * of the SPI controller
 */
#define XSPIPS_CR_MANSTRT_MASK	0x00010000 /* Manual TX Start */
#define XSPIPS_CR_CPHA_MASK	0x00000004 /* Clock Phase Control */
#define XSPIPS_CR_CPOL_MASK	0x00000002 /* Clock Polarity Control */
#define XSPIPS_CR_SSCTRL_MASK	0x00003C00 /* Slave Select Mask */

/*
 * SPI Interrupt Registers bit Masks
 *
 * All the four interrupt registers (Status/Mask/Enable/Disable) have the same
 * bit definitions.
 */
#define XSPIPS_IXR_TXOW_MASK	0x00000004 /* SPI TX FIFO Overwater */
#define XSPIPS_IXR_MODF_MASK	0x00000002 /* SPI Mode Fault */
#define XSPIPS_IXR_RXNEMTY_MASK 0x00000010 /* SPI RX FIFO Not Empty */
#define XSPIPS_IXR_ALL_MASK	(XSPIPS_IXR_TXOW_MASK | XSPIPS_IXR_MODF_MASK)

/*
 * SPI Enable Register bit Masks
 *
 * This register is used to enable or disable the SPI controller
 */
#define XSPIPS_ER_ENABLE_MASK	0x00000001 /* SPI Enable Bit Mask */

/*
 * The spi->mode bits understood by this driver
 */
#define MODEBITS		(SPI_CPOL | SPI_CPHA)

/*
 * Definitions for the status of queue
 */
#define XSPIPS_QUEUE_STOPPED	0
#define XSPIPS_QUEUE_RUNNING	1

/*
 * Macros for the SPI controller read/write
 */
#define xspips_read(addr)	__raw_readl(addr)
#define xspips_write(addr, val)	__raw_writel((val), (addr))


/**
 * struct xspips - This definition defines spi driver instance
 * @workqueue:		Queue of all the transfers
 * @work:		Information about current transfer
 * @queue:		Head of the queue
 * @queue_state:	Queue status
 * @regs:		Virtual address of the SPI controller registers
 * @input_clk_hz:	Input clock frequency of the SPI controller in Hz
 * @irq:		IRQ number
 * @speed_hz:		Current SPI bus clock speed in Hz
 * @trans_queue_lock:	Lock used for accessing transfer queue
 * @ctrl_reg_lock:	Lock used for accessing configuration register
 * @txbuf:		Pointer	to the TX buffer
 * @rxbuf:		Pointer to the RX buffer
 * @remaining_bytes:	Number of bytes left to transfer
 * @dev_busy:		Device busy flag
 * @done:		Transfer complete status
 **/
struct xspips {
	struct workqueue_struct *workqueue;
	struct work_struct work;
	struct list_head queue;
	int queue_state;
	void __iomem *regs;
	u32 input_clk_hz;
	u32 irq;
	u32 speed_hz;
	spinlock_t trans_queue_lock;
	spinlock_t ctrl_reg_lock;
	const u8 *txbuf;
	u8 *rxbuf;
	int remaining_bytes;
	u8 dev_busy;
	struct completion done;
};


/**
 * xspips_init_hw - Initialize the hardware and configure the SPI controller
 * @regs_base:		Base address of SPI controller
 *
 * On reset the SPI controller is configured to be in master mode, baud rate
 * divisor is set to 2, threshold value for TX FIFO not full interrupt is set
 * to 1 and size of the word to be transferred as 8 bit.
 * This function initializes the SPI controller to disable and clear all the
 * interrupts, enable manual slave select and manual start, deselect all the
 * chip select lines, and enable the SPI controller.
 **/
static void xspips_init_hw(void __iomem *regs_base)
{
	xspips_write(regs_base + XSPIPS_ER_OFFSET, ~XSPIPS_ER_ENABLE_MASK);
	xspips_write(regs_base + XSPIPS_IDR_OFFSET, 0x7F);

	/* Clear the RX FIFO */
	while (xspips_read(regs_base + XSPIPS_ISR_OFFSET) &
			XSPIPS_IXR_RXNEMTY_MASK)
		xspips_read(regs_base + XSPIPS_RXD_OFFSET);

	xspips_write(regs_base + XSPIPS_ISR_OFFSET, 0x7F);
	xspips_write(regs_base + XSPIPS_CR_OFFSET, 0x0000FC01);
	xspips_write(regs_base + XSPIPS_ER_OFFSET, XSPIPS_ER_ENABLE_MASK);
}

/**
 * xspips_chipselect - Select or deselect the chip select line
 * @spi:	Pointer to the spi_device structure
 * @is_on:	Select(1) or deselect (0) the chip select line
 **/
static void xspips_chipselect(struct spi_device *spi, int is_on)
{
	struct xspips *xspi = spi_master_get_devdata(spi->master);
	u32 ctrl_reg;
	unsigned long flags;

	spin_lock_irqsave(&xspi->ctrl_reg_lock, flags);

	ctrl_reg = xspips_read(xspi->regs + XSPIPS_CR_OFFSET);

	if (is_on) {
		/* Select the slave */
		ctrl_reg &= ~XSPIPS_CR_SSCTRL_MASK;
		ctrl_reg |= (((~(0x0001 << spi->chip_select)) << 10) &
				XSPIPS_CR_SSCTRL_MASK);
	} else
		/* Deselect the slave */
		ctrl_reg |= XSPIPS_CR_SSCTRL_MASK;

	xspips_write(xspi->regs + XSPIPS_CR_OFFSET, ctrl_reg);

	spin_unlock_irqrestore(&xspi->ctrl_reg_lock, flags);
}

/**
 * xspips_setup_transfer - Configure SPI controller for specified transfer
 * @spi:	Pointer to the spi_device structure
 * @transfer:	Pointer to the spi_transfer structure which provides information
 *		about next transfer setup parameters
 *
 * Sets the operational mode of SPI controller for the next SPI transfer and
 * sets the requested clock frequency.
 *
 * returns:	0 on success and error value on error
 *
 * Note: If the requested frequency is not an exact match with what can be
 * obtained using the prescalar value the driver sets the clock frequency which
 * is lower than the requested frequency (maximum lower) for the transfer. If
 * the requested frequency is higher or lower than that is supported by the SPI
 * controller the driver will set the highest or lowest frequency supported by
 * controller.
 **/
static int xspips_setup_transfer(struct spi_device *spi,
		struct spi_transfer *transfer)
{
	struct xspips *xspi = spi_master_get_devdata(spi->master);
	u8 bits_per_word;
	u32 ctrl_reg;
	u32 req_hz;
	u32 baud_rate_val;
	unsigned long flags;

	bits_per_word = (transfer) ?
			transfer->bits_per_word : spi->bits_per_word;
	req_hz = (transfer) ? transfer->speed_hz : spi->max_speed_hz;

	if (spi->mode & ~MODEBITS) {
		dev_err(&spi->dev, "%s, unsupported mode bits %x\n",
			__func__, spi->mode & ~MODEBITS);
		return -EINVAL;
	}

	if (bits_per_word != 8) {
		dev_err(&spi->dev, "%s, unsupported bits per word %x\n",
			__func__, spi->bits_per_word);
		return -EINVAL;
	}

	spin_lock_irqsave(&xspi->ctrl_reg_lock, flags);

	ctrl_reg = xspips_read(xspi->regs + XSPIPS_CR_OFFSET);

	/* Set the SPI clock phase and clock polarity */
	ctrl_reg &= (~XSPIPS_CR_CPHA_MASK) & (~XSPIPS_CR_CPOL_MASK);
	if (spi->mode & SPI_CPHA)
		ctrl_reg |= XSPIPS_CR_CPHA_MASK;
	if (spi->mode & SPI_CPOL)
		ctrl_reg |= XSPIPS_CR_CPOL_MASK;

	/* Set the clock frequency */
	if (xspi->speed_hz != req_hz) {
		baud_rate_val = 0;
		while ((baud_rate_val < 8)  &&
			(xspi->input_clk_hz / (2 << baud_rate_val)) > req_hz) {
				baud_rate_val++;
		}

		ctrl_reg &= 0xFFFFFFC7;
		ctrl_reg |= (baud_rate_val << 3);

		xspi->speed_hz = (xspi->input_clk_hz / (2 << baud_rate_val));
	}

	xspips_write(xspi->regs + XSPIPS_CR_OFFSET, ctrl_reg);

	spin_unlock_irqrestore(&xspi->ctrl_reg_lock, flags);

	dev_dbg(&spi->dev, "%s, mode %d, %u bits/w, %u clock speed\n",
		__func__, spi->mode & MODEBITS, spi->bits_per_word,
		xspi->speed_hz);

	return 0;
}

/**
 * xspips_setup - Configure the SPI controller
 * @spi:	Pointer to the spi_device structure
 *
 * Sets the operational mode of SPI controller for the next SPI transfer, sets
 * the baud rate and divisor value to setup the requested spi clock.
 *
 * returns:	0 on success and error value on error
 **/
static int xspips_setup(struct spi_device *spi)
{
	if ((spi->mode & SPI_LSB_FIRST) != 0)
		return -EINVAL;

	if (!spi->max_speed_hz)
		return -EINVAL;

	if (!spi->bits_per_word)
		spi->bits_per_word = 8;

	return xspips_setup_transfer(spi, NULL);
}

/**
 * xspips_fill_tx_fifo - Fills the TX FIFO with as many bytes as possible
 * @xspi:	Pointer to the xspips structure
 **/
static void xspips_fill_tx_fifo(struct xspips *xspi)
{
	while ((xspips_read(xspi->regs + XSPIPS_ISR_OFFSET) & 0x00000008) == 0
		&& (xspi->remaining_bytes > 0)) {
		if (xspi->txbuf)
			xspips_write(xspi->regs + XSPIPS_TXD_OFFSET,
					*xspi->txbuf++);
		else
			xspips_write(xspi->regs + XSPIPS_TXD_OFFSET, 0);

		xspi->remaining_bytes--;
	}
}

/**
 * xspips_irq - Interrupt service routine of the SPI controller
 * @irq:	IRQ number
 * @dev_id:	Pointer to the xspi structure
 *
 * This function handles TX empty and Mode Fault interrupts only.
 * On TX empty interrupt this function reads the received data from RX FIFO and
 * fills the TX FIFO if there is any data remaining to be transferred.
 * On Mode Fault interrupt this function indicates that transfer is completed,
 * the SPI subsystem will identify the error as the remaining bytes to be
 * transferred is non-zero.
 *
 * returns:	IRQ_HANDLED always
 **/
static irqreturn_t xspips_irq(int irq, void *dev_id)
{
	struct xspips *xspi = dev_id;
	u32 intr_status;

	intr_status = xspips_read(xspi->regs + XSPIPS_ISR_OFFSET);
	xspips_write(xspi->regs + XSPIPS_ISR_OFFSET, intr_status);
	xspips_write(xspi->regs + XSPIPS_IDR_OFFSET, XSPIPS_IXR_ALL_MASK);

	if (intr_status & XSPIPS_IXR_MODF_MASK) {
		/* Indicate that transfer is completed, the SPI subsystem will
		 * identify the error as the remaining bytes to be
		 * transferred is non-zero */
		complete(&xspi->done);
	} else if (intr_status & XSPIPS_IXR_TXOW_MASK) {
		u32 ctrl_reg;

		/* Read out the data from the RX FIFO */
		while (xspips_read(xspi->regs + XSPIPS_ISR_OFFSET) &
				XSPIPS_IXR_RXNEMTY_MASK) {
			u8 data;

			data = xspips_read(xspi->regs + XSPIPS_RXD_OFFSET);
			if (xspi->rxbuf)
				*xspi->rxbuf++ = data;

			/* Data memory barrier is placed here to ensure that
			 * data read operation is completed before the status
			 * read is initiated. Without dmb, there are chances
			 * that data and status reads will appear at the SPI
			 * peripheral back-to-back which results in an
			 * incorrect status read.
			 */
			dmb();
		}

		if (xspi->remaining_bytes) {
			/* There is more data to send */
			xspips_fill_tx_fifo(xspi);

			xspips_write(xspi->regs + XSPIPS_IER_OFFSET,
					XSPIPS_IXR_ALL_MASK);

			spin_lock(&xspi->ctrl_reg_lock);

			ctrl_reg = xspips_read(xspi->regs + XSPIPS_CR_OFFSET);
			ctrl_reg |= XSPIPS_CR_MANSTRT_MASK;
			xspips_write(xspi->regs + XSPIPS_CR_OFFSET, ctrl_reg);

			spin_unlock(&xspi->ctrl_reg_lock);
		} else {
			/* Transfer is completed */
			complete(&xspi->done);
		}
	}

	return IRQ_HANDLED;
}

/**
 * xspips_start_transfer - Initiates the SPI transfer
 * @spi:	Pointer to the spi_device structure
 * @transfer:	Pointer to the spi_transfer structure which provide information
 *		about next transfer parameters
 *
 * This function fills the TX FIFO, starts the SPI transfer, and waits for the
 * transfer to be completed.
 *
 * returns:	Number of bytes transferred in the last transfer
 **/
static int xspips_start_transfer(struct spi_device *spi,
			struct spi_transfer *transfer)
{
	struct xspips *xspi = spi_master_get_devdata(spi->master);
	u32 ctrl_reg;
	unsigned long flags;

	xspi->txbuf = transfer->tx_buf;
	xspi->rxbuf = transfer->rx_buf;
	xspi->remaining_bytes = transfer->len;
	INIT_COMPLETION(xspi->done);

	xspips_fill_tx_fifo(xspi);

	xspips_write(xspi->regs + XSPIPS_IER_OFFSET, XSPIPS_IXR_ALL_MASK);

	spin_lock_irqsave(&xspi->ctrl_reg_lock, flags);

	/* Start the transfer by enabling manual start bit */
	ctrl_reg = xspips_read(xspi->regs + XSPIPS_CR_OFFSET);
	ctrl_reg |= XSPIPS_CR_MANSTRT_MASK;
	xspips_write(xspi->regs + XSPIPS_CR_OFFSET, ctrl_reg);

	spin_unlock_irqrestore(&xspi->ctrl_reg_lock, flags);

	wait_for_completion(&xspi->done);

	return (transfer->len) - (xspi->remaining_bytes);
}

/**
 * xspips_work_queue - Get the transfer request from queue to perform transfers
 * @work:	Pointer to the work_struct structure
 **/
static void xspips_work_queue(struct work_struct *work)
{
	struct xspips *xspi = container_of(work, struct xspips, work);
	unsigned long flags;

	spin_lock_irqsave(&xspi->trans_queue_lock, flags);
	xspi->dev_busy = 1;

	if (list_empty(&xspi->queue) ||
		xspi->queue_state == XSPIPS_QUEUE_STOPPED) {
		xspi->dev_busy = 0;
		spin_unlock_irqrestore(&xspi->trans_queue_lock, flags);
		return;
	}

	while (!list_empty(&xspi->queue)) {
		struct spi_message *msg;
		struct spi_device *spi;
		struct spi_transfer *transfer = NULL;
		unsigned cs_change = 1;
		int status = 0;

		msg = container_of(xspi->queue.next, struct spi_message, queue);
		list_del_init(&msg->queue);
		spin_unlock_irqrestore(&xspi->trans_queue_lock, flags);
		spi = msg->spi;

		list_for_each_entry(transfer, &msg->transfers, transfer_list) {
			if (transfer->bits_per_word || transfer->speed_hz) {
				status = xspips_setup_transfer(spi, transfer);
				if (status < 0)
					break;
			}

			if (cs_change)
				xspips_chipselect(spi, 1);

			cs_change = transfer->cs_change;

			if (!transfer->tx_buf && !transfer->rx_buf &&
				transfer->len) {
				status = -EINVAL;
				break;
			}

			if (transfer->len)
				status = xspips_start_transfer(spi, transfer);

			if (status != transfer->len) {
				if (status > 0)
					status = -EMSGSIZE;
				break;
			}
			msg->actual_length += status;
			status = 0;

			if (transfer->delay_usecs)
				udelay(transfer->delay_usecs);

			if (!cs_change)
				continue;
			if (transfer->transfer_list.next == &msg->transfers)
				break;

			xspips_chipselect(spi, 0);
		}

		msg->status = status;
		msg->complete(msg->context);

		xspips_setup_transfer(spi, NULL);

		if (!(status == 0 && cs_change))
			xspips_chipselect(spi, 0);

		spin_lock_irqsave(&xspi->trans_queue_lock, flags);
	}
	xspi->dev_busy = 0;
	spin_unlock_irqrestore(&xspi->trans_queue_lock, flags);
}

/**
 * xspips_transfer - Add a new transfer request at the tail of work queue
 * @spi:	Pointer to the spi_device structure
 * @message:	Pointer to the spi_transfer structure which provide information
 *		about next transfer parameters
 *
 * returns:	0 on success and error value on error
 **/
static int xspips_transfer(struct spi_device *spi, struct spi_message *message)
{
	struct xspips *xspi = spi_master_get_devdata(spi->master);
	struct spi_transfer *transfer;
	unsigned long flags;

	if (xspi->queue_state == XSPIPS_QUEUE_STOPPED)
		return -ESHUTDOWN;

	message->actual_length = 0;
	message->status = -EINPROGRESS;

	/* Check each transfer's parameters */
	list_for_each_entry(transfer, &message->transfers, transfer_list) {
		u8 bits_per_word =
			transfer->bits_per_word ? : spi->bits_per_word;

		bits_per_word = bits_per_word ? : 8;
		if (!transfer->tx_buf && !transfer->rx_buf && transfer->len)
			return -EINVAL;
		if (bits_per_word != 8)
			return -EINVAL;
	}

	spin_lock_irqsave(&xspi->trans_queue_lock, flags);
	list_add_tail(&message->queue, &xspi->queue);
	if (!xspi->dev_busy)
		queue_work(xspi->workqueue, &xspi->work);
	spin_unlock_irqrestore(&xspi->trans_queue_lock, flags);

	return 0;
}

/**
 * xspips_start_queue - Starts the queue of the SPI driver
 * @xspi:	Pointer to the xspips structure
 *
 * returns:	0 on success and error value on error
 **/
static inline int xspips_start_queue(struct xspips *xspi)
{
	unsigned long flags;

	spin_lock_irqsave(&xspi->trans_queue_lock, flags);

	if (xspi->queue_state == XSPIPS_QUEUE_RUNNING || xspi->dev_busy) {
		spin_unlock_irqrestore(&xspi->trans_queue_lock, flags);
		return -EBUSY;
	}

	xspi->queue_state = XSPIPS_QUEUE_RUNNING;
	spin_unlock_irqrestore(&xspi->trans_queue_lock, flags);

	return 0;
}

/**
 * xspips_stop_queue - Stops the queue of the SPI driver
 * @xspi:	Pointer to the xspips structure
 *
 * This function waits till queue is empty and then stops the queue.
 * Maximum time out is set to 5 seconds.
 *
 * returns:	0 on success and error value on error
 **/
static inline int xspips_stop_queue(struct xspips *xspi)
{
	unsigned long flags;
	unsigned limit = 500;
	int ret = 0;

	if (xspi->queue_state != XSPIPS_QUEUE_RUNNING)
		return ret;

	spin_lock_irqsave(&xspi->trans_queue_lock, flags);

	while ((!list_empty(&xspi->queue) || xspi->dev_busy) && limit--) {
		spin_unlock_irqrestore(&xspi->trans_queue_lock, flags);
		msleep(10);
		spin_lock_irqsave(&xspi->trans_queue_lock, flags);
	}

	if (!list_empty(&xspi->queue) || xspi->dev_busy)
		ret = -EBUSY;

	if (ret == 0)
		xspi->queue_state = XSPIPS_QUEUE_STOPPED;

	spin_unlock_irqrestore(&xspi->trans_queue_lock, flags);

	return ret;
}

/**
 * xspips_destroy_queue - Destroys the queue of the SPI driver
 * @xspi:	Pointer to the xspips structure
 *
 * returns:	0 on success and error value on error
 **/
static inline int xspips_destroy_queue(struct xspips *xspi)
{
	int ret;

	ret = xspips_stop_queue(xspi);
	if (ret != 0)
		return ret;

	destroy_workqueue(xspi->workqueue);

	return 0;
}

/**
 * xspips_probe - Probe method for the SPI driver
 * @dev:	Pointer to the platform_device structure
 *
 * This function initializes the driver data structures and the hardware.
 *
 * returns:	0 on success and error value on error
 **/
static int __devinit xspips_probe(struct platform_device *dev)
{
	int ret = 0;
	struct spi_master *master;
	struct xspips *xspi;
	struct resource *r;
#ifdef CONFIG_OF
	const unsigned int *prop;
#else
	struct xspi_platform_data *platform_info;
#endif

	master = spi_alloc_master(&dev->dev, sizeof(struct xspips));
	if (master == NULL)
		return -ENOMEM;

	xspi = spi_master_get_devdata(master);
	master->dev.of_node = dev->dev.of_node;
	platform_set_drvdata(dev, master);

#ifndef CONFIG_OF
	platform_info = dev->dev.platform_data;
	if (platform_info == NULL) {
		ret = -ENODEV;
		dev_err(&dev->dev, "platform data not available\n");
		goto put_master;
	}
#endif
	r = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (r == NULL) {
		ret = -ENODEV;
		dev_err(&dev->dev, "platform_get_resource failed\n");
		goto put_master;
	}

	if (!request_mem_region(r->start,
			r->end - r->start + 1, dev->name)) {
		ret = -ENXIO;
		dev_err(&dev->dev, "request_mem_region failed\n");
		goto put_master;
	}

	xspi->regs = ioremap(r->start, r->end - r->start + 1);
	if (xspi->regs == NULL) {
		ret = -ENOMEM;
		dev_err(&dev->dev, "ioremap failed\n");
		goto release_mem;
	}

	xspi->irq = platform_get_irq(dev, 0);
	if (xspi->irq < 0) {
		ret = -ENXIO;
		dev_err(&dev->dev, "irq number is negative\n");
		goto unmap_io;
	}

	ret = request_irq(xspi->irq, xspips_irq, 0, dev->name, xspi);
	if (ret != 0) {
		ret = -ENXIO;
		dev_err(&dev->dev, "request_irq failed\n");
		goto unmap_io;
	}

	/* SPI controller initializations */
	xspips_init_hw(xspi->regs);

	init_completion(&xspi->done);

#ifdef CONFIG_OF
	prop = of_get_property(dev->dev.of_node, "bus-num", NULL);
	if (prop)
		master->bus_num = be32_to_cpup(prop);
	else {
		ret = -ENXIO;
		dev_err(&dev->dev, "couldn't determine bus-num\n");
		goto free_irq;
	}

	prop = of_get_property(dev->dev.of_node, "num-chip-select", NULL);
	if (prop)
		master->num_chipselect = be32_to_cpup(prop);
	else {
		ret = -ENXIO;
		dev_err(&dev->dev, "couldn't determine num-chip-select\n");
		goto free_irq;
	}
#else
	master->bus_num = platform_info->bus_num;
	master->num_chipselect = platform_info->num_chipselect;
#endif
	master->setup = xspips_setup;
	master->transfer = xspips_transfer;

#ifdef CONFIG_OF
	prop = of_get_property(dev->dev.of_node, "speed-hz", NULL);
	if (prop) {
		xspi->input_clk_hz = be32_to_cpup(prop);
		xspi->speed_hz = xspi->input_clk_hz / 2;
	} else {
		ret = -ENXIO;
		dev_err(&dev->dev, "couldn't determine speed-hz\n");
		goto free_irq;
	}
#else
	xspi->input_clk_hz = platform_info->speed_hz;
	xspi->speed_hz = platform_info->speed_hz / 2;
#endif
	xspi->dev_busy = 0;

	INIT_LIST_HEAD(&xspi->queue);
	spin_lock_init(&xspi->trans_queue_lock);
	spin_lock_init(&xspi->ctrl_reg_lock);

	xspi->queue_state = XSPIPS_QUEUE_STOPPED;
	xspi->dev_busy = 0;

	INIT_WORK(&xspi->work, xspips_work_queue);
	xspi->workqueue =
		create_singlethread_workqueue(dev_name(&dev->dev));
	if (!xspi->workqueue) {
		ret = -ENOMEM;
		dev_err(&dev->dev, "problem initializing queue\n");
		goto free_irq;
	}

	ret = xspips_start_queue(xspi);
	if (ret != 0) {
		dev_err(&dev->dev, "problem starting queue\n");
		goto remove_queue;
	}

	ret = spi_register_master(master);
	if (ret) {
		dev_err(&dev->dev, "spi_register_master failed\n");
		goto remove_queue;
	}

	dev_info(&dev->dev, "at 0x%08X mapped to 0x%08X, irq=%d\n", r->start,
			(u32 __force)xspi->regs, xspi->irq);

	return ret;

remove_queue:
	(void)xspips_destroy_queue(xspi);
free_irq:
	free_irq(xspi->irq, xspi);
unmap_io:
	iounmap(xspi->regs);
release_mem:
	release_mem_region(r->start, r->end - r->start + 1);
put_master:
	platform_set_drvdata(dev, NULL);
	spi_master_put(master);
	return ret;
}

/**
 * xspips_remove - Remove method for the SPI driver
 * @dev:	Pointer to the platform_device structure
 *
 * This function is called if a device is physically removed from the system or
 * if the driver module is being unloaded. It frees all resources allocated to
 * the device.
 *
 * returns:	0 on success and error value on error
 **/
static int __devexit xspips_remove(struct platform_device *dev)
{
	struct spi_master *master = platform_get_drvdata(dev);
	struct xspips *xspi = spi_master_get_devdata(master);
	struct resource *r;
	int ret = 0;

	r = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (r == NULL) {
		dev_err(&dev->dev, "platform_get_resource failed\n");
		return -ENODEV;
	}

	ret = xspips_destroy_queue(xspi);
	if (ret != 0)
		return ret;

	xspips_write(xspi->regs + XSPIPS_ER_OFFSET, ~XSPIPS_ER_ENABLE_MASK);

	free_irq(xspi->irq, xspi);
	iounmap(xspi->regs);
	release_mem_region(r->start, r->end - r->start + 1);

	spi_unregister_master(master);
	spi_master_put(master);

	/* Prevent double remove */
	platform_set_drvdata(dev, NULL);

	dev_dbg(&dev->dev, "remove succeeded\n");
	return 0;

}

#ifdef CONFIG_PM
/**
 * xspips_suspend - Suspend method for the SPI driver
 * @dev:	Address of the platform_device structure
 * @msg:	Power management event message
 *
 * This function stops the SPI driver queue and disables the SPI controller
 *
 * returns:	0 on success and error value on error
 **/
static int xspips_suspend(struct platform_device *dev, pm_message_t msg)
{
	struct spi_master *master = platform_get_drvdata(dev);
	struct xspips *xspi = spi_master_get_devdata(master);
	int ret = 0;

	ret = xspips_stop_queue(xspi);
	if (ret != 0)
		return ret;

	xspips_write(xspi->regs + XSPIPS_ER_OFFSET, ~XSPIPS_ER_ENABLE_MASK);

	dev_dbg(&dev->dev, "suspend succeeded\n");
	return 0;
}

/**
 * xspips_resume - Resume method for the SPI driver
 * @dev:	Address of the platform_device structure
 *
 * This function starts the SPI driver queue and initializes the SPI controller
 *
 * returns:	0 on success and error value on error
 **/
static int xspips_resume(struct platform_device *dev)
{
	struct spi_master *master = platform_get_drvdata(dev);
	struct xspips *xspi = spi_master_get_devdata(master);
	int ret = 0;

	xspips_init_hw(xspi->regs);

	ret = xspips_start_queue(xspi);
	if (ret != 0) {
		dev_err(&dev->dev, "problem starting queue (%d)\n", ret);
		return ret;
	}

	dev_dbg(&dev->dev, "resume succeeded\n");
	return 0;
}
#else
#define xspips_suspend NULL
#define xspips_resume  NULL
#endif /* CONFIG_PM */

/* Work with hotplug and coldplug */
MODULE_ALIAS("platform:" XSPIPS_NAME);

#ifdef CONFIG_OF
static struct of_device_id xspips_of_match[] __devinitdata = {
	{ .compatible = "xlnx,ps7-spi-1.00.a", },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, xspips_of_match);
#else
#define xspips_of_match NULL
#endif /* CONFIG_OF */

/*
 * xspips_driver - This structure defines the SPI subsystem platform driver
 */
static struct platform_driver xspips_driver = {
	.probe	= xspips_probe,
	.remove	= __devexit_p(xspips_remove),
	.suspend = xspips_suspend,
	.resume = xspips_resume,
	.driver = {
		.name = XSPIPS_NAME,
		.owner = THIS_MODULE,
		.of_match_table = xspips_of_match,
	},
};

module_platform_driver(xspips_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx PS SPI driver");
MODULE_LICENSE("GPL");

