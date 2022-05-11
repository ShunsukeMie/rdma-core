/*
 * Copyright (C) 2022 Bytedance Inc. and/or its affiliates. All rights reserved.
 *
 * Authors: Wei Junji <weijunji@bytedance.com>
 *          Xie Yongji <xieyongji@bytedance.com>
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __UVIRTIO_RDMA_ABI_H__
#define __UVIRTIO_RDMA_ABI_H__

#include <infiniband/kern-abi.h>
#include <rdma/virtio_rdma_abi.h>
#include <kernel-abi/virtio_rdma_abi.h>

DECLARE_DRV_CMD(uvirtio_rdma_alloc_pd, IB_USER_VERBS_CMD_ALLOC_PD,
		empty, virtio_rdma_alloc_pd_uresp);
DECLARE_DRV_CMD(uvirtio_rdma_create_cq, IB_USER_VERBS_CMD_CREATE_CQ,
		empty, virtio_rdma_create_cq_uresp);
DECLARE_DRV_CMD(uvirtio_rdma_create_qp, IB_USER_VERBS_CMD_CREATE_QP,
		empty, virtio_rdma_create_qp_uresp);
DECLARE_DRV_CMD(uvirtio_rdma_create_ah, IB_USER_VERBS_CMD_CREATE_AH,
		empty, virtio_rdma_create_ah_uresp);
#endif
