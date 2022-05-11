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

#ifndef __VIRTIO_RDMA_H__
#define __VIRTIO_RDMA_H__

#include <linux/virtio_ring.h>
#include <infiniband/driver.h>
#include <infiniband/kern-abi.h>
#include <rdma/virtio_rdma_abi.h>

#include "virtio.h"

struct virtio_rdma_device {
    struct verbs_device ibv_dev;
};

struct virtio_rdma_context {
    struct verbs_context ibv_ctx;
};

struct virtio_rdma_pd {
    struct ibv_pd ibv_pd;
    __u32 pdn;
};

struct virtio_rdma_cq {
    struct verbs_cq ibv_cq;

    pthread_spinlock_t lock;
    struct virtio_rdma_vring vring;

    uint32_t num_cqe;
};

struct virtio_rdma_qp {
    struct verbs_qp ibv_qp;

    pthread_spinlock_t slock;
    pthread_spinlock_t rlock;
    struct virtio_rdma_vring sq;
    struct virtio_rdma_vring rq;

    uint32_t num_sqe;
    uint32_t num_rqe;
    uint32_t num_sq_sge;
    uint32_t num_rq_sge;

    uint32_t qpn;
};

struct virtio_rdma_ah {
    struct ibv_ah ibv_ah;
    uint16_t ah_num;
};

inline struct virtio_rdma_device *to_vdev(struct ibv_device *ibv_dev) {
    return container_of(ibv_dev, struct virtio_rdma_device, ibv_dev.device);
}

inline struct virtio_rdma_context *to_vctx(struct ibv_context *ibv_ctx) {
    return container_of(ibv_ctx, struct virtio_rdma_context, ibv_ctx.context);
}

inline struct virtio_rdma_pd *to_vpd(struct ibv_pd *ibv_pd) {
    return container_of(ibv_pd, struct virtio_rdma_pd, ibv_pd);
}

inline struct virtio_rdma_cq *to_vcq(struct ibv_cq *ibv_cq) {
    return container_of(ibv_cq, struct virtio_rdma_cq, ibv_cq.cq);
}

inline struct virtio_rdma_qp *to_vqp(struct ibv_qp *ibv_qp) {
    return container_of(ibv_qp, struct virtio_rdma_qp, ibv_qp.qp);
}

inline struct virtio_rdma_ah *to_vah(struct ibv_ah *ibv_ah) {
    return container_of(ibv_ah, struct virtio_rdma_ah, ibv_ah);
}

#endif
