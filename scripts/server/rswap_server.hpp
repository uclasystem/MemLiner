#ifndef __RSWAP_SERVER_HPP
#define __RSWAP_SERVER_HPP

// Include standard libraries. Search the configured path first.
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <linux/kernel.h>
#include <linux/wait.h>

#include "constants.h"
#include "utils.h"
//	Memory server is developed in user space, so use user-space IB API.
//	1)	"rdma/rdma_cma.h" is user space library, which is defined in /usr/include/rdma/rdma_cma.h.
//			"linux/rdma_cm.h" is kernel space library.
//       cpp -v /dev/null -o /dev/null   print the search path.
//

// Enable the debug information.
//#define DEBUG_RDMA_SERVER 1

// Used for describing RDMA QP status.
// [?] Does this matter ?
// This stauts is only used in function, handle_cqe(struct ibv_wc *wc).
#define CQ_QP_IDLE 0
#define CQ_QP_BUSY 1
#define CQ_QP_DOWN 2

#define ONLINE_CORES 16				     // Larger or equal to the online cores of CPU server
#define RDMA_NUM_QUEUES (ONLINE_CORES * NUM_QP_TYPE) // Larger or equal to the num queues of CPU server

/**
 * This memory server support multiple QP for each CPU.
 *
 */
struct rswap_rdma_queue
{
	// RDMA client ID, one for per QP.
	struct rdma_cm_id *cm_id; //  ? bind to QP

	// ib events
	struct ibv_cq *cq; // Completion queue
	struct ibv_qp *qp; // Queue Pair

	//enum rdma_queue_state state;  // the current status of the QP.
	//wait_queue_head_t 		sem;    // semaphore for wait/wakeup
	uint8_t connected; // some function can only be called once, this is the flag to record this.

	int q_index;
	enum rdma_queue_type type;
	struct context *rdma_session; // Record the RDMA session this queue belongs to.
};

struct rswap_rdma_dev
{
	struct ibv_context *ctx; // Memory server only needs one CQ, use the CQ of rdma_queue[0].
	struct ibv_pd *pd;
};

enum server_states
{
	S_WAIT,
	S_BIND,
	S_DONE
};

enum send_states
{
	SS_INIT,
	SS_MR_SENT,
	SS_STOP_SENT,
	SS_DONE_SENT
};

enum recv_states
{
	RS_INIT,
	RS_STOPPED_RECV,
	RS_DONE_RECV
};

/**
 *	RDMA conection context.
 *
 *  [?] Maintain a daemon thread to poll the RDMA message
 *
 */
struct context
{
	struct rswap_rdma_dev *rdma_dev;
	struct rswap_rdma_queue *rdma_queues;

	struct ibv_comp_channel *comp_channel;
	pthread_t cq_poller_thread; // Deamon thread to handle the 2-sided RDMA messages.

	// 2) Reserve wr for 2-sided RDMA communications
	//
	struct message *recv_msg; // RDMA commandline attached to each RDMA request.
	struct ibv_mr *recv_mr;	  // Need to register recv_msg as RDMA MR, then RDMA device can read/write it.

	struct message *send_msg;
	struct ibv_mr *send_mr;

	// 3) Used for 1-sided RDMA communications
	//
	struct rdma_mem_pool *mem_pool; // Manage the whole heap and Region, RDMA_MR information.
	int connected;			// global connection state, if any QP is connected to CPU server.

	// In our design, the memory pool on Memory server will be exited also.
	// Can't see benefits for reusing the Memory pool ? It also causes privacy problems.
	//
	server_states server_state;
};

/**
 * Describe the memory pool
 *  Start address;
 * 	Size;
 *
 * 	Start address of each Region.
 * 	The RDMA MR descripor for each Region.
 *
 * More explanation
 * 		Registering RDMA buffer at Region status is good for dynamic scaling.
 * 		The heap can expand at Region granularity.
 *
 */
struct rdma_mem_pool
{
	char *heap_start; // Start address of heap.
	int region_num;   // Number of Regions. Regions size is defined by Macro : CHUNK_SIZE_GB * ONE_GB.

	struct ibv_mr *mr_buffer[MAX_REGION_NUM];  // Register whole heap as RDMA buffer.
	char *region_list[MAX_REGION_NUM];	   // Start address of each Region. region_list[0] == Java_start.
	size_t region_mapped_size[MAX_REGION_NUM]; // The byte size of the corresponding Region. Count at bytes.
	int cache_status[MAX_REGION_NUM];	   // -1 NOT bind with CPU server.
};

/**
 * Define tools
 *
 */
void die(const char *reason);

#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) | \
		   (unsigned int)ntohl(((int)(x >> 32))))

#define TEST_NZ(x)                                                        \
	do {                                                              \
		if ((x))                                                  \
			die("error: " #x " failed (returned non-zero)."); \
	} while (0) // ERROR if NON-NULL.
#define TEST_Z(x)                                                          \
	do {                                                               \
		if (!(x))                                                  \
			die("error: " #x " failed (returned zero/null)."); \
	} while (0) // ERROR if NULL

/**
 * Declare functions
 */
int on_cm_event(struct rdma_cm_event *event);
int on_connect_request(struct rdma_cm_id *id);
int rdma_connected(struct rswap_rdma_queue *rdma_queue);
int on_disconnect(struct rswap_rdma_queue *rdma_queue);

void build_connection(struct rswap_rdma_queue *rdma_queue);
void build_params(struct rdma_conn_param *params);
void get_device_info(struct rswap_rdma_queue *rdma_queue);
void build_qp_attr(struct rswap_rdma_queue *rdma_queue, struct ibv_qp_init_attr *qp_attr);
void handle_cqe(struct ibv_wc *wc);

void inform_memory_pool_available(struct rswap_rdma_queue *rdma_queue);
void send_free_mem_size(struct rswap_rdma_queue *rdma_queue);
void send_regions(struct rswap_rdma_queue *rdma_queue);
void send_message(struct rswap_rdma_queue *rdma_queue);

void destroy_connection(struct context *rdma_session);
void *poll_cq(void *ctx);
void post_receives(struct rswap_rdma_queue *rdma_queue);

void init_memory_pool(struct context *rdma_ctx);
void register_rdma_comm_buffer(struct context *rdma_ctx);

enum rdma_queue_type get_qp_type(int idx);
struct rswap_rdma_queue *get_rdma_queue(unsigned int cpu, enum rdma_queue_type type);
/**
 * Debug Utils
 */
static inline void print_debug(FILE *file, const char *format, ...)
{
#ifdef DEBUG_RDMA_SERVER
	va_list args;
	va_start(args, format);
	fprintf(file, format, args);
	va_end(args);
#endif
}

/**
 * Global variables
 *
 * More Explanations
 *
 *  "static" : For both C and C++, using "static" before a global variable will limit its usage scope, the defined .cpp file.
 *             For example,  "static struct context * global_rdma_ctx" in header, if multiple .cpp include this header,
 *             each .cpp has a local copy of variable, struct context* global_rdma_ctx. There is no conflict at all.
 *
 *             For "struct context * global_rdma_ctx" in header, and multiple .cpp include this header,
 *             then each .cpp has a global variable, struct context *global_rdma_ctx, with global usage scope.
 *             This will cause "Multiple definitions issue". We need to use "extern".
 *
 *              Warning : static in Class, funtion means a "single version" and "duration" variable with the same lifetime of the program.
 *
 * "extern" : linkage. Only one instance of this global variable and it's defined in some source file.
 *
 */
extern struct context *global_rdma_ctx; // The RDMA controll context.
extern int rdma_queue_count;
//extern struct rdma_mem_pool* global_mem_pool;

extern int errno;

#endif // __RSWAP_SERVER_HPP
