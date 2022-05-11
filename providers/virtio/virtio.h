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

#ifndef __VIRTIO_H__
#define __VIRTIO_H__

#include <stdbool.h>
#include <endian.h>

#include <linux/virtio_ring.h>

struct virtio_rdma_buf_pool_entry {
    struct virtio_rdma_buf_pool_entry *next;

    struct vring_desc* desc;
    void* buf;
    __u32 len;

    __u16 index; // index in pool_table
};

struct virtio_rdma_vring {
    struct vring ring;
    uint16_t index;
    volatile void* doorbell;
    uint16_t next_avail;
    uint16_t last_used_idx;

    void* buf; // mmap's buf
    __u64 buf_size; // mmap's total size

    void* kbuf; // physical buffer
    __u64 kbuf_addr; // physical address of kbuf
    __u64 kbuf_len; // physical buffer length

    struct virtio_rdma_buf_pool_entry *pool_table;
    struct virtio_rdma_buf_pool_entry free_head; // dummy head of free list
};

static inline void vring_flist_push(struct virtio_rdma_vring* ring,
                            struct virtio_rdma_buf_pool_entry* entry) {
    entry->next = ring->free_head.next;
    ring->free_head.next = entry;
}

static inline struct virtio_rdma_buf_pool_entry* vring_flist_pop(
    struct virtio_rdma_vring* ring) {
    struct virtio_rdma_buf_pool_entry* entry;
    if (ring->free_head.next == NULL)
        return NULL;

    entry = ring->free_head.next;
    ring->free_head.next = entry->next;
    return entry;
}

#define cpu_to_virtio16 htole16
#define cpu_to_virtio32 htole32
#define cpu_to_virtio64 htole64

#define virtio16_to_cpu le16toh
#define virtio32_to_cpu le32toh
#define virtio64_to_cpu le64toh

void vring_notify(struct virtio_rdma_vring *vring);
int vring_init_pool(struct virtio_rdma_vring *vring, __u32 num, __u32 len,
                    bool device_writable);
void vring_add_one(struct virtio_rdma_vring *vring,
                   struct virtio_rdma_buf_pool_entry* entry, __u32 len);
struct virtio_rdma_buf_pool_entry* vring_get_one(
        struct virtio_rdma_vring *vring);

static __inline__ void vring_init_by_off(struct vring *vr, unsigned int num,
            void *p, uint64_t used_off)
{
	vr->num = num;
	vr->desc = p;
	vr->avail = p + num * sizeof(struct vring_desc);
	vr->used = p + used_off;
}
#endif
