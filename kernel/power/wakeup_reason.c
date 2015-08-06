/*
 * kernel/power/wakeup_reason.c
 *
 * Logs the reasons which caused the kernel to resume from
 * the suspend mode.
 *
 * Copyright (C) 2014 Google, Inc.
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/wakeup_reason.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/suspend.h>


#define MAX_WAKEUP_REASON_IRQS 32
static int irq_list[MAX_WAKEUP_REASON_IRQS];
static int irqcount;
static bool suspend_abort;
static char abort_reason[MAX_SUSPEND_ABORT_LEN];
static struct kobject *wakeup_reason;
static DEFINE_SPINLOCK(resume_reason_lock);

static unsigned long wakeup_ready_timeout;
static unsigned long wakeup_ready_wait;
static unsigned long wakeup_ready_nowait;

static struct timespec last_xtime; /* wall time before last suspend */
static struct timespec curr_xtime; /* wall time after last suspend */
static struct timespec last_stime; /* total_sleep_time before last suspend */
static struct timespec curr_stime; /* total_sleep_time after last suspend */

static ssize_t last_resume_reason_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	int irq_no, buf_offset = 0;
	struct irq_desc *desc;
	spin_lock(&resume_reason_lock);
	if (suspend_abort) {
		buf_offset = sprintf(buf, "Abort: %s", abort_reason);
	} else {
		for (irq_no = 0; irq_no < irqcount; irq_no++) {
			desc = irq_to_desc(irq_list[irq_no]);
			if (desc && desc->action && desc->action->name)
				buf_offset += sprintf(buf + buf_offset, "%d %s\n",
						irq_list[irq_no], desc->action->name);
			else
				buf_offset += sprintf(buf + buf_offset, "%d\n",
						irq_list[irq_no]);
		}
	}
	spin_unlock(&resume_reason_lock);
	return buf_offset;
}

static ssize_t last_suspend_time_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	struct timespec sleep_time;
	struct timespec total_time;
	struct timespec suspend_resume_time;

	sleep_time = timespec_sub(curr_stime, last_stime);
	total_time = timespec_sub(curr_xtime, last_xtime);
	suspend_resume_time = timespec_sub(total_time, sleep_time);

	/*
	 * suspend_resume_time is calculated from sleep_time. Userspace would
	 * always need both. Export them in pair here.
	 */
	return sprintf(buf, "%lu.%09lu %lu.%09lu\n",
				suspend_resume_time.tv_sec, suspend_resume_time.tv_nsec,
				sleep_time.tv_sec, sleep_time.tv_nsec);
}


static ssize_t suspend_since_boot_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	struct timespec xtime;

	xtime = timespec_sub(total_xtime, total_stime);
	return sprintf(buf, "%lu %lu %lu.%09lu %lu.%09lu %lu.%09lu\n"
			    "%lu %lu %u\n",
				suspend_count, abort_count,
				xtime.tv_sec, xtime.tv_nsec,
				total_atime.tv_sec, total_atime.tv_nsec,
				total_stime.tv_sec, total_stime.tv_nsec,
				wakeup_ready_nowait, wakeup_ready_timeout,
				jiffies_to_msecs(wakeup_ready_wait));
}

static struct kobj_attribute resume_reason = __ATTR_RO(last_resume_reason);
static struct kobj_attribute suspend_time = __ATTR_RO(last_suspend_time);

static struct attribute *attrs[] = {
	&resume_reason.attr,
	&suspend_time.attr,
	NULL,
};
static struct attribute_group attr_group = {
	.attrs = attrs,
};

/*
 * logs all the wake up reasons to the kernel
 * stores the irqs to expose them to the userspace via sysfs
 */
void log_wakeup_reason(int irq)
{
	struct irq_desc *desc;
	desc = irq_to_desc(irq);
	if (desc && desc->action && desc->action->name)
		printk(KERN_INFO "Resume caused by IRQ %d, %s\n", irq,
				desc->action->name);
	else
		printk(KERN_INFO "Resume caused by IRQ %d\n", irq);

	spin_lock(&resume_reason_lock);
	if (irqcount == MAX_WAKEUP_REASON_IRQS) {
		spin_unlock(&resume_reason_lock);
		printk(KERN_WARNING "Resume caused by more than %d IRQs\n",
				MAX_WAKEUP_REASON_IRQS);
		return;
	}

	irq_list[irqcount++] = irq;
	spin_unlock(&resume_reason_lock);
}

int check_wakeup_reason(int irq)
{
	int irq_no;
	int ret = false;

	spin_lock(&resume_reason_lock);
	for (irq_no = 0; irq_no < irqcount; irq_no++)
		if (irq_list[irq_no] == irq) {
			ret = true;
			break;
	}
	spin_unlock(&resume_reason_lock);
	return ret;
}

void log_suspend_abort_reason(const char *fmt, ...)
{
	va_list args;

	spin_lock(&resume_reason_lock);

	//Suspend abort reason has already been logged.
	if (suspend_abort) {
		spin_unlock(&resume_reason_lock);
		return;
	}

	suspend_abort = true;
	va_start(args, fmt);
	vsnprintf(abort_reason, MAX_SUSPEND_ABORT_LEN, fmt, args);
	va_end(args);
	spin_unlock(&resume_reason_lock);
}

static bool match_node(struct wakeup_irq_node *n, void *_p)
{
	int irq = *((int *)_p);
	return n->irq != irq;
}

int check_wakeup_reason(int irq)
{
	bool found;
	spin_lock(&resume_reason_lock);
	found = !walk_irq_node_tree(base_irq_nodes, match_node, &irq);
	spin_unlock(&resume_reason_lock);
	return found;
}

static bool build_leaf_nodes(struct wakeup_irq_node *n, void *_p)
{
	struct list_head *wakeups = _p;
	if (!n->child)
		list_add(&n->next, wakeups);
	return true;
}

static const struct list_head* get_wakeup_reasons_nosync(void)
{
	BUG_ON(logging_wakeup_reasons());
	INIT_LIST_HEAD(&wakeup_irqs);
	walk_irq_node_tree(base_irq_nodes, build_leaf_nodes, &wakeup_irqs);
	return &wakeup_irqs;
}

static bool build_unfinished_nodes(struct wakeup_irq_node *n, void *_p)
{
	struct list_head *unfinished = _p;
	if (!n->handled) {
		pr_warning("%s: wakeup irq %d was not handled\n",
			   __func__, n->irq);
		list_add(&n->next, unfinished);
	}
	return true;
}

const struct list_head* get_wakeup_reasons(unsigned long timeout,
					struct list_head *unfinished)
{
	INIT_LIST_HEAD(unfinished);

	if (logging_wakeup_reasons()) {
		unsigned long signalled = 0;
		unsigned long time_waited;

		if (timeout)
			signalled = wait_for_completion_timeout(&wakeups_completion, timeout);
		if (!signalled) {
			pr_warn("%s: completion timeout\n", __func__);
			wakeup_ready_timeout++;
			stop_logging_wakeup_reasons();
			walk_irq_node_tree(base_irq_nodes, build_unfinished_nodes, unfinished);
			return NULL;
		}
		time_waited = timeout - signalled;
		pr_info("%s: waited for %u ms\n",
				__func__,
				jiffies_to_msecs(time_waited));
		if (time_waited > wakeup_ready_wait)
			wakeup_ready_wait = time_waited;
	} else {
		wakeup_ready_nowait++;
	}

	return get_wakeup_reasons_nosync();
}

static bool delete_node(struct wakeup_irq_node *n, void *unused)
{
	list_del(&n->siblings);
	kmem_cache_free(wakeup_irq_nodes_cache, n);
	return true;
}

static void clear_wakeup_reasons_nolock(void)
{
	BUG_ON(logging_wakeup_reasons());
	walk_irq_node_tree(base_irq_nodes, delete_node, NULL);
	base_irq_nodes = NULL;
	cur_irq_tree = NULL;
	cur_irq_tree_depth = 0;
	INIT_LIST_HEAD(&wakeup_irqs);
	suspend_abort = false;
}

void clear_wakeup_reasons(void)
{
	unsigned long flags;

	spin_lock_irqsave(&resume_reason_lock, flags);
	clear_wakeup_reasons_nolock();
	spin_unlock_irqrestore(&resume_reason_lock, flags);
}

/* Detects a suspend and clears all the previous wake up reasons*/
static int wakeup_reason_pm_event(struct notifier_block *notifier,
		unsigned long pm_event, void *unused)
{
	struct timespec xtom; /* wall_to_monotonic, ignored */

	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		spin_lock(&resume_reason_lock);
		irqcount = 0;
		suspend_abort = false;
		spin_unlock(&resume_reason_lock);

		get_xtime_and_monotonic_and_sleep_offset(&last_xtime, &xtom,
			&last_stime);
		break;
	case PM_POST_SUSPEND:
		get_xtime_and_monotonic_and_sleep_offset(&curr_xtime, &xtom,
			&curr_stime);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block wakeup_reason_pm_notifier_block = {
	.notifier_call = wakeup_reason_pm_event,
};

/* Initializes the sysfs parameter
 * registers the pm_event notifier
 */
int __init wakeup_reason_init(void)
{
	int retval;

	retval = register_pm_notifier(&wakeup_reason_pm_notifier_block);
	if (retval)
		printk(KERN_WARNING "[%s] failed to register PM notifier %d\n",
				__func__, retval);

	wakeup_reason = kobject_create_and_add("wakeup_reasons", kernel_kobj);
	if (!wakeup_reason) {
		printk(KERN_WARNING "[%s] failed to create a sysfs kobject\n",
				__func__);
		return 1;
	}
	retval = sysfs_create_group(wakeup_reason, &attr_group);
	if (retval) {
		kobject_put(wakeup_reason);
		printk(KERN_WARNING "[%s] failed to create a sysfs group %d\n",
				__func__, retval);
	}
	return 0;
}

late_initcall(wakeup_reason_init);
