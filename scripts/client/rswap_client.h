/**
 * The main header of Semeru cpu header.
 * It contains 2 separate parts for now:
 *  2) Remote memory go through frontswap-RDMA
 * 		include files:
 * 		frontswap.h
 * 		frontswap.c
 * 		frontswap_rdma.c
 */

#ifndef __RSWAP_CLIENT_H
#define __RSWAP_CLIENT_H

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>

#define RSWAP_FRONTSWAP_RDMA 1
#include "rswap_rdma.h"

#endif // __RSWAP_CLIENT_H
