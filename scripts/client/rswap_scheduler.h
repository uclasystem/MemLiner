#ifndef __RSWAP_SCHEDULER_H
#define __RSWAP_SCHEDULER_H

#include <linux/list.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include "rswap_rdma.h"

#define RSWAP_VQUEUE_MAX_SIZE 4096

struct rswap_request {
	pgoff_t offset;
	struct page *page;
	enum op { STORE, LOAD_SYNC, LOAD_ASYNC } op;
	int cpu; // destination physical RDMA queue
};

static inline int rswap_request_copy(struct rswap_request *dst, struct rswap_request *src)
{
	if (!src || !dst) {
		return -EINVAL;
	}
	memcpy(dst, src, sizeof(struct rswap_request));

	return 0;
}

struct rswap_vqueue {
	int id; // identifier of the queue

	atomic_t cnt;
	int max_cnt;
	unsigned head;
	unsigned tail;
	struct rswap_request *reqs;
	spinlock_t lock;

	struct list_head list_node;

	// debug
	int rid;
};

int rswap_vqueue_init(struct rswap_vqueue *queue);
int rswap_vqueue_enqueue(struct rswap_vqueue *queue, struct rswap_request *request);
int rswap_vqueue_dequeue(struct rswap_vqueue *queue, struct rswap_request *request);
int rswap_vqueue_drain(struct rswap_vqueue *queue);

struct rswap_vqueue_list {
	int cnt;
	int max_id;
	struct list_head vqlist_head; // list for rswap_vqueue
	spinlock_t lock;
};

int rswap_vqueue_list_init(void);
struct rswap_vqueue *rswap_vqueue_list_get(struct rswap_vqueue_list *list, int qid);
int rswap_vqueue_list_add(struct rswap_vqueue_list *list, struct rswap_vqueue *queue);
int rswap_vqueue_list_del(struct rswap_vqueue_list *list, struct rswap_vqueue *queue);

int rswap_register_virtual_queue(struct rswap_vqueue_list *vqlist, int *qid);
int rswap_unregister_virtual_queue(struct rswap_vqueue_list *vqlist, int qid);

struct rswap_scheduler {
	struct rswap_vqueue_list *vqlist;
	struct rdma_session_context *rdma_session;

	struct task_struct *scher_thd;
};

int rswap_scheduler_init(struct rdma_session_context *rdma_session);
int rswap_scheduler_stop(void);
int rswap_scheduler_thread(void *args); // struct rswap_scheduler *

// simulate vqueue cache for processes
extern struct rswap_vqueue **global_rswap_vqueue_array;
extern int vq_array_cnt;

extern struct rswap_vqueue_list *global_rswap_vqueue_list;
extern struct rswap_scheduler *gloabl_rswap_scheduler;

// utils
#define print_err(errno) pr_err(KERN_ERR "%s, line %d : %d\n", __func__, __LINE__, errno)

#endif /* __RSWAP_SCHEDULER_H */