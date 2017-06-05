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

#define abort_disable(retval) \
	if (atomic_read(&enabled) <= 0) {						\
		pr_info("%s: hotplugger is disabled\n", __func__);	\
		return retval;										\
	}														\

static struct hotplugger_driver *__find_driver(const char *name)
{
	struct hotplugger_driver *d = NULL;

	if (name == NULL) {
		pr_debug("%s: driver name search query is NULL\n", __func__);
		return NULL;
	}

	list_for_each_entry(d, &hotplugger_driver_list, list)
		if (!strnicmp(name, d->name, DRIVER_NAME_LEN))
			return d;

	return NULL;
}

static struct hotplugger_driver *__find_driver_ptr(struct hotplugger_driver *driver)
{
	struct hotplugger_driver *d = NULL;

	if (driver == NULL) {
		pr_debug("%s: driver var is NULL\n", __func__);
		return NULL;
	}

	list_for_each_entry(d, &hotplugger_driver_list, list)
		if (driver == d)
			return d;

	return NULL;
}

static bool __find_name_in_list(char **list, char *name)
{
	char *l;

	pr_debug("%s was called\n", __func__);

	if (list == NULL) {
		pr_debug("%s: list is NULL\n", __func__);
		return false;
	}
	if (*list == NULL) {
		pr_debug("%s: list is EMPTY\n", __func__);
		return false;
	}
	if (name == NULL) {
		pr_debug("%s: driver name is NULL\n", __func__);
		return false;
	}

	for ( ; (l = *list) ; list++)
		if (!strnicmp(l, name, DRIVER_NAME_LEN))
			return true;

	return NULL;
}

static int __state_change(struct hotplugger_driver *caller,
                                 struct hotplugger_driver *d,
                                 bool state)
{
	int ret = -EFAULT;

	if ((d) && (d != caller) && d->change_state && 
	    (d->is_enabled() != state)) {
		pr_debug("%s: %s \"%s\" driver \n", __func__,
		          state ? "enabling" : "disabling", d->name);

		ret = d->change_state(state);

		if (ret)
			pr_debug("%s: %s: %pf failed with err %d\n", __func__,
		         d->name, d->change_state, ret);
	}
#ifdef DEBUG
	  else if (d->is_enabled() == state) {
		pr_debug("%s: \"%s\" driver is already %s\n", __func__,
		         d->name, state ? "enabled" : "disabled");
	} else if (!d->change_state) {
		pr_debug("%s: change_state for \"%s\" driver is NULL\n", __func__,
		         d->name);
	}
#endif

	return ret;
}

static ssize_t __show_drivers_by_state(char *buf, bool state)
{
	struct hotplugger_driver *d;
	ssize_t i = 0;
	ssize_t buf_size = ((PAGE_SIZE / sizeof(char)) - (DRIVER_NAME_LEN + 2));

	mutex_lock(&hotplugger_driver_mutex);

	list_for_each_entry(d, &hotplugger_driver_list, list) {
		if (i >= buf_size) {
			pr_info("%s: buffer is full...\n", __func__);
			break;
		}

		if (d && d->name && d->is_enabled && d->is_enabled() == state) {
			i += scnprintf(&buf[i], DRIVER_NAME_LEN + 1, "%s ", d->name);
		} 
#ifdef DEBUG
		  else if (!d->name) {
			pr_debug("%s: d->name is NULL\n", __func__);
		} else if (!d->is_enabled) {
			pr_debug("%s: d->is_enabled is NULL\n", __func__);
		} else if (!d) {
			pr_debug("%s: d is NULL\n", __func__);
		}
#endif
	}
	if (i == 0)
		i += sprintf(buf, "NaN");

	mutex_unlock(&hotplugger_driver_mutex);

	i += sprintf(&buf[i], "\n");
	pr_debug("%s: printed %d bytes\n", __func__, i);
	return i;
}

static ssize_t __store_state_by_name(const char *buf,
                                     size_t count, bool state)
{
	unsigned int ret;
	char name[DRIVER_NAME_LEN];
	struct hotplugger_driver *d;

	abort_disable(-EPERM);

	ret = sscanf(buf, "%31s", name);
	if (ret != 1) {
		pr_debug("%s: sscanf returns %d\n", __func__, ret);
		return -EINVAL;
	}

	d = __find_driver(name);

	if (d == NULL) {
		pr_debug("%s: \"%s\" driver is not found\n", __func__, name);
		return -EINVAL;
	} else {
		pr_debug("%s: \"%s\" driver found!\n", __func__, name);
	}

	ret = __state_change(NULL, d, state);

	if (ret)
		return ret;
	else
		return count;
}


static int hotplugger_disable_one(struct hotplugger_driver *caller,
                                          struct hotplugger_driver *d)
{
	return __state_change(caller, d, false);
}

/*************
 * sysfs start
 *************/


static ssize_t show_enabled(struct device *dev,
                            struct device_attribute *attr, char *buf)
{
	return snprintf(buf,10, "%d\n", atomic_read(&enabled));
}

static ssize_t store_enabled(struct device *dev,
                             struct device_attribute *attr,
                             const char *buf, size_t count)
{
	bool state;
	int ret;
	unsigned int input;

	ret = sscanf(buf, "%u", &input);

	if (ret != 1) {
		pr_debug("%s: sscanf returns %d\n", __func__, ret);
		return -EINVAL;
	}

	state = input > 0 ? true : false;
	pr_debug("%s: setting %s state\n",
	          __func__,
	          state ? "enabled" : "disabled");
	atomic_set(&enabled, state);

	return count;
}

static ssize_t show_enable_driver(struct device *dev,
                                   struct device_attribute *attr,char *buf)
{
	return __show_drivers_by_state(buf, true);
}

static ssize_t store_enable_driver(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count)
{
	return __store_state_by_name(buf, count, true);
}

static ssize_t show_disable_driver(struct device *dev,
                                   struct device_attribute *attr,char *buf)
{
	return __show_drivers_by_state(buf, false);
}

static ssize_t store_disable_driver(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count)
{
	return __store_state_by_name(buf, count, false);
}

static ssize_t show_available_drivers(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf)
{
	ssize_t i = 0;
	ssize_t buf_size = ((PAGE_SIZE / sizeof(char)) - (DRIVER_NAME_LEN + 2));
	struct hotplugger_driver *d;

	mutex_lock(&hotplugger_driver_mutex);

	list_for_each_entry(d, &hotplugger_driver_list, list) {
		if (i >= buf_size) {
			pr_info("%s: buffer is full...\n", __func__);
			break;
		}
		if (d->is_enabled())
			i += scnprintf(&buf[i], DRIVER_NAME_LEN + 1, "[%s] ", d->name);
		else
			i += scnprintf(&buf[i], DRIVER_NAME_LEN + 1, "%s ", d->name);
	}
	if (i == 0)
		sprintf(buf, "NaN");

	mutex_unlock(&hotplugger_driver_mutex);

	i += sprintf(&buf[i], "\n");
	pr_debug("%s: finishing with %d bytes\n", __func__, i);
	return i;
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
#ifdef DEBUG
void __inspect(struct hotplugger_driver *driver) {
	pr_debug("----------------%s----------------\n", driver->name);
	pr_debug("address: %p\n", driver);
	pr_debug("name: %s\n", driver->name);
	pr_debug("change_state: %pf<%p>\n", driver->change_state, driver->change_state);
	pr_debug("is_enabled: %pf<%p>\n", driver->is_enabled, driver->is_enabled);
	pr_debug("list: %s\n", driver->whitelist ? "yes" : "no");
}
#endif

int hotplugger_register_driver(struct hotplugger_driver *driver)
{
	int err = -EINVAL;
	struct hotplugger_driver *d;

	if (!driver) {
		pr_debug("%s: driver is NULL\n", __func__);
		return err;
	}

	if (!driver->name) {
		pr_debug("%s: driver name is NULL\n", __func__);
		return err;
	}

#ifdef DEBUG
	__inspect(driver);
#endif

	mutex_lock(&hotplugger_driver_mutex);
	/* Checks */
	d = __find_driver(driver->name);
	if (d && (d != driver)) {
		pr_debug("%s: A driver with name \"%s\" exists!\n",
		          __func__, driver->name);
	} else if (d && d == driver) {
		pr_debug("%s: driver \"%s\" already registered\n",
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

void hotplugger_unregister_driver(struct hotplugger_driver *driver)
{
	struct hotplugger_driver *d;

	if (!driver) {
		pr_debug("%s: driver is NULL\n", __func__);
		return;
	}

	mutex_lock(&hotplugger_driver_mutex);
	d = __find_driver(driver->name);
	if ((d) && (d == driver)) {
			__state_change(NULL, driver, false);
			pr_debug("%s: Removing \"%s\" driver from list\n",
			          __func__, driver->name);
			list_del_init(&(driver->list));
	} else if ((d) && (d != driver)) {
		pr_debug("%s: A driver with name \"%s\" exists but their pointers"
		         "differ [%p(list) =/= %p(yours)]\n",
		         __func__, driver->name, d, driver);
	} else {
		pr_debug("%s: No matching \"%s\" driver found!\n",
		         __func__, driver->name);
	}
	mutex_unlock(&hotplugger_driver_mutex);

	return;
}
EXPORT_SYMBOL_GPL(hotplugger_unregister_driver);

static int __init hotplugger_init(void)
{
	int ret;

	pr_debug("hotplugger sysfs init START!!!\n");
	ret = sysfs_create_group(kernel_kobj, &hotplugger_attr_group);
	if (ret) {
		pr_err("%s: sysfs_create_group failed\n", __func__);
		return ret;
	}
	pr_debug("hotplugger sysfs init END!!!\n");

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

int hotplugger_disable_conflicts(struct hotplugger_driver *driver)
{
	struct hotplugger_driver *d;
	int ret = 0;
	static int lock_invoice = 0;

	abort_disable(-EPERM);

	if (driver == NULL) {
		pr_debug("%s: undefined driver.\n", __func__);
		pr_debug("%s: aborting\n", __func__);
		return -ENXIO;
	}

	if (mutex_is_locked(&hotplugger_driver_mutex)) {
		pr_debug("%s: another \"disabling\" is in progress\n", __func__);
		lock_invoice++;
		if (lock_invoice >= 2) {
			pr_debug("%s: There seems to be a deadlock. Forcibly unlocking\n", __func__);
			pr_debug("%s: retry this again.\n", __func__);
			mutex_unlock(&hotplugger_driver_mutex);
			lock_invoice = 0;
		}
		return -EBUSY;
	}


	mutex_lock(&hotplugger_driver_mutex);

	/* TODO: refactor this whole function as it uses more than 3 linear searches
	 *       and a lot of duplications due to design error.
	 *       No one would try and register about 100 hotplug drivers/modules
	 *       right? */
	/* First, check if driver exists */
	d =__find_driver_ptr(driver);
	if (d == NULL) {
		pr_debug("%s: driver \"%s\" is unregistered\n", __func__, driver->name);
		pr_debug("%s: aborting...\n", __func__);
		ret = -ENODEV;
	} else {
		pr_debug("%s: driver \"%s\" requests conflict resolution\n", __func__,
		          driver->name);

		/* Check if the driver specified a white list*/
		if (driver->whitelist && *driver->whitelist != NULL) {
			pr_debug("%s: whitelist found!\n", __func__);
			list_for_each_entry(d, &hotplugger_driver_list, list) {
				/* check if the other driver's name is on the whitelist */
				if (!d->is_enabled())
					continue;

				if (d != driver &&
					__find_name_in_list(driver->whitelist, d->name) == true) {
					pr_debug("%s: driver \"%s\" is whitelisted in  driver \"%s\".\n",
					          __func__, d->name, driver->name);

					/* We need both of the drivers to whitelist themselves as a
					   safety measure
					 */
					if (d->whitelist && *d->whitelist == NULL) {
						pr_debug("%s: BUT driver \"%s\" is NOT whitelisted"
						         " in  driver \"%s\".\n",
						          __func__, driver->name, d->name);
						hotplugger_disable_one(driver, d);
					} else if ((d->whitelist && *d->whitelist != NULL) &&
						__find_name_in_list(d->whitelist, driver->name) == true) {
						pr_debug("%s: Both are whitelisted for each other.\n",
						          __func__);
						continue;
					}
				} else {
					hotplugger_disable_one(driver, d);
				}
			}
		} else if ((driver->whitelist == NULL) ||
		           (driver->whitelist && *driver->whitelist == NULL)) {
			pr_debug("%s: driver \"%s\" has empty lists, disabling all\n",
			          __func__, driver->name);
			list_for_each_entry(d, &hotplugger_driver_list, list)
				hotplugger_disable_one(driver, d);
		}
	}
	// Reset
	mutex_unlock(&hotplugger_driver_mutex);
	lock_invoice = 0;

	pr_debug("%s: disable_conflicts is done\n", __func__);
	return ret;
}
EXPORT_SYMBOL_GPL(hotplugger_disable_conflicts);

int hotplugger_enable_one(const char *name)
{
	struct hotplugger_driver *d = __find_driver(name);

	abort_disable(-EPERM);

	if (d == NULL) {
		pr_debug("%s: \"%s\" driver cannot be found\n",
		          __func__, name);
		return -EINVAL;
	}

	pr_debug("%s: activating \"%s\" driver\n", __func__, name);

	return d->change_state(true);
}
EXPORT_SYMBOL_GPL(hotplugger_enable_one);

MODULE_AUTHOR("ME");
MODULE_DESCRIPTION("Manage hotplug modules so they"
                   " won't run simultaneously naivete style");
MODULE_LICENSE("GPL");

fs_initcall(hotplugger_init);
