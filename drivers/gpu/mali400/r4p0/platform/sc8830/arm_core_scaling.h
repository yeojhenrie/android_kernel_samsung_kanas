/*
 * Copyright (C) 2013 ARM Limited. All rights reserved.
 * 
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 * 
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/**
 * @file arm_core_scaling.h
 * Example core scaling policy.
 */

#ifndef __ARM_CORE_SCALING_H__
#define __ARM_CORE_SCALING_H__

struct mali_gpu_utilization_data;

/**
 * 'Boolean' variable to indicate whether the core scaling policy is active.
 *
 * @note Exposed as a module param, disabling will activate all cores.
 */
extern int mali_core_scaling;

/**
 * Queue a task to turn off/on some cores.
 */
void mali_core_freq_set_saved(void);

/**
 * Turn off/on some cores now.
 *
 * @note This avoid the workqueue unlike mali_core_freq_set_saved()
 */
void mali_core_freq_quick_set_saved(void);

/**
 * Update core scaling policy with new utilization data.
 *
 * @return Proposed core frequency.
 *
 * @param data Utilization data.
 * @param data Current frequency.
 * @param data Maximum frequency set.
 */
int mali_core_freq_scale(struct mali_gpu_utilization_data *data, int old_freq, int new_freq1, int max_freq);


#endif /* __ARM_CORE_SCALING_H__ */
