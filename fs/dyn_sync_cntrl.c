/*
 * Dynamic sync control driver V2
 *
 * by andip71 (alias Lord Boeffla)
 *
 * All credits for original implementation to faux123
 *
 * Generalized by impasta for most android devices.
 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/writeback.h>
#include <linux/dyn_sync_cntrl.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#elif defined CONFIG_POWERSUSPEND
#include <linux/powersuspend.h>
#elif defined CONFIG_FB_MSM_MDSS // a check from lcd_notify.h
#include <linux/lcd_notify.h>
static struct notifier_block lcd_notif;
#else
#error dyn_fsync will not work without a power event trigger
#endif


// fsync_mutex protects dyn_fsync_active during transitions
static DEFINE_MUTEX(fsync_mutex);

// Declarations

bool suspend_active __read_mostly = false;
bool dyn_fsync_active __read_mostly = DYN_FSYNC_ACTIVE_DEFAULT;
extern void sync_filesystems(void);

// Functions

static void dyn_fsync_enable(bool state)
{
	mutex_lock(&fsync_mutex);

	/*
	 * Call sync() when transitioning from on to off
	 * as a good measure
	 */
	if (!state && dyn_fsync_active != state)
		sync_filesystems();

	dyn_fsync_active = state;

	mutex_unlock(&fsync_mutex);
}

static ssize_t dyn_fsync_active_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (dyn_fsync_active ? 1 : 0));
}


static ssize_t dyn_fsync_active_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int data;

	if(sscanf(buf, "%u\n", &data) == 1) {
		if (data == 1) {
			pr_info("%s: dynamic fsync enabled\n", __FUNCTION__);
			dyn_fsync_enable(true);

		} else if (data == 0) {
			pr_info("%s: dynamic fsync disabled\n", __FUNCTION__);
			dyn_fsync_enable(false);
		} else {
			pr_info("%s: bad value: %u\n", __FUNCTION__, data);
		}
	} else {
		pr_info("%s: unknown input!\n", __FUNCTION__);
	}

	return count;
}


static ssize_t dyn_fsync_version_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "version: %u.%u\n",
		DYN_FSYNC_VERSION_MAJOR,
		DYN_FSYNC_VERSION_MINOR);
}


static ssize_t dyn_fsync_suspend_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "suspend active: %u\n", suspend_active);
}

static int dyn_fsync_panic_event(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	suspend_active = false;
	pr_warn("dynamic fsync: panic - force flush!\n");
	emergency_sync();

	return NOTIFY_DONE;
}


static int dyn_fsync_notify_sys(struct notifier_block *this, unsigned long code,
				void *unused)
{
	if (code == SYS_DOWN || code == SYS_HALT) {
		suspend_active = false;
		pr_warn("dynamic fsync: reboot - force flush!\n");
		emergency_sync();
	}
	return NOTIFY_DONE;
}

/*
 * Call this function when triggering a FB blank event, generally.
 * Or, just shove it up in a wrapper function for either
 * powersuspend, earlysuspend or the pm system of your FB device.
 *
 * The bool suspend, tells whether it blanked (usually called as,
 * screen off) or unblanked (screen on).
 */
static void dyn_fsync_switch(bool suspend)
{
	mutex_lock(&fsync_mutex);

	if (suspend == false && dyn_fsync_active)
		sync_filesystems();

	suspend_active = suspend;

	mutex_unlock(&fsync_mutex);
}

// Power event triggers and handlers

#ifdef CONFIG_HAS_EARLYSUSPEND
static void dyn_fsync_suspend(struct early_suspend *h)
{
	dyn_fsync_switch(true);
}
static void dyn_fsync_resume(struct early_suspend *h)
{
	dyn_fsync_switch(false);
}
static struct early_suspend dyn_fsync_early_suspend_handler =
{
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = dyn_fsync_suspend,
	.resume = dyn_fsync_resume,
};
#include <linux/earlysuspend.h>
#elif defined CONFIG_POWERSUSPEND
static void dyn_fsync_suspend(struct power_suspend *h)
{
	dyn_fsync_switch(true);
}
static void dyn_fsync_resume(struct power_suspend *h)
{
	dyn_fsync_switch(false);
}

static struct power_suspend dyn_fsync_power_suspend_handler =
{
	.suspend = dyn_fsync_suspend,
	.resume = dyn_fsync_resume,
};
#elif defined CONFIG_FB_MSM_MDSS // LCD Notifier
static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	switch (event) {
		case LCD_EVENT_OFF_START:
			dyn_fsync_switch(false);
			break;
		case LCD_EVENT_ON_END:
			dyn_fsync_switch(true);
		default:
			break;
	}

	return 0;
}
#endif


// Module structures

static struct notifier_block dyn_fsync_notifier =
{
	.notifier_call = dyn_fsync_notify_sys,
};

static struct kobj_attribute dyn_fsync_active_attribute =
	__ATTR(Dyn_fsync_active, 0666,
		dyn_fsync_active_show,
		dyn_fsync_active_store);

static struct kobj_attribute dyn_fsync_version_attribute =
	__ATTR(Dyn_fsync_version, 0444, dyn_fsync_version_show, NULL);

static struct kobj_attribute dyn_fsync_suspend_attribute =
	__ATTR(Dyn_fsync_suspend, 0444, dyn_fsync_suspend_show, NULL);

static struct attribute *dyn_fsync_active_attrs[] =
{
	&dyn_fsync_active_attribute.attr,
	&dyn_fsync_version_attribute.attr,
	&dyn_fsync_suspend_attribute.attr,
	NULL,
};

static struct attribute_group dyn_fsync_active_attr_group =
{
	.attrs = dyn_fsync_active_attrs,
};

static struct notifier_block dyn_fsync_panic_block =
{
	.notifier_call  = dyn_fsync_panic_event,
	.priority       = INT_MAX,
};

static struct kobject *dyn_fsync_kobj;


// Module init/exit

static int dyn_fsync_init(void)
{
	int ret;

	register_reboot_notifier(&dyn_fsync_notifier);

	atomic_notifier_chain_register(&panic_notifier_list,
		&dyn_fsync_panic_block);

	dyn_fsync_kobj = kobject_create_and_add("dyn_fsync", kernel_kobj);

	if (!dyn_fsync_kobj) {
		pr_err("%s dyn_fsync_kobj create failed!\n", __FUNCTION__);
		ret = -ENOMEM;
		goto err;
	}

	ret = sysfs_create_group(dyn_fsync_kobj,
			&dyn_fsync_active_attr_group);

	if (ret) {
		pr_err("%s dyn_fsync sysfs create failed!\n", __FUNCTION__);
		kobject_put(dyn_fsync_kobj);
		goto err_sysfs;
	}

	/*
	 * Register the suspend/resume handlers
	 * Earlysuspend and powersuspend register and
	 * unregister will never fail.
	 * */
#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&dyn_fsync_early_suspend_handler);
#elif defined CONFIG_POWERSUSPEND
	register_power_suspend(&dyn_fsync_power_suspend_handler);
#elif defined CONFIG_FB_MSM_MDSS
	lcd_notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&lcd_notif) != 0) {
		pr_err("%s: Failed to register lcd callback\n", __func__);
		ret = -EFAULT;
	}
#endif

	if (ret)
		goto err_sysfs;


	pr_info("%s dynamic fsync initialisation complete\n", __FUNCTION__);

	return 0;

err_sysfs:
	kobject_put(dyn_fsync_kobj);
err:
	unregister_reboot_notifier(&dyn_fsync_notifier);
	atomic_notifier_chain_unregister(&panic_notifier_list,
		&dyn_fsync_panic_block);
	pr_err("%s dynamic fsync initialisation failed\n", __FUNCTION__);
	return ret;
}


static void dyn_fsync_exit(void)
{
	unregister_reboot_notifier(&dyn_fsync_notifier);

	atomic_notifier_chain_unregister(&panic_notifier_list,
		&dyn_fsync_panic_block);

	if (dyn_fsync_kobj != NULL)
		kobject_put(dyn_fsync_kobj);


#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&dyn_fsync_early_suspend_handler);
#elif defined CONFIG_POWERSUSPEND
	unregister_power_suspend(&dyn_fsync_power_suspend_handler);
#elif defined CONFIG_FB_MSM_MDSS
	lcd_unregister_client(&lcd_notif);
#endif

	pr_info("%s dynamic fsync unregistration complete\n", __FUNCTION__);
}

module_init(dyn_fsync_init);
module_exit(dyn_fsync_exit);

MODULE_AUTHOR("andip71");
MODULE_AUTHOR("impasta");
MODULE_DESCRIPTION("dynamic fsync - automatic fs sync optimization");
MODULE_LICENSE("GPL v2");
