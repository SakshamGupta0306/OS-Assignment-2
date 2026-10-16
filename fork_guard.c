// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/fork_guard.c
 * Fork storm containment (THROTTLE -> FREEZE -> REAP) and tiered CPU
 * scheduling (GOLD / SILVER / BRONZE) for CS F372 Problem 4.
 *
 * Design rules:
 *  - fg_check_fork() is independent of the tier table (safety > privilege).
 *  - fg_lock is never held while sleeping, sending signals or renicing.
 */
#define pr_fmt(fmt) "fork_guard: " fmt

#include <linux/fork_guard.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/uaccess.h>
#include <linux/string.h>

/* ------------------------------------------------------------------ */
/* Tunables (all exposed under /proc/sys/kernel/fg_*)                  */
/* ------------------------------------------------------------------ */
static unsigned int fg_enabled     = 1;
static unsigned int fg_threshold   = 200;	/* forks per window        */
static unsigned int fg_window_ms   = 1000;	/* sliding window length   */
static unsigned int fg_throttle_ms = 40;	/* penalty delay           */
static unsigned int fg_cooldown_s  = 10;	/* calm time to forgive    */
static unsigned int fg_step_ms     = 200;	/* time spent at a stage   */
/* Users with uid below this are never monitored (system accounts, and the
 * installer account 1000 that runs the desktop). Raise/lower at runtime. */
static unsigned int fg_min_uid     = 1001;

static unsigned int fg_uid_max = 1000000;
static unsigned int fg_zero = 0, fg_one = 1;
static unsigned int fg_thr_min = 1,  fg_thr_max = 100000;
static unsigned int fg_win_min = 10, fg_win_max = 60000;
static unsigned int fg_thro_max = 1000;
static unsigned int fg_cd_min = 1,   fg_cd_max = 3600;
static unsigned int fg_step_min = 10, fg_step_max = 10000;

static struct ctl_table fg_sysctl[] = {
	{ .procname = "fg_enabled", .data = &fg_enabled,
	  .maxlen = sizeof(unsigned int), .mode = 0644,
	  .proc_handler = proc_douintvec_minmax,
	  .extra1 = &fg_zero, .extra2 = &fg_one },
	{ .procname = "fg_threshold", .data = &fg_threshold,
	  .maxlen = sizeof(unsigned int), .mode = 0644,
	  .proc_handler = proc_douintvec_minmax,
	  .extra1 = &fg_thr_min, .extra2 = &fg_thr_max },
	{ .procname = "fg_window_ms", .data = &fg_window_ms,
	  .maxlen = sizeof(unsigned int), .mode = 0644,
	  .proc_handler = proc_douintvec_minmax,
	  .extra1 = &fg_win_min, .extra2 = &fg_win_max },
	{ .procname = "fg_throttle_ms", .data = &fg_throttle_ms,
	  .maxlen = sizeof(unsigned int), .mode = 0644,
	  .proc_handler = proc_douintvec_minmax,
	  .extra1 = &fg_zero, .extra2 = &fg_thro_max },
	{ .procname = "fg_cooldown_s", .data = &fg_cooldown_s,
	  .maxlen = sizeof(unsigned int), .mode = 0644,
	  .proc_handler = proc_douintvec_minmax,
	  .extra1 = &fg_cd_min, .extra2 = &fg_cd_max },
	{ .procname = "fg_step_ms", .data = &fg_step_ms,
	  .maxlen = sizeof(unsigned int), .mode = 0644,
	  .proc_handler = proc_douintvec_minmax,
	  .extra1 = &fg_step_min, .extra2 = &fg_step_max },
	{ .procname = "fg_min_uid", .data = &fg_min_uid,
	  .maxlen = sizeof(unsigned int), .mode = 0644,
	  .proc_handler = proc_douintvec_minmax,
	  .extra1 = &fg_zero, .extra2 = &fg_uid_max },
	{ }	/* 6.8 needs the empty sentinel */
};

/* ------------------------------------------------------------------ */
/* Per-user state                                                      */
/* ------------------------------------------------------------------ */
enum { FG_SILVER = 0, FG_GOLD = 1, FG_BRONZE = 2 };
static const char * const fg_tier_name[] = { "SILVER", "GOLD", "BRONZE" };

#define FG_MAX_USERS 64

struct fg_user {
	bool used;
	kuid_t uid;
	int tier;			/* FG_SILVER/GOLD/BRONZE            */
	int stage;			/* 0 normal,1 throttle,2 freeze,3 reaped */
	unsigned long win_start;	/* start of the "cur" bucket        */
	unsigned int cur, prev;		/* fork attempts, current/previous bucket */
	unsigned long stage_since;
	unsigned long last_over;	/* last time rate was over threshold */
	struct pid *pgrp;		/* group we froze (reaped later)    */
	struct delayed_work reap_work;
	unsigned long n_throttle, n_freeze, n_reap;
};

static struct fg_user fg_tab[FG_MAX_USERS];
static DEFINE_SPINLOCK(fg_lock);

static inline unsigned int fg_uidn(kuid_t uid)
{
	return from_kuid(&init_user_ns, uid);
}

static inline int fg_nice_of(int tier)
{
	if (tier == FG_GOLD)
		return FG_NICE_GOLD;
	if (tier == FG_BRONZE)
		return FG_NICE_BRONZE;
	return 0;
}

/* caller holds fg_lock */
static struct fg_user *fg_find(kuid_t uid, bool create)
{
	struct fg_user *freeslot = NULL;
	int i;

	for (i = 0; i < FG_MAX_USERS; i++) {
		if (fg_tab[i].used) {
			if (uid_eq(fg_tab[i].uid, uid))
				return &fg_tab[i];
		} else if (!freeslot) {
			freeslot = &fg_tab[i];
		}
	}
	if (!create || !freeslot)
		return NULL;

	freeslot->used = true;
	freeslot->uid = uid;
	freeslot->tier = FG_SILVER;
	freeslot->stage = 0;
	freeslot->win_start = jiffies;
	freeslot->cur = freeslot->prev = 0;
	return freeslot;
}

/*
 * Sliding-window counter: estimate = cur + prev * (fraction of the previous
 * bucket still inside the window). Caller holds fg_lock.
 */
static unsigned int fg_rate(struct fg_user *u, unsigned long now)
{
	unsigned long win = msecs_to_jiffies(READ_ONCE(fg_window_ms));
	unsigned long el = now - u->win_start;

	if (!win)
		win = 1;
	if (el >= 2 * win) {
		u->prev = 0;
		u->cur = 0;
		u->win_start = now;
		return 0;
	}
	if (el >= win) {
		u->prev = u->cur;
		u->cur = 0;
		u->win_start += win;
		el -= win;
	}
	return u->cur + (unsigned int)((u->prev * (win - el)) / win);
}

/* caller holds fg_lock */
static void fg_deescalate(struct fg_user *u, unsigned long now)
{
	if ((u->stage == 1 || u->stage == 3) &&
	    time_after(now, u->last_over + (unsigned long)READ_ONCE(fg_cooldown_s) * HZ)) {
		pr_info("uid=%u back to stage 0 (quiet for >%u s)\n",
			fg_uidn(u->uid), fg_cooldown_s);
		u->stage = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Mechanism 1: containment ladder                                     */
/* ------------------------------------------------------------------ */

/* Delayed work: FREEZE -> REAP. A frozen group makes no more fork attempts,
 * so "still over threshold" cannot be observed from forks; the timer decides. */
static void fg_reap_fn(struct work_struct *w)
{
	struct fg_user *u = container_of(to_delayed_work(w), struct fg_user, reap_work);
	struct pid *grp = NULL;
	unsigned int uidn = 0;
	unsigned long flags, now;

	spin_lock_irqsave(&fg_lock, flags);
	if (u->stage == 2 && u->pgrp) {
		now = jiffies;
		grp = u->pgrp;
		u->pgrp = NULL;
		u->stage = 3;
		u->stage_since = now;
		u->last_over = now;
		u->n_reap++;
		u->cur = u->prev = 0;	/* clean slate: shell must work again */
		u->win_start = now;
		uidn = fg_uidn(u->uid);
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	if (!grp)
		return;

	pr_alert("REAP     uid=%u pgid=%d (SIGKILL process group)\n", uidn, pid_nr(grp));
	kill_pgrp(grp, SIGKILL, 1);	/* SIGKILL works on stopped tasks */
	put_pid(grp);
}

int fg_check_fork(void)
{
	kuid_t uid = current_uid();
	unsigned long flags, now = jiffies;
	struct fg_user *u;
	struct pid *kill_grp = NULL, *old = NULL;
	int kill_sig = 0, ret = 0;
	bool sleepit = false, arm_reap = false;
	unsigned int rate, thr, uidn;

	/* Exempt: guard disabled, kernel threads, root, and system accounts
	 * (uid < fg_min_uid). Thread creation (CLONE_THREAD) is filtered out
	 * by the hook in kernel_clone(). */
	if (!READ_ONCE(fg_enabled) || (current->flags & PF_KTHREAD) ||
	    uid_eq(uid, GLOBAL_ROOT_UID) ||
	    fg_uidn(uid) < READ_ONCE(fg_min_uid))
		return 0;

	thr = READ_ONCE(fg_threshold);
	uidn = fg_uidn(uid);

	spin_lock_irqsave(&fg_lock, flags);
	u = fg_find(uid, true);
	if (!u)			/* table full: fail open */
		goto out;

	u->cur++;		/* every attempt counts, even refused ones */
	rate = fg_rate(u, now);

	if (rate <= thr) {
		fg_deescalate(u, now);
		goto out;
	}

	u->last_over = now;
	ret = -EAGAIN;

	switch (u->stage) {
	case 0:		/* Stage 1: THROTTLE */
		u->stage = 1;
		u->stage_since = now;
		u->n_throttle++;
		pr_warn("THROTTLE uid=%u pgid=%d (>%u forks in %u ms)\n",
			uidn, task_pgrp_nr(current), thr, fg_window_ms);
		sleepit = true;
		break;
	case 1:
		if (time_before(now, u->stage_since + msecs_to_jiffies(fg_step_ms))) {
			sleepit = true;		/* keep throttling */
			break;
		}
		/* Stage 2: FREEZE (still over threshold after a full step) */
		u->stage = 2;
		u->stage_since = now;
		u->n_freeze++;
		rcu_read_lock();
		kill_grp = get_pid(task_pgrp(current));
		rcu_read_unlock();
		old = u->pgrp;
		u->pgrp = get_pid(kill_grp);
		kill_sig = SIGSTOP;
		arm_reap = true;
		pr_warn("FREEZE   uid=%u pgid=%d (SIGSTOP process group)\n",
			uidn, pid_nr(kill_grp));
		break;
	case 2:		/* frozen, reap pending: just refuse */
		break;
	default:	/* 3: repeat offender inside the cooldown -> reap again */
		u->n_reap++;
		u->cur = u->prev = 0;
		u->win_start = now;
		rcu_read_lock();
		kill_grp = get_pid(task_pgrp(current));
		rcu_read_unlock();
		kill_sig = SIGKILL;
		pr_alert("REAP     uid=%u pgid=%d (SIGKILL, repeat offender)\n",
			 uidn, pid_nr(kill_grp));
		break;
	}
out:
	spin_unlock_irqrestore(&fg_lock, flags);

	/* ---- everything below runs WITHOUT the lock ---- */
	if (kill_grp) {
		kill_pgrp(kill_grp, kill_sig, 1);
		put_pid(kill_grp);
	}
	if (old)
		put_pid(old);
	if (arm_reap)
		schedule_delayed_work(&u->reap_work, msecs_to_jiffies(fg_step_ms));
	if (sleepit && fg_throttle_ms)
		msleep(fg_throttle_ms);	/* penalty delay */
	return ret;
}

/* ------------------------------------------------------------------ */
/* Mechanism 2: tiers                                                  */
/* ------------------------------------------------------------------ */

/* Re-nice every thread of a user, including already-running ones. */
static void fg_retier_uid(kuid_t uid, int nice)
{
	struct task_struct *g, *t;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (uid_eq(task_uid(t), uid))
			set_user_nice(t, nice);
	}
	rcu_read_unlock();
}

bool fg_tier_nice(int *nice)
{
	struct fg_user *u;
	unsigned long flags;
	bool ret = false;

	spin_lock_irqsave(&fg_lock, flags);
	u = fg_find(current_uid(), false);
	if (u && u->tier != FG_SILVER) {
		*nice = fg_nice_of(u->tier);
		ret = true;
	}
	spin_unlock_irqrestore(&fg_lock, flags);
	return ret;
}

static int fg_tier_set(kuid_t uid, int tier)
{
	struct fg_user *u;
	unsigned long flags;
	kuid_t old_uid = GLOBAL_ROOT_UID;
	bool demoted = false;
	int i;

	spin_lock_irqsave(&fg_lock, flags);
	u = fg_find(uid, true);
	if (!u) {
		spin_unlock_irqrestore(&fg_lock, flags);
		return -ENOSPC;
	}
	if (tier == FG_GOLD) {		/* at most one GOLD user */
		for (i = 0; i < FG_MAX_USERS; i++) {
			if (fg_tab[i].used && fg_tab[i].tier == FG_GOLD &&
			    !uid_eq(fg_tab[i].uid, uid)) {
				fg_tab[i].tier = FG_SILVER;
				old_uid = fg_tab[i].uid;
				demoted = true;
				break;
			}
		}
	}
	u->tier = tier;
	spin_unlock_irqrestore(&fg_lock, flags);

	if (demoted) {
		pr_info("tier: uid=%u GOLD -> SILVER (demoted by new GOLD)\n",
			fg_uidn(old_uid));
		fg_retier_uid(old_uid, 0);
	}
	pr_info("tier: uid=%u -> %s (nice %d)\n", fg_uidn(uid),
		fg_tier_name[tier], fg_nice_of(tier));
	fg_retier_uid(uid, fg_nice_of(tier));	/* takes effect on running jobs */
	return 0;
}

static void fg_tier_reset(void)
{
	kuid_t changed[FG_MAX_USERS];
	unsigned long flags;
	int i, n = 0;

	spin_lock_irqsave(&fg_lock, flags);
	for (i = 0; i < FG_MAX_USERS; i++) {
		if (fg_tab[i].used && fg_tab[i].tier != FG_SILVER) {
			changed[n++] = fg_tab[i].uid;
			fg_tab[i].tier = FG_SILVER;
		}
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	for (i = 0; i < n; i++)
		fg_retier_uid(changed[i], 0);
	pr_info("tier: all users reset to SILVER\n");
}

/* ------------------------------------------------------------------ */
/* Mechanism 3: /proc                                                  */
/* ------------------------------------------------------------------ */
static int fg_stats_show(struct seq_file *m, void *v)
{
	unsigned long flags, now = jiffies;
	struct fg_user *u;
	int i;

	seq_printf(m, "enabled=%u threshold=%u window_ms=%u throttle_ms=%u cooldown_s=%u step_ms=%u min_uid=%u\n\n",
		   fg_enabled, fg_threshold, fg_window_ms, fg_throttle_ms,
		   fg_cooldown_s, fg_step_ms, fg_min_uid);
	seq_puts(m, "UID\tTIER\tFORKS\tSTAGE\tTHROTTLE\tFREEZE\tREAP\n");

	spin_lock_irqsave(&fg_lock, flags);
	for (i = 0; i < FG_MAX_USERS; i++) {
		u = &fg_tab[i];
		if (!u->used)
			continue;
		seq_printf(m, "%u\t%s\t%u\t%d\t%lu\t%lu\t%lu\n",
			   fg_uidn(u->uid), fg_tier_name[u->tier],
			   fg_rate(u, now), (fg_deescalate(u, now), u->stage),
			   u->n_throttle, u->n_freeze, u->n_reap);
	}
	spin_unlock_irqrestore(&fg_lock, flags);
	return 0;
}

static int fg_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_stats_show, NULL);
}

static const struct proc_ops fg_stats_ops = {
	.proc_open	= fg_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int fg_tier_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i, shown = 0;

	spin_lock_irqsave(&fg_lock, flags);
	for (i = 0; i < FG_MAX_USERS; i++) {
		if (fg_tab[i].used && fg_tab[i].tier != FG_SILVER) {
			if (!shown++)
				seq_puts(m, "UID\tTIER\tNICE\n");
			seq_printf(m, "%u\t%s\t%d\n", fg_uidn(fg_tab[i].uid),
				   fg_tier_name[fg_tab[i].tier],
				   fg_nice_of(fg_tab[i].tier));
		}
	}
	spin_unlock_irqrestore(&fg_lock, flags);
	if (!shown)
		seq_puts(m, "(no user is tiered; everyone is SILVER)\n");
	return 0;
}

static int fg_tier_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_tier_show, NULL);
}

static const struct proc_ops fg_tier_ops = {
	.proc_open	= fg_tier_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* Control file: "set <uid> <gold|silver|bronze>" or "reset" (root only) */
static ssize_t fg_ctl_write(struct file *f, const char __user *ubuf,
			    size_t len, loff_t *off)
{
	char buf[64], name[16];
	unsigned int uidn;
	kuid_t uid;
	int tier, ret;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;
	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (!strncmp(buf, "reset", 5)) {
		fg_tier_reset();
		return len;
	}
	if (sscanf(buf, "set %u %15s", &uidn, name) == 2) {
		if (!strcmp(name, "gold"))
			tier = FG_GOLD;
		else if (!strcmp(name, "silver"))
			tier = FG_SILVER;
		else if (!strcmp(name, "bronze"))
			tier = FG_BRONZE;
		else
			return -EINVAL;
		uid = make_kuid(&init_user_ns, uidn);
		if (!uid_valid(uid))
			return -EINVAL;
		ret = fg_tier_set(uid, tier);
		return ret ? ret : len;
	}
	return -EINVAL;
}

static const struct proc_ops fg_ctl_ops = {
	.proc_write	= fg_ctl_write,
};

static int __init fg_init(void)
{
	int i;

	for (i = 0; i < FG_MAX_USERS; i++)
		INIT_DELAYED_WORK(&fg_tab[i].reap_work, fg_reap_fn);

	register_sysctl("kernel", fg_sysctl);
	proc_create("fork_guard", 0444, NULL, &fg_stats_ops);
	proc_create("sched_tier", 0444, NULL, &fg_tier_ops);
	proc_create("fork_guard_ctl", 0200, NULL, &fg_ctl_ops);

	pr_info("initialised (threshold=%u/%ums, gold nice=%d, bronze nice=%d)\n",
		fg_threshold, fg_window_ms, FG_NICE_GOLD, FG_NICE_BRONZE);
	return 0;
}
late_initcall(fg_init);
