/*
 * Copyright (C) 2014-2017, Sultanxda <sultanxda@gmail.com>
 *           (C) 2017, Joe Maples <joe@frap129.org>
 *
 * fp-boost is based on cpu_input_boost by Sultan Alsawaf.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "fp-boost: " fmt

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/fp_boost.h>
#include <linux/input.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/cpuhotplug.h>
#include <linux/wakelock.h>

#define DRIVER_ENABLED		(1 << 0)
#define FINGERPRINT_BOOST	(1 << 1)

#define FINGERPRINT_KEY		0x2ee
#define FP_BOOST_MS_DEFAULT	2000
#define FP_BOOST_MS_MIN		100
#define FP_BOOST_MS_MAX		5000
#define FP_RELAX_MS		200

struct boost_drv {
	struct workqueue_struct *wq;
	struct work_struct boost_work;
	struct delayed_work unboost_work;
	struct kobject *kobj;
	struct wake_lock wlock;
	atomic_t state;
	unsigned int duration_ms;
};

static struct boost_drv *boost_drv_g;

static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	get_online_cpus();
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void fp_boost_apply(struct work_struct *work)
{
	struct boost_drv *b = boost_drv_g;

	if (!b || !(atomic_read(&b->state) & FINGERPRINT_BOOST))
		return;

	pr_debug("applying max-freq boost\n");
	update_online_cpu_policy();
}

static void fp_unboost(struct work_struct *work)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	atomic_andnot(FINGERPRINT_BOOST, &b->state);
	pr_debug("releasing boost\n");
	wake_unlock(&b->wlock);
	update_online_cpu_policy();
}

static int do_cpu_boost(struct notifier_block *nb,
			unsigned long action, void *data)
{
	struct cpufreq_policy *policy = data;
	struct boost_drv *b = boost_drv_g;
	int state;

	if (!b || action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	state = atomic_read(&b->state);

	if (!(state & DRIVER_ENABLED) &&
	    policy->min == policy->cpuinfo.min_freq)
		return NOTIFY_OK;

	if (state & FINGERPRINT_BOOST)
		policy->min = policy->max;

	return NOTIFY_OK;
}

static struct notifier_block do_cpu_boost_nb = {
	.notifier_call = do_cpu_boost,
	.priority = INT_MAX,
};

static int fp_boost_cpu_online(unsigned int cpu)
{
	struct boost_drv *b = READ_ONCE(boost_drv_g);

	if (b && (atomic_read(&b->state) & FINGERPRINT_BOOST))
		cpufreq_update_policy(cpu);
	return 0;
}

void fp_boost_kick(void)
{
	struct boost_drv *b = READ_ONCE(boost_drv_g);
	int state;

	if (!b)
		return;

	state = atomic_read(&b->state);
	if (!(state & DRIVER_ENABLED))
		return;

	if (!(state & FINGERPRINT_BOOST)) {
		atomic_or(FINGERPRINT_BOOST, &b->state);
		queue_work(b->wq, &b->boost_work);
	}

	wake_lock_timeout(&b->wlock,
			  msecs_to_jiffies(READ_ONCE(b->duration_ms)));
	mod_delayed_work(b->wq, &b->unboost_work,
			 msecs_to_jiffies(READ_ONCE(b->duration_ms)));
}
EXPORT_SYMBOL_GPL(fp_boost_kick);

void fp_boost_relax(void)
{
	struct boost_drv *b = READ_ONCE(boost_drv_g);

	if (!b)
		return;

	if (!(atomic_read(&b->state) & FINGERPRINT_BOOST))
		return;

	mod_delayed_work(b->wq, &b->unboost_work,
			 msecs_to_jiffies(FP_RELAX_MS));
}
EXPORT_SYMBOL_GPL(fp_boost_relax);

static void cpu_fp_input_event(struct input_handle *handle, unsigned int type,
			       unsigned int code, int value)
{
	if (value)
		fp_boost_kick();
}

static int cpu_fp_input_connect(struct input_handler *handler,
				struct input_dev *dev,
				const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_fp_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;

	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;

	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void cpu_fp_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_fp_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT,
		.keybit = { [BIT_WORD(FINGERPRINT_KEY)] = BIT_MASK(FINGERPRINT_KEY) },
	},
	{ }
};

static struct input_handler cpu_fp_input_handler = {
	.event		= cpu_fp_input_event,
	.connect	= cpu_fp_input_connect,
	.disconnect	= cpu_fp_input_disconnect,
	.name		= "cpu_fp_handler",
	.id_table	= cpu_fp_ids,
};

static ssize_t enabled_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct boost_drv *b = boost_drv_g;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 !!(atomic_read(&b->state) & DRIVER_ENABLED));
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	struct boost_drv *b = boost_drv_g;
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	if (val) {
		atomic_or(DRIVER_ENABLED, &b->state);
	} else {
		atomic_andnot(DRIVER_ENABLED, &b->state);
		cancel_work_sync(&b->boost_work);
		cancel_delayed_work_sync(&b->unboost_work);
		atomic_andnot(FINGERPRINT_BOOST, &b->state);
		update_online_cpu_policy();
	}

	return count;
}

static ssize_t duration_ms_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 READ_ONCE(boost_drv_g->duration_ms));
}

static ssize_t duration_ms_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	if (val < FP_BOOST_MS_MIN)
		val = FP_BOOST_MS_MIN;
	if (val > FP_BOOST_MS_MAX)
		val = FP_BOOST_MS_MAX;

	WRITE_ONCE(boost_drv_g->duration_ms, val);
	return count;
}

static ssize_t active_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 !!(atomic_read(&boost_drv_g->state) & FINGERPRINT_BOOST));
}

static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);
static struct kobj_attribute duration_ms_attr = __ATTR_RW(duration_ms);
static struct kobj_attribute active_attr = __ATTR_RO(active);

static struct attribute *fp_boost_attrs[] = {
	&enabled_attr.attr,
	&duration_ms_attr.attr,
	&active_attr.attr,
	NULL,
};

static struct attribute_group fp_boost_attr_group = {
	.attrs = fp_boost_attrs,
};

static int __init cpu_fp_init(void)
{
	struct boost_drv *b;
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->wq = alloc_workqueue("fp_boost_wq",
				WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!b->wq) {
		pr_err("failed to allocate workqueue\n");
		ret = -ENOMEM;
		goto err_free;
	}

	INIT_WORK(&b->boost_work, fp_boost_apply);
	INIT_DELAYED_WORK(&b->unboost_work, fp_unboost);
	atomic_set(&b->state, DRIVER_ENABLED);
	wake_lock_init(&b->wlock, WAKE_LOCK_SUSPEND, "fp_boost");
	b->duration_ms = FP_BOOST_MS_DEFAULT;

	ret = input_register_handler(&cpu_fp_input_handler);
	if (ret) {
		pr_err("failed to register input handler: %d\n", ret);
		goto err_wq;
	}

	b->kobj = kobject_create_and_add("fp_boost", kernel_kobj);
	if (!b->kobj) {
		ret = -ENOMEM;
		goto err_input;
	}

	ret = sysfs_create_group(b->kobj, &fp_boost_attr_group);
	if (ret) {
		pr_err("failed to create sysfs group\n");
		goto err_kobj;
	}

	ret = cpufreq_register_notifier(&do_cpu_boost_nb,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("failed to register cpufreq notifier: %d\n", ret);
		goto err_sysfs;
	}

	/* Publish only after everything is ready so IRQ kick is safe. */
	cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "fp-boost:online",
				  fp_boost_cpu_online, NULL);
	WRITE_ONCE(boost_drv_g, b);
	pr_info("initialized (duration=%u ms)\n", b->duration_ms);
	return 0;

err_sysfs:
	sysfs_remove_group(b->kobj, &fp_boost_attr_group);
err_kobj:
	kobject_put(b->kobj);
err_input:
	input_unregister_handler(&cpu_fp_input_handler);
err_wq:
	destroy_workqueue(b->wq);
err_free:
	kfree(b);
	return ret;
}
late_initcall(cpu_fp_init);
