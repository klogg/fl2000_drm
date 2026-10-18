// SPDX-License-Identifier: GPL-2.0
/* Guest-only fault injection; never linked into the production driver. */
#include <linux/atomic.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/sched.h>

static char *mode = "status";
module_param(mode, charp, 0400);
static int failures = 3;
module_param(failures, int, 0600);
static struct task_struct *irq_task;
static bool i2c_mode;
static atomic_t injected = ATOMIC_INIT(0);
static atomic_t polling_on = ATOMIC_INIT(0);
static atomic_t polling_off = ATOMIC_INIT(0);

static int count_get(char *buffer, const struct kernel_param *parameter)
{
	return scnprintf(buffer, PAGE_SIZE, "%d\n", atomic_read(parameter->arg));
}

static const struct kernel_param_ops count_ops = { .get = count_get };
module_param_cb(injected, &count_ops, &injected, 0400);
module_param_cb(polling_on, &count_ops, &polling_on, 0400);
module_param_cb(polling_off, &count_ops, &polling_off, 0400);

static void inject(struct pt_regs *regs)
{
	int count = READ_ONCE(failures);

	if ((int)regs_return_value(regs) >= 0 && count > 0 &&
	    cmpxchg(&failures, count, count - 1) == count) {
		regs_set_return_value(regs, -EIO);
		atomic_inc(&injected);
	}
}

static int status_return(struct kretprobe_instance *instance __always_unused, struct pt_regs *regs)
{
	if (!i2c_mode) {
		inject(regs);
	} else if ((int)regs_return_value(regs) >= 0) {
		/* Exercise bridge acknowledgment without requiring a physical HPD edge. */
		if (READ_ONCE(failures) > 0)
			regs_set_return_value(regs, 1);
	}
	return 0;
}

static int i2c_return(struct kretprobe_instance *instance __always_unused, struct pt_regs *regs)
{
	if (READ_ONCE(irq_task) == current)
		inject(regs);
	return 0;
}

static int irq_entry(struct kretprobe_instance *instance __always_unused,
		     struct pt_regs *regs __always_unused)
{
	WRITE_ONCE(irq_task, current);
	return 0;
}

static int irq_return(struct kretprobe_instance *instance __always_unused,
		      struct pt_regs *regs __always_unused)
{
	WRITE_ONCE(irq_task, NULL);
	return 0;
}

static struct kretprobe irq_probe = {
	.kp.symbol_name = "it66121_irq_thread",
	.entry_handler = irq_entry,
	.handler = irq_return,
	.maxactive = 8,
};

static int polling_entry(struct kprobe *probe __always_unused, struct pt_regs *regs)
{
	atomic_inc(regs_get_kernel_argument(regs, 1) ? &polling_on : &polling_off);
	return 0;
}

static struct kretprobe status_probe = {
	.kp.symbol_name = "fl2000_check_interrupt",
	.handler = status_return,
	.maxactive = 8,
};

static struct kretprobe i2c_probe = {
	.kp.symbol_name = "fl2000_i2c_dword",
	.handler = i2c_return,
	.maxactive = 8,
};

static struct kprobe polling_probe = {
	.symbol_name = "fl2000_drm_set_polling",
	.pre_handler = polling_entry,
};

static int __init lifecycle_fault_init(void)
{
	int ret;

	if (!dmi_match(DMI_SYS_VENDOR, "QEMU"))
		return -EPERM;
	if (strcmp(mode, "status") && strcmp(mode, "i2c"))
		return -EINVAL;
	i2c_mode = !strcmp(mode, "i2c");
	ret = register_kprobe(&polling_probe);
	if (ret)
		return ret;
	ret = register_kretprobe(&status_probe);
	if (ret)
		goto polling;
	if (i2c_mode) {
		ret = register_kretprobe(&irq_probe);
		if (ret)
			goto status;
		ret = register_kretprobe(&i2c_probe);
		if (ret) {
			unregister_kretprobe(&irq_probe);
			goto status;
		}
	}
	return 0;
status:
	unregister_kretprobe(&status_probe);
polling:
	unregister_kprobe(&polling_probe);
	return ret;
}

static void __exit lifecycle_fault_exit(void)
{
	if (i2c_mode) {
		unregister_kretprobe(&i2c_probe);
		unregister_kretprobe(&irq_probe);
	}
	unregister_kretprobe(&status_probe);
	unregister_kprobe(&polling_probe);
	pr_info("LIFECYCLE_FAULT mode=%s injected=%d missed=%d\n", mode, atomic_read(&injected),
		status_probe.nmissed + (i2c_mode ? i2c_probe.nmissed + irq_probe.nmissed : 0));
}

module_init(lifecycle_fault_init);
module_exit(lifecycle_fault_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QEMU-only FL2000 status and I2C return fault injection");
