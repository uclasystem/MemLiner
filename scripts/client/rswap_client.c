#include "rswap_client.h"

MODULE_AUTHOR("Chenxi Wang");
MODULE_DESCRIPTION("RSWAP, remote memory paging over RDMA");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");

// invoked by insmod
int __init rswap_cpu_init(void)
{
	int ret = 0;
#ifdef RSWAP_FRONTSWAP_RDMA
	ret = rswap_rdma_client_init();
	if (unlikely(ret)) {
		printk(KERN_ERR "%s, rswap_rdma_client_init failed. \n", __func__);
		goto out;
	}
#else
	rswap_register_frontswap();
#endif

out:
	return ret;
}

// invoked by rmmod
void __exit rswap_cpu_exit(void)
{
	printk(" Prepare to remove the CPU Server module.\n");
#ifdef RSWAP_FRONTSWAP_RDMA
	rswap_rdma_client_exit();
#else
	printk(KERN_ERR "%s, TO BE DONE.\n", __func__);
#endif
	printk(" Remove CPU Server module DONE. \n");
	return;
}

module_init(rswap_cpu_init);
module_exit(rswap_cpu_exit);
