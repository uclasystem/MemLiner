#ifndef _LINUX_EXTENDED_SYSCALLS_H
#define _LINUX_EXTENDED_SYSCALLS_H

/* [ADC] extended syscalls.
 * Syscalls for profiling.
 */
asmlinkage long sys_reset_swap_stats(void);
asmlinkage long sys_get_swap_stats(int __user *on_demand_swapin_num,
				   int __user *prefetch_swapin_num,
				   int __user *hiton_swap_cache_num);
asmlinkage long sys_set_async_prefetch(int enable);

asmlinkage long sys_set_prefetch_buffer(int enable);
asmlinkage long sys_get_all_procs_swap_pkts(int __user *num_procs,
					    char __user *buf);

#endif // _LINUX_EXTENDED_SYSCALLS_H