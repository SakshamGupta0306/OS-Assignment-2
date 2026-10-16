/* SPDX-License-Identifier: GPL-2.0 */
/* include/linux/fork_guard.h - fork storm containment + tiered CPU scheduling */
#ifndef _LINUX_FORK_GUARD_H
#define _LINUX_FORK_GUARD_H

#include <linux/types.h>

/* Nice values used for the tiers (weights: -7 -> 4904, 0 -> 1024, +5 -> 335) */
#define FG_NICE_GOLD	(-7)
#define FG_NICE_BRONZE	5

/*
 * Called at the very top of kernel_clone().
 * Returns 0 to allow the fork, or a negative errno (-EAGAIN) to refuse it.
 * NEVER consults the tier table: safety overrides privilege.
 */
int fg_check_fork(void);

/*
 * Called from sched_fork(). If the current user has a non-default tier,
 * stores the tier's nice value in *nice and returns true.
 */
bool fg_tier_nice(int *nice);

#endif /* _LINUX_FORK_GUARD_H */
