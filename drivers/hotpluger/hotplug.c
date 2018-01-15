/*
 *  linux/drivers/hotplugger/hotplug.c
 *
 * Copyright (c) 2017, Mark Enriquez <enriquezmark36@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include <linux/hotplugger.h>

static LIST_HEAD(hotplugger_driver_list);
static DEFINE_MUTEX(hotplugger_driver_mutex);
atomic_t enabled = ATOMIC_INIT(1);

#define abort_disable(retval) 						\
	if (atomic_read(&enabled) <= 0) {				\
		pr_info("%s: hotplugger is disabled\n", __func__);	\
		return retval;						\
	}

static void __inspect(struct hotplugger_driver *driver) {
	pr_debug("----------------%s----------------\n", driver->name);
	pr_debug("address: %p\n", driver);
	pr_debug("name: %s\n", driver->name);
	pr_debug("change_state: %pf<%p>\n", driver->change_state,
	         driver->change_state);
	pr_debug("is_enabled: %pf<%p>\n", driver->is_enabled, driver->is_enabled);
	pr_debug("list: %s\n", driver->whitelist ? "yes" : "no");
}

static struct hotplugger_driver *find_driver(const char *name,
                                             struct hotplugger_driver *driver){
	struct hotplugger_driver *d;

	if (name) {
		list_for_each_entry(d, &hotplugger_driver_list, list)
			if (!strnicmp(name, d->name, DRIVER_NAME_LEN))
				return d;
	} else if (driver) {
		list_for_each_entry(d, &hotplugger_driver_list, list)
			if (driver == d)
				return d;
	}

	pr_debug("%s: No matches!\n", __func__);
	return NULL;
}

static bool find_name_in_list(char **list, char *name) {
	char *l;

    if (IS_ENABLED(CONFIG_HOTPLUGGER_INTERFACE_DEBUG)) {
		if (list == NULL) {
			pr_debug("%s: list is NULL\n", __func__);
		} else if (*list == NULL) {
			pr_debug("%s: list is EMPTY\n", __func__);
		}
	}

	if (name && list) {
		for ( ; (l = *list) ; list++)
			if (!strnicmp(l, name, DRIVER_NAME_LEN))
				return true;
	}

	return false;
}

static int state_change(struct hotplugger_driver *caller,
                                 struct hotplugger_driver *d,
                                 bool state) {
	if ((d) && (d != caller) && d->change_state &&
	    (d->is_enabled() != state)) {
		pr_debug("%s: %s \"%s\" driver \n", __func__,
		          state ? "enabling" : "disabling", d->name);

		return d->change_state(state);
	}

	return -EFAULT;
}

static ssize_t show_drivers_by_state(char *buf, int state) {
	struct hotplugger_driver *d;
	char *fmt = "%s ";
	ssize_t bufsize = ((PAGE_SIZE / sizeof(char)) - (DRIVER_NAME_LEN + 2));
	ssize_t len = bufsize;
	ssize_t x;

	mutex_lock(&hotplugger_driver_mutex);
	list_for_each_entry(d, &hotplugger_driver_list, list) {
		if (state == 2) {
			if (d->is_enabled())
				fmt = "[%s] ";
			else
				fmt= "%s ";
		} else if (d->is_enabled() != state) {
			continue;
		}
		x = scnprintf(buf, len, fmt, d->name);
		buf += x;
		len -= x;

		if (len <= 0)
			break;
	}
	mutex_unlock(&hotplugger_driver_mutex);

	x = bufsize - len;
	if (!x)
		buf += sprintf(buf, "[none]");

	*buf = '\n';

	return x + 1;
}

static ssize_t store_state_by_name(const char *buf,
                                     size_t count, bool state) {
	unsigned int ret;
	char name[DRIVER_NAME_LEN];
	struct hotplugger_driver *d;

	abort_disable(-EPERM);

	ret = sscanf(buf, "%30s", name);
	if (ret != 1)
		return -EINVAL;

	d = find_driver(name, NULL);

	if (d == NULL)
		return -EINVAL;

	pr_debug("%s: \"%s\" driver found!\n", __func__, name);

	ret = state_change(NULL, d, state);

	if (ret)
		return ret;

	return count;
}

static void selective_suspension (struct hotplugger_driver *driver) {
	struct hotplugger_driver *d;

	list_for_each_entry(d, &hotplugger_driver_list, list) {
		/* check if the other driver's name is on the whitelist */
		if (d != driver &&
		    find_name_in_list(driver->whitelist, d->name) == true) {
			pr_debug("%s: driver \"%s\" is whitelisted "
				 "in driver \"%s\".\n", __func__,
				 d->name, driver->name);

			/* We need both of the drivers to whitelist
			 * themselves as a safety measure.
			 * The condition is too long so the == true is ommited
			 */
			if (find_name_in_list(d->whitelist, driver->name))
				continue;

			pr_debug("%s: BUT driver \"%s\" is NOT whitelisted"
				 " in  driver \"%s\".\n",
				 __func__, driver->name, d->name);
		}
		state_change(driver, d, false);
	}
}

/*************
 * sysfs start
 *************/

static ssize_t show_enabled(struct device *dev,
                            struct device_attribute *attr, char *buf) {
	return snprintf(buf, 10, "%d\n", atomic_read(&enabled));
}

static ssize_t store_enabled(struct device *dev,
                             struct device_attribute *attr,
                             const char *buf, size_t count) {
	bool state;
	int ret;
	unsigned int input;

	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	state = input > 0 ? true : false;
	pr_debug("%s: setting %s interface's state\n",
	          __func__,
	          state ? "enabled" : "disabled");

	atomic_set(&enabled, state);

	return count;
}

static ssize_t show_enable_driver(struct device *dev,
                                   struct device_attribute *attr,char *buf) {
	return show_drivers_by_state(buf, true);
}

static ssize_t store_enable_driver(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count) {
	return store_state_by_name(buf, count, true);
}

static ssize_t show_disable_driver(struct device *dev,
                                   struct device_attribute *attr,char *buf) {
	return show_drivers_by_state(buf, false);
}

static ssize_t store_disable_driver(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count) {
	return store_state_by_name(buf, count, false);
}

static ssize_t show_available_drivers(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf) {
	return show_drivers_by_state(buf, 2);
}

static DEVICE_ATTR(available_drivers, 0444, show_available_drivers, NULL);
static DEVICE_ATTR(disable_driver, 0644, show_disable_driver, store_disable_driver);
static DEVICE_ATTR(enable_driver, 0644, show_enable_driver, store_enable_driver);
static DEVICE_ATTR(enabled, 0644, show_enabled, store_enabled);

static struct attribute *hotplugger_attrs[] = {
	&dev_attr_available_drivers.attr,
	&dev_attr_disable_driver.attr,
	&dev_attr_enable_driver.attr,
	&dev_attr_enabled.attr,
	NULL
};

static struct attribute_group hotplugger_attr_group = {
	.attrs = hotplugger_attrs,
	.name = "hotplugger",
};

/*************
 * sysfs end
 *************/

int hotplugger_register_driver(struct hotplugger_driver *driver) {
	int err = -EINVAL;

	if (!driver || !driver->name) {
		pr_debug("%s: driver is invalid\n", __func__);
		return err;
	}

	if (IS_ENABLED(CONFIG_HOTPLUGGER_INTERFACE_DEBUG))
		__inspect(driver);

	mutex_lock(&hotplugger_driver_mutex);
	/* Checks */
	if (find_driver(driver->name, NULL)) {
		pr_debug("%s: A driver with name \"%s\" exists!\n",
		          __func__, driver->name);
	} else {
		INIT_LIST_HEAD(&(driver->list));
		list_add_tail(&(driver->list), &hotplugger_driver_list);
		pr_debug("%s: driver \"%s\" registered\n",
		          __func__, driver->name);
		err = 0;
	}
	mutex_unlock(&hotplugger_driver_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(hotplugger_register_driver);

void hotplugger_unregister_driver(struct hotplugger_driver *driver) {
	struct hotplugger_driver *d;

	if (!driver)
		return;

	mutex_lock(&hotplugger_driver_mutex);

	d = find_driver(NULL, driver);
	if (d) {
		state_change(NULL, driver, false);
		pr_debug("%s: Removing \"%s\" driver from list\n",
		          __func__, driver->name);
		list_del_init(&(driver->list));
	}

	mutex_unlock(&hotplugger_driver_mutex);

	return;
}
EXPORT_SYMBOL_GPL(hotplugger_unregister_driver);

static int __init hotplugger_init(void) {
	int ret;

	pr_debug("hotplugger sysfs init START>.<\n");
	ret = sysfs_create_group(kernel_kobj, &hotplugger_attr_group);
	if (ret) {
		pr_err("%s: sysfs_create_group failed\n", __func__);
		return ret;
	}
	pr_debug("hotplugger sysfs init END>.<\n");

	return 0;
}

int hotplugger_get_running(void) {
	int num = 0;
	struct hotplugger_driver *d = NULL;

	mutex_lock(&hotplugger_driver_mutex);
	list_for_each_entry(d, &hotplugger_driver_list, list)
		num++;
	mutex_unlock(&hotplugger_driver_mutex);

	return num;
}
EXPORT_SYMBOL_GPL(hotplugger_get_running);

int hotplugger_disable_conflicts(struct hotplugger_driver *driver) {
	struct hotplugger_driver *d;
	int ret = 0;
	static int lock_invoice = 0;

	abort_disable(-EPERM);

	if (driver == NULL) {
		pr_debug("%s: undefined driver. Aborting.\n", __func__);
		return -ENXIO;
	}

	if (mutex_is_locked(&hotplugger_driver_mutex)) {
		pr_debug("%s: another \"disabling\" is in progress\n", __func__);
		lock_invoice++;
		if (lock_invoice >= 2) {
			pr_debug("%s: There seems to be a deadlock."
				 "Forcibly unlocking. Retry this again.\n",
				 __func__);
			mutex_unlock(&hotplugger_driver_mutex);
			lock_invoice = 0;
		}
		return -EBUSY;
	}

	mutex_lock(&hotplugger_driver_mutex);

	/* First, check if driver exists */
	d = find_driver(NULL, driver);
	if (d == NULL) {
		pr_debug("%s: driver \"%s\" is unregistered, aborting...\n",
			 __func__, driver->name);
		ret = -ENODEV;
		goto clean_up_w_mutex;
	}

	pr_debug("%s: driver \"%s\" requests conflict resolution\n",
		 __func__, driver->name);

	/* Check if the driver specified a white list*/
	if (driver->whitelist && *driver->whitelist != NULL) {
		pr_debug("%s: whitelist found!\n", __func__);
		selective_suspension(driver);
	} else {
		list_for_each_entry(d, &hotplugger_driver_list, list) {
			state_change(driver, d, false);
		}
	}

clean_up_w_mutex:
	mutex_unlock(&hotplugger_driver_mutex);
	lock_invoice = 0;

	pr_debug("%s: disable_conflicts is done\n", __func__);
	return ret;
}
EXPORT_SYMBOL_GPL(hotplugger_disable_conflicts);

int hotplugger_enable_one(const char *name) {
	struct hotplugger_driver *d = find_driver(name, NULL);

	abort_disable(-EPERM);

	if (d == NULL)
		return -EINVAL;

	return state_change(NULL, d, true);
}
EXPORT_SYMBOL_GPL(hotplugger_enable_one);

int hotplugger_disable_one(const char *name) {
	struct hotplugger_driver *d = find_driver(name, NULL);

	abort_disable(-EPERM);

	if (d == NULL)
		return -EINVAL;

	return state_change(NULL, d, false);
}
EXPORT_SYMBOL_GPL(hotplugger_disable_one);

MODULE_AUTHOR("ME");
MODULE_DESCRIPTION("Manage hotplug modules so they"
                   " won't run simultaneously, naivete style");
MODULE_LICENSE("GPL");

fs_initcall(hotplugger_init);
