// SPDX-License-Identifier: GPL-2.0
/* Fixed-packet AXI-ST C2H receive ring for the read() interface. */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include "libxdma.h"

static unsigned int c2h_fifo_slots = 256;
module_param(c2h_fifo_slots, uint, 0444);
MODULE_PARM_DESC(c2h_fifo_slots, "C2H read FIFO slots (default 256, 0 disables, 2..512)");

static unsigned int c2h_fifo_credit_batch;
module_param(c2h_fifo_credit_batch, uint, 0444);
MODULE_PARM_DESC(c2h_fifo_credit_batch, "Consumed slots per credit update (0 selects min(32, slots), otherwise 1..slots)");

struct xdma_c2h_fifo {
	struct xdma_desc *descs;
	void *data;
	dma_addr_t desc_dma;
	dma_addr_t data_dma;
	u32 saved_control;
	u32 saved_credit_mode;
	u32 consumed_count;
	unsigned int next;
	unsigned int offset;
	unsigned int pending_credits;
	unsigned int credit_batch;
	unsigned int frame_bytes;
	bool running;
	int error;
	unsigned int peak_available;
	u64 packets;
	u64 bytes;
	u64 credit_writes;
	u64 empty_waits;
	u64 wait_ns;
};

bool xdma_fifo_enabled(struct xdma_engine *engine)
{
	return c2h_fifo_slots && engine->streaming &&
		engine->dir == DMA_FROM_DEVICE;
}

static struct sgdma_common_regs *fifo_common(struct xdma_engine *engine)
{
	BUILD_BUG_ON(offsetof(struct sgdma_common_regs, credit_mode_enable) != 0x20);
	return (struct sgdma_common_regs *)(engine->xdev->bar[
		engine->xdev->config_bar_idx] + 6 * TARGET_SPACING);
}

static void fifo_free(struct xdma_engine *engine, struct xdma_c2h_fifo *fifo)
{
	struct device *dev = &engine->xdev->pdev->dev;

	if (fifo->data)
		dma_free_coherent(dev, c2h_fifo_slots * fifo->frame_bytes,
				  fifo->data, fifo->data_dma);
	if (fifo->descs)
		dma_free_coherent(dev, c2h_fifo_slots * sizeof(*fifo->descs),
				  fifo->descs, fifo->desc_dma);
	kfree(fifo);
}

#ifdef XDMA_POLL_MODE
/* The descriptor field describes adjacency starting at its NEXT pointer. */
static unsigned int fifo_adjacent(struct xdma_c2h_fifo *fifo, unsigned int index)
{
	dma_addr_t address = fifo->desc_dma + index * sizeof(*fifo->descs);
	unsigned int block_index = ((address & (XDMA_PAGE_SIZE - 1)) /
				    sizeof(*fifo->descs)) % 64;

	return min(63 - block_index, c2h_fifo_slots - index - 1);
}

static int fifo_start(struct xdma_engine *engine, size_t count)
{
	struct device *dev = &engine->xdev->pdev->dev;
	struct sgdma_common_regs *common = fifo_common(engine);
	struct xdma_c2h_fifo *fifo;
	unsigned int i;
	u32 credit_bit = BIT(16 + engine->channel);

	if (c2h_fifo_slots < 2 || c2h_fifo_slots > 512 ||
	    count < 64 || count > 65536 || count % 64 ||
	    c2h_fifo_credit_batch > c2h_fifo_slots)
		return -EINVAL;
	if (ioread32(&engine->regs->status) & XDMA_STAT_BUSY)
		return -EBUSY;
	fifo = kzalloc(sizeof(*fifo), GFP_KERNEL);
	if (!fifo)
		return -ENOMEM;
	/* The first read supplies the fixed packet length for this engine. */
	fifo->frame_bytes = count;
	fifo->credit_batch = c2h_fifo_credit_batch ? c2h_fifo_credit_batch :
		min(32U, c2h_fifo_slots);
	fifo->descs = dma_alloc_coherent(dev,
		c2h_fifo_slots * sizeof(*fifo->descs), &fifo->desc_dma, GFP_KERNEL);
	fifo->data = dma_alloc_coherent(dev, c2h_fifo_slots * fifo->frame_bytes,
		&fifo->data_dma, GFP_KERNEL);
	if (!fifo->descs || !fifo->data) {
		fifo_free(engine, fifo);
		return -ENOMEM;
	}
	memset(fifo->descs, 0, c2h_fifo_slots * sizeof(*fifo->descs));
	for (i = 0; i < c2h_fifo_slots; ++i) {
		struct xdma_desc *desc = fifo->descs + i;
		dma_addr_t next = fifo->desc_dma +
			((i + 1) % c2h_fifo_slots) * sizeof(*desc);
		dma_addr_t data = fifo->data_dma + i * fifo->frame_bytes;
		unsigned int adjacent;

		/* Bound the NEXT fetch by its 64-descriptor block and the ring end. */
		adjacent = fifo_adjacent(fifo, (i + 1) % c2h_fifo_slots);

		desc->control = cpu_to_le32(DESC_MAGIC | (adjacent << DESC_ADJ_SHIFT) |
			XDMA_DESC_COMPLETED);
		desc->bytes = cpu_to_le32(fifo->frame_bytes);
		desc->dst_addr_lo = cpu_to_le32(lower_32_bits(data));
		desc->dst_addr_hi = cpu_to_le32(upper_32_bits(data));
		desc->next_lo = cpu_to_le32(lower_32_bits(next));
		desc->next_hi = cpu_to_le32(upper_32_bits(next));
	}
	fifo->saved_control = ioread32(&engine->regs->control);
	fifo->saved_credit_mode = ioread32(&common->credit_mode_enable);
	iowrite32(credit_bit, &common->credit_mode_enable_w1s);
	if (!(ioread32(&common->credit_mode_enable) & credit_bit)) {
		pr_err("%s: C2H FIFO credit mode unavailable\n", engine->name);
		fifo_free(engine, fifo);
		return -EOPNOTSUPP;
	}
	/* Keep the existing fixed-packet mode and descriptor-count writeback. */
	iowrite32(fifo->saved_control & ~XDMA_CTRL_RUN_STOP,
		  &engine->regs->control);
	WRITE_ONCE(engine->poll_mode_wb.virtual_addr->completed_desc_count, 0);
	dma_wmb();
	iowrite32(lower_32_bits(fifo->desc_dma), &engine->sgdma_regs->first_desc_lo);
	iowrite32(upper_32_bits(fifo->desc_dma), &engine->sgdma_regs->first_desc_hi);
	iowrite32(fifo_adjacent(fifo, 0),
		  &engine->sgdma_regs->first_desc_adjacent);
	/* Old transfer credits are cleared when RUN is reset by the hardware. */
	iowrite32(c2h_fifo_slots, &engine->sgdma_regs->credits);
	iowrite32(XDMA_CTRL_RUN_STOP, &engine->regs->control_w1s);
	ioread32(&engine->regs->status); /* Flush posted control writes. */
	fifo->running = true;
	engine->running = 1;
	engine->fifo = fifo;
	pr_info("%s: C2H read FIFO started slots=%u packet_bytes=%u credit_batch=%u\n",
		engine->name, c2h_fifo_slots, fifo->frame_bytes,
		fifo->credit_batch);
	return 0;
}

static void fifo_return_credits(struct xdma_engine *engine)
{
	struct xdma_c2h_fifo *fifo = engine->fifo;

	if (!fifo->pending_credits)
		return;
	/* Finish CPU accesses before allowing hardware to reuse consumed slots. */
	dma_wmb();
	iowrite32(fifo->pending_credits, &engine->sgdma_regs->credits);
	fifo->pending_credits = 0;
	fifo->credit_writes++;
}

#endif /* XDMA_POLL_MODE */

ssize_t xdma_fifo_read(struct xdma_engine *engine, char __user *buf,
		       size_t count, bool nonblock)
{
#ifndef XDMA_POLL_MODE
	return count ? -EOPNOTSUPP : 0;
#else
	struct xdma_c2h_fifo *fifo;
	unsigned long deadline;
	u64 wait_start = 0;
	unsigned int polls = 0;
	unsigned int timeout = READ_ONCE(c2h_timeout_ms);
	u32 completed, available, writeback;
	u32 length;
	size_t bytes;
	int rv;

	if (!count)
		return 0;
	if (!engine->fifo) {
		rv = fifo_start(engine, count);
		if (rv)
			return rv;
	}
	fifo = engine->fifo;
	length = fifo->frame_bytes;
	if (fifo->error)
		return fifo->error;
	deadline = jiffies + msecs_to_jiffies(timeout);
	for (;;) {
		writeback = le32_to_cpu(READ_ONCE(
			engine->poll_mode_wb.virtual_addr->completed_desc_count));
		completed = writeback & WB_COUNT_MASK;
		available = (completed - fifo->consumed_count) & WB_COUNT_MASK;
		if ((writeback & WB_ERR_MASK) || available > c2h_fifo_slots) {
			pr_err("%s: FIFO count invalid wb=%08x consumed=%u available=%u\n",
			       engine->name, writeback, fifo->consumed_count, available);
			fifo->error = -EIO;
			return -EIO;
		}
		if (available) {
			fifo->peak_available = max(fifo->peak_available, available);
			break;
		}
		fifo_return_credits(engine);
		if (nonblock)
			return -EAGAIN;
		if (!wait_start) {
			wait_start = ktime_get_ns();
			fifo->empty_waits++;
		}
		if (signal_pending(current)) {
			rv = -ERESTARTSYS;
			goto wait_out;
		}
		if ((++polls & 1023) == 0) {
			u32 hw_status = ioread32(&engine->regs->status);

			if (hw_status & XDMA_STAT_C2H_ERR_MASK) {
				pr_err("%s: C2H FIFO engine status=%08x\n",
				       engine->name, hw_status);
				fifo->error = -EIO;
				rv = -EIO;
				goto wait_out;
			}
			if (timeout && time_after_eq(jiffies, deadline)) {
				pr_warn("%s: FIFO empty timeout status=%08x wb=%08x\n",
					engine->name, hw_status, writeback);
				rv = -ETIMEDOUT;
				goto wait_out;
			}
			usleep_range(10, 50);
		}
		cpu_relax();
	}
	if (wait_start)
		fifo->wait_ns += ktime_get_ns() - wait_start;
	/* Count writeback follows the payload DMA writes. Packet size is fixed. */
	dma_rmb();
	bytes = min_t(size_t, count, length - fifo->offset);
	if (copy_to_user(buf, (char *)fifo->data +
		fifo->next * fifo->frame_bytes + fifo->offset, bytes))
		return -EFAULT;
	fifo->offset += bytes;
	fifo->bytes += bytes;
	if (fifo->offset == length) {
		fifo->consumed_count = (fifo->consumed_count + 1) & WB_COUNT_MASK;
		fifo->offset = 0;
		fifo->next = (fifo->next + 1) % c2h_fifo_slots;
		fifo->packets++;
		fifo->pending_credits++;
		if (fifo->pending_credits >= fifo->credit_batch)
			fifo_return_credits(engine);
	}
	return bytes;

wait_out:
	if (wait_start)
		fifo->wait_ns += ktime_get_ns() - wait_start;
	return rv;
#endif /* XDMA_POLL_MODE */
}

int xdma_fifo_close(struct xdma_engine *engine)
{
	struct xdma_c2h_fifo *fifo = engine->fifo;
	struct sgdma_common_regs *common;
	u32 status, credit_bit = BIT(16 + engine->channel);
	int rv;

	if (!fifo)
		return 0;
	if (fifo->running) {
		iowrite32(XDMA_CTRL_RUN_STOP, &engine->regs->control_w1c);
		rv = read_poll_timeout(ioread32, status, !(status & XDMA_STAT_BUSY),
			10, 5000000, false, &engine->regs->status);
		if (rv) {
			/* Do not release memory while the device may still write to it. */
			pr_err("%s: FIFO stop timed out; DMA memory retained\n", engine->name);
			fifo->error = -EBUSY;
			return rv;
		}
		fifo->running = false;
		engine->running = 0;
	}
	common = fifo_common(engine);
	if (!(fifo->saved_credit_mode & credit_bit))
		iowrite32(credit_bit, &common->credit_mode_enable_w1c);
	iowrite32(fifo->saved_control & ~XDMA_CTRL_RUN_STOP, &engine->regs->control);
	ioread32(&engine->regs->status);
	pr_info("%s: C2H FIFO packets=%llu bytes=%llu starts=1 credit_writes=%llu peak_available=%u empty_waits=%llu wait_ns=%llu\n",
		engine->name, fifo->packets, fifo->bytes, fifo->credit_writes,
		fifo->peak_available, fifo->empty_waits, fifo->wait_ns);
	engine->fifo = NULL;
	fifo_free(engine, fifo);
	return 0;
}
