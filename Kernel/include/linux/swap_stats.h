/**
 * swap_stats.h - collect swap stats for profiling
 */

#ifndef _LINUX_SWAP_STATS_H
#define _LINUX_SWAP_STATS_H

#include <linux/swap.h>
#include <linux/atomic.h>

// limit swap cache size. Ported from Leap.
extern bool enable_prefetch_buffer;
void activate_prefetch_buffer(int val);
static inline bool prefetch_buffer_enabled(void)
{
	return enable_prefetch_buffer;
}

extern unsigned long buffer_size;
struct pref_buffer {
	atomic_t head;
	atomic_t tail;
	atomic_t size;
	swp_entry_t *offset_list;
	struct page **page_data;
	spinlock_t buffer_lock;
};

extern struct pref_buffer prefetch_buffer;

static inline int get_buffer_head(void)
{
	return atomic_read(&prefetch_buffer.head);
}

static inline int get_buffer_tail(void)
{
	return atomic_read(&prefetch_buffer.tail);
}

static inline int get_buffer_size(void)
{
	return atomic_read(&prefetch_buffer.size);
}

static inline void inc_buffer_head(void)
{
	atomic_set(&prefetch_buffer.head,
		   (atomic_read(&prefetch_buffer.head) + 1) % buffer_size);
	return;
}

static inline void inc_buffer_tail(void)
{
	atomic_set(&prefetch_buffer.tail,
		   (atomic_read(&prefetch_buffer.tail) + 1) % buffer_size);
	return;
}

static inline void inc_buffer_size(void)
{
	atomic_inc(&prefetch_buffer.size);
}

static inline void dec_buffer_size(void)
{
	atomic_dec(&prefetch_buffer.size);
}

static inline int is_buffer_full(void)
{
	return (buffer_size <= atomic_read(&prefetch_buffer.size));
}

void add_page_to_buffer(swp_entry_t entry, struct page *page);
void prefetch_buffer_init(unsigned long _size);

// async prefetch switch
extern bool enable_async_prefetch;

static inline bool async_prefetch_enabled(void)
{
	return enable_async_prefetch;
}

static inline void __set_async_prefetch(int async_bit)
{
	enable_async_prefetch = !!async_bit;
}

// profile swap-in cnts
enum adc_counter_type {
	ADC_ONDEMAND_SWAPIN,
	ADC_PREFETCH_SWAPIN,
	ADC_HIT_ON_PREFETCH,
	NUM_ADC_COUNTER_TYPE
};

extern atomic_t adc_profile_counters[NUM_ADC_COUNTER_TYPE];

static inline void reset_adc_profile_counter(enum adc_counter_type type)
{
	atomic_set(&adc_profile_counters[type], 0);
}

static inline void adc_profile_counter_inc(enum adc_counter_type type)
{
	atomic_inc(&adc_profile_counters[type]);
}

static inline int get_adc_profile_counter(enum adc_counter_type type)
{
	return (int)atomic_read(&adc_profile_counters[type]);
}

// profile page fault latency
enum adc_profile_flag { ADC_PROFILE_SWAP_BIT = 1, ADC_PROFILE_MAJOR_BIT = 2 };
// profile accumulated time stats
struct adc_time_stat {
	atomic64_t accum_val;
	atomic_t cnt;
};

enum adc_time_stat_type {
	ADC_SWAP_MAJOR_DUR,
	ADC_SWAP_MINOR_DUR,
	ADC_NON_SWAP_DUR,
	ADC_RDMA_LATENCY,
	ADC_SWAPOUT_LATENCY,
	NUM_ADC_TIME_STAT_TYPE
};

extern struct adc_time_stat adc_time_stats[NUM_ADC_TIME_STAT_TYPE];

static inline void reset_adc_time_stat(enum adc_time_stat_type type)
{
	struct adc_time_stat *ts = &adc_time_stats[type];
	atomic64_set(&(ts->accum_val), 0);
	atomic_set(&(ts->cnt), 0);
}

static inline void accum_adc_time_stat(enum adc_time_stat_type type, uint64_t val)
{
	struct adc_time_stat *ts = &adc_time_stats[type];
	atomic64_add(val, &(ts->accum_val));
	atomic_inc(&(ts->cnt));
}

static inline unsigned report_adc_time_stat(enum adc_time_stat_type type)
{
	struct adc_time_stat *ts = &adc_time_stats[type];
	if ((unsigned)atomic_read(&(ts->cnt)) == 0) {
		return 0;
	} else {
		return (int64_t)atomic64_read(&(ts->accum_val)) /
		       (int64_t)atomic_read(&(ts->cnt));
	}
}

static inline void reset_adc_swap_stats(void)
{
	int i;
	for (i = ADC_ONDEMAND_SWAPIN; i < NUM_ADC_COUNTER_TYPE; i++) {
		reset_adc_profile_counter(i);
	}

	for (i = ADC_SWAP_MAJOR_DUR; i < NUM_ADC_TIME_STAT_TYPE; i++) {
		reset_adc_time_stat(i);
	}
}

// profile per-process RDMA bandwidth
extern void (*set_swap_bw_control)(int);
extern void (*get_all_procs_swap_pkts)(int *, char *);

// time utils
static inline uint64_t get_time_in_ns(void)
{
	uint64_t rax, rdx;
	__asm__ __volatile__("rdtscp; "
	                     : "=a"(rax), "=d"(rdx)
	                     :
	                     :);

	return (rdx << 32) + rax;
}

static inline uint64_t get_time_in_us(void)
{
	return get_time_in_ns() / 1000;
}

#endif /* _LINUX_SWAP_STATS_H */