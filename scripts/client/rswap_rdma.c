/**
 * frontswap_rdma is the RDMA component for frontswap path.
 * Its RDMA queues are different from the swap version.
 *
 *
 * 1. Global variables
 *
 * 2. Structure for RDMA connection
 * 	One session for each Memory server.
 *
 * 	2.1 Structure of RDMA session
 * 		1 RDMA queue for Data/Control path
 *
 * 	2.2	Structure of RDMA queue
 * 		1 cm_id
 * 		1 QP
 * 		1 IB_POLL_DIRECT CQ for control/data path
 *
 * 	2.3 There are 3 types of communication here
 * 		a. Data Path. Triggered by swap
 * 			=> all the rdma_queues[]
 * 		b. Control Path. Invoked by User space.
 * 			=> rdma_queue[0]
 * 		c. Exchange meta data with memory server. 2-sided RDMA and only used during initializaiton.
 * 			=> rdma_queue[0]
 *
 */

#include "rswap_rdma.h"

//
// Implement the global vatiables here
//

struct rdma_session_context rdma_session_global;
int online_cores; // Control the parallelism
int num_queues;   // Total #(queues)

//debug
u64 rmda_ops_count = 0;
u64 cq_notify_count = 0;
u64 cq_get_count = 0;

// Utilities functions
inline enum rdma_queue_type get_qp_type(int idx) {
	if (idx < online_cores) {
		return QP_SYNC;
	} else if (idx < 2 * online_cores) {
		return QP_ASYNC;
	}

	pr_err("wrong rdma queue type %d\n", idx / online_cores);
	return QP_SYNC;
}

inline struct rswap_rdma_queue *get_rdma_queue(struct rdma_session_context *rdma_session,
					       unsigned int cpu, enum rdma_queue_type type)
{
	switch (type) {
	case QP_SYNC:
		return &(rdma_session->rdma_queues[cpu]);
	case QP_ASYNC:
		return &(rdma_session->rdma_queues[cpu + online_cores]);
	default:
		BUG();
	};
	return &(rdma_session->rdma_queues[cpu]);
}

//
// >>>>>>>>>>>>>>>>>>>>>>  Start of handle  TWO-SIDED RDMA message section >>>>>>>>>>>>>>>>>>>>>>
//

/**
 * Receive a WC, IB_WC_RECV.
 * Read the data from the posted WR.
 * 		For this WR, its associated DMA buffer is rdma_session_context->recv_buf.
 *
 * Action
 * 		According to the RDMA message information, rdma_session_context->state, to set some fields.
 * 		FREE_SIZE : set the
 *
 * More Explanation
 * 		For the 2-sided RDMA, the receiver even can not responds any message back ?
 * 		The character of 2-sided RDMA communication is just to send a interrupt to receiver's CPU.
 *
 */
int handle_recv_wr(struct rswap_rdma_queue *rdma_queue, struct ib_wc *wc)
{
	int ret = 0;
	struct rdma_session_context *rdma_session = rdma_queue->rdma_session;

	if (wc->byte_len != sizeof(struct message)) { // Check the length of received message
		printk(KERN_ERR "%s, Received bogus data, size %d\n", __func__, wc->byte_len);
		ret = -1;
		goto out;
	}

	// Is this check necessary ?
	if (unlikely(rdma_queue->state < CONNECTED)) {
		print_debug(KERN_ERR "%s, RDMA is not connected\n", __func__);
		return -1;
	}

	switch (rdma_session->rdma_recv_req.recv_buf->type) {
	case AVAILABLE_TO_QUERY:
		print_debug("%s, Received AVAILABLE_TO_QERY, memory server is prepared well. We can qu\n ", __func__);
		rdma_queue->state = MEMORY_SERVER_AVAILABLE;

		wake_up_interruptible(&rdma_queue->sem);
		break;
	case FREE_SIZE:
		// get the number of Free Regions.
		printk(KERN_INFO "%s, Received FREE_SIZE, avaible chunk number : %d \n ", __func__, rdma_session->rdma_recv_req.recv_buf->mapped_chunk);

		rdma_session->remote_chunk_list.chunk_num = rdma_session->rdma_recv_req.recv_buf->mapped_chunk;
		rdma_queue->state = FREE_MEM_RECV;

		ret = init_remote_chunk_list(rdma_session);
		if (unlikely(ret)){
			printk(KERN_ERR "Initialize the remote chunk failed. \n");
		}

		wake_up_interruptible(&rdma_queue->sem);
		break;
	case GOT_CHUNKS:
		// Got memory chunks from remote memory server, do contiguous mapping.
		bind_remote_memory_chunks(rdma_session);

		// Received free chunks from remote memory,
		// Wakeup the waiting main thread and continure.
		rdma_queue->state = RECEIVED_CHUNKS;
		wake_up_interruptible(&rdma_queue->sem); // Finish main function.

		break;
	default:
		printk(KERN_ERR "%s, Recieved WRONG RDMA message %d \n", __func__, rdma_session->rdma_recv_req.recv_buf->type);
		ret = -1;
		goto out;
	}

out:
	return ret;
}

/**
 * Received the CQE for the posted recv_wr
 *
 */
void two_sided_message_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rswap_rdma_queue *rdma_queue = cq->cq_context;
	int ret = 0;

	// 1) check the wc status
	if (wc->status != IB_WC_SUCCESS) {
		printk(KERN_ERR "%s, cq completion failed with wr_id 0x%llx status %d,  status name %s, opcode %d,\n", __func__,
		       wc->wr_id, wc->status, rdma_wc_status_name(wc->status), wc->opcode);
		goto out;
	}

	// 2) process the received message
	switch (wc->opcode) {
	case IB_WC_RECV:
		// Recieve 2-sided RDMA recive wr
		printk("%s, Got a WC from CQ, IB_WC_RECV. \n", __func__);

		// Need to do actions based on the received message type.
		ret = handle_recv_wr(rdma_queue, wc);
		if (unlikely(ret)) {
			printk(KERN_ERR "%s, recv wc error: %d\n", __func__, ret);
			goto out;
		}
		break;
	case IB_WC_SEND:
		printk("%s, Got a WC from CQ, IB_WC_SEND. 2-sided RDMA post done. \n", __func__);
		break;
	default:
		printk(KERN_ERR "%s:%d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc->opcode);
		goto out;
	}

	atomic_dec(&rdma_queue->rdma_post_counter);
out:
	return;
}

/**
 * Send a RDMA message to remote server.
 * Used for RDMA conenction build.
 *
 */
int send_message_to_remote(struct rdma_session_context *rdma_session, int rdma_queue_ind, int messge_type, int chunk_num)
{
	int ret = 0;
	const struct ib_recv_wr *recv_bad_wr;
	const struct ib_send_wr *send_bad_wr;
	struct rswap_rdma_queue *rdma_queue;

	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_ind]);
	rdma_session->rdma_send_req.send_buf->type = messge_type;
	rdma_session->rdma_send_req.send_buf->mapped_chunk = chunk_num; // 1 Meta , N-1 Data Regions

	// post a 2-sided RDMA recv wr first.
	ret = ib_post_recv(rdma_queue->qp, &rdma_session->rdma_recv_req.rq_wr, &recv_bad_wr);
	if (ret) {
		printk(KERN_ERR "%s, Post 2-sided message to receive data failed.\n", __func__);
		goto err;
	}
	atomic_inc(&rdma_queue->rdma_post_counter);

	print_debug("Send a Message to memory server. send_buf->type : %d, %s \n", messge_type, rdma_message_print(messge_type));
	ret = ib_post_send(rdma_queue->qp, &rdma_session->rdma_send_req.sq_wr, &send_bad_wr);
	if (ret) {
		printk(KERN_ERR "%s: BIND_SINGLE MSG send error %d\n", __func__, ret);
		goto err;
	}
	atomic_inc(&rdma_queue->rdma_post_counter);

err:
	return ret;
}

/**
 * Check the available memory size on this session/memory server
 * All the QPs of the session share the same memory, request for any QP is good.
 *
 */
int rswap_query_available_memory(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	struct rswap_rdma_queue *rdma_queue;

	rdma_queue = &(rdma_session->rdma_queues[num_queues - 1]);
	// wait untile all the rdma_queue are connected.
	// wait for the last rdma_queue[num_queues-1] get MEMORY_SERVER_AVAILABLE
	wait_event_interruptible(rdma_queue->sem, rdma_queue->state == MEMORY_SERVER_AVAILABLE);
	printk(KERN_INFO "%s, All %d rdma queues are prepared well. Query its available memory. \n", __func__, num_queues);

	// And then use the rdma_queue[0] for communication.
	// Post a 2-sided RDMA to query memory server's available memory.
	rdma_queue = &(rdma_session->rdma_queues[0]);
	ret = send_message_to_remote(rdma_session, 0, QUERY, 0); // for QERY, chunk_num = 0
	if (ret) {
		printk(KERN_ERR "%s, Post 2-sided message to remote server failed.\n", __func__);
		goto err;
	}

	// recive 2-sided wr.
	drain_rdma_queue(rdma_queue);
	// Sequence controll
	wait_event_interruptible(rdma_queue->sem, rdma_queue->state == FREE_MEM_RECV);

	printk("%s: Got %d free memory chunks from remote memory server. Request for Chunks \n",
	       __func__,
	       rdma_session->remote_chunk_list.chunk_num);

err:
	return ret;
}

/**
 *  Post a 2-sided request for chunk mapping.
 *
 * More Explanation
 * 	We can only post a cqe notification per time, OR these cqe notifications may overlap and lost.
 * 	So we post the cqe notification in cq_event_handler function.
 *
 */
int rswap_request_for_chunk(struct rdma_session_context *rdma_session, int num_chunk)
{
	int ret = 0;
	struct rswap_rdma_queue *rdma_queue;

	rdma_queue = &(rdma_session->rdma_queues[0]); // We can request the registerred memory from any QP of the target memory server.
	if (num_chunk == 0 || rdma_session == NULL) {
		printk(KERN_ERR "%s, current memory server has no available memory at all. Exit. \n", __func__);
		goto err;
	}

	// Post a 2-sided RDMA to requst all the available regions from current memory server.
	ret = send_message_to_remote(rdma_session, 0, REQUEST_CHUNKS, num_chunk);
	if (ret) {
		printk(KERN_ERR "%s, Post 2-sided message to remote server failed.\n", __func__);
		goto err;
	}

	drain_rdma_queue(rdma_queue);
	// Sequence controll
	wait_event_interruptible(rdma_queue->sem, rdma_queue->state == RECEIVED_CHUNKS);

	printk(KERN_INFO "%s, Got %d chunks from memory server.\n", __func__, num_chunk);
err:
	return ret;
}

//
// <<<<<<<<<<<<<<  End of handling TWO-SIDED RDMA message section <<<<<<<<<<<<<<
//

//
// ############# Start of RDMA Communication (CM) event handler ########################
//
// [?] All the RDMA Communication(CM) event is triggered by hardware and mellanox driver.
//		No need to maintain a daemon thread to handle the CM events ?  -> stateless handler
//

/**
 * The rdma CM event handler function
 *
 * [?] Seems that this function is triggered when a CM even arrives this device.
 *
 * More Explanation
 * 	 CMA Event handler  && cq_event_handler , 2 different functions for CM event and normal RDMA message handling.
 *
 */
int rswap_rdma_cm_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
	int ret = 0;
	struct rswap_rdma_queue *rdma_queue = cma_id->context;

	print_debug("cma_event type %d, type_name: %s \n", event->event, rdma_cm_message_print(event->event));
	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		rdma_queue->state = ADDR_RESOLVED;
		print_debug("%s,  get RDMA_CM_EVENT_ADDR_RESOLVED. Send RDMA_ROUTE_RESOLVE to Memory server \n", __func__);

		// Go to next step directly, resolve the rdma route.
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR "%s,rdma_resolve_route error %d\n", __func__, ret);
		}
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		// RDMA route is solved, wake up the main process  to continue.
		print_debug("%s : RDMA_CM_EVENT_ROUTE_RESOLVED, wake up rdma_queue->sem\n ", __func__);

		// Sequencial controll
		rdma_queue->state = ROUTE_RESOLVED;
		wake_up_interruptible(&rdma_queue->sem);
		break;
	case RDMA_CM_EVENT_CONNECT_REQUEST: // Receive RDMA connection request
		printk("Receive but Not Handle : RDMA_CM_EVENT_CONNECT_REQUEST \n");
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		printk("%s, ESTABLISHED, wake up rdma_queue->sem\n", __func__);
		rdma_queue->state = CONNECTED;
		wake_up_interruptible(&rdma_queue->sem);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR "%s, cma event %d, event name %s, error code %d \n", __func__, event->event,
		       rdma_cm_message_print(event->event), event->status);
		rdma_queue->state = ERROR;
		wake_up_interruptible(&rdma_queue->sem);
		break;
	case RDMA_CM_EVENT_DISCONNECTED: //should get error msg from here
		printk("%s, Receive DISCONNECTED  signal \n", __func__);

		if (rdma_queue->freed) { // 1, during free process.
			// Client request for RDMA disconnection.
			print_debug("%s, RDMA disconnect evetn, requested by client. \n", __func__);
		} else { // freed ==0, newly start free process
			// Remote server requests for disconnection.

			// !! DEAD PATH NOW --> CAN NOT FREE CM_ID !!
			// wait for RDMA_CM_EVENT_TIMEWAIT_EXIT ??
			//			TO BE DONE
			print_debug("%s, RDMA disconnect evetn, requested by client. \n", __func__);
			//do we need to inform the client, the connect is broken ?
			rdma_disconnect(rdma_queue->cm_id);

			// ########### TO BE DONE ###########
			// Disconnect and free the resource of THIS QUEUE.

			// NOT free the resource of the whole session !
			//rswap_disconnect_and_collect_resource(rdma_queue->rdma_session);
			//octopus_free_block_devicce(rdma_session->rmem_dev_ctrl);	// Free block device resource
		}
		break;
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		// After the received DISCONNECTED_EVENT, need to wait the on-the-fly RDMA message
		// https://linux.die.net/man/3/rdma_get_cm_event
		printk("%s, Wait for in-the-fly RDMA message finished. \n", __func__);
		rdma_queue->state = CM_DISCONNECT;

		//Wakeup caller
		wake_up_interruptible(&rdma_queue->sem);
		break;
	case RDMA_CM_EVENT_DEVICE_REMOVAL: //this also should be treated as disconnection, and continue disk swap
		printk(KERN_ERR "%s, cma detected device removal!!!!\n", __func__);
		return -1;
		break;
	default:
		printk(KERN_ERR "%s,oof bad type!\n", __func__);
		wake_up_interruptible(&rdma_queue->sem);
		break;
	}
	return ret;
}

// Resolve the destination IB device by the destination IP.
// [?] Need to build some route table ?
//
static int rdma_resolve_ip_to_ib_device(struct rdma_session_context *rdma_session, struct rswap_rdma_queue *rdma_queue)
{
	int ret;
	struct sockaddr_storage sin;
	struct sockaddr_in *sin4 = (struct sockaddr_in *)&sin;

	sin4->sin_family = AF_INET;
	memcpy((void *)&(sin4->sin_addr.s_addr), rdma_session->addr, 4); // copy 32bits/ 4bytes from cb->addr to sin4->sin_addr.s_addr
	sin4->sin_port = rdma_session->port;				 // assign cb->port to sin4->sin_port

	ret = rdma_resolve_addr(rdma_queue->cm_id, NULL, (struct sockaddr *)&sin, 2000); // timeout time 2000ms
	if (ret) {
		printk(KERN_ERR "%s, rdma_resolve_ip_to_ib_device error %d\n", __func__, ret);
		return ret;
	} else {
		printk("rdma_resolve_ip_to_ib_device - rdma_resolve_addr success.\n");
	}

	// Wait for the CM events to be finished:  handled by rdma_cm_event_handler()
	// 	1) resolve addr
	//	2) resolve route
	// Come back here and continue:
	//
	wait_event_interruptible(rdma_queue->sem, rdma_queue->state >= ROUTE_RESOLVED); //[?] Wait on cb->sem ?? Which process will wake up it.
	if (rdma_queue->state != ROUTE_RESOLVED) {
		printk(KERN_ERR "%s, addr/route resolution did not resolve: state %d\n", __func__, rdma_queue->state);
		return -EINTR;
	}

	printk(KERN_INFO "%s, resolve address and route successfully\n", __func__);
	return ret;
}

/**
 * Build the Queue Pair (QP).
 *
 */
int rswap_create_qp(struct rdma_session_context *rdma_session, struct rswap_rdma_queue *rdma_queue)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = rdma_session->send_queue_depth;
	init_attr.cap.max_recv_wr = rdma_session->recv_queue_depth;
	//init_attr.cap.max_recv_sge = MAX_REQUEST_SGL;					// enable the scatter
	init_attr.cap.max_recv_sge = 1;		      // for the receive, no need to enable S/G.
	init_attr.cap.max_send_sge = MAX_REQUEST_SGL; // enable the gather
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;     // Receive a signal when posted wr is done.
	init_attr.qp_type = IB_QPT_RC;		      // Queue Pair connect type, Reliable Communication.  [?] Already assign this during create cm_id.

	// Both recv_cq and send_cq use the same cq.
	init_attr.send_cq = rdma_queue->cq;
	init_attr.recv_cq = rdma_queue->cq;

	ret = rdma_create_qp(rdma_queue->cm_id, rdma_session->rdma_dev->pd, &init_attr);
	if (!ret) {
		// Record this queue pair.
		rdma_queue->qp = rdma_queue->cm_id->qp;
	} else {
		printk(KERN_ERR "%s:  Create QP falied. errno : %d \n", __func__, ret);
	}

	return ret;
}

// Prepare for building the Connection to remote IB servers.
// Create Queue Pair : pd, cq, qp,
int rswap_create_rdma_queue(struct rdma_session_context *rdma_session, int rdma_queue_index)
{
	int ret = 0;
	struct rdma_cm_id *cm_id;
	struct rswap_rdma_queue *rdma_queue;
	int comp_vector = 0; // used for IB_POLL_DIRECT

	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_index]);
	cm_id = rdma_queue->cm_id;

	// 1) Build PD.
	// flags of Protection Domain, (ib_pd) : Protect the local OR remote memory region.  [??] the pd is for local or for the remote attached to the cm_id ?
	// Local Read is default.
	// Allocate and initialize for one rdma_session should be good.
	if (rdma_session->rdma_dev == NULL) {
		rdma_session->rdma_dev = kzalloc(sizeof(struct rswap_rdma_dev), GFP_KERNEL);

		rdma_session->rdma_dev->pd = ib_alloc_pd(cm_id->device, IB_ACCESS_LOCAL_WRITE |
									IB_ACCESS_REMOTE_READ |
									IB_ACCESS_REMOTE_WRITE); // No local read ??  [?] What's the cb->pd used for ?
		rdma_session->rdma_dev->dev = rdma_session->rdma_dev->pd->device;

		if (IS_ERR(rdma_session->rdma_dev->pd)) {
			printk(KERN_ERR "%s, ib_alloc_pd failed\n", __func__);
			goto err;
		}
		printk(KERN_INFO "%s, created pd %p\n", __func__, rdma_session->rdma_dev->pd);

		// Time to reserve RDMA buffer for this session.
		setup_rdma_session_comm_buffer(rdma_session);
	}

	// 2) Build CQ
	int cq_num_cqes = rdma_session->send_queue_depth + rdma_session->recv_queue_depth;
	if (rdma_queue->type == QP_ASYNC) {
		rdma_queue->cq = ib_alloc_cq(cm_id->device, rdma_queue, cq_num_cqes,
					     comp_vector, IB_POLL_SOFTIRQ);
	} else {
		rdma_queue->cq = ib_alloc_cq(cm_id->device, rdma_queue, cq_num_cqes,
					     comp_vector, IB_POLL_DIRECT);
	}

	if (IS_ERR(rdma_queue->cq)) {
		printk(KERN_ERR "%s, ib_create_cq failed\n", __func__);
		ret = PTR_ERR(rdma_queue->cq);
		goto err;
	}
	printk(KERN_INFO "%s, created cq %p\n", __func__, rdma_queue->cq);

	// 3) Build QP.
	ret = rswap_create_qp(rdma_session, rdma_queue);
	if (ret) {
		printk(KERN_ERR "%s, failed: %d\n", __func__, ret);
		goto err;
	}
	printk(KERN_INFO "%s, created qp %p\n", __func__, rdma_queue->qp);

err:
	return ret;
}

/**
 * Reserve two RDMA wr for receive/send meesages
 * 		rdma_session_context->rq_wr
 * 		rdma_session_context->send_sgl
 * Post these 2 WRs to receive/send controll messages.
 */
void rswap_setup_message_wr(struct rdma_session_context *rdma_session)
{
	// 1) Reserve a wr for 2-sided RDMA recieve wr
	rdma_session->rdma_recv_req.recv_sgl.addr = rdma_session->rdma_recv_req.recv_dma_addr; // sg entry addr
	rdma_session->rdma_recv_req.recv_sgl.length = sizeof(struct message);		       // address of the length
	rdma_session->rdma_recv_req.recv_sgl.lkey = rdma_session->rdma_dev->dev->local_dma_lkey;

	rdma_session->rdma_recv_req.rq_wr.sg_list = &(rdma_session->rdma_recv_req.recv_sgl);
	rdma_session->rdma_recv_req.rq_wr.num_sge = 1; // ib_sge scatter-gather's number is 1
	rdma_session->rdma_recv_req.cqe.done = two_sided_message_done;
	rdma_session->rdma_recv_req.rq_wr.wr_cqe = &(rdma_session->rdma_recv_req.cqe);

	// 2) Reserve a wr for 2-sided RDMA send wr
	rdma_session->rdma_send_req.send_sgl.addr = rdma_session->rdma_send_req.send_dma_addr;
	rdma_session->rdma_send_req.send_sgl.length = sizeof(struct message);
	rdma_session->rdma_send_req.send_sgl.lkey = rdma_session->rdma_dev->dev->local_dma_lkey;

	rdma_session->rdma_send_req.sq_wr.opcode = IB_WR_SEND; // ib_send_wr.opcode , passed to wc.
	rdma_session->rdma_send_req.sq_wr.send_flags = IB_SEND_SIGNALED;
	rdma_session->rdma_send_req.sq_wr.sg_list = &rdma_session->rdma_send_req.send_sgl;
	rdma_session->rdma_send_req.sq_wr.num_sge = 1;
	rdma_session->rdma_send_req.cqe.done = two_sided_message_done;
	rdma_session->rdma_send_req.sq_wr.wr_cqe = &(rdma_session->rdma_send_req.cqe);

	return;
}

/**
 * We reserve two WRs for send/receive RDMA messages in a 2-sieded way.
 * 	a. Allocate 2 buffers
 * 		dma_session->recv_buf
 * 		rdma_session->send_buf
 *	b. Bind their DMA/BUS address to
 * 		rdma_context->recv_sgl
 * 		rdma_context->send_sgl
 * 	c. Bind the ib_sge to send/receive WR
 * 		rdma_context->rq_wr
 * 		rdma_context->sq_wr
 */
int rswap_setup_buffers(struct rdma_session_context *rdma_session)
{
	int ret = 0;

	// 1) Allocate some DMA buffers.
	// [x] Seems that any memory can be registered as DMA buffers, if they satisfy the constraints:
	// 1) Corresponding physical memory is allocated. The page table is built.
	//		If the memory is allocated by user space allocater, malloc, we need to walk  through the page table.
	// 2) The physial memory is pinned, can't be swapt/paged out.
	rdma_session->rdma_recv_req.recv_buf = kzalloc(sizeof(struct message), GFP_KERNEL); //[?] Or do we need to allocate DMA memory by get_dma_addr ???
	rdma_session->rdma_send_req.send_buf = kzalloc(sizeof(struct message), GFP_KERNEL);

	// Get DMA/BUS address for the receive buffer
	rdma_session->rdma_recv_req.recv_dma_addr = ib_dma_map_single(rdma_session->rdma_dev->dev, rdma_session->rdma_recv_req.recv_buf,
								      sizeof(struct message), DMA_BIDIRECTIONAL);
	rdma_session->rdma_send_req.send_dma_addr = ib_dma_map_single(rdma_session->rdma_dev->dev, rdma_session->rdma_send_req.send_buf,
								      sizeof(struct message), DMA_BIDIRECTIONAL);

	print_debug("%s, Got dma/bus address 0x%llx, for the recv_buf 0x%llx \n", __func__, (unsigned long long)rdma_session->rdma_recv_req.recv_dma_addr,
	       (unsigned long long)rdma_session->rdma_recv_req.recv_buf);
	print_debug("%s, Got dma/bus address 0x%llx, for the send_buf 0x%llx \n", __func__, (unsigned long long)rdma_session->rdma_send_req.send_dma_addr,
	       (unsigned long long)rdma_session->rdma_send_req.send_buf);

	// 3) Add the allocated (DMA) buffer to reserved WRs
	rswap_setup_message_wr(rdma_session);
	print_debug(KERN_INFO "%s, allocated & registered buffers...\n", __func__);
	print_debug(KERN_INFO "%s is done. \n", __func__);

	return ret;
}

/**
 * All the PD, QP, CP are setted up, connect to remote IB servers.
 * This will send a CM event to remote IB server && get a CM event response back.
 */
int rswap_connect_remote_memory_server(struct rdma_session_context *rdma_session, int rdma_queue_inx)
{
	struct rdma_conn_param conn_param;
	int ret;
	struct rswap_rdma_queue *rdma_queue;
	const struct ib_recv_wr *bad_wr;

	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_inx]);
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	// After rdma connection built, memory server will send a 2-sided RDMA message immediately
	// post a recv on cq to wait wc
	// MT safe during the connection process
	atomic_inc(&rdma_queue->rdma_post_counter);
	ret = ib_post_recv(rdma_queue->qp, &rdma_session->rdma_recv_req.rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR "%s: post a 2-sided RDMA message error \n", __func__);
		goto err;
	}

	ret = rdma_connect(rdma_queue->cm_id, &conn_param); // RDMA CM event
	if (ret) {
		printk(KERN_ERR "%s, rdma_connect error %d\n", __func__, ret);
		return ret;
	}
	printk(KERN_INFO "%s, Send RDMA connect request to remote server \n", __func__);

	// Get free memory information from Remote Mmeory Server
	// [X] After building the RDMA connection, server will send its free memory to the client.
	// Post the WR to get this RDMA two-sided message.
	// When the receive WR is finished, cq_event_handler will be triggered.

	// a. post a receive wr

	// b. Request a notification (IRQ), if an event arrives on CQ entry.
	// For a IB_POLL_DIRECT cq, we need to poll the cqe manually.

	// After receiving the CONNECTED state, means the RDMA connection is built.
	// Prepare to receive 2-sided RDMA message
	wait_event_interruptible(rdma_queue->sem, rdma_queue->state >= CONNECTED);
	if (rdma_queue->state == ERROR) {
		printk(KERN_ERR "%s, Received ERROR response, state %d\n", __func__, rdma_queue->state);
		return -1;
	}

	// for the posted ib_recv_wr.
	// check available memory
	drain_rdma_queue(rdma_queue);

	printk(KERN_INFO "%s, RDMA connect successful\n", __func__);

err:
	return ret;
}

//
// <<<<<<<<<<<<<<<<<<<<<<<  End of RDMA Communication (CM) event handler <<<<<<<<<<<<<<<<<<<<<<<
//

//
// >>>>>>>>>>>>>>>  Start of handling chunk management >>>>>>>>>>>>>>>
//
// After building the RDMA connection, build the Client-Chunk Remote-Chunk mapping information.
//

/**
 * Invoke this information after getting the free size of remote memory pool.
 * Initialize the chunk_list based on the chunk size and remote free memory size.
 *
 *
 * More Explanation:
 * 	Record the address of the mapped chunk:
 * 		remote_rkey : Used by the client, read/write data here.
 * 		remote_addr : The actual virtual address of the mapped chunk ?
 *
 */
int init_remote_chunk_list(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	uint32_t i;

	// 1) initialize chunk related variables
	//		The first Region may not be fullly mapped. Not clear for now.
	//		The 2nd -> rest are fully mapped at REGION_SIZE_GB size.
	rdma_session->remote_chunk_list.chunk_ptr = 0;	      // Points to the first empty chunk.
	rdma_session->remote_chunk_list.remote_free_size = 0; // not clear the exactly free size now.
	rdma_session->remote_chunk_list.remote_chunk = (struct remote_mapping_chunk *)kzalloc(
	    sizeof(struct remote_mapping_chunk) * rdma_session->remote_chunk_list.chunk_num,
	    GFP_KERNEL);

	for (i = 0; i < rdma_session->remote_chunk_list.chunk_num; i++) {
		rdma_session->remote_chunk_list.remote_chunk[i].chunk_state = EMPTY;
		rdma_session->remote_chunk_list.remote_chunk[i].remote_addr = 0x0;
		rdma_session->remote_chunk_list.remote_chunk[i].mapped_size = 0x0;
		rdma_session->remote_chunk_list.remote_chunk[i].remote_rkey = 0x0;
	}

	return ret;
}

/**
 * Get a chunk mapping 2-sided RDMA message.
 * Bind these chunks to the cient in order.
 *
 *	1) The information of Chunks to be bound,  is stored in the recv WR associated DMA buffer.
 * Record the address of the mapped chunk:
 * 		remote_rkey : Used by the client, read/write data here.
 * 		remote_addr : The actual virtual address of the mapped chunk
 *
 *	2) Attach the received chunks to the rdma_session_context->remote_mapping_chunk_list->remote_mapping_chunk[]
 */
void bind_remote_memory_chunks(struct rdma_session_context *rdma_session)
{
	int i;
	uint32_t *chunk_ptr;

	chunk_ptr = &(rdma_session->remote_chunk_list.chunk_ptr);
	// Traverse the receive WR to find all the got chunks.
	for (i = 0; i < MAX_REGION_NUM; i++) {
		if (*chunk_ptr >= rdma_session->remote_chunk_list.chunk_num) {
			print_debug(KERN_ERR "%s, Get too many chunks. \n", __func__);
			break;
		}

		if (rdma_session->rdma_recv_req.recv_buf->rkey[i]) {
			// Sent chunk, attach to current chunk_list's tail.
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_rkey = rdma_session->rdma_recv_req.recv_buf->rkey[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_addr = rdma_session->rdma_recv_req.recv_buf->buf[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].mapped_size = rdma_session->rdma_recv_req.recv_buf->mapped_size[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].chunk_state = MAPPED;

			rdma_session->remote_chunk_list.remote_free_size += rdma_session->rdma_recv_req.recv_buf->mapped_size[i]; // byte size, 4KB alignment

			print_debug(KERN_INFO "Got chunk[%d] : remote_addr : 0x%llx, remote_rkey: 0x%x, mapped_size: 0x%llx \n", i,
			       rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_addr,
			       rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_rkey,
			       rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].mapped_size);

			(*chunk_ptr)++;
		}

	}

	rdma_session->remote_chunk_list.chunk_num = *chunk_ptr; // Record the number of received chunks.
}

//
// <<<<<<<<<<<<<<<<<<<<<  End of handling chunk management <<<<<<<<<<<<<<<<<<<<<
//

//
// >>>>>>>>>>>>>>>  Start of RDMA intialization >>>>>>>>>>>>>>>
//

/**
 * Init the rdma sessions for each memory server.
 * 	One rdma_session_context for each memory server
 *  	one QP for each core (on cpu server)
 *
 */
int init_rdma_sessions(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	char ip[] = "10.0.0.4"; // the memory server ip
	uint16_t port = 9400;

	// 1) RDMA queue information
	// The number of outstanding wr the QP's send queue and recv queue.
	// the on-the-fly RDMA request should be more than on-the-fly i/o request.
	// use 1-sided RDMA to transfer data.
	// Only 2-sided RDMA needs to post the recv wr.
	rdma_session->rdma_queues = kzalloc(sizeof(struct rswap_rdma_queue) * num_queues, GFP_KERNEL);
	rdma_session->send_queue_depth = RDMA_SEND_QUEUE_DEPTH + 1;
	rdma_session->recv_queue_depth = RDMA_RECV_QUEUE_DEPTH + 1;

	// 2) Setup socket information
	rdma_session->port = htons(port);		      // After transffer to big endian, the decimal value is 47140
	ret = in4_pton(ip, strlen(ip), rdma_session->addr, -1, NULL); // char* to ipv4 address ?
	if (ret == 0) { // kernel 4.11.0 , success 1; failed 0.
		printk(KERN_ERR "Assign ip %s to  rdma_session->addr : %s failed.\n", ip, rdma_session->addr);
		goto err;
	}
	rdma_session->addr_type = AF_INET; //ipv4

err:
	return ret;
}

/**
 * Reserve some rdma_wr, registerred buffer for fast communiction.
 * 	e.g. 2-sided communication, meta data communication.
 */
int setup_rdma_session_comm_buffer(struct rdma_session_context *rdma_session)
{
	int ret = 0;

	if (rdma_session->rdma_dev == NULL) {
		printk(KERN_ERR "%s, rdma_session->rdma_dev is NULL. too early to regiseter RDMA buffer.\n", __func__);
		goto err;
	}

	// 1) Reserve some buffer for 2-sided RDMA communication
	ret = rswap_setup_buffers(rdma_session);
	if (unlikely(ret)) {
		printk(KERN_ERR "%s, Bind DMA buffer error\n", __func__);
		goto err;
	}

err:
	return ret;
}

/**
 * Build and Connect all the QP for each Session/memory server.
 *
 */
int rswap_init_rdma_queue(struct rdma_session_context *rdma_session, int idx)
{
	int ret = 0;
	struct rswap_rdma_queue *rdma_queue = &(rdma_session->rdma_queues[idx]);

	rdma_queue->rdma_session = rdma_session;
	rdma_queue->q_index = idx;
	rdma_queue->type = get_qp_type(idx);
	rdma_queue->cm_id = rdma_create_id(&init_net, rswap_rdma_cm_event_handler, rdma_queue, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(rdma_queue->cm_id)) {
		printk(KERN_ERR "failed to create cm id: %ld\n", PTR_ERR(rdma_queue->cm_id));
		ret = -ENODEV;
		goto err;
	}

	rdma_queue->state = IDLE;
	init_waitqueue_head(&rdma_queue->sem);		 // Initialize the semaphore.
	spin_lock_init(&(rdma_queue->cq_lock));		 // initialize spin lock
	atomic_set(&(rdma_queue->rdma_post_counter), 0); // Initialize the counter to 0
	rdma_queue->fs_rdma_req_cache = kmem_cache_create("fs_rdma_req_cache", sizeof(struct fs_rdma_req), 0,
							  SLAB_TEMPORARY | SLAB_HWCACHE_ALIGN, NULL);
	if (unlikely(rdma_queue->fs_rdma_req_cache == NULL)) {
		printk(KERN_ERR "%s, allocate rdma_queue->fs_rdma_req_cache failed.\n", __func__);
		ret = -1;
		goto err;
	}

	//2) Resolve address(ip:port) and route to destination IB.
	ret = rdma_resolve_ip_to_ib_device(rdma_session, rdma_queue);
	if (unlikely(ret)) {
		printk(KERN_ERR "%s, bind socket error (addr or route resolve error)\n", __func__);
		return ret;
	}

	return ret;

err:
	rdma_destroy_id(rdma_queue->cm_id);
	return ret;
}

/**
 * Build the RDMA connection to remote memory server.
 *
 * Parameters
 * 		rdma_session, RDMA controller/context.
 *
 *
 * More Exlanation:
 * 		[?] This function is too big, it's better to cut it into several pieces.
 *
 */
int rdma_session_connect(struct rdma_session_context *rdma_session)
{
	int ret;
	int i;

	// 1) Build and connect all the QPs of this session
	for (i = 0; i < num_queues; i++) {
		// crete cm_id and other fields e.g. ip
		ret = rswap_init_rdma_queue(rdma_session, i);
		if (unlikely(ret)) {
			printk(KERN_ERR "%s,init rdma queue [%d] failed.\n", __func__, i);
		}

		// Create device PD, QP CP
		ret = rswap_create_rdma_queue(rdma_session, i);
		if (unlikely(ret)) {
			printk(KERN_ERR "%s, Create rdma queues failed. \n", __func__);
		}

		// Connect to memory server
		ret = rswap_connect_remote_memory_server(rdma_session, i);
		if (unlikely(ret)) {
			printk(KERN_ERR "%s: Connect to remote server error \n", __func__);
			goto err;
		}

		printk(KERN_INFO "%s, RDMA queue[%d] Connect to remote server successfully \n", __func__, i);
	}

	//
	// 2) Get the memory pool from memory server.
	//

	// 2.1 Send a request to query available memory for this rdma_session
	//     All the QPs within one session/memory server share the same avaialble buffer
	ret = rswap_query_available_memory(rdma_session);
	if (unlikely(ret)) {
		printk("%s, request for chunk failed.\n", __func__);
		goto err;
	}

	// 2.2  Request free memory from Memory Server
	// 1st Region is the Meta Region,
	// Next are serveral Data Regions.
	ret = rswap_request_for_chunk(rdma_session, rdma_session->remote_chunk_list.chunk_num);
	if (unlikely(ret)) {
		printk("%s, request for chunk failed.\n", __func__);
		goto err;
	}

	// FINISHED.

	// [!!] Only reach here afeter got STOP_ACK signal from remote memory server.
	// Sequence controll - FINISH.
	// Be carefull, this will lead to all the local variables collected.

	// [?] Can we wait here with function : rswap_disconnect_and_collect_resource together?
	//  NO ! https://stackoverflow.com/questions/16163932/multiple-threads-can-wait-on-a-semaphore-at-same-time
	// Use  differeent semapore signal.
	//wait_event_interruptible(rdma_session->sem, rdma_session->state == CM_DISCONNECT);

	//wait_event_interruptible( rdma_session->sem, rdma_session->state == TEST_DONE );

	printk("%s,Exit the main() function with built RDMA conenction rdma_session_context:0x%llx .\n",
	       __func__,
	       (uint64_t)rdma_session);

	return ret;

err:
	printk(KERN_ERR "ERROR in %s \n", __func__);
	return ret;
}

/**
 * >>>>>>>>>>>>>>> Start of Resource Free Functions >>>>>>>>>>>>>>>
 *
 * For kernel space, there is no concept of multi-processes.
 * There is only multilple kernel threads which share the same kernel virtual memory address.
 * So it's the thread's resposibility to free the allocated memory in the kernel heap.
 *
 *
 *
 */

/**
 * Free the RDMA buffers.
 *
 * 	1) 2-sided DMA buffer
 * 	2) mapped remote chunks.
 *
 */
void rswap_free_buffers(struct rdma_session_context *rdma_session)
{
	// Free the DMA buffer for 2-sided RDMA messages
	if (rdma_session == NULL)
		return;

	// Free 1-sided dma buffers
	if (rdma_session->rdma_recv_req.recv_buf != NULL)
		kfree(rdma_session->rdma_recv_req.recv_buf);
	if (rdma_session->rdma_send_req.send_buf != NULL)
		kfree(rdma_session->rdma_send_req.send_buf);

	// Free the remote chunk management,
	if (rdma_session->remote_chunk_list.remote_chunk != NULL)
		kfree(rdma_session->remote_chunk_list.remote_chunk);

	print_debug("%s, Free RDMA buffers done. \n", __func__);
}

/**
 * Free InfiniBand related structures.
 *
 * rdma_cm_id : the main structure to maintain the IB.
 *
 *
 *
 */
void rswap_free_rdma_structure(struct rdma_session_context *rdma_session)
{
	struct rswap_rdma_queue *rdma_queue;
	int i;

	if (rdma_session == NULL)
		return;

	// Free each QP
	for (i = 0; i < num_queues; i++) {
		rdma_queue = &(rdma_session->rdma_queues[i]);
		if (rdma_queue->cm_id != NULL) {
			rdma_destroy_id(rdma_queue->cm_id);
			print_debug("%s, free rdma_queue[%d] rdma_cm_id done. \n", __func__, i);
		}

		if (rdma_queue->qp != NULL) {
			ib_destroy_qp(rdma_queue->qp);
			print_debug("%s, free rdma_queue[%d] ib_qp  done. \n", __func__, i);
		}

		//
		// Both send_cq/recb_cq should be freed in ib_destroy_qp() ?
		//
		if (rdma_queue->cq != NULL) {
			ib_destroy_cq(rdma_queue->cq);
			print_debug("%s, free rdma_queue[%d] ib_cq  done. \n", __func__, i);
		}

	} // end of free RDMA QP

	// Before invoke this function, free all the resource binded to pd.
	if (rdma_session->rdma_dev->pd != NULL) {
		ib_dealloc_pd(rdma_session->rdma_dev->pd);
		print_debug("%s, Free device PD  done. \n", __func__);
	}
	print_debug("%s, Free RDMA structures,cm_id,qp,cq,pd done. \n", __func__);
}

/**
 * The main entry of resource free.
 *
 * [x] 2 call site.
 * 		1) Called by client, at the end of function, octopus_rdma_client_cleanup_module().
 * 		2) Triggered by DISCONNECT CM event, in rswap_rdma_cm_event_handler()
 *
 */
int rswap_disconnect_and_collect_resource(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	int i;
	struct rswap_rdma_queue *rdma_queue;

	// 1) Disconnect each QP
	for (i = 0; i < num_queues; i++) {
		rdma_queue = &(rdma_session->rdma_queues[i]);

		if (unlikely(rdma_queue->freed != 0)) {
			// already called by some thread,
			// just return and wait.
			printk(KERN_WARNING "%s, rdma_queue[%d] already freed. \n", __func__, i);
			continue;
		}
		rdma_queue->freed++;

		// The RDMA connection maybe already disconnected.
		if (rdma_queue->state != CM_DISCONNECT) {
			ret = rdma_disconnect(rdma_queue->cm_id);
			if (ret) {
				printk(KERN_ERR "%s, RDMA disconnect failed. \n", __func__);
				goto err;
			}

			// wait the ack of RDMA disconnected successfully
			wait_event_interruptible(rdma_queue->sem, rdma_queue->state == CM_DISCONNECT);
		}
		print_debug("%s, RDMA queue[%d] disconnected, start to free resoutce. \n", __func__, i);
	}

	// 2) Free resouces
	rswap_free_buffers(rdma_session);
	rswap_free_rdma_structure(rdma_session);

	// If not allocated by kzalloc, no need to free it.
	//kfree(rdma_session);  // Free the RDMA context.

	// DEBUG --
	// Exit the main function first.
	// wake_up_interruptible will cause context switch,
	// Just skip the code below this invocation.
	//rdma_session_global.state = TEST_DONE;
	//wake_up_interruptible(&(rdma_session_global.sem));

	print_debug("%s, RDMA memory resouce freed. \n", __func__);
err:
	return ret;
}

/**
 * <<<<<<<<<<<<<<<<<<<<< End of  Resource Free Functions <<<<<<<<<<<<<<<<<<<<<
 */

//
// Kernel Module registration functions.
//

// invoked by insmod
int rswap_rdma_client_init(void)
{
	int ret = 0;
	printk(KERN_INFO "%s, start \n", __func__);

	// online cores decide the parallelism. e.g. number of QP, CP etc.
	online_cores = num_online_cpus();
	num_queues = online_cores * NUM_QP_TYPE;
	printk(KERN_INFO "%s, num_queues : %d (Can't exceed the slots on Memory server) \n", __func__, num_queues);

	// init the rdma session to memory server
	ret = init_rdma_sessions(&rdma_session_global);

	// Build both the RDMA and Disk driver
	ret = rdma_session_connect(&rdma_session_global);
	if (unlikely(ret)) {
		printk(KERN_ERR "%s, rdma_session_connect failed. \n", __func__);
		goto out;
	}

	// Enable the frontswap path
	ret = rswap_register_frontswap();
	if (unlikely(ret)) {
		printk(KERN_ERR "%s, Enable frontswap path failed. \n", __func__);
		goto out;
	}

out:
	return ret;
}

// invoked by rmmod
void rswap_rdma_client_exit(void)
{
	int ret = 0;

	ret = rswap_disconnect_and_collect_resource(&rdma_session_global);
	if (unlikely(ret)) {
		printk(KERN_ERR "%s,  failed.\n", __func__);
	}

	rswap_deregister_frontswap();
	printk(KERN_INFO "%s done.\n", __func__);

	return;
}

/**
 * >>>>>>>>>>>>>>> Start of Debug functions >>>>>>>>>>>>>>>
 *
 */

//
// Print the RDMA Communication Message
//
char *rdma_cm_message_print(int cm_message_id)
{
	char *type2str[17] = {
		"RDMA_CM_EVENT_ADDR_RESOLVED",
		"RDMA_CM_EVENT_ADDR_ERROR",
		"RDMA_CM_EVENT_ROUTE_RESOLVED",
		"RDMA_CM_EVENT_ROUTE_ERROR",
		"RDMA_CM_EVENT_CONNECT_REQUEST",
		"RDMA_CM_EVENT_CONNECT_RESPONSE",
		"RDMA_CM_EVENT_CONNECT_ERROR",
		"RDMA_CM_EVENT_UNREACHABLE",
		"RDMA_CM_EVENT_REJECTED",
		"RDMA_CM_EVENT_ESTABLISHED",
		"RDMA_CM_EVENT_DISCONNECTED",
		"RDMA_CM_EVENT_DEVICE_REMOVAL",
		"RDMA_CM_EVENT_MULTICAST_JOIN",
		"RDMA_CM_EVENT_MULTICAST_ERROR",
		"RDMA_CM_EVENT_ADDR_CHANGE",
		"RDMA_CM_EVENT_TIMEWAIT_EXIT",
		"ERROR Message Type"
	};

	char *message_type_name = (char *)kzalloc(32, GFP_KERNEL); // 32 bytes
	strcpy(message_type_name, type2str[cm_message_id < 16 ? cm_message_id : 16]);
	return message_type_name;
}

/**
 * wc.status name
 *
 */
char *rdma_wc_status_name(int wc_status_id)
{
	char *type2str[23] = {
		"IB_WC_SUCCESS",
		"IB_WC_LOC_LEN_ERR",
		"IB_WC_LOC_QP_OP_ERR",
		"IB_WC_LOC_EEC_OP_ERR",
		"IB_WC_LOC_PROT_ERR",
		"IB_WC_WR_FLUSH_ERR",
		"IB_WC_MW_BIND_ERR",
		"IB_WC_BAD_RESP_ERR",
		"IB_WC_LOC_ACCESS_ERR",
		"IB_WC_REM_INV_REQ_ERR",
		"IB_WC_REM_ACCESS_ERR",
		"IB_WC_REM_OP_ERR",
		"IB_WC_RETRY_EXC_ERR",
		"IB_WC_RNR_RETRY_EXC_ERR",
		"IB_WC_LOC_RDD_VIOL_ERR",
		"IB_WC_REM_INV_RD_REQ_ERR",
		"IB_WC_REM_ABORT_ERR",
		"IB_WC_INV_EECN_ERR",
		"IB_WC_INV_EEC_STATE_ERR",
		"IB_WC_FATAL_ERR",
		"IB_WC_RESP_TIMEOUT_ERR",
		"IB_WC_GENERAL_ERR",
		"ERROR Message Type",
	};

	char *message_type_name = (char *)kzalloc(32, GFP_KERNEL); // 32 bytes
	strcpy(message_type_name, type2str[wc_status_id < 22 ? wc_status_id : 22]);
	return message_type_name;
}

/**
 * The message type name, used for 2-sided RDMA communication.
 */
char *rdma_message_print(int message_id)
{
	char *message_type_name;
	char *type2str[12] = {
		"DONE",
		"GOT_CHUNKS",
		"GOT_SINGLE_CHUNK",
		"FREE_SIZE",
		"EVICT",
		"ACTIVITY",
		"STOP",
		"REQUEST_CHUNKS",
		"REQUEST_SINGLE_CHUNK",
		"QUERY",
		"AVAILABLE_TO_QUERY",
		"ERROR Message Type",
	};

	// message_id starts from 1
	message_id -= 1;

	message_type_name = (char *)kzalloc(32, GFP_KERNEL); // 32 bytes
	strcpy(message_type_name, type2str[message_id < 11 ? message_id : 11]);
	return message_type_name;
}

// Print the string of rdma_session_context state.
// void rdma_session_context_state_print(int id){
char *rdma_session_context_state_print(int id)
{
	char *rdma_seesion_state_name;

	char *state2str[20] = {
		"IDLE",
		"CONNECT_REQUEST",
		"ADDR_RESOLVED",
		"ROUTE_RESOLVED",
		"CONNECTED",
		"FREE_MEM_RECV",
		"RECEIVED_CHUNKS",
		"RDMA_BUF_ADV",
		"WAIT_OPS",
		"RECV_STOP",
		"RECV_EVICT",
		"RDMA_WRITE_RUNNING",
		"RDMA_READ_RUNNING",
		"SEND_DONE",
		"RDMA_DONE",
		"RDMA_READ_ADV",
		"RDMA_WRITE_ADV",
		"CM_DISCONNECT",
		"ERROR",
		"Un-defined state."
	};

	// message id starts from 1
	id -= 1;
	rdma_seesion_state_name = (char *)kzalloc(32, GFP_KERNEL); // 32 bytes.
	strcpy(rdma_seesion_state_name, state2str[id < 19 ? id : 19]);
	return rdma_seesion_state_name;
}

/**
 * print the information of this scatterlist
 */
void print_scatterlist_info(struct scatterlist *sl_ptr, int nents)
{
	int i;

	printk(KERN_INFO "\n %s, %d entries , Start\n", __func__, nents);
	for (i = 0; i < nents; i++)
	{
		// if(sl_ptr[i] != NULL){   // for array[N], the item can't be NULL.
		printk(KERN_INFO "%s, \n page_link(struct page*) : 0x%lx \n  offset : 0x%x bytes\n  length : 0x%x bytes \n dma_addr : 0x%llx \n ",
		       __func__, sl_ptr[i].page_link, sl_ptr[i].offset, sl_ptr[i].length, sl_ptr[i].dma_address);
		//  }
	}
	printk(KERN_INFO " %s End\n\n", __func__);
}

/**
 * <<<<<<<<<<<<<<<<<<<<< End of Debug Functions <<<<<<<<<<<<<<<<<<<<<
 */
