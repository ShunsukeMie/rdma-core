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

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include <linux/virtio_ring.h>

#include "virtio.h"

static inline void writew(uint16_t val, volatile void *addr)
{
    __sync_synchronize();
    asm volatile(
        "movw %0,%1"
        :
        :"r" (val), "m" (*(volatile uint16_t *)addr)
    );
    __sync_synchronize();
}

void vring_notify(struct virtio_rdma_vring *vring) {
    bool needs_kick;
    needs_kick = !(vring->ring.used->flags &
					cpu_to_virtio16(VRING_USED_F_NO_NOTIFY));
    if (needs_kick)
        writew(virtio16_to_cpu(vring->index), vring->doorbell);
}

int vring_init_pool(struct virtio_rdma_vring *vring, __u32 num, __u32 len,
                    bool writable) {
    struct vring_desc *desc = vring->ring.desc;
    __u32 i;
    void* buf = vring->kbuf;
    __u64 phys = vring->kbuf_addr;

    if (!buf || !phys)
        return -EINVAL;

    assert(vring->kbuf_len >= num * len);

    vring->free_head.next = NULL;
    vring->next_avail = 0;
    vring->last_used_idx = 0;
    vring->pool_table = calloc(num, sizeof(struct virtio_rdma_buf_pool_entry));
    if (!vring->pool_table)
        return -ENOMEM;

    for (i = 0; i < num; i++, buf += len, phys += len) {
        desc[i].addr = cpu_to_virtio64(phys);
        desc[i].flags = writable ? cpu_to_virtio16(VRING_DESC_F_WRITE) : 0;

        vring->pool_table[i].desc = &desc[i];
        vring->pool_table[i].buf = buf;
        vring->pool_table[i].index = i;
        vring->pool_table[i].len = len;

        vring_flist_push(vring, &vring->pool_table[i]);
    }
    return 0;
}

// This will not fail because buf_pool is smaller than vring
void vring_add_one(struct virtio_rdma_vring *vring,
                   struct virtio_rdma_buf_pool_entry* entry, __u32 len) {
    __u16 avail_idx;

    assert(len <= entry->len);
    entry->desc->len = cpu_to_virtio32(len);

    avail_idx = vring->next_avail & (vring->ring.num - 1);
    vring->ring.avail->ring[avail_idx] = cpu_to_virtio16(entry->index);

    __sync_synchronize();
    vring->next_avail++;
    vring->ring.avail->idx = cpu_to_virtio16(vring->next_avail);
}

struct virtio_rdma_buf_pool_entry* vring_get_one(
        struct virtio_rdma_vring *vring) {
    __u16 last_used;
    __u16 idx;

    if (vring->last_used_idx == virtio16_to_cpu(vring->ring.used->idx))
        return NULL;

    __sync_synchronize();

    last_used = vring->last_used_idx & (vring->ring.num - 1);
    idx = virtio32_to_cpu(vring->ring.used->ring[last_used].id);
    if (idx >= vring->ring.num) {
        printf("Bad vring used idx\n");
        return NULL;
    }

    vring->last_used_idx++;
    return &vring->pool_table[idx];
}
