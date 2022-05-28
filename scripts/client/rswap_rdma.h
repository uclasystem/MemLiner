/**
 *  1）Define operations for frontswap
 *  2) Define the RDMA operations for frontswap path
 *
 */

#ifndef __RSWAP_RDMA_H
#define __RSWAP_RDMA_H

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>

// Swap
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/swapfile.h>
#include <linux/swap.h>
#include <linux/frontswap.h>

// For infiniband
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/pci.h> // Use the dma_addr_t defined in types.h as the DMA/BUS address.
#include <linux/inet.h>
#include <linux/lightnvm.h>
#include <linux/sed-opal.h>

// Utilities
#include <linux/log2.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/scatterlist.h>
#include <asm/uaccess.h> // copy data from kernel space to user space
#include <linux/slab.h>	 // kmem_cache
#include <linux/debugfs.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/page-flags.h>
#include <linux/smp.h>

#include "constants.h"
#include "utils.h"

// #define ENABLE_VQUEUE

// for the qp. Find the max number without warning.
#define RDMA_SEND_QUEUE_DEPTH 4096
#define RDMA_RECV_QUEUE_DEPTH 32

#define GB_SHIFT 30
// Used to calculate the chunk index in Client
// (File chunk). Initialize it before using.
#define CHUNK_SHIFT (u64)(GB_SHIFT + ilog2(REGION_SIZE_GB))
// get the address within a chunk
#define CHUNK_MASK (u64)(((u64)1 << CHUNK_SHIFT) - 1)

//
// ####################### variable declaration #######################
//

/**
 * Used for message passing control
 * For both CM event, data evetn.
 * RDMA data transfer is desinged in an asynchronous style.
 */
enum rdma_queue_state
{
	IDLE = 1, // 1, Start from 1.
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED, // 5,  updated by IS_cma_event_handler()

	MEMORY_SERVER_AVAILABLE, // 6

	FREE_MEM_RECV,	 // After query, we know the available regions on memory server.
	RECEIVED_CHUNKS, // get chunks from remote memory server
	RDMA_BUF_ADV,	 // designed for server
	WAIT_OPS,
	RECV_STOP, // 11

	RECV_EVICT,
	RDMA_WRITE_RUNNING,
	RDMA_READ_RUNNING,
	SEND_DONE,
	RDMA_DONE, // 16

	RDMA_READ_ADV, // updated by IS_cq_event_handler()
	RDMA_WRITE_ADV,
	CM_DISCONNECT,
	ERROR,
	TEST_DONE, // 21, for debug
};

/**
 *  CS - Build the RDMA connection to MS
 *
 * Two-sided RDMA message structure.
 * We use 2-sieded RDMA communication to exchange information between Client and Server.
 * Both Client and Server have the same message structure.
 */

/**
 * Need to update to Region status.
 */
enum chunk_mapping_state
{
	EMPTY,	// 0
	MAPPED, // 1, Cached ?
};

// Meta Region/chunk: a contiguous chunk of a Region, may be not fully mapped.
// Data Region/chunk: Default mapping size is REGION_SIZE_GB, 4 GB default.
// RDMA mapping size isn't the Java Region size.
//
struct remote_mapping_chunk
{
	uint32_t remote_rkey; // RKEY of the remote mapped chunk
	uint64_t remote_addr; // Virtual address of remote mapped chunk
	uint64_t mapped_size; // For some specific Chunk, we may only map a contigunous range.
	enum chunk_mapping_state chunk_state;
};

/**
 * Use the chunk as contiguous File chunks.
 *
 * For example,
 * 	File address 0x0 to 0x3fff,ffff  is mapped to the first chunk, remote_mapping_chunk_list->remote_mapping_chunk[0].
 * 	When send 1 sided RDMA read/write, the remote address shoudl be remote_mapping_chunk_list->remote_mapping_chunk[0].remote_addr + [0 to 0x3fff,ffff]
 *
 * For each Semery Memory Server, its mapping size has 2 parts:
 * 		1) Meta Data Region. Maybe not fully mapped.
 * 		2) Data Region. Mpped at REGION_SIZE_GB granularity.
 *
 */
struct remote_mapping_chunk_list
{
	struct remote_mapping_chunk *remote_chunk;
	uint32_t remote_free_size; // total mapped byte size. Accumulated each remote_mapping_chunk[i]->mapped_size
	uint32_t chunk_num;	   // length of remote_chunk list
	uint32_t chunk_ptr;	   // points to first empty chunk.
};

/**
 * 1-sided RDMA WR for frontswap path.
 * only support sinlge page for now.
 *
 */
struct fs_rdma_req
{
	struct ib_cqe cqe;	   // CQE complete function
	struct page *page;	   // [?] Make it support multiple pages
	u64 dma_addr;		   // dma address for the page
	struct ib_sge sge;	   // points to local data
	struct ib_rdma_wr rdma_wr; // wr for 1-sided RDMA write/send.

	struct completion done;		     // spinlock. caller wait on it.
	struct rswap_rdma_queue *rdma_queue; // which rdma_queue is enqueued.
};

struct two_sided_rdma_send
{
	struct ib_cqe cqe;	  // CQE complete function
	struct ib_send_wr sq_wr;  // send queue wr
	struct ib_sge send_sgl;	  // attach the rdma buffer to sg entry. scatter-gather entry number is limitted by IB hardware.
	struct message *send_buf; // the rdma buffer.
	u64 send_dma_addr;

	struct rswap_rdma_queue *rdma_queue; // which rdma_queue is enqueued.
};

struct two_sided_rdma_recv
{
	struct ib_cqe cqe;	 // CQE complete function
	struct ib_recv_wr rq_wr; // receive queue wr
	struct ib_sge recv_sgl;	 // recv single SGE entry
	struct message *recv_buf;
	u64 recv_dma_addr;

	struct rswap_rdma_queue *rdma_queue; // which rdma_queue is enqueued.
};

/**
 * Build a QP for each core on cpu server.
 * [?] This design assume we only have one memory server.
 * 			Extern this design later.
 *
 */
struct rswap_rdma_queue
{
	// RDMA client ID, one for per QP.
	struct rdma_cm_id *cm_id; //  ? bind to QP

	// ib events
	struct ib_cq *cq; // Completion queue
	struct ib_qp *qp; // Queue Pair

	enum rdma_queue_state state; // the current status of the QP.
	wait_queue_head_t sem;	     // semaphore for wait/wakeup
	spinlock_t cq_lock;	     // used for CQ
	uint8_t freed;		     // some function can only be called once, this is the flag to record this.
	atomic_t rdma_post_counter;

	int q_index; // initialized to disk hardware queue index
	enum rdma_queue_type type;
	struct rdma_session_context *rdma_session; // Record the RDMA session this queue belongs to.

	// cache for fs_rdma_request. One for each rdma_queue
	struct kmem_cache *fs_rdma_req_cache; // only for fs_rdma_req ?
	struct kmem_cache *rdma_req_sg_cache; // used for rdma request with scatter/gather
};

/**
 * The rdma device.
 * It's shared by multiple QP and CQ.
 * One RDMA device per server.
 *
 */
struct rswap_rdma_dev
{
	struct ib_device *dev;
	struct ib_pd *pd;
};

/**
 * Mange the RDMA connection to a remote server.
 * Every Remote Memory Server has a dedicated rdma_session_context as controller.
 *
 * More explanation
 * [?] One session per server. Extend here later.
 *
 */
struct rdma_session_context
{
	// 1) RDMA QP/CQ management
	struct rswap_rdma_dev *rdma_dev;      // The RDMA device of cpu server.
	struct rswap_rdma_queue *rdma_queues; // point to multiple QP

	// For infiniband connection rdma_cm operation
	uint16_t port;	      /* dst port in NBO */
	u8 addr[16];	      /* dst addr in NBO */
	uint8_t addr_type;    /* ADDR_FAMILY - IPv4/V6 */
	int send_queue_depth; // Send queue depth. Both 1-sided/2-sided RDMA wr is limited by this number.
	int recv_queue_depth; // Receive Queue depth. 2-sided RDMA need to post a recv wr.

	//
	// 2) 2-sided RDMA section.
	//		This section is used for RDMA connection and basic information exchange with remote memory server.
	struct two_sided_rdma_recv rdma_recv_req;
	struct two_sided_rdma_send rdma_send_req;

	// 3) manage the CHUNK mapping.
	struct remote_mapping_chunk_list remote_chunk_list;
};

//
// ###################### function declaration #######################
//

enum rdma_queue_type get_qp_type(int idx);
struct rswap_rdma_queue *get_rdma_queue(struct rdma_session_context *rdma_session, unsigned int cpu, enum rdma_queue_type type);

// functions for RDMA connection
int rswap_rdma_client_init(void);
void rswap_rdma_client_exit(void);
int rdma_session_connect(struct rdma_session_context *rdma_session);
int rswap_init_rdma_queue(struct rdma_session_context *rdma_session, int cpu);
int rswap_create_rdma_queue(struct rdma_session_context *rdma_session, int rdma_queue_index);
int rswap_connect_remote_memory_server(struct rdma_session_context *rdma_session, int rdma_queue_inx);
int rswap_query_available_memory(struct rdma_session_context *rdma_session);
int setup_rdma_session_comm_buffer(struct rdma_session_context *rdma_session);
int rswap_setup_buffers(struct rdma_session_context *rdma_session);

int init_remote_chunk_list(struct rdma_session_context *rdma_session);
void bind_remote_memory_chunks(struct rdma_session_context *rdma_session);

int rswap_disconnect_and_collect_resource(struct rdma_session_context *rdma_session);
void rswap_free_buffers(struct rdma_session_context *rdma_session);
void rswap_free_rdma_structure(struct rdma_session_context *rdma_session);

// functions for 2-sided RDMA.
void two_sided_message_done(struct ib_cq *cq, struct ib_wc *wc);
int handle_recv_wr(struct rswap_rdma_queue *rdma_queue, struct ib_wc *wc);
int send_message_to_remote(struct rdma_session_context *rdma_session, int rdma_queue_ind, int messge_type, int chunk_num);

// functions for frontswap
int rswap_register_frontswap(void);
void rswap_deregister_frontswap(void);
int rswap_frontswap_store(unsigned type, pgoff_t page_offset, struct page *page);
int rswap_frontswap_load(unsigned type, pgoff_t page_offset, struct page *page);
int rswap_frontswap_load_async(unsigned type, pgoff_t page_offset, struct page *page);
int rswap_frontswap_poll_load(int cpu);

int fs_rdma_send(struct rdma_session_context *rdma_session, struct rswap_rdma_queue *rdma_queue, struct fs_rdma_req *rdma_req,
		 struct remote_mapping_chunk *remote_chunk_ptr, size_t offset_within_chunk, struct page *page, enum dma_data_direction dir);

int fs_build_rdma_wr(struct rdma_session_context *rdma_session, struct rswap_rdma_queue *rdma_queue, struct fs_rdma_req *rdma_req,
		     struct remote_mapping_chunk *remote_chunk_ptr, size_t offset_within_chunk, struct page *page, enum dma_data_direction dir);

int fs_enqueue_send_wr(struct rdma_session_context *rdma_session, struct rswap_rdma_queue *rdma_queue, struct fs_rdma_req *rdma_req);
void fs_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc);
void fs_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc);

void drain_rdma_queue(struct rswap_rdma_queue *rdma_queue);
void drain_all_rdma_queues(int target_mem_server);

//
// Debug functions
//
char *rdma_message_print(int message_id);
char *rdma_session_context_state_print(int id);
char *rdma_cm_message_print(int cm_message_id);
char *rdma_wc_status_name(int wc_status_id);

void print_scatterlist_info(struct scatterlist *sl_ptr, int nents);

static inline void print_debug(const char *format, ...)
{
#ifdef DEBUG_MODE_BRIEF
	va_list args;
	va_start(args, format);
	printk(format, args);
	va_end(args);
#endif
}

/**
 * ########## Declare some global varibles ##########
 *
 * There are 2 parts, RDMA parts and Disk Driver parts.
 * Some functions are stateless and they are only used for allocate and initialize the variables.
 * So, we need to keep some global variables and not not exit the Main function.
 *
 * 	1). Data allocated by kzalloc, kmalloc etc. will not be freed by the kernel.
 *  	1.1) If we don't want to use any data, we need to free them mannually.
 * 		1.2) Fileds allocated by kzalloc and initiazed in the stateless functions will stay available.
 *
 *  2) Do NOT define global variables in header, only declare extern variables in header and implement them in .c file.
 */

// Initialize in main().
// One rdma_session_context per memory server connected by IB.
extern struct rdma_session_context rdma_session_global; // [!!] Unify the RDMA context and Disk Driver context global var [!!]

extern int online_cores;
extern int num_queues; // Both dispatch queues, rdma_queues equal to this num_queues.

//debug
extern u64 rmda_ops_count;
extern u64 cq_notify_count;
extern u64 cq_get_count;

#ifdef DEBUG_LATENCY_CLIENT
extern u64 *total_cycles;
extern u64 *last_cycles;
#endif

#endif // __RSWAP_RDMA_H
