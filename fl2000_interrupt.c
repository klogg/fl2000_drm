// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */
#include "fl2000.h"
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/property.h>

#define FL2000_INTR_STALL_RETRIES   3
#define FL2000_INTR_STATUS_RETRY_MS 1000

/**
 * struct fl2000_interrupt - USB event reception and nested IRQ lifetime
 * @usb_dev: Owning USB parent.
 * @domain: One-input domain keyed by the I2C adapter software node.
 * @irq: Linux IRQ mapped from domain input zero.
 * @urb: Single reusable interrupt-IN URB.
 * @buf: One-byte interrupt packet buffer.
 * @work: Status acknowledgment, halt clearing and rearm worker.
 * @stalls: Consecutive endpoint stalls; successful packets clear the count.
 * @stalled: Worker must clear the endpoint halt before resubmission.
 * @rearm: Completion awaits acknowledgment before resubmission.
 * @fallback: Worker must enable polling after reception has stopped.
 * @irq_rescan: Startup or IRQ unmask requested a rescan; consumed with xchg().
 * @status_polling: Status retry mode; worker-owned, reset after cancellation.
 * @enabled: Submission allowed; stop clears it before draining URB and work.
 *
 * The USB worker dispatches the nested IRQ synchronously, so bridge I2C
 * acknowledgment completes before USB rearm. Status or acknowledgment errors
 * retry once per second while enabled. A fourth consecutive endpoint stall
 * stops reception; cancellation and device loss stop without polling fallback.
 * Bulk-stream recovery has its own independent retry budget.
 * Stop must not hold a bridge/DRM lock needed by @work. Domain and mapping
 * outlive provider unbind and are freed before the adapter software node.
 */
struct fl2000_interrupt {
	struct usb_device *usb_dev;
	struct irq_domain *domain;
	unsigned int irq;
	struct urb *urb;
	u8 *buf;
	struct delayed_work work;
	/** @mutex: Serializes enable, disable and resource release. */
	struct mutex mutex;
	/** @lock: Serializes submission and completion queue state against stop. */
	spinlock_t lock;
	unsigned int stalls;
	bool stalled;
	bool rearm;
	bool fallback;
	bool irq_rescan;
	bool status_polling;
	bool enabled;
};

/**
 * fl2000_irq_mask() - Leave nested dispatch masking to IRQ core
 * @data: IRQ descriptor argument; unused.
 *
 * Context: Any context. Does not sleep.
 */
static void fl2000_irq_mask(struct irq_data *data __always_unused)
{
	/* IRQ core masks nested dispatch; the USB worker owns packet submission. */
}

/**
 * fl2000_irq_unmask() - Request a rescan of pending bridge status
 * @data: Mapped IRQ descriptor.
 *
 * Context: Any context. Does not sleep.
 */
static void fl2000_irq_unmask(struct irq_data *data)
{
	struct fl2000_interrupt *intr = irq_data_get_irq_chip_data(data);
	unsigned long flags;

	spin_lock_irqsave(&intr->lock, flags);
	WRITE_ONCE(intr->irq_rescan, true);
	if (intr->enabled)
		mod_delayed_work(system_wq, &intr->work, 0);
	spin_unlock_irqrestore(&intr->lock, flags);
}

/**
 * fl2000_irq_set_type() - Accept only unspecified or level-low trigger type
 * @data: IRQ descriptor argument; unused.
 * @type: Requested IRQ_TYPE_* trigger encoding.
 *
 * Context: Any context. Does not sleep.
 * Return: 0 for a supported type or -EINVAL.
 */
static int fl2000_irq_set_type(struct irq_data *data __always_unused, unsigned int type)
{
	return type == IRQ_TYPE_NONE || type == IRQ_TYPE_LEVEL_LOW ? 0 : -EINVAL;
}

/**
 * var fl2000_irq_chip - Nested level-low IRQ operations without hardware mask writes
 */
static const struct irq_chip fl2000_irq_chip = {
	.name = "fl2000",
	.irq_mask = fl2000_irq_mask,
	.irq_unmask = fl2000_irq_unmask,
	.irq_set_type = fl2000_irq_set_type,
};

/**
 * fl2000_irq_map() - Install the nested chip and handler for domain input zero
 * @domain: Adapter IRQ domain.
 * @irq: Linux IRQ number.
 * @hwirq: Domain input, always zero; unused.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int fl2000_irq_map(struct irq_domain *domain, unsigned int irq,
			  irq_hw_number_t hwirq __always_unused)
{
	irq_set_chip_data(irq, domain->host_data);
	irq_set_chip_and_handler(irq, &fl2000_irq_chip, handle_simple_irq);
	irq_set_nested_thread(irq, true);
	irq_set_noprobe(irq);
	return 0;
}

/**
 * var fl2000_irq_domain_ops - Map the adapter domain to a nested Linux IRQ
 */
static const struct irq_domain_ops fl2000_irq_domain_ops = {
	.map = fl2000_irq_map,
};

/**
 * fl2000_intr_irq_init() - Create the adapter domain and map its one input
 * @intr: Interrupt resource with a registered I2C adapter.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int fl2000_intr_irq_init(struct fl2000_interrupt *intr)
{
	struct i2c_adapter *adapter = fl2000_i2c_adapter(intr->usb_dev);

	intr->domain = irq_domain_create_linear(dev_fwnode(&adapter->dev), 1,
						&fl2000_irq_domain_ops, intr);
	if (!intr->domain)
		return -ENOMEM;
	intr->irq = irq_create_mapping(intr->domain, 0);
	return intr->irq ? 0 : -ENOMEM;
}

/**
 * fl2000_intr_recover_stall() - Clear a halt outside URB completion context
 * @intr: Interrupt state with a stalled endpoint.
 *
 * After the settle delay, recheck enabled state before submission. Transfer
 * cancellation stops reception; other failures enable connector polling.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_intr_recover_stall(struct fl2000_interrupt *intr)
{
	unsigned long flags;
	bool polling = false;
	int ret;

	/* Recover a halted endpoint outside completion context. */
	msleep(20);
	if (!READ_ONCE(intr->enabled))
		return;
	ret = usb_clear_halt(intr->usb_dev, intr->urb->pipe);
	spin_lock_irqsave(&intr->lock, flags);
	if (!intr->enabled)
		goto unlock;
	if (!ret) {
		intr->stalled = false;
		if (intr->status_polling)
			intr->rearm = true;
		else
			ret = usb_submit_urb(intr->urb, GFP_ATOMIC);
	}
	if (ret) {
		intr->enabled = false;
		polling = !fl2000_usb_error_terminal(ret);
	} else if (intr->status_polling) {
		schedule_delayed_work(&intr->work, msecs_to_jiffies(FL2000_INTR_STATUS_RETRY_MS));
	}
unlock:
	spin_unlock_irqrestore(&intr->lock, flags);
	if (polling)
		fl2000_drm_set_polling(intr->usb_dev, true);
	if (ret)
		dev_err_ratelimited(&intr->usb_dev->dev, "Interrupt stall recovery failed: %d\n",
				    ret);
}

/**
 * fl2000_intr_process_status() - Acknowledge controller and nested bridge events
 * @intr: Enabled interrupt resource.
 *
 * Compare I2C error counts around synchronous nested dispatch and detection.
 * The worker uses the result to select polling and retries after rechecking
 * queue state, without depending on an IRQ handler return value.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int fl2000_intr_process_status(struct fl2000_interrupt *intr)
{
	const struct fl2000_device *device = dev_get_drvdata(&intr->usb_dev->dev);
	bool rescan = xchg(&intr->irq_rescan, false);
	int errors;
	int ret;

	ret = fl2000_check_interrupt(intr->usb_dev);
	/* A previous failed read may already have cleared the FL2000 event bit. */
	if (ret >= 0 && (ret > 0 || intr->status_polling || rescan)) {
		errors = atomic_read(&device->i2c_errors);
		/* Nested dispatch finishes bridge acknowledgment before USB rearm. */
		handle_nested_irq(intr->irq);
		ret = fl2000_drm_hotplug(intr->usb_dev);
		/* IRQ handlers return irqreturn_t, not an I2C transfer errno. */
		if (!ret && atomic_read(&device->i2c_errors) != errors)
			ret = -EIO;
	}
	if (ret < 0)
		dev_err_ratelimited(&intr->usb_dev->dev, "Interrupt status processing failed: %d\n",
				    ret);

	return ret;
}

/**
 * fl2000_intr_work() - Process status before rearming the interrupt URB
 * @work: Scheduled subsystem work item.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_intr_work(struct work_struct *work)
{
	struct fl2000_interrupt *intr =
		container_of(to_delayed_work(work), struct fl2000_interrupt, work);
	unsigned long flags;
	const unsigned long retry_delay = msecs_to_jiffies(FL2000_INTR_STATUS_RETRY_MS);
	bool polling = false;
	bool update_polling = false;
	bool retry;
	int status;
	int ret = 0;

	if (!READ_ONCE(intr->enabled)) {
		if (READ_ONCE(intr->fallback)) {
			WRITE_ONCE(intr->fallback, false);
			fl2000_drm_set_polling(intr->usb_dev, true);
		}
		return;
	}
	if (READ_ONCE(intr->stalled)) {
		fl2000_intr_recover_stall(intr);
		return;
	}
	status = fl2000_intr_process_status(intr);
	retry = status < 0;

	/* Acknowledge the device and bridge before requesting another packet. */
	spin_lock_irqsave(&intr->lock, flags);
	if (!intr->enabled)
		goto unlock;
	if (fl2000_usb_error_terminal(status)) {
		intr->enabled = false;
		goto unlock;
	}
	if (intr->status_polling != retry) {
		intr->status_polling = retry;
		polling = retry;
		update_polling = true;
	}
	if (intr->rearm && !retry) {
		intr->rearm = false;
		ret = usb_submit_urb(intr->urb, GFP_ATOMIC);
		if (ret) {
			intr->enabled = false;
			if (!fl2000_usb_error_terminal(ret)) {
				polling = true;
				update_polling = true;
			}
		}
	}
	if (intr->enabled && retry)
		schedule_delayed_work(&intr->work, retry_delay);
unlock:
	spin_unlock_irqrestore(&intr->lock, flags);
	if (update_polling)
		fl2000_drm_set_polling(intr->usb_dev, polling);
	if (ret)
		dev_err_ratelimited(&intr->usb_dev->dev, "Interrupt submission failed: %d\n", ret);
}

/**
 * fl2000_intr_completion() - Record endpoint status and defer all I2C work
 * @urb: Completed USB request.
 *
 * Context: USB completion context. Takes the interrupt spinlock.
 */
static void fl2000_intr_completion(struct urb *urb)
{
	struct fl2000_interrupt *intr = urb->context;
	unsigned long flags;
	int status = urb->status;
	bool exhausted = false;

	spin_lock_irqsave(&intr->lock, flags);
	if (!intr->enabled)
		goto unlock;
	switch (status) {
	case -ENOENT:
	case -ECONNRESET:
	case -ENODEV:
	case -ESHUTDOWN:
		intr->enabled = false;
		intr->fallback = false;
		goto unlock;
	default:
		break;
	}
	if (status == -EPIPE) {
		if (++intr->stalls > FL2000_INTR_STALL_RETRIES) {
			intr->enabled = false;
			intr->fallback = true;
			mod_delayed_work(system_wq, &intr->work, 0);
			exhausted = true;
		} else {
			intr->stalled = true;
			mod_delayed_work(system_wq, &intr->work, 0);
		}
	} else if (status) {
		intr->enabled = false;
		intr->fallback = true;
		mod_delayed_work(system_wq, &intr->work, 0);
	} else {
		intr->stalls = 0;
		intr->rearm = true;
		mod_delayed_work(system_wq, &intr->work, 0);
	}
unlock:
	spin_unlock_irqrestore(&intr->lock, flags);
	if (exhausted)
		dev_warn_ratelimited(&intr->usb_dev->dev, "Interrupt endpoint repeatedly stalled; "
							  "using connector polling\n");
	if (status)
		dev_err_ratelimited(&intr->usb_dev->dev, "Interrupt transfer failed: %d\n", status);
}

/**
 * fl2000_intr_stop() - Drain the interrupt URB and synchronize its worker
 * @intr: Interrupt resource to stop.
 *
 * Context: Process context. Caller holds the interrupt mutex.
 */
static void fl2000_intr_stop(struct fl2000_interrupt *intr)
{
	unsigned long flags;

	spin_lock_irqsave(&intr->lock, flags);
	intr->enabled = false;
	intr->stalled = false;
	intr->rearm = false;
	intr->fallback = false;
	intr->stalls = 0;
	spin_unlock_irqrestore(&intr->lock, flags);
	usb_kill_urb(intr->urb);
	cancel_delayed_work_sync(&intr->work);
	intr->status_polling = false;
}

/**
 * fl2000_intr_release() - Stop delivery and free IRQ-domain and USB resources
 * @dev: Resource owner; unused.
 * @res: Interrupt resource whose bridge component is already unbound.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_intr_release(struct device *dev __always_unused, void *res)
{
	struct fl2000_interrupt *intr = res;

	mutex_lock(&intr->mutex);
	fl2000_intr_stop(intr);
	if (intr->irq)
		irq_dispose_mapping(intr->irq);
	if (intr->domain)
		irq_domain_remove(intr->domain);
	usb_free_urb(intr->urb);
	kfree(intr->buf);
	mutex_unlock(&intr->mutex);
}

/**
 * fl2000_enable_interrupt() - Start interrupt reception and request a status rescan
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * Idempotent while enabled. A fresh start clears stale halt state, restores
 * HPD-only connector detection and schedules a bridge rescan immediately.
 * Call after bridge IRQ binding and initial display setup.
 *
 * Context: Process context. Takes the interrupt mutex; must not run from its
 *          worker.
 * Return: 0 on success or a negative error code.
 */
int fl2000_enable_interrupt(struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);
	struct fl2000_interrupt *intr;
	unsigned long flags;
	int ret = 0;

	intr = device->interrupt;
	if (!intr)
		return -ENODEV;
	mutex_lock(&intr->mutex);
	if (fl2000_usb_unavailable(usb_dev)) {
		ret = -ENODEV;
		goto out;
	}
	if (!READ_ONCE(intr->enabled)) {
		fl2000_intr_stop(intr);
		if (intr->urb->status == -EPIPE) {
			ret = usb_clear_halt(usb_dev, intr->urb->pipe);
			if (ret)
				goto out;
		}
		/* Restore HPD-only mode before a new completion can request fallback. */
		fl2000_drm_set_polling(usb_dev, false);
		spin_lock_irqsave(&intr->lock, flags);
		/* The controller event may have been consumed before a failed acknowledgment. */
		WRITE_ONCE(intr->irq_rescan, true);
		intr->enabled = true;
		ret = usb_submit_urb(intr->urb, GFP_ATOMIC);
		if (ret)
			intr->enabled = false;
		else
			/* A stale pending event may suppress further notifications. */
			mod_delayed_work(system_wq, &intr->work, 0);
		spin_unlock_irqrestore(&intr->lock, flags);
	}
out:
	mutex_unlock(&intr->mutex);
	if (ret && !fl2000_usb_error_terminal(ret))
		fl2000_drm_set_polling(usb_dev, true);
	return ret;
}

/**
 * fl2000_disable_interrupt() - Quiesce USB and nested IRQ delivery
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * Kills the interrupt URB and synchronizes status/recovery work. Call before
 * detaching the bridge or freeing its requested IRQ.
 *
 * Context: Process context. Must not hold locks needed by bridge/DRM
 *          callbacks.
 */
void fl2000_disable_interrupt(const struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);
	struct fl2000_interrupt *intr;

	intr = device->interrupt;
	if (!intr)
		return;
	mutex_lock(&intr->mutex);
	fl2000_intr_stop(intr);
	mutex_unlock(&intr->mutex);
}

/**
 * fl2000_interrupt_create() - Create USB interrupt resources and a nested IRQ
 * @interface: Claimed interrupt interface; the I2C adapter is registered.
 *
 * Creates a one-input IRQ domain keyed by the adapter software node and
 * maps input zero. Does not start the interrupt URB.
 *
 * Context: Process context. Caller serializes interface initialization.
 * Return: 0 on success or a negative error code.
 */
int fl2000_interrupt_create(struct usb_interface *interface)
{
	struct fl2000_device *device = dev_get_drvdata(&interface_to_usbdev(interface)->dev);
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct usb_endpoint_descriptor *ep;
	struct fl2000_interrupt *intr;
	int ret;

	ret = usb_find_int_in_endpoint(interface->cur_altsetting, &ep);
	if (ret || usb_endpoint_num(ep) != INTERRUPT_EP)
		return -ENODEV;
	intr = devres_alloc(fl2000_intr_release, sizeof(*intr), GFP_KERNEL);
	if (!intr)
		return -ENOMEM;
	intr->usb_dev = usb_dev;
	mutex_init(&intr->mutex);
	spin_lock_init(&intr->lock);
	INIT_DELAYED_WORK(&intr->work, fl2000_intr_work);
	intr->urb = usb_alloc_urb(0, GFP_KERNEL);
	intr->buf = kmalloc(1, GFP_KERNEL);
	if (!intr->urb || !intr->buf) {
		ret = -ENOMEM;
		goto release;
	}
	ret = fl2000_intr_irq_init(intr);
	if (ret)
		goto release;
	usb_fill_int_urb(intr->urb, usb_dev, usb_rcvintpipe(usb_dev, INTERRUPT_EP), intr->buf, 1,
			 fl2000_intr_completion, intr, ep->bInterval);
	devres_add(&usb_dev->dev, intr);
	device->interrupt = intr;
	return 0;
release:
	fl2000_intr_release(&usb_dev->dev, intr);
	devres_free(intr);
	return ret;
}

/**
 * fl2000_interrupt_destroy() - Release interrupt resources and IRQ mapping
 * @interface: Interrupt interface; the component master is removed.
 *
 * Drains work before removing the IRQ mapping and domain. The adapter and
 * its software node must remain alive through this call.
 *
 * Context: Process context. Caller excludes interface reinitialization.
 */
void fl2000_interrupt_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
	device->interrupt = NULL;
}
