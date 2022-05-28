#ifndef __RSWAP_UTILS_H
#define __RSWAP_UTILS_H


enum rdma_queue_type
{
	QP_SYNC, // TODO: load and store share the same queue now. separate them.
	QP_ASYNC,
	NUM_QP_TYPE
};

// 2-sided RDMA message type
// Used to communicate with cpu servers.
enum message_type
{
	DONE = 1,	  // Start from 1
	GOT_CHUNKS,	  // Get the remote_addr/rkey of multiple Chunks
	GOT_SINGLE_CHUNK, // Get the remote_addr/rkey of a single Chunk
	FREE_SIZE,	  //
	EVICT,		  // 5

	ACTIVITY, // 6
	STOP,	  //7, upper SIGNALs are used by server, below SIGNALs are used by client.
	REQUEST_CHUNKS,
	REQUEST_SINGLE_CHUNK, // Send a request to ask for a single chunk.
	QUERY,		      // 10

	AVAILABLE_TO_QUERY // 11 This memory server is oneline to server.
};

/**
 * RDMA command line attached to each RDMA message.
 *
 */
struct message
{
	// Information of the chunk to be mapped to remote memory server.
	uint64_t buf[MAX_REGION_NUM];	      // Remote addr, usd by clinet for RDMA read/write.
	uint64_t mapped_size[MAX_REGION_NUM]; // For a single Region, Maybe not fully mapped
	uint32_t rkey[MAX_REGION_NUM];	      // remote key
	int mapped_chunk;		      // Chunk number in current message.

	enum message_type type;
};


#endif // __RSWAP_UTILS_H
