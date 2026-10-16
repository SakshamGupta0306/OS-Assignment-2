// Inside sched_fork(struct task_struct *p, struct kernel_clone_args *args)
extern void apply_tier_nice_value(struct task_struct *p);
apply_tier_nice_value(p); // Sets NICE_TO_PRIO based on GOLD/SILVER/BRONZE
