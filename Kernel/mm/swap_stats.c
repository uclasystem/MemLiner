#include <linux/swap_stats.h>
#include <linux/swap.h>
#include <linux/swapops.h>

bool enable_async_prefetch = true;

atomic_t adc_profile_counters[NUM_ADC_COUNTER_TYPE];

struct adc_time_stat adc_time_stats[NUM_ADC_TIME_STAT_TYPE];

// limit swap cache size
bool enable_prefetch_buffer = false;

void activate_prefetch_buffer(int val)
{
	enable_prefetch_buffer = !!val;
	printk("prefetch buffer: %s\n",
	       enable_prefetch_buffer ? "active" : "inactive");
}
EXPORT_SYMBOL(activate_prefetch_buffer);

unsigned long buffer_size = 8000;
struct pref_buffer prefetch_buffer;

void add_page_to_buffer(swp_entry_t entry, struct page *page)
{
	int tail, head, find = 0;
	swp_entry_t head_entry;
	struct page *head_page;

	spin_lock_irq(&prefetch_buffer.buffer_lock);
	inc_buffer_tail();
	tail = get_buffer_tail();

	while (is_buffer_full() && find == 0) {
		// printk("%s: buffer is full for entry: %ld, head at: %d, tail at: %d\n",
		//        __func__, entry.val, get_buffer_head(),
		//        get_buffer_tail());
		head = get_buffer_head();
		head_entry = prefetch_buffer.offset_list[head];
		head_page = prefetch_buffer.page_data[head];

		if (!non_swap_entry(head_entry) && head_page) {
			// printk("%s: going to remove entry %ld with mapcount %d\n",
			//        __func__, head_entry.val,
			//        page_mapcount(head_page));

			// page has already been moved out
			if (!PageSwapCache(head_page)) {
				find = 1;
			// in swap cache but not mapped yet
			} else if (!page_mapped(head_page)) {
				if (trylock_page(head_page)) {
					test_clear_page_writeback(head_page);
					head_page = compound_head(head_page);
					delete_from_swap_cache(head_page);
					SetPageDirty(head_page);
					unlock_page(head_page);
					find = 1;
					// printk("%s: after freeing entry %ld with mapcount %d\n",
					//        __func__, head_entry.val,
					//        page_mapcount(head_page));
				}
			} else if (page_mapcount(head_page) == 1) {
				if (trylock_page(head_page)) {
					// swap_free(head_entry);
					find = try_to_free_swap(head_page);
					unlock_page(head_page);
					// printk("%s: after freeing entry %ld with mapcount %d\n",
					//        __func__, head_entry.val,
					//        page_mapcount(head_page));
				}
			} else {
				// printk("%s: failed to delete entry %ld with mapcount %d\n",
				//        __func__, head_entry.val,
				//        page_mapcount(head_page));
				inc_buffer_tail();
				tail = get_buffer_tail();
			}
		} else {
			find = 1;
		}
		if (find) {
			dec_buffer_size();
		};
		// printk("%s: try_to_free_swap is %s\n", __func__,
		//        (find != 0) ? "successful" : "failed");
		inc_buffer_head();
	}
	prefetch_buffer.offset_list[tail] = entry;
	prefetch_buffer.page_data[tail] = page;
	inc_buffer_size();
	spin_unlock_irq(&prefetch_buffer.buffer_lock);
}

/* static void delete_page_from_buffer(swp_entry_t entry) { return; }
*/

void prefetch_buffer_init(unsigned long _size)
{
	printk("%s: initiating prefetch buffer with size %ld!\n", __func__,
	       _size);
	if (!_size || _size <= 0) {
		printk("%s: invalid buffer size\n", __func__);
		return;
	}

	buffer_size = _size;
	prefetch_buffer.offset_list = (swp_entry_t *)kzalloc(
		buffer_size * sizeof(swp_entry_t), GFP_KERNEL);
	prefetch_buffer.page_data = (struct page **)kzalloc(
		buffer_size * sizeof(struct page *), GFP_KERNEL);
	atomic_set(&prefetch_buffer.head, 0);
	atomic_set(&prefetch_buffer.tail, 0);
	atomic_set(&prefetch_buffer.size, 0);

	printk("%s: prefetch buffer initiated with size: %d, head at: %d, tail at: %d\n",
	       __func__, get_buffer_size(), get_buffer_head(),
	       get_buffer_tail());
	return;
}
EXPORT_SYMBOL(prefetch_buffer_init);

// for swap RDMA bandwidth control
void (*set_swap_bw_control)(int) = NULL;
EXPORT_SYMBOL(set_swap_bw_control);
void (*get_all_procs_swap_pkts)(int *, char *) = NULL;
EXPORT_SYMBOL(get_all_procs_swap_pkts);