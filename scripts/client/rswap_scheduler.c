#include "rswap_scheduler.h"
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/swap_stats.h>

struct rswap_vqueue_list *global_rswap_vqueue_list = NULL;
struct rswap_vqueue **global_rswap_vqueue_array = NULL;
int vq_array_cnt = 0;
struct rswap_scheduler *global_rswap_scheduler = NULL;

int rswap_vqueue_init(struct rswap_vqueue *vqueue)
{
	if (!vqueue) {
		return -EINVAL;
	}

	vqueue->id = 0;
	// invariant: (head + cnt) % max_cnt == tail
	atomic_set(&vqueue->cnt, 0);
	vqueue->max_cnt = RSWAP_VQUEUE_MAX_SIZE;
	vqueue->head = 0;
	vqueue->tail = 0;

	vqueue->reqs =
	    (struct rswap_request *)vmalloc(sizeof(struct rswap_request) * vqueue->max_cnt);

	spin_lock_init(&vqueue->lock);
	INIT_LIST_HEAD(&vqueue->list_node);

	// debug
	vqueue->rid = 0;

	return 0;
}

int rswap_vqueue_enqueue(struct rswap_vqueue *vqueue, struct rswap_request *request)
{
	unsigned long flags;
	int cnt;
	if (!vqueue || !request) {
		return -EINVAL;
	}

	cnt = atomic_read(&vqueue->cnt);
	// Fast path. No contention if head != tail
	if (cnt < vqueue->max_cnt) {
		rswap_request_copy(&vqueue->reqs[vqueue->tail], request);
		vqueue->tail = (vqueue->tail + 1) % vqueue->max_cnt;
		atomic_inc(&vqueue->cnt);
		return 0;
		// Slow path. When cnt == vqueue->max_cnt
	} else {
		unsigned head_len;
		unsigned tail_len;
		struct rswap_request *old_buf;
		spin_lock_irqsave(&vqueue->lock, flags);
		if (vqueue->head < vqueue->tail) {
			head_len = vqueue->tail - vqueue->head;
			old_buf = vqueue->reqs;
			// double the new buffer size
			vqueue->reqs = (struct rswap_request *)vmalloc(
			    sizeof(struct rswap_request) * vqueue->max_cnt * 2);
			// only one part
			memcpy(vqueue->reqs, old_buf + vqueue->head,
			       sizeof(struct rswap_request) * head_len);
		} else {
			head_len = vqueue->max_cnt - vqueue->head;
			tail_len = vqueue->head;
			old_buf = vqueue->reqs;
			// double the new buffer size
			vqueue->reqs = (struct rswap_request *)vmalloc(
			    sizeof(struct rswap_request) * vqueue->max_cnt * 2);
			// head part
			memcpy(vqueue->reqs, old_buf + vqueue->head,
			       sizeof(struct rswap_request) * head_len);
			// tail part
			memcpy(vqueue->reqs + head_len, old_buf,
			       sizeof(struct rswap_request) * tail_len);
		}
		// reset head and tail
		vqueue->head = 0;
		vqueue->tail = vqueue->max_cnt;
		vqueue->max_cnt *= 2;
		vfree(old_buf);
		pr_info("Enlarge vqueue for %d, to %u\n", vqueue->id, vqueue->max_cnt);

		// cnt < max_cnt now. Same as fast path.
		rswap_request_copy(&vqueue->reqs[vqueue->tail], request);
		vqueue->tail = (vqueue->tail + 1) % vqueue->max_cnt;
		atomic_inc(&vqueue->cnt);
		spin_unlock_irqrestore(&vqueue->lock, flags);
		return 0;
	}

	return 0;
}

int rswap_vqueue_dequeue(struct rswap_vqueue *vqueue, struct rswap_request *request)
{
	unsigned long flags;
	int cnt;
	if (!vqueue || !request) {
		return -EINVAL;
	}

	cnt = atomic_read(&vqueue->cnt);
	// Fast path.
	if (cnt == 0) {
		return -1;
	} else if (cnt < vqueue->max_cnt) {
		rswap_request_copy(request, &vqueue->reqs[vqueue->head]);
		vqueue->head = (vqueue->head + 1) % vqueue->max_cnt;
		return 0;
	// Slow path. cnt == max_cnt here. Enqueue thread will enlarge the vqueue.
	} else {
		spin_lock_irqsave(&vqueue->lock, flags);
		rswap_request_copy(request, &vqueue->reqs[vqueue->head]);
		vqueue->head = (vqueue->head + 1) % vqueue->max_cnt;
		// we shouldn't dec counter here.
		// counter is decremented after pushing the request to RDMA queue
		spin_unlock_irqrestore(&vqueue->lock, flags);
		return 0;
	}

	return 0;
}

int rswap_vqueue_drain(struct rswap_vqueue *vqueue)
{
	while (atomic_read(&vqueue->cnt) > 0) {
		cpu_relax();
	}
	return 0;
}

int rswap_vqueue_list_init(void)
{
	struct rswap_vqueue_list *vqlist;

	if (!global_rswap_vqueue_list) {
		global_rswap_vqueue_list = (struct rswap_vqueue_list *)kzalloc(
		    sizeof(struct rswap_vqueue_list), GFP_ATOMIC);
		global_rswap_vqueue_array =
		    (struct rswap_vqueue **)kzalloc(sizeof(void *) * online_cores, GFP_ATOMIC);
	}
	vqlist = global_rswap_vqueue_list;
	if (!vqlist) {
		return -ENOMEM;
	}

	vqlist->cnt = 0;
	vqlist->max_id = 0;
	spin_lock_init(&vqlist->lock);
	INIT_LIST_HEAD(&vqlist->vqlist_head);

	return 0;
}

struct rswap_vqueue *rswap_vqueue_list_get(struct rswap_vqueue_list *vqlist, int qid)
{
	unsigned long flags;
	struct rswap_vqueue *pos, *tmp;

	if (!vqlist || qid < 0 || qid >= vqlist->max_id) {
		return NULL;
	}

	// fast path. [WARNING] for debug only! Remove this later!
	if (global_rswap_vqueue_array && global_rswap_vqueue_array[qid]) {
		return global_rswap_vqueue_array[qid];
	}

	spin_lock_irqsave(&vqlist->lock, flags);
	list_for_each_entry_safe(pos, tmp, &vqlist->vqlist_head, list_node)
	{
		if (pos->id == qid) {
			break;
		}
	}
	spin_unlock_irqrestore(&vqlist->lock, flags);
	if (pos->id == qid) {
		return pos;
	}
	return NULL;
}

int rswap_vqueue_list_add(struct rswap_vqueue_list *vqlist, struct rswap_vqueue *vqueue)
{
	unsigned long flags;
	if (!vqlist || !vqueue) {
		return -EINVAL;
	}

	spin_lock_irqsave(&vqlist->lock, flags);
	// add to global array. [WARNING] For debug only! Remove this later!
	global_rswap_vqueue_array[vq_array_cnt++] = vqueue;

	list_add_tail(&vqueue->list_node, &vqlist->vqlist_head);
	vqueue->id = vqlist->max_id;
	vqlist->max_id++;
	vqlist->cnt++;
	spin_unlock_irqrestore(&vqlist->lock, flags);

	return 0;
}
int rswap_vqueue_list_del(struct rswap_vqueue_list *vqlist, struct rswap_vqueue *vqueue)
{
	unsigned long flags;
	if (!vqlist || !vqueue) {
		return -EINVAL;
	}

	spin_lock_irqsave(&vqlist->lock, flags);
	list_del(&vqueue->list_node);
	vqlist->cnt--;
	spin_unlock_irqrestore(&vqlist->lock, flags);

	return 0;
}

int rswap_register_virtual_queue(struct rswap_vqueue_list *vqlist, int *qid)
{
	int ret = 0;
	struct rswap_vqueue *vqueue;
	if (!vqlist || !qid) {
		return -EINVAL;
	}

	vqueue = (struct rswap_vqueue *)kzalloc(sizeof(struct rswap_vqueue), GFP_ATOMIC);
	rswap_vqueue_init(vqueue);
	if ((ret = rswap_vqueue_list_add(vqlist, vqueue)) != 0) {
		print_err(ret);
		*qid = -1;
		return ret;
	}
	*qid = vqueue->id;

	return ret;
}

int rswap_unregister_virtual_queue(struct rswap_vqueue_list *vqlist, int qid)
{
	int ret = 0;
	struct rswap_vqueue *vqueue;
	if (!vqlist || qid < 0 || qid >= vqlist->max_id) {
		return -EINVAL;
	}

	vqueue = rswap_vqueue_list_get(vqlist, qid);
	if (vqueue) {
		ret = rswap_vqueue_list_del(vqlist, vqueue);
	}
	return ret;
}

int rswap_scheduler_init(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	int fake_qid;
	const int online_cores = num_online_cpus();
	int cpu;

	pr_info("%s starts.\n", __func__);
	// init physical queues

	// init virual queues (fake)
	rswap_vqueue_list_init();
	for (cpu = 0; cpu < online_cores; cpu++) {
		if ((ret = rswap_register_virtual_queue(global_rswap_vqueue_list, &fake_qid)) !=
		    0) {
			print_err(ret);
			goto cleanup;
		}
	}

	pr_info("%s inits vqueues.\n", __func__);
	// start scheduler thread
	global_rswap_scheduler = (struct rswap_scheduler *)vmalloc(sizeof(struct rswap_scheduler));
	global_rswap_scheduler->vqlist = global_rswap_vqueue_list;
	global_rswap_scheduler->rdma_session = rdma_session;
	global_rswap_scheduler->scher_thd = kthread_create(
	    rswap_scheduler_thread, (void *)global_rswap_scheduler, "RSWAP scheduler");
	kthread_bind(global_rswap_scheduler->scher_thd, 7);
	wake_up_process(global_rswap_scheduler->scher_thd);
	pr_info("%s launches scheduler thd.\n", __func__);
	return 0;
cleanup:
	pr_info("%s, line %d error.\n", __func__, __LINE__);
	// in our simulation we must have cpu == fake_qid
	for (; cpu >= 0; cpu--) {
		int ret;
		if ((ret = rswap_unregister_virtual_queue(global_rswap_vqueue_list, cpu)) != 0) {
			print_err(ret);
		}
	}
	return ret;
}

int rswap_scheduler_stop(void)
{
	int ret = 0;
	if (!global_rswap_scheduler) {
		return -EINVAL;
	}
	ret = kthread_stop(global_rswap_scheduler->scher_thd);
	pr_info("%s, line %d, after kthread stop\n", __func__, __LINE__);
	return ret;
}

int rswap_scheduler_thread(void *args)
{
	struct rswap_vqueue_list *vqlist;
	struct rdma_session_context *rdma_session;
	const unsigned online_cores = num_online_cpus();

	vqlist = ((struct rswap_scheduler *)args)->vqlist;
	rdma_session = ((struct rswap_scheduler *)args)->rdma_session;
	while (!kthread_should_stop() && (!vqlist || !rdma_session || !vqlist->cnt)) {
		udelay(5000); // 5ms
	}

	pr_info("RSWAP scheduler starts polling...\n");
	// for fake vqueue simulation
	while (!kthread_should_stop() && vqlist->cnt < online_cores) {
		udelay(5000);
	}
	pr_info("RSWAP scheduler gets all vqueues.");

	while (!kthread_should_stop()) {
		int ret;
		struct rswap_vqueue *pos, *tmp;

		list_for_each_entry_safe(pos, tmp, &vqlist->vqlist_head, list_node)
		{
			// unsigned long flags;
			struct rswap_request vrequest;
			size_t page_addr;
			size_t chunk_idx;
			size_t offset_within_chunk;
			struct rswap_rdma_queue *pqueue;
			struct fs_rdma_req *rdma_req;
			struct remote_mapping_chunk *remote_chunk_ptr;

			if ((ret = rswap_vqueue_dequeue(pos, &vrequest)) == 0) {
				// page offset, compared start of Data Region
				// The real virtual address is RDMA_DATA_SPACE_START_ADDR +
				// page_addr.
#ifdef ENABLE_SWP_ENTRY_VIRT_REMAPPING
				page_addr =
				    retrieve_swap_remmaping_virt_addr_via_offset(vrequest.offset)
				    << PAGE_SHIFT; // calculate the remote addr
#else
				// For the default kernel, no need to do the swp_offset -> virt
				// translation
				page_addr = vrequest.offset << PAGE_SHIFT;
#endif
				chunk_idx = page_addr >> CHUNK_SHIFT;
				offset_within_chunk = page_addr & CHUNK_MASK;

				if (vrequest.op == STORE || vrequest.op == LOAD_SYNC) {
					pqueue =
					    get_rdma_queue(rdma_session, vrequest.cpu, QP_SYNC);
				} else {
					pqueue =
					    get_rdma_queue(rdma_session, vrequest.cpu, QP_ASYNC);
				}

				rdma_req = (struct fs_rdma_req *)kmem_cache_alloc(
				    pqueue->fs_rdma_req_cache, GFP_ATOMIC);
				if (!rdma_req) {
					pr_err("%s, get reserved fs_rdma_req failed. \n", __func__);
					continue;
				}

				remote_chunk_ptr =
				    &(rdma_session->remote_chunk_list.remote_chunk[chunk_idx]);

				switch (vrequest.op) {
				case STORE: {
					ret = fs_rdma_send(rdma_session, pqueue, rdma_req,
					                   remote_chunk_ptr, offset_within_chunk,
					                   vrequest.page, DMA_TO_DEVICE);
					break;
				}
				case LOAD_SYNC: {
					ret = fs_rdma_send(rdma_session, pqueue, rdma_req,
					                   remote_chunk_ptr, offset_within_chunk,
					                   vrequest.page, DMA_FROM_DEVICE);
					break;
				}
				case LOAD_ASYNC: {
					ret = fs_rdma_send(rdma_session, pqueue, rdma_req,
					                   remote_chunk_ptr, offset_within_chunk,
					                   vrequest.page, DMA_FROM_DEVICE);
					break;
				}
				default:
					pr_info("Unknown operation %d", vrequest.op);
				}

				atomic_dec(&pos->cnt);
			}
			else if (ret != -1) {
				print_err(ret);
			}

			continue;
		}

		// avoid CPU stuck
		cond_resched();
	}

	return 0;
}
