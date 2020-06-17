/*
 * Copyright (C) 2013 ARM Limited. All rights reserved.
 * 
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 * 
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "arm_core_scaling.h"

#include <linux/mali/mali_utgard.h>
#include <linux/moduleparam.h>
#include "mali_kernel_common.h"
#include "mali_pp_scheduler.h"
#include <linux/workqueue.h>
#include <linux/errno.h>

#if !defined MALI_PP_CORE_NUMBER
#define MALI_PP_CORE_NUMBER 1
#endif
static int num_cores_total = MALI_PP_CORE_NUMBER;
static int target_num_cores = MALI_PP_CORE_NUMBER;

static void set_num_cores(struct work_struct *work);
static struct work_struct gpu_plug_work;
static DECLARE_WORK(gpu_plug_work, &set_num_cores);

#if defined CONFIG_MACH_KANAS || defined MACH_KANAS_W
	/*
	 * For scx35, kanas_w (Samsung SM-G355H), the capacities
	 * and scales are derived from a simple app that uses
	 * the GPU to render a still scenario.
	 *
	 * The app is the Aquarium WebGL demo, set to have the
	 * rotation and all motion stopped with everything
	 * under "Options" disabled to increase the framerate
	 * as high as possible. CPU and GPU DFS are disabled
	 * as well during the benchmark.
	 *
	 * 1 core : 51%   -> 262  (51%)   20 fps (+1)
	 * 2 core : 77%   -> 393  (26%)   30 fps
	 * 3 cores: 92%   -> 472  (15%)   36 fps
	 * 4 cores: 100%  -> 512  ( 8%)   39 fps
	 *
	 * The scale provided by the frequency is as follows:
	 * 0  312000  : 39 fps        10000.000000000000
	 * 1  256000  : 35 fps         8974.358974358974
	 * 2  208000  : 30 fps (+1)    7692.307692307692
	 * 3  156000  : 24 fps (+1)    6153.846153846154
	 * 4  128000  : 21 fps (+1)    5384.615384615385
	 * 5  104000  : 18 fps         4615.384615384615
	 * 6   85333  : 14 fps         3589.743589743590
	 * 7   78000  : 13 fps         3333.333333333333
	 * 8   69333  : 12 fps         3076.923076923077
	 * 9   64000  : 11 fps         2820.512820512821
	 * 10  52000  :  9 fps         2307.692307692308
	 */

static const int core_capacity[] = {262, 393, 472, 512};
static const int frequencies[] = {
	312000, 256000, 208000, 156000, 128000,
	104000, 85333, 78000,  69333, 64000, 52000};
static const int freq_scales[] = {
	10000, 8974, 7692, 6153, 5384,
	4615, 3589, 3333, 3076, 2820, 2307};
#else
#error Mali GPU scales and capacities not available for scaler to work
#endif

static int mali_core_minload = 160;
static int mali_core_tarutil = 205;
int mali_core_scaling = 0;

static void set_num_cores(struct work_struct *work)
{
	mali_perf_set_num_pp_cores(target_num_cores);
}

void mali_core_freq_set_saved(void) {
	if (mali_pp_scheduler_get_num_cores_enabled() == target_num_cores)
		return;

	schedule_work(&gpu_plug_work);
}

static int frequency_to_scale(int freq) {
	int l, m, h;
	l = 0;
	h = ARRAY_SIZE(frequencies);

	while (l <= h) {
		m = (l + h) / 2;
		if (freq > frequencies[m])
			h = m - 1;
		else if (freq < frequencies[m])
			l = m + 1;
		else
			return freq_scales[m]; // return scale not the index
	}
	/*
	 * This is a simple function with known input values.
	 * But when we encounter wrong/unexpected values.
	 * The programmer has missed something important.
	 */
	return -1;
}

/*
 * Determines the lowest frequency that will contain the scaled load
 * Spot the difference with the above code.
 *
 * Scaled load ranges from 0 to 10000.
 */
static int approx_scale_to_freq(int load) {
	int l, m, h;
	l = 0;
	h = ARRAY_SIZE(freq_scales);

	while (1) {
		m = (l + h) / 2;
		if (l >= h || m == l)
			break;

		if (load > freq_scales[m])
			h = m - 1;
		else if (load < freq_scales[m])
			l = m;
		else
			break;
	}
	return frequencies[m];
}

int mali_core_freq_scale(struct mali_gpu_utilization_data *data, int old_freq, int new_freq1, int max_freq) {
	int scaled_capacity; // [0,512]
	int scaled_load; // range: [0,512]
	int cores; // range: in loop [0,3], elsewhere [1,4]
	int rel_load; // predicted load range: [0, 10000]
	int new_freq; // next frequency
	int max_freq_scale;

	/*
	 * Compute absolute load.
	 * We base the computations on data->utilization_gpu which is
	 * a relative quantity with respect to the current capacity.
	 * The capacities vary between the number of active cores and
	 * are empirically derived.
	 *
	 * The core frequencies also scale the capacity, although
	 * in contrast with the previous assumption, it does not
	 * scale linearly.
	 *
	 * Note:
	 * When data->utilization_gpu is nearly maxed out
	 * it has lost nearly all useful information we can
	 * use to deduce how much "higher" the load
	 * is compared to the current capacity.
	 *
	 * This also implies that this algorithm not always great
	 * in scaling up, or it can be said that this "converges"
	 * slower to the 'higher' load since it doesn't know
	 * how 'high' the load is.
	 */
	scaled_capacity =
		core_capacity[
			mali_pp_scheduler_get_num_cores_enabled() -1];

	scaled_load =
		( data->utilization_gpu * scaled_capacity *
		  frequency_to_scale(old_freq) ) / 2560000;

	max_freq_scale = frequency_to_scale(max_freq);

	MALI_DEBUG_PRINT(3, ("Core scaling: cores active %d/4 cap:%d load:%d\n",
			     mali_pp_scheduler_get_num_cores_enabled(),
			     scaled_capacity, scaled_load));

	// Setting minimum capacity hastes scaling up from idle
	if (scaled_load < mali_core_minload)
		scaled_load = mali_core_minload;

	// Another weird trick to speed up the slow scale up is to
	// pull up the load to the current capacity
	if (231 <= data->utilization_gpu && (231 <= mali_core_tarutil )) {
		scaled_load = mali_pp_scheduler_get_num_cores_enabled();
		scaled_load = core_capacity[scaled_load - 1];
	}
	MALI_DEBUG_PRINT(3, ("Core scaling: scaled load:%d\n", scaled_load));
	/*
	 * Check for the appropriate number of cores that may contain the
	 * load.
	 *
	 * Then fine tune the frequency so that the next data->utilization_gpu
	 * may become close to mali_core_tarutil (by default, it's 205).
	 *
	 * NOTE: This algorithm can scale up because of
	 * mali_core_tarutil < 256 which also dictates how much
	 * of a processing power will be reserved for the
	 * time being (in case of utilization spikes, etc...)
	 */
	for (cores = 0; cores < num_cores_total ; cores++) {
		scaled_capacity =
			(core_capacity[cores] * max_freq_scale)
			 / 10000;

		rel_load =
			(2570000 * scaled_load)
			/ (scaled_capacity * (mali_core_tarutil + 1));

		MALI_DEBUG_PRINT(3, ("Core scaling: core %d capacity:%d rel_load:%d\n",
			     cores,
			     scaled_capacity, rel_load));

		if (rel_load <= 10000)
			break;
	}

	/*
	 * No capacities could contain the load
	 * but we still expect that rel_load to be within [0,10000]
	 */
	if (rel_load > 10000)
		rel_load = 10000;

	target_num_cores = cores == num_cores_total ? cores : cores + 1;

	MALI_DEBUG_PRINT(3, ("Core scaling: target cores: %d\n", target_num_cores));

	/*
	 * The relative load is computed from a core capacity
	 * that was scaled down by the arbitrarily set max frequency.
	 * This was to have an 'absolute' reference in comparing loads.
	 * Due to this, the rel_load variable will not reflect the
	 * 'set' limits when max_freq is less than default maximum.
	 *
	 * To accommodate this, simply scale down the current rel_load
	 * with the maximum set frequency.
	 */
	rel_load = (rel_load * max_freq_scale) / (10000);

	new_freq = approx_scale_to_freq(rel_load);
	MALI_DEBUG_PRINT(3, ("Core scaling: desired load: %d freq:%d->%d\n",
			     rel_load, old_freq, new_freq));

	return new_freq;
}

static int param_set_minload(const char *val, const struct kernel_param *kp)
{
	int prev = *((int *)kp->arg);
	int ret = param_set_int(val, kp);

	if (!ret) {
		if (*((int *)kp->arg) < 0) {
			pr_err("%s takes only non-negative integers\n",
			       kp->name);
			*((int *)kp->arg) = prev;
			ret = -EINVAL;
		}

		if (*((int *)kp->arg) > core_capacity[MALI_PP_CORE_NUMBER - 1]) {
			*((int *)kp->arg) = core_capacity[MALI_PP_CORE_NUMBER - 1];
		}
	}

	return ret;
}

static struct kernel_param_ops param_ops_core_minload = {
	.set = param_set_minload,
	.get = param_get_int,
};

module_param_cb(mali_core_minload, &param_ops_core_minload, &mali_core_minload, 0644);
MODULE_PARM_DESC(mali_core_minload, "Core scaler's reserved load at idle");

static int param_set_tarutil(const char *val, const struct kernel_param *kp)
{
	int prev = *((int *)kp->arg);
	int ret = param_set_int(val, kp);

	if (!ret) {
		if (*((int *)kp->arg) < 0) {
			pr_err("%s takes only non-negative integers\n",
			       kp->name);
			*((int *)kp->arg) = prev;
			ret = -EINVAL;
		}

		if (*((int *)kp->arg) > 256) {
			*((int *)kp->arg) = 256;
		}
	}

	return ret;
}

static struct kernel_param_ops param_ops_core_tarutil = {
	.set = param_set_tarutil,
	.get = param_get_int,
};

module_param_cb(mali_core_tarutil, &param_ops_core_tarutil, &mali_core_tarutil, 0644);
MODULE_PARM_DESC(mali_core_tarutil, "Core scaler's target utilization score for the next iteration");

static int param_set_core_scaling(const char *val, const struct kernel_param *kp)
{
	int prev = *((int *)kp->arg);
	int ret = param_set_int(val, kp);

	if (!ret && mali_core_scaling == 0 && prev != 0) {
		target_num_cores = num_cores_total;
		mali_core_freq_set_saved();
	} else if (mali_core_scaling && mali_core_scaling != 1){
		mali_core_scaling = 1;
	}
	return ret;
}

static struct kernel_param_ops param_ops_core_scaling = {
	.set = param_set_core_scaling,
	.get = param_get_int,
};

module_param_cb(mali_core_scaling, &param_ops_core_scaling, &mali_core_scaling, 0644);
MODULE_PARM_DESC(mali_core_scaling, "Core scaler's on-off switch: 0 for off; any number for on");
