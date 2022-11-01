/**
 * Only put defined macros here.
 *
 */
#ifndef __LINUX_SWAP_SWAP_GLOBAL_MACRO_H
#define __LINUX_SWAP_SWAP_GLOBAL_MACRO_H




//
// Control options
//

// Used to debug the shared path
//#define DEBUG_SEMERU_DATA_PLANE_DETAIL 1
// #define DEBUG_SEMERU_DATA_PLANE_BRIEF	1


//#define DEBUG_SWAP_FAST_DETAIL 1
#define DEBUG_SWAP_FAST_BRIEF	1

//#define DEBUG_SWAP_SLOW_DETAIL 1
//#define DEBUG_SWAP_SLOW_BRIEF	1




//
//  PC && Server shared macro
//

//
// ## Basic Macros ##
//
#ifndef ONE_MB
	#define ONE_MB    1048576UL		// 1024 x 2014 bytes
#endif

#ifndef ONE_GB
	#define ONE_GB    1073741824UL   	// 1024 x 1024 x 1024 bytes
#endif

#ifndef PAGE_SIZE
	#define PAGE_SIZE		((unsigned long)4096)	// bytes, use the define of kernel.
#endif


#ifndef PAGE_SHIFT
	#define PAGE_SHIFT 12
#endif



// Version#1
// Macro for pc development


/*
#define SEMERU_START_ADDR   ((unsigned long)0x400000000000)	// Assume both CPU server and Memory server start at this address
#define REGION_SIZE_GB    	4UL   	// RDMA manage granularity, not the Heap Region.
#define MAX_REGION_NUM     	3UL	// 12GB per process

#define SLOW_PATH_REGION	1UL  // count at region granularity
#define FAST_PATH_REGION	(MAX_REGION_NUM-SLOW_PATH_REGION)
*/

// Version#2
// Macro for server development

 #define SEMERU_START_ADDR   	((unsigned long)0x400000000000)	// Assume both CPU server and Memory server start at this address
 #define REGION_SIZE_GB    	4UL  	// RDMA manage granularity, not the Heap Region.
 #define MAX_REGION_NUM     12UL	// 48GB per process
 #define SEMERU_END_ADDR	SEMERU_START_ADDR+ MAX_REGION_NUM*REGION_SIZE_GB*ONE_GB
 #define SLOW_PATH_REGION	4UL  // count at region granularity
 #define FAST_PATH_REGION	(MAX_REGION_NUM-SLOW_PATH_REGION)

 // the start virtual address of fastpath range
 // FAST_PATH_REGION regions are in fastpath






//
// ## Swap-Part ##
//
// Each application's swap partition is divided into 2 parts
// 1) the swap part for slowpath.
// 2) the swap part for fastpath. Used for un-shared anonymous pages
//
//	|--- slowpath part ---| --- fastpathpart --- |
//

#define RDMA_SLOWPATH_SWAP_PART_OSSFET 		0UL
#define RDMA_SLOWPATH_SWAP_PART_SIZE		SLOW_PATH_REGION*REGION_SIZE_GB*ONE_GB		// 16 GB
#define RDMA_SLOWPATH_SWAP_SLOTS_LIMIT		((RDMA_SLOWPATH_SWAP_PART_OSSFET + RDMA_SLOWPATH_SWAP_PART_SIZE) >> PAGE_SHIFT)


#define RDMA_FASTPATH_SWAP_PART_OSSFET		RDMA_SLOWPATH_SWAP_PART_OSSFET + RDMA_SLOWPATH_SWAP_PART_SIZE
// size , until the end of the available size
//#define RDMA_FASTPATH_SWAP_PART_SIZE		MAX_REGION_NUM * REGION_SIZE_GB*ONE_GB - RDMA_SLOWPATH_SWAP_PART_SIZE // 32GB, mapping to the java heap strictly
#define RDMA_FASTPATH_SWAP_PART_SIZE		(MAX_REGION_NUM - SLOW_PATH_REGION) * REGION_SIZE_GB * ONE_GB // 32GB, mapping to the java heap strictly
#define RDMA_FASTAPATH_SWAP_SLOTS_LIMIT		((RDMA_FASTPATH_SWAP_PART_OSSFET + RDMA_FASTPATH_SWAP_PART_SIZE) >> PAGE_SHIFT)

#define FALIED_SWP_ENTRY ((unsigned long)-1)









//
// Control the prefetch
//


#define UFFD_SWAP_PREFETCH_NUM	8UL  // the max page number can be prefetched




//
// The epoch related macros
//

// 64GB memory
#define COVERED_MEM_LENGTH 16*ONE_MB


#endif // end of __LINUX_SWAP_SWAP_GLOBAL_MACRO_H
