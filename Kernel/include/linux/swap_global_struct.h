#ifndef __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H
#define __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/mm_inline.h>
#include <linux/list.h>
#include <asm-generic/bug.h>

#include <linux/swap_global_macro.h>


//
// global variables
//

// defined in swap_slots.c
extern pid_t debug_tgid;  // typedef signed int to pid_t


//
// ## Functions
//
void print_semeru_macro(void);

inline bool is_virt_addr_in_fastpath_range(unsigned long val);
inline bool is_swap_entry_in_fastpath_range(swp_entry_t entry);
inline bool is_swap_offset_in_fastpath_range(unsigned long entry_offset);
inline unsigned long virt_addr_to_swp_offset(unsigned long virt_addr);





#endif // end of __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H