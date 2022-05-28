#include <linux/swap_stats.h>
#include <linux/syscalls.h>
#include <linux/printk.h>

SYSCALL_DEFINE0(reset_swap_stats)
{
	reset_adc_swap_stats();
	return 0;
}

SYSCALL_DEFINE3(get_swap_stats, int __user *, ondemand_swapin_num, int __user *,
		prefetch_swapin_num, int __user *, hit_on_prefetch_num)
{
	int swap_major_dur = report_adc_time_stat(ADC_SWAP_MAJOR_DUR);
	int swap_minor_dur = report_adc_time_stat(ADC_SWAP_MINOR_DUR);
	int non_swap_dur = report_adc_time_stat(ADC_NON_SWAP_DUR);
	int swap_rdma_latency = report_adc_time_stat(ADC_RDMA_LATENCY);
	int swapout_latency = report_adc_time_stat(ADC_SWAPOUT_LATENCY);

	*ondemand_swapin_num = get_adc_profile_counter(ADC_ONDEMAND_SWAPIN);
	*prefetch_swapin_num = get_adc_profile_counter(ADC_PREFETCH_SWAPIN);
	*hit_on_prefetch_num = get_adc_profile_counter(ADC_HIT_ON_PREFETCH);

	printk("Major fault: %dus, Minor fault: %dus, RDMA latency: %dus\n"
	       "Swap out latency: %dus\n"
	       // "Major cnt: %u, Minor cnt: %u, RDMA cnt: %u\n"
	       "Non-swap fault: %dus\n",
	       swap_major_dur, swap_minor_dur, swap_rdma_latency,
	       swapout_latency,
	       // (unsigned)atomic_read(&(swap_major_durations.cnt)),
	       // (unsigned)atomic_read(&(swap_minor_durations.cnt)),
	       // (unsigned)atomic_read(&(swap_rdma_latencies.cnt)),
	       non_swap_dur);

	return 0;
}

SYSCALL_DEFINE1(set_async_prefetch, int, enable)
{
	__set_async_prefetch(enable);

	printk("Current prefetch swap-in mode: %s\n",
	       enable ? "async" : "sync");
	return 0;
}

SYSCALL_DEFINE1(activate_prefetch_buf, int, enable)
{
	activate_prefetch_buffer(enable);

	printk("Current swap cache status: size %s\n",
	       enable ? "limited" : "unlimited");
	return 0;
}

SYSCALL_DEFINE1(set_swap_bw_control, int, enable)
{
	if (set_swap_bw_control) {
		set_swap_bw_control(enable);
		printk("Current swap BW status: %s\n",
		       enable ? "under control" : "uncontrolled");
		return 0;
	} else {
		printk("rswap is not registered yet!");
		return 1;
	}
}

SYSCALL_DEFINE2(get_all_procs_swap_pkts, int __user *, num_procs, char __user *,
		buf)
{
	if (get_all_procs_swap_pkts) {
		get_all_procs_swap_pkts(num_procs, buf);
		return 0;
	} else {
		printk("rswap is not registered yet!");
		return 1;
	}
}