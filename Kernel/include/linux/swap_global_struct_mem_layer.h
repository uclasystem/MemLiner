
#ifndef __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H
#define __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H

#include <linux/swap_global_struct.h>



//
// Functions
//


enum check_mode{
  CHECK_FLUSH_MOD   = 1,
  CHECK_SWAP_SYSTEM = 2,
  DEFAULT,
};

void print_page_flags(struct page *page, enum check_mode print_mode, const char* message);
void print_virt_addr_of_page(struct page *page, const char* message);
void print_swap_info_struct(swp_entry_t entry, const char* message);

swp_entry_t walk_page_table_for_swap_entry(struct mm_struct * mm, unsigned long user_virt_addr );  // ONLY walk the page table of user process. Return the pte copied pte value or 0 for empty page table.


#endif // __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H