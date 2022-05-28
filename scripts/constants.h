#ifndef __RSWAP_CONSTANTS_H
#define __RSWAP_CONSTANTS_H

#ifndef ONE_MB
#define ONE_MB ((size_t)1024 * 1024)
#endif

#ifndef ONE_GB
#define ONE_GB ((size_t)1024 * 1024 * 1024)
#endif

// RDMA manage granularity, not the Heap Region.

#ifdef REGION_SIZE_GB
#undef REGION_SIZE_GB
#endif
#define REGION_SIZE_GB ((size_t)4)


#ifdef RDMA_DATA_REGION_NUM
#undef RDMA_DATA_REGION_NUM
#endif
#define RDMA_DATA_REGION_NUM 12


#ifdef MAX_FREE_MEM_GB
#undef MAX_FREE_MEM_GB
#endif
#define MAX_FREE_MEM_GB ((size_t)48) // Up to 48GB swap space


#ifdef MAX_REGION_NUM
#undef MAX_REGION_NUM
#endif
#define MAX_REGION_NUM \
	((size_t)MAX_FREE_MEM_GB / REGION_SIZE_GB) // for msg passing?

// number of segments, get from ibv_query_device. Use 30, or it's not safe..
#define MAX_REQUEST_SGL 32

#endif // __RSWAP_CONSTANTS_H
