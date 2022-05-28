// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/page_io.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95,
 *  Asynchronous swapping added 30.12.95. Stephen Tweedie
 *  Removed race in async swapping. 14.4.1996. Bruno Haible
 *  Add swap of shared pages through the page cache. 20.2.1998. Stephen Tweedie
 *  Always use brw_page, life becomes simpler. 12 May 1998 Eric Biederman
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/swapops.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/frontswap.h>
#include <linux/blkdev.h>
#include <linux/uio.h>
#include <linux/sched/task.h>
#include <asm/pgtable.h>

/* [ADC] profile swap out latency */
#include <linux/swap_stats.h>
#include <linux/swap_global_struct_mem_layer.h>

#include "internal.h"

// end of semeru

static struct bio *get_swap_bio(gfp_t gfp_flags,
				struct page *page, bio_end_io_t end_io)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, 1);
	if (bio) {
		struct block_device *bdev;

		bio->bi_iter.bi_sector = map_swap_page(page, &bdev);
		bio_set_dev(bio, bdev);
		bio->bi_iter.bi_sector <<= PAGE_SHIFT - 9;
		bio->bi_end_io = end_io;

		bio_add_page(bio, page, PAGE_SIZE * hpage_nr_pages(page), 0);
	}
	return bio;
}

void end_swap_bio_write(struct bio *bio)
{
	struct page *page = bio_first_page_all(bio);

	if (bio->bi_status) {
		SetPageError(page);
		/*
		 * We failed to write the page out to swap-space.
		 * Re-dirty the page in order to avoid it being reclaimed.
		 * Also print a dire warning that things will go BAD (tm)
		 * very quickly.
		 *
		 * Also clear PG_reclaim to avoid rotate_reclaimable_page()
		 */
		set_page_dirty(page);
		pr_alert("Write-error on swap-device (%u:%u:%llu)\n",
			 MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
			 (unsigned long long)bio->bi_iter.bi_sector);
		ClearPageReclaim(page);
	}
	end_page_writeback(page);
	bio_put(bio);
}

static void swap_slot_free_notify(struct page *page)
{
	struct swap_info_struct *sis;
	struct gendisk *disk;
	swp_entry_t entry;

	/*
	 * There is no guarantee that the page is in swap cache - the software
	 * suspend code (at least) uses end_swap_bio_read() against a non-
	 * swapcache page.  So we must check PG_swapcache before proceeding with
	 * this optimization.
	 */
	if (unlikely(!PageSwapCache(page)))
		return;

	sis = page_swap_info(page);
	if (!(sis->flags & SWP_BLKDEV))
		return;

	/*
	 * The swap subsystem performs lazy swap slot freeing,
	 * expecting that the page will be swapped out again.
	 * So we can avoid an unnecessary write if the page
	 * isn't redirtied.
	 * This is good for real swap storage because we can
	 * reduce unnecessary I/O and enhance wear-leveling
	 * if an SSD is used as the as swap device.
	 * But if in-memory swap device (eg zram) is used,
	 * this causes a duplicated copy between uncompressed
	 * data in VM-owned memory and compressed data in
	 * zram-owned memory.  So let's free zram-owned memory
	 * and make the VM-owned decompressed page *dirty*,
	 * so the page should be swapped out somewhere again if
	 * we again wish to reclaim it.
	 */
	disk = sis->bdev->bd_disk;
	entry.val = page_private(page);
	if (disk->fops->swap_slot_free_notify && __swap_count(entry) == 1) {
		unsigned long offset;

		offset = swp_offset(entry);

		SetPageDirty(page);
		disk->fops->swap_slot_free_notify(sis->bdev,
				offset);
	}
}

static void end_swap_bio_read(struct bio *bio)
{
	struct page *page = bio_first_page_all(bio);
	struct task_struct *waiter = bio->bi_private;

	if (bio->bi_status) {
		SetPageError(page);
		ClearPageUptodate(page);
		pr_alert("Read-error on swap-device (%u:%u:%llu)\n",
			 MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
			 (unsigned long long)bio->bi_iter.bi_sector);
		goto out;
	}

	SetPageUptodate(page);
	swap_slot_free_notify(page);
out:
	unlock_page(page);
	WRITE_ONCE(bio->bi_private, NULL);
	bio_put(bio);
	if (waiter) {
		blk_wake_io_task(waiter);
		put_task_struct(waiter);
	}
}

int generic_swapfile_activate(struct swap_info_struct *sis,
				struct file *swap_file,
				sector_t *span)
{
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned blocks_per_page;
	unsigned long page_no;
	unsigned blkbits;
	sector_t probe_block;
	sector_t last_block;
	sector_t lowest_block = -1;
	sector_t highest_block = 0;
	int nr_extents = 0;
	int ret;

	blkbits = inode->i_blkbits;
	blocks_per_page = PAGE_SIZE >> blkbits;

	/*
	 * Map all the blocks into the extent tree.  This code doesn't try
	 * to be very smart.
	 */
	probe_block = 0;
	page_no = 0;
	last_block = i_size_read(inode) >> blkbits;
	while ((probe_block + blocks_per_page) <= last_block &&
			page_no < sis->max) {
		unsigned block_in_page;
		sector_t first_block;

		cond_resched();

		first_block = bmap(inode, probe_block);
		if (first_block == 0)
			goto bad_bmap;

		/*
		 * It must be PAGE_SIZE aligned on-disk
		 */
		if (first_block & (blocks_per_page - 1)) {
			probe_block++;
			goto reprobe;
		}

		for (block_in_page = 1; block_in_page < blocks_per_page;
					block_in_page++) {
			sector_t block;

			block = bmap(inode, probe_block + block_in_page);
			if (block == 0)
				goto bad_bmap;
			if (block != first_block + block_in_page) {
				/* Discontiguity */
				probe_block++;
				goto reprobe;
			}
		}

		first_block >>= (PAGE_SHIFT - blkbits);
		if (page_no) {	/* exclude the header page */
			if (first_block < lowest_block)
				lowest_block = first_block;
			if (first_block > highest_block)
				highest_block = first_block;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, page_no, 1, first_block);
		if (ret < 0)
			goto out;
		nr_extents += ret;
		page_no++;
		probe_block += blocks_per_page;
reprobe:
		continue;
	}
	ret = nr_extents;
	*span = 1 + highest_block - lowest_block;
	if (page_no == 0)
		page_no = 1;	/* force Empty message */
	sis->max = page_no;
	sis->pages = page_no - 1;
	sis->highest_bit = page_no - 1;
out:
	return ret;
bad_bmap:
	pr_err("swapon: swapfile has holes\n");
	ret = -EINVAL;
	goto out;
}

/*
 * We may have stale swap cache pages in memory: notice
 * them here and get rid of the unnecessary final write.
 */
int swap_writepage(struct page *page, struct writeback_control *wbc)
{
	int ret = 0;

	// [ADC] for profile
	uint64_t pf_ts_stt, pf_ts_end;

	if (try_to_free_swap(page)) {
		unlock_page(page);
		goto out;
	}

	pf_ts_stt = get_time_in_us();
	if (frontswap_store(page) == 0) {
		set_page_writeback(page);
		unlock_page(page);
		end_page_writeback(page);

		pf_ts_end = get_time_in_us();
		accum_adc_time_stat(ADC_SWAPOUT_LATENCY, pf_ts_end - pf_ts_stt);
		goto out;
	}
	ret = __swap_writepage(page, wbc, end_swap_bio_write);
out:
	return ret;
}

static sector_t swap_page_sector(struct page *page)
{
	return (sector_t)__page_file_index(page) << (PAGE_SHIFT - 9);
}

static inline void count_swpout_vm_event(struct page *page)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (unlikely(PageTransHuge(page)))
		count_vm_event(THP_SWPOUT);
#endif
	count_vm_events(PSWPOUT, hpage_nr_pages(page));
}

int __swap_writepage(struct page *page, struct writeback_control *wbc,
		bio_end_io_t end_write_func)
{
	struct bio *bio;
	int ret;
	struct swap_info_struct *sis = page_swap_info(page);

	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	if (sis->flags & SWP_FS) {
		struct kiocb kiocb;
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;
		struct bio_vec bv = {
			.bv_page = page,
			.bv_len  = PAGE_SIZE,
			.bv_offset = 0
		};
		struct iov_iter from;

		iov_iter_bvec(&from, WRITE, &bv, 1, PAGE_SIZE);
		init_sync_kiocb(&kiocb, swap_file);
		kiocb.ki_pos = page_file_offset(page);

		set_page_writeback(page);
		unlock_page(page);
		ret = mapping->a_ops->direct_IO(&kiocb, &from);
		if (ret == PAGE_SIZE) {
			count_vm_event(PSWPOUT);
			ret = 0;
		} else {
			/*
			 * In the case of swap-over-nfs, this can be a
			 * temporary failure if the system has limited
			 * memory for allocating transmit buffers.
			 * Mark the page dirty and avoid
			 * rotate_reclaimable_page but rate-limit the
			 * messages but do not flag PageError like
			 * the normal direct-to-bio case as it could
			 * be temporary.
			 */
			set_page_dirty(page);
			ClearPageReclaim(page);
			pr_err_ratelimited("Write error on dio swapfile (%llu)\n",
					   page_file_offset(page));
		}
		end_page_writeback(page);
		return ret;
	}

	ret = bdev_write_page(sis->bdev, swap_page_sector(page), page, wbc);
	if (!ret) {
		count_swpout_vm_event(page);
		return 0;
	}

	ret = 0;
	bio = get_swap_bio(GFP_NOIO, page, end_write_func);
	if (bio == NULL) {
		set_page_dirty(page);
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	bio->bi_opf = REQ_OP_WRITE | REQ_SWAP | wbc_to_write_flags(wbc);
	bio_associate_blkg_from_page(bio, page);
	count_swpout_vm_event(page);
	set_page_writeback(page);
	unlock_page(page);
	submit_bio(bio);
out:
	return ret;
}

int swap_readpage(struct page *page, bool synchronous)
{
	struct bio *bio;
	int ret = 0;
	struct swap_info_struct *sis = page_swap_info(page);
	blk_qc_t qc;
	struct gendisk *disk;

	VM_BUG_ON_PAGE(!PageSwapCache(page) && !synchronous, page);
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageUptodate(page), page);
	if (frontswap_load(page) == 0) {
		/* [ADC] bits are set when RDMA truly gets the page */
		// SetPageUptodate(page);
		// unlock_page(page);
		goto out;
	}

	if (sis->flags & SWP_FS) {
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;

		ret = mapping->a_ops->readpage(swap_file, page);
		if (!ret)
			count_vm_event(PSWPIN);
		return ret;
	}

	ret = bdev_read_page(sis->bdev, swap_page_sector(page), page);
	if (!ret) {
		if (trylock_page(page)) {
			swap_slot_free_notify(page);
			unlock_page(page);
		}

		count_vm_event(PSWPIN);
		return 0;
	}

	ret = 0;
	bio = get_swap_bio(GFP_KERNEL, page, end_swap_bio_read);
	if (bio == NULL) {
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	disk = bio->bi_disk;
	/*
	 * Keep this task valid during swap readpage because the oom killer may
	 * attempt to access it in the page fault retry time check.
	 */
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	if (synchronous) {
		bio->bi_opf |= REQ_HIPRI;
		get_task_struct(current);
		bio->bi_private = current;
	}
	count_vm_event(PSWPIN);
	bio_get(bio);
	qc = submit_bio(bio);
	while (synchronous) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (!READ_ONCE(bio->bi_private))
			break;

		if (!blk_poll(disk->queue, qc, true))
			io_schedule();
	}
	__set_current_state(TASK_RUNNING);
	bio_put(bio);

out:
	return ret;
}

/* [ADC] async swap-in*/
int swap_readpage_async(struct page *page)
{
	bool synchronous = false; // YIFAN: not sure if `true` is necessary.
	struct bio *bio;
	int ret = 0;
	struct swap_info_struct *sis = page_swap_info(page);
	blk_qc_t qc;
	struct gendisk *disk;

	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageUptodate(page), page);
	if (frontswap_load_async(page) == 0) {
		/* [ADC] bits are set when RDMA truly gets the page */
		// SetPageUptodate(page);
		// unlock_page(page);
		goto out;
	}

	if (sis->flags & SWP_FS) {
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;

		ret = mapping->a_ops->readpage(swap_file, page);
		if (!ret)
			count_vm_event(PSWPIN);
		return ret;
	}

	ret = bdev_read_page(sis->bdev, swap_page_sector(page), page);
	if (!ret) {
		if (trylock_page(page)) {
			swap_slot_free_notify(page);
			unlock_page(page);
		}

		count_vm_event(PSWPIN);
		return 0;
	}

	ret = 0;
	bio = get_swap_bio(GFP_KERNEL, page, end_swap_bio_read);
	if (bio == NULL) {
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	disk = bio->bi_disk;
	/*
	 * Keep this task valid during swap readpage because the oom killer may
	 * attempt to access it in the page fault retry time check.
	 */
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	if (synchronous) {
		bio->bi_opf |= REQ_HIPRI;
		get_task_struct(current);
		bio->bi_private = current;
	}
	count_vm_event(PSWPIN);
	bio_get(bio);
	qc = submit_bio(bio);
	while (synchronous) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (!READ_ONCE(bio->bi_private))
			break;

		if (!blk_poll(disk->queue, qc, true))
			io_schedule();
	}
	__set_current_state(TASK_RUNNING);
	bio_put(bio);

out:
	return ret;
}

int swap_set_page_dirty(struct page *page)
{
	struct swap_info_struct *sis = page_swap_info(page);

	if (sis->flags & SWP_FS) {
		struct address_space *mapping = sis->swap_file->f_mapping;

		VM_BUG_ON_PAGE(!PageSwapCache(page), page);
		return mapping->a_ops->set_page_dirty(page);
	} else {
		return __set_page_dirty_no_writeback(page);
	}
}

//
// Semeru Data Plane OPT
//

//
// Check physical memory flags
//

/**
 * print all the necesaary flags,
 * we can divide them into several groups according the needs.
 *
 *
 * Warning:
 *
 * [?]
 * page_private(page)  is to check the page->private field,
 * but page_has_private(page) is check a flag,
 * seems these 2 are checking different things ?
 *
 */
void print_page_flags(struct page *page, enum check_mode mode,
		      const char *message)
{
	switch (mode) {
	case CHECK_SWAP_SYSTEM:
		pr_warn("%s, page 0x%lx, swapcache %d, page.private(swap_entry) 0x%lx,  anony %d, PageLRU %d, PageDirty %d, PageWriteback %d . page_mapcount(+1) %d, page->_refcount(the value) %d, page_has_private(page) %d \n",
			message, (unsigned long)page, PageSwapCache(page),
			page_private(page), PageAnon(page), PageLRU(page),
			PageDirty(page), PageWriteback(page),
			page_mapcount(page), page_count(page),
			page_has_private(page));

		break;

	default:
		// Print the message at the head.
		pr_warn("%s, page 0x%lx, swapcache %d, anony %d, PageLRU %d, PageDirty %d, PageWriteback %d . page_mapped(>=0?) %d, page->_refcount(the value) %d \n",
			message, (unsigned long)page, PageSwapCache(page),
			PageAnon(page), PageLRU(page), PageDirty(page),
			PageWriteback(page), page_mapped(page),
			page_count(page));
	}
}

/**
 * Print the virtual address of a physical page
 *
 */

void print_virt_addr_of_page(struct page *page, const char *message)
{
	struct anon_vma *anon_vma;
	pgoff_t pgoff_start, pgoff_end;
	struct anon_vma_chain *avc;

	if (page_mapped(page)) {
		// assume this page is locked here.
		anon_vma = page_anon_vma(page);
		/* anon_vma disappear under us? */
		VM_BUG_ON_PAGE(!anon_vma, page);

		pgoff_start = page_to_pgoff(page);
		pgoff_end = pgoff_start + hpage_nr_pages(page) - 1;
		anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root,
					       pgoff_start, pgoff_end)
		{
			struct vm_area_struct *vma = avc->vma;
			unsigned long address = vma_address(page, vma);

			pr_warn("%s, page 0x%lx, virt_addr 0x%lx \n", message,
				(unsigned long)page, address);

		} // end of traverse vma

	} else {
		pr_warn("%s, page 0x%lx is not mapped into any user process. page->_mapcount(+1) %d \n",
			message, (unsigned long)page, page_mapcount(page));
	}
}

/**
 * ONLY walk the page table of user process. Return the pte copied pte value or 0 for empty page table.
 *
 * Used for linux 5.4, 4 level page tables.
 *
 * Parameter
 * 	virtual address of a user process
 * 	struct mm_struct of the user process, containing the virtual address.
 *
 * return
 * 	0 , failed. some level of the pte is empty.
 * 	a valid swp_entry
 *
 *
 * Questions
 * 	From pgd to pmd, can only be none or present, can't be NULL, why ?
 *
 * 	pte_t are different.
 * 	NULL : not touched/allocated yet.
 * 	pte_none, node flag is set. not touched.
 * 	present, page is in memory.
 *
 *
 */
swp_entry_t walk_page_table_for_swap_entry(struct mm_struct *mm,
					   unsigned long user_virt_addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	swp_entry_t swap_entry = { .val = 0 };

	pgd = pgd_offset(mm, user_virt_addr);
	if (!pgd_present(*pgd))
		goto out;
	p4d = p4d_offset(pgd, user_virt_addr);
	if (!p4d_present(*p4d))
		goto out;
	pud = pud_offset(p4d, user_virt_addr);
	if (!pud_present(*pud))
		goto out;
	pmd = pmd_offset(pud, user_virt_addr);
	if (!pmd_present(*pmd))
		goto out;

	// Map the user space pte into kernel space varible, ptep.
	// we should unmap the ptep somewhere ?
	ptep = pte_offset_map(pmd, user_virt_addr);
	if (!ptep) // pte_t* can be NULL
		goto out;

	pte = *ptep;
	if (pte_none(pte)) // allocated, but note touched.
		goto out;
	if (pte_present(pte)) // Its page is not swapped out
		goto out;

	// can be valid swp_entry_r or not, judged by non_swap_entry
	swap_entry = pte_to_swp_entry(pte);

	pte_unmap(ptep); // paired with pte_offset_map

out:
	return swap_entry;
}
