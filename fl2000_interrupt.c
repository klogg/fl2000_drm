// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */
#include "fl2000.h"
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

#define FL2000_INTR_STALL_RETRIES   3
#define FL2000_INTR_STATUS_RETRY_MS 1000

struct fl2000_interrupt {
	struct usb_device *usb_dev;
	struct irq_domain *domain;
	struct fwnode_handle *fwnode;
	unsigned int irq;
	struct urb *urb;
	u8 *buf;
	struct delayed_work work;
	struct mutex mutex; /* Serializes enable, disable and resource release. */
	spinlock_t lock; /* Serializes submission against stop. */
	unsigned int stalls;
	bool stalled;
	bool rearm;
	bool fallback;
	bool irq_rescan; /* Recheck bridge status after IRQ unmask. */
	bool status_polling; /* Owned by the worker; reset after cancellation. */
	bool enabled;
};

static void fl2000_irq_mask(struct irq_data *data __always_unused)
{
	/* IRQ core masks nested dispatch; the USB worker owns packet submission. */
}

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

static int fl2000_irq_set_type(struct irq_data *data __always_unused, unsigned int type)
{
	return type == IRQ_TYPE_NONE || type == IRQ_TYPE_LEVEL_LOW ? 0 : -EINVAL;
}

static const struct irq_chip fl2000_irq_chip = {
	.name = "fl2000",
	.irq_mask = fl2000_irq_mask,
	.irq_unmask = fl2000_irq_unmask,
	.irq_set_type = fl2000_irq_set_type,
};

static int fl2000_irq_map(struct irq_domain *domain, unsigned int irq,
			  irq_hw_number_t hwirq __always_unused)
{
	irq_set_chip_data(irq, domain->host_data);
	irq_set_chip_and_handler(irq, &fl2000_irq_chip, handle_simple_irq);
	irq_set_nested_thread(irq, true);
	irq_set_noprobe(irq);
	return 0;
}

static const struct irq_domain_ops fl2000_irq_domain_ops = {
	.map = fl2000_irq_map,
};

static int fl2000_intr_irq_init(struct fl2000_interrupt *intr)
{
	intr->fwnode = irq_domain_alloc_named_fwnode(dev_name(&intr->usb_dev->dev));
	if (!intr->fwnode)
		return -ENOMEM;
	intr->domain = irq_domain_create_linear(intr->fwnode, 1, &fl2000_irq_domain_ops, intr);
	if (!intr->domain)
		return -ENOMEM;
	intr->irq = irq_create_mapping(intr->domain, 0);
	return intr->irq ? 0 : -ENOMEM;
}

int fl2000_interrupt_irq(const struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	if (!device->interrupt)
		return -ENODEV;
	return device->interrupt->irq;
}

static void fl2000_intr_recover_stall(struct fl2000_interrupt *intr)
{
	unsigned long flags;
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
	if (ret)
		intr->enabled = false;
	else if (intr->status_polling)
		schedule_delayed_work(&intr->work, msecs_to_jiffies(FL2000_INTR_STATUS_RETRY_MS));
unlock:
	spin_unlock_irqrestore(&intr->lock, flags);
	if (ret) {
		fl2000_drm_set_polling(intr->usb_dev, true);
		dev_err_ratelimited(&intr->usb_dev->dev, "Interrupt stall recovery failed: %d\n",
				    ret);
	}
}

static int fl2000_intr_process_status(struct fl2000_interrupt *intr)
{
	struct fl2000_device *device = dev_get_drvdata(&intr->usb_dev->dev);
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
		if (atomic_read(&device->i2c_errors) != errors)
			ret = -EIO;
	}
	if (ret < 0) {
		if (!intr->status_polling) {
			intr->status_polling = true;
			fl2000_drm_set_polling(intr->usb_dev, true);
		}
		dev_err_ratelimited(&intr->usb_dev->dev, "Interrupt status processing failed: %d\n",
				    ret);
	} else if (intr->status_polling) {
		intr->status_polling = false;
		fl2000_drm_set_polling(intr->usb_dev, false);
	}

	return ret;
}

static void fl2000_intr_work(struct work_struct *work)
{
	struct fl2000_interrupt *intr =
		container_of(to_delayed_work(work), struct fl2000_interrupt, work);
	unsigned long flags;
	const unsigned long retry_delay = msecs_to_jiffies(FL2000_INTR_STATUS_RETRY_MS);
	bool retry;
	int ret;

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
	retry = fl2000_intr_process_status(intr) < 0;

	/* Acknowledge the device and bridge before requesting another packet. */
	ret = 0;
	spin_lock_irqsave(&intr->lock, flags);
	if (intr->enabled && intr->rearm && !retry) {
		intr->rearm = false;
		ret = usb_submit_urb(intr->urb, GFP_ATOMIC);
		if (ret)
			intr->enabled = false;
	}
	if (intr->enabled && retry)
		schedule_delayed_work(&intr->work, retry_delay);
	spin_unlock_irqrestore(&intr->lock, flags);
	if (ret) {
		fl2000_drm_set_polling(intr->usb_dev, true);
		dev_err_ratelimited(&intr->usb_dev->dev, "Interrupt submission failed: %d\n", ret);
	}
}

static void fl2000_intr_completion(struct urb *urb)
{
	struct fl2000_interrupt *intr = urb->context;
	unsigned long flags;
	int status = urb->status;
	bool exhausted = false;

	spin_lock_irqsave(&intr->lock, flags);
	if (!intr->enabled)
		goto unlock;
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
	if (status && status != -EPIPE && status != -ENOENT && status != -ECONNRESET &&
	    status != -ESHUTDOWN)
		dev_err_ratelimited(&intr->usb_dev->dev, "Interrupt transfer failed: %d\n", status);
}

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

static void fl2000_intr_release(struct device *dev __always_unused, void *res)
{
	struct fl2000_interrupt *intr = res;

	mutex_lock(&intr->mutex);
	fl2000_intr_stop(intr);
	if (intr->irq)
		irq_dispose_mapping(intr->irq);
	if (intr->domain)
		irq_domain_remove(intr->domain);
	irq_domain_free_fwnode(intr->fwnode);
	usb_free_urb(intr->urb);
	kfree(intr->buf);
	mutex_unlock(&intr->mutex);
}

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
	if (!READ_ONCE(intr->enabled)) {
		usb_kill_urb(intr->urb);
		cancel_delayed_work_sync(&intr->work);
		intr->status_polling = false;
		if (intr->urb->status == -EPIPE) {
			ret = usb_clear_halt(usb_dev, intr->urb->pipe);
			if (ret)
				goto out;
		}
		/* Restore HPD-only mode before a new completion can request fallback. */
		fl2000_drm_set_polling(usb_dev, false);
		spin_lock_irqsave(&intr->lock, flags);
		intr->stalled = false;
		intr->rearm = false;
		intr->fallback = false;
		intr->stalls = 0;
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
	if (ret)
		fl2000_drm_set_polling(usb_dev, true);
	return ret;
}

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

void fl2000_interrupt_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
	device->interrupt = NULL;
}
