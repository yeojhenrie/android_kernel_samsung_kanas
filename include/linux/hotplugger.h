/*
 * include/linux/hotplugger.h
 *
 * Copyright (c) 2017, Mark Enriquez <enriquezmark36@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _LINUX_HOTPLUGGER_H
#define _LINUX_HOTPLUGGER_H

#define DRIVER_NAME_LEN 32

struct hotplugger_driver {
	char	name[DRIVER_NAME_LEN];
	int	(*change_state)	(bool state);
	bool	(*is_enabled)	(void);
	char	**whitelist;
	struct list_head	list;
};

int hotplugger_register_driver(struct hotplugger_driver *driver);

void hotplugger_unregister_driver(struct hotplugger_driver *driver);

int hotplugger_get_running(void);

int hotplugger_disable_conflicts(struct hotplugger_driver *driver);

int hotplugger_enable_one(const char *name);

#define is_enabled_func(variable) \
static bool is_enabled (void) \
{ \
	return variable > 0 ? true : false; \
}

#define is_not_enabled_func(variable) \
static bool is_enabled (void) \
{ \
	return variable <= 0 ? true : false; \
}

#endif /* _LINUX_HOTPLUGGER_H */
