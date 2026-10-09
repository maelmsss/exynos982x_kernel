/*
 * SchedTune add-on features
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/sched.h>
#include <linux/kobject.h>
#include <linux/ems.h>
#include <trace/events/ems.h>

#include "ems.h"
#include "../sched.h"
#include "../tune.h"

/**********************************************************************
 *                            Prefer Perf                             *
 **********************************************************************/
/*
 * If the prefger_perf of the group to which the task belongs is set, the task
 * is assigned to the performance cpu preferentially.
 */
int prefer_perf_cpu(struct task_struct *p)
{
	if (schedtune_prefer_perf(p) <= 0)
		return -1;

	return select_perf_cpu(p);
}

/**********************************************************************
 *                            Prefer Idle                             *
 **********************************************************************/
static bool mark_lowest_idle_util_cpu(int cpu, unsigned long new_util,
			int *lowest_idle_util_cpu, unsigned long *lowest_idle_util)
{
	if (!idle_cpu(cpu))
		return false;

	if (new_util >= *lowest_idle_util)
		return false;

	*lowest_idle_util = new_util;
	*lowest_idle_util_cpu = cpu;

	return true;
}

static bool mark_lowest_util_cpu(int cpu, unsigned long new_util,
			int *lowest_util_cpu, unsigned long *lowest_util,
			unsigned long *target_capacity)
{
	if (capacity_orig_of(cpu) > *target_capacity)
		return false;

	if (new_util >= *lowest_util)
		return false;

	*lowest_util = new_util;
	*lowest_util_cpu = cpu;
	*target_capacity = capacity_orig_of(cpu);

	return true;
}

static int select_idle_cpu(struct task_struct *p)
{
	unsigned long lowest_idle_util = ULONG_MAX;
	unsigned long lowest_util = ULONG_MAX;
	unsigned long target_capacity = ULONG_MAX;
	int lowest_idle_util_cpu = -1;
	int lowest_util_cpu = -1;
	int target_cpu = -1;
	int cpu;
	int i;
	char state[30] = "prev_cpu";

	for_each_cpu(cpu, cpu_active_mask) {
		if (cpu != cpumask_first(cpu_coregroup_mask(cpu)))
			continue;

		for_each_cpu_and(i, tsk_cpus_allowed(p), cpu_coregroup_mask(cpu)) {
			unsigned long capacity_orig = capacity_orig_of(i);
			unsigned long new_util;

			new_util = ml_task_attached_cpu_util(i, p);
			new_util = max(new_util, ml_boosted_task_util(p));

			trace_ems_prefer_idle(p, task_cpu(p), i, capacity_orig, ml_task_util_est(p),
							new_util, idle_cpu(i));

			if (new_util > capacity_orig)
				continue;

			/* Priority #1 : idle cpu with lowest util */
			if (mark_lowest_idle_util_cpu(i, new_util,
				&lowest_idle_util_cpu, &lowest_idle_util))
				continue;

			/* Priority #2 : active cpu with lowest util */
			mark_lowest_util_cpu(i, new_util,
				&lowest_util_cpu, &lowest_util, &target_capacity);
		}

		if (cpu_selected(lowest_idle_util_cpu)) {
			strcpy(state, "lowest_idle_util");
			target_cpu = lowest_idle_util_cpu;
			break;
		}

		if (cpu_selected(lowest_util_cpu)) {
			strcpy(state, "lowest_util");
			target_cpu = lowest_util_cpu;
			break;
		}
	}

	target_cpu = !cpu_selected(target_cpu) ? task_cpu(p) : target_cpu;

	trace_ems_select_idle_cpu(p, target_cpu, state);

	return target_cpu;
}

int prefer_idle_cpu(struct task_struct *p)
{
	if (schedtune_prefer_idle(p) <= 0)
		return -1;

	return select_idle_cpu(p);
}

#ifdef CONFIG_SCHED_BORE
extern uint sched_bore;

#define BORE_LAT_SCORE 4

static bool bore_in_ux_cpuset(struct task_struct *p)
{
	struct cgroup_subsys_state *css;
	char name[32];

	if (schedtune_prefer_idle(p) > 0 ||
	    schedtune_prefer_perf(p) > 0)
		return true;

#ifdef CONFIG_CPUSETS
	css = task_css(p, cpuset_cgrp_id);
	if (!css || !css->cgroup)
		return false;
	if (cgroup_name(css->cgroup, name, sizeof(name)) <= 0)
		return false;
	return !strcmp(name, "top-app") ||
	       !strcmp(name, "foreground") ||
	       !strcmp(name, "foreground_window");
#else
	return false;
#endif
}

int bore_wakeup_cpu(struct task_struct *p)
{
	if (!sched_bore)
		return -1;
	if (p->sched_class != &fair_sched_class)
		return -1;
	if (p->se.burst_score > BORE_LAT_SCORE)
		return -1;
	if (!bore_in_ux_cpuset(p))
		return -1;
	return select_perf_cpu(p);
}
#endif
