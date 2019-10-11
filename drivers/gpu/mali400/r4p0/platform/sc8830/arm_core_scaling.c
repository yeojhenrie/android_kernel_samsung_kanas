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
 * @file arm_core_scaling.c
 * Example core scaling policy.
 */

#include "arm_core_scaling.h"

#include <linux/mali/mali_utgard.h>
#include "mali_kernel_common.h"
#include "mali_pp_scheduler.h"
#include <linux/workqueue.h>

#if !defined MALI_PP_CORE_NUMBER
#define MALI_PP_CORE_NUMBER 1
#endif
static int num_cores_total = MALI_PP_CORE_NUMBER;
static int target_num_cores = MALI_PP_CORE_NUMBER;


static void set_num_cores(struct work_struct *work);
static struct work_struct gpu_plug_work;
static DECLARE_WORK(gpu_plug_work, &set_num_cores);

static void set_num_cores(struct work_struct *work)
{
	int err = mali_perf_set_num_pp_cores(target_num_cores);
	MALI_DEBUG_ASSERT(0 == err);
	MALI_IGNORE(err);
}

void mali_core_scaling_term(void)
{
	flush_scheduled_work();
}

void mali_core_scaling_update(struct mali_gpu_utilization_data *data, int old_freq, int new_freq, int max_freq)
{
	/*
	 * This function implements a very trivial PP core scaling algorithm.
	 *
	 * It is _NOT_ of production quality.
	 * The only intention behind this algorithm is to exercise and test the
	 * core scaling functionality of the driver.
	 * It is _NOT_ tuned for neither power saving nor performance!
	 *
	 * Other metrics than PP utilization need to be considered as well
	 * in order to make a good core scaling algorithm.
	 */


	/* NOTE: this function is normally called directly from the utilization callback which is in
	 * timer context. */

	/*
	 * Calculate the predicted absolute load based on core enabled count
	 * The core count follows the rule of decreasing marginal returns.
	 * With respect to 100%:
	 * 1 core : 47%   -> 240  15fps
	 * 2 core : 72%   -> 368  23fps
	 * 3 cores: 90%   -> 464  29fps
	 * 4 cores: 100%  -> 512  32fps
	 *
	 *
	 * We have one assumption (in reality, only true when > 156KHz):
	 * the frequency linearly determines the scale of the capacity.
	 */

	static int capacity[] = {240, 368, 464, 512,0,0,0};

	int scaled_capacity = capacity[mali_pp_scheduler_get_num_cores_enabled() - 1];
	int scaled_load;
	int cores;
	int rel_load;

	int d;
	int j;

	// Trying to multiply all at once will cause an
	// overflow over 32 bit integer arithmetic
	scaled_load = (old_freq * scaled_capacity) / max_freq;
	scaled_load = (data->utilization_gpu * scaled_load) / 256;

	/*
	 * Choose the appropriate capacity where the scaled_capacity
	 * would be 60% to 90% of the new capacity.
	 *
	 * Why 60%, to have margins since this function would run in
	 * a timer that executes about 3-4 times per second and the gpu
	 * has to work at least 60 times to get that 60fps going, there's
	 * a lot of room for spiking usages that happends within that 300msec
	 *
	 * In 90% where scales become somewhat exponential, try to guess which
	 * cores should be enabled by scaling the remaining 10% to the capacity
	 * array.
	 */

	for (cores = num_cores_total; cores--; ) {
		rel_load = scaled_load * 1000;
		rel_load /= (capacity[cores] * new_freq) / max_freq;

		MALI_DEBUG_PRINT(3, ("Core scaling: check core %d: %d\n", cores, rel_load));

		if (rel_load >= 600 && rel_load < 900)
			break;

		// Special case where d is the discriminant
		if (rel_load >= 900 ) {
			d = rel_load - 900;
			d = (d * 512) / 100;

			MALI_DEBUG_PRINT(3, ("Core scaling: Guessing cores\n", cores));
			for (j = cores + 1; j < num_cores_total; j++) {
				cores = j;
				if (d <= capacity[j])
					break;
			}

// 			if (++cores >= num_cores_total)
// 				cores--;
			break;
		}

	}

	// The previous loop works on 0-index, num_cores_enabled is 1-indexed
	if (cores < 0)
		cores = 1;
	else if (++cores > num_cores_total)
		cores--;

	if (cores <= num_cores_total && cores >= 0) {
		if (mali_pp_scheduler_get_num_cores_enabled() != cores) {
			target_num_cores = cores;
			schedule_work(&gpu_plug_work);
		}
	}
}
