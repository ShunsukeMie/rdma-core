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

#include <config.h>

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <errno.h>
#include <stddef.h>

#include <linux/virtio_ring.h>

#include <infiniband/driver.h>
#include <infiniband/verbs.h>

#include "virtio_rdma.h"
#include "virtio_rdma_abi.h"
#include "virtio.h"

static void virtio_rdma_free_context(struct ibv_context *ibctx);

static const struct verbs_match_ent hca_table[] = {
	VERBS_DRIVER_ID(RDMA_DRIVER_VIRTIO),
	VERBS_NAME_MATCH("virtio_rdma", NULL),
	{},
};

static int virtio_rdma_query_device(struct ibv_context *context,
			    const struct ibv_query_device_ex_input *input,
			    struct ibv_device_attr_ex *attr, size_t attr_size)
{
	struct ib_uverbs_ex_query_device_resp resp;
	size_t resp_size = sizeof(resp);
	uint64_t raw_fw_ver;
	unsigned int major, minor, sub_minor;
	int ret;

	ret = ibv_cmd_query_device_any(context, input, attr, attr_size, &resp,
				       &resp_size);
	if (ret)
		return ret;

	raw_fw_ver = resp.base.fw_ver;
	major = (raw_fw_ver >> 32) & 0xffff;
	minor = (raw_fw_ver >> 16) & 0xffff;
	sub_minor = raw_fw_ver & 0xffff;

	snprintf(attr->orig_attr.fw_ver, sizeof(attr->orig_attr.fw_ver),
		 "%d.%d.%d", major, minor, sub_minor);

	return 0;
}

static int virtio_rdma_query_port(struct ibv_context *context, uint8_t port,
				  struct ibv_port_attr *attr)
{
	struct ibv_query_port cmd;

	return ibv_cmd_query_port(context, port, attr, &cmd, sizeof(cmd));
}

static struct ibv_pd *virtio_rdma_alloc_pd(struct ibv_context *context)
{
	struct ibv_alloc_pd cmd;
	struct uvirtio_rdma_alloc_pd_resp resp;
	struct virtio_rdma_pd *pd;

	pd = malloc(sizeof(*pd));
	if (!pd)
		return NULL;

	if (ibv_cmd_alloc_pd(context, &pd->ibv_pd, &cmd, sizeof(cmd),
					&resp.ibv_resp, sizeof(resp))) {
		free(pd);
		return NULL;
	}

	pd->pdn = resp.pdn;
	printf("alloc pdn %u", pd->pdn);
	return &pd->ibv_pd;
}

static int virtio_rdma_dealloc_pd(struct ibv_pd *pd)
{
	int ret;

	ret = ibv_cmd_dealloc_pd(pd);
	if (!ret)
		free(pd);

	return ret;
}

static struct ibv_mr *virtio_rdma_reg_mr(struct ibv_pd *pd, void *addr,
					 size_t length, uint64_t hca_va,
					 int access)
{
	struct verbs_mr *vmr;
	struct ibv_reg_mr cmd;
	struct ib_uverbs_reg_mr_resp resp;
	int ret;

	vmr = malloc(sizeof(*vmr));
	if (!vmr)
		return NULL;

	ret = ibv_cmd_reg_mr(pd, addr, length, hca_va, access, vmr, &cmd,
			     sizeof(cmd), &resp, sizeof(resp));
	if (ret) {
		free(vmr);
		return NULL;
	}

	return &vmr->ibv_mr;
}

static int virtio_rdma_dereg_mr(struct verbs_mr *vmr)
{
	int ret;

	ret = ibv_cmd_dereg_mr(vmr);
	if (ret)
		return ret;

	free(vmr);
	return 0;
}

static struct ibv_cq *virtio_rdma_create_cq(struct ibv_context *ctx, int num_cqe,
					    struct ibv_comp_channel *channel,
					    int comp_vector)
{
	struct virtio_rdma_cq *cq;
	struct uvirtio_rdma_create_cq_resp resp;
	struct virtio_rdma_buf_pool_entry *buf_entry;
	int rc, i;

	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return NULL;

	rc = ibv_cmd_create_cq(ctx, num_cqe, channel, comp_vector, &cq->ibv_cq.cq,
			       NULL, 0, &resp.ibv_resp, sizeof(resp));
	if (rc) {
		printf("cq creation failed: %d\n", rc);
		free(cq);
		return NULL;
	}

	pthread_spin_init(&cq->lock, PTHREAD_PROCESS_PRIVATE);
	cq->num_cqe = resp.num_cqe;

	cq->vring.buf = mmap(NULL, resp.cq_size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, ctx->cmd_fd, resp.offset);
	if (cq->vring.buf == MAP_FAILED) {
		printf("CQ mapping failed: %d", errno);
		goto fail;
	}

	cq->vring.buf_size = resp.cq_size;
	cq->vring.kbuf = cq->vring.buf + resp.vq_size;
	cq->vring.kbuf_addr = resp.cq_phys_addr;
	cq->vring.kbuf_len = resp.cq_size - resp.vq_size;

	vring_init_by_off(&cq->vring.ring, resp.num_cvqe, cq->vring.buf, resp.used_off);
	if (vring_init_pool(&cq->vring, cq->num_cqe,
			    sizeof(struct virtio_rdma_cq_req), true)) {
		munmap(cq->vring.buf, cq->vring.buf_size);
		goto fail;
	}

	for (i = 0; i < cq->num_cqe; i++) {
		buf_entry = vring_flist_pop(&cq->vring);
		vring_add_one(&cq->vring, buf_entry, sizeof(struct virtio_rdma_cq_req));
	}

	printf("num_cqe %u %u\n", cq->num_cqe, resp.num_cvqe);

	return &cq->ibv_cq.cq;

fail:
	ibv_cmd_destroy_cq(&cq->ibv_cq.cq);
	free(cq);

	return NULL;
}

static inline uint8_t to_ib_status(uint8_t status)
{
	switch (status) {
	case VIRTIO_IB_WC_SUCCESS:
		return IBV_WC_SUCCESS;
	case VIRTIO_IB_WC_LOC_LEN_ERR:
		return IBV_WC_LOC_LEN_ERR;
	case VIRTIO_IB_WC_LOC_QP_OP_ERR:
		return IBV_WC_LOC_QP_OP_ERR;
	case VIRTIO_IB_WC_LOC_PROT_ERR:
		return IBV_WC_LOC_PROT_ERR;
	case VIRTIO_IB_WC_WR_FLUSH_ERR:
		return IBV_WC_WR_FLUSH_ERR;
	case VIRTIO_IB_WC_BAD_RESP_ERR:
		return IBV_WC_BAD_RESP_ERR;
	case VIRTIO_IB_WC_LOC_ACCESS_ERR:
		return IBV_WC_LOC_ACCESS_ERR;
	case VIRTIO_IB_WC_REM_INV_REQ_ERR:
		return IBV_WC_REM_INV_REQ_ERR;
	case VIRTIO_IB_WC_REM_ACCESS_ERR:
		return IBV_WC_REM_ACCESS_ERR;
	case VIRTIO_IB_WC_REM_OP_ERR:
		return IBV_WC_REM_OP_ERR;
	case VIRTIO_IB_WC_RETRY_EXC_ERR:
		return IBV_WC_RETRY_EXC_ERR;
	case VIRTIO_IB_WC_RNR_RETRY_EXC_ERR:
		return IBV_WC_RNR_RETRY_EXC_ERR;
	case VIRTIO_IB_WC_REM_ABORT_ERR:
		return IBV_WC_REM_ABORT_ERR;
	case VIRTIO_IB_WC_FATAL_ERR:
		return IBV_WC_FATAL_ERR;
	case VIRTIO_IB_WC_RESP_TIMEOUT_ERR:
		return IBV_WC_RESP_TIMEOUT_ERR;
	case VIRTIO_IB_WC_GENERAL_ERR:
		return IBV_WC_GENERAL_ERR;
	}
	return -1;
}

static inline uint8_t to_ib_wc_opcode(uint8_t opcode)
{
	switch (opcode) {
	case VIRTIO_IB_WC_SEND:
		return IBV_WC_SEND;
	case VIRTIO_IB_WC_RDMA_WRITE:
		return IBV_WC_RDMA_WRITE;
	case VIRTIO_IB_WC_RDMA_READ:
		return IBV_WC_RDMA_READ;
	case VIRTIO_IB_WC_RECV:
		return IBV_WC_RECV;
	case VIRTIO_IB_WC_RECV_RDMA_WITH_IMM:
		return IBV_WC_RECV_RDMA_WITH_IMM;
	}
	return -1;
}

static inline uint8_t to_virtio_wr_opcode(uint8_t opcode)
{
	switch (opcode) {
	case IBV_WR_RDMA_WRITE:
		return VIRTIO_IB_WR_RDMA_WRITE;
	case IBV_WR_RDMA_WRITE_WITH_IMM:
		return VIRTIO_IB_WR_RDMA_WRITE_WITH_IMM;
	case IBV_WR_SEND:
		return VIRTIO_IB_WR_SEND;
	case IBV_WR_SEND_WITH_IMM:
		return VIRTIO_IB_WR_SEND_WITH_IMM;
	case IBV_WR_RDMA_READ:
		return VIRTIO_IB_WR_RDMA_READ;
	}
	return -1;
}

static inline uint8_t to_ib_wc_flags(uint8_t flags)
{
	switch (flags) {
	case VIRTIO_IB_WC_GRH:
		return IBV_WC_GRH;
	case VIRTIO_IB_WC_WITH_IMM:
		return IBV_WC_WITH_IMM;
	}
	return -1;
}

static int virtio_rdma_poll_cq(struct ibv_cq *ibcq, int num_entries,
			       struct ibv_wc *wc)
{
	struct virtio_rdma_cq *cq = to_vcq(ibcq);
	struct virtio_rdma_buf_pool_entry* buf_entry;
	struct virtio_rdma_cq_req *req;
	int i = 0;

	pthread_spin_lock(&cq->lock);
	while (i < num_entries) {
		buf_entry = vring_get_one(&cq->vring);
		if (!buf_entry)
			break;

		req = buf_entry->buf;
		wc[i].wr_id = req->wr_id;
		wc[i].status = to_ib_status(req->status);
		wc[i].opcode = to_ib_wc_opcode(req->opcode);
		wc[i].vendor_err = req->vendor_err;
		wc[i].byte_len = req->byte_len;
		// TODO: wc[i].qp_num
		wc[i].imm_data = req->imm_data;
		wc[i].src_qp = req->src_qp;
		wc[i].wc_flags = to_ib_wc_flags(req->wc_flags);
		wc[i].pkey_index = 0;
		vring_add_one(&cq->vring, buf_entry, buf_entry->len);
		i++;
	}
	pthread_spin_unlock(&cq->lock);
	return i;
}

static int virtio_rdma_destroy_cq(struct ibv_cq *ibcq)
{
	struct virtio_rdma_cq *cq = to_vcq(ibcq);
	int rc;

	rc = ibv_cmd_destroy_cq(ibcq);
	if (rc)
		return rc;

	if (cq->vring.buf)
		munmap(cq->vring.buf, cq->vring.buf_size);
	free(cq->vring.pool_table);
	free(cq);
	return 0;
}

static struct ibv_qp *virtio_rdma_create_qp(struct ibv_pd *pd,
					    struct ibv_qp_init_attr *attr)
{
	struct virtio_rdma_qp *qp;
	struct uvirtio_rdma_create_qp_resp resp;
	int rc;
	__u32 notifier_size;

	qp = calloc(1, sizeof(*qp));
	if (!qp)
		return NULL;

	printf("qp size: %d %d\n", attr->cap.max_send_wr,
		attr->cap.max_recv_wr);
	rc = ibv_cmd_create_qp(pd, &qp->ibv_qp.qp, attr,
			       NULL, 0, &resp.ibv_resp, sizeof(resp));
	if (rc) {
		printf("qp creation failed: %d\n", rc);
		free(qp);
		return NULL;
	}

	notifier_size = resp.notifier_size;

	pthread_spin_init(&qp->slock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_init(&qp->rlock, PTHREAD_PROCESS_PRIVATE);
	qp->num_sqe = resp.num_sqe;
	qp->num_rqe = resp.num_rqe;
	qp->num_sq_sge = attr->cap.max_send_sge;
	qp->num_rq_sge = attr->cap.max_recv_sge;
	qp->qpn = resp.qpn;

	qp->sq.buf = mmap(NULL, resp.sq_size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, pd->context->cmd_fd, resp.sq_offset);
	if (qp->sq.buf == MAP_FAILED) {
		printf("QP mapping failed: %d\n", errno);
		goto fail;
	}

	if (notifier_size)
		qp->sq.doorbell = qp->sq.buf + resp.sq_size - notifier_size;
	else
		qp->sq.doorbell = NULL;
	qp->sq.index = resp.sq_idx;
	qp->sq.buf_size = resp.sq_size;
	qp->sq.kbuf = qp->sq.buf + resp.svq_size;
	qp->sq.kbuf_addr = resp.sq_phys_addr;
	qp->sq.kbuf_len = resp.sq_size - notifier_size - resp.svq_size;
	vring_init_by_off(&qp->sq.ring, resp.num_svqe, qp->sq.buf, resp.svq_used_off);
	if (vring_init_pool(&qp->sq, qp->num_sqe,
		sizeof(struct virtio_rdma_sq_req) + qp->num_sq_sge *
		sizeof(struct virtio_rdma_sge), false))
		goto fail_sq;

	qp->rq.buf = mmap(NULL, resp.rq_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, pd->context->cmd_fd, resp.rq_offset);
	if (qp->rq.buf == MAP_FAILED) {
		printf("QP mapping failed: %d\n", errno);
		goto fail_sq;
	}

	if (notifier_size)
		qp->rq.doorbell = qp->rq.buf + resp.rq_size - notifier_size;
	else
		qp->rq.doorbell = NULL;
	qp->rq.index = resp.rq_idx;
	qp->rq.buf_size = resp.rq_size;
	qp->rq.kbuf = qp->rq.buf + resp.rvq_size;
	qp->rq.kbuf_addr = resp.rq_phys_addr;
	qp->rq.kbuf_len = resp.rq_size - notifier_size - resp.rvq_size;
	vring_init_by_off(&qp->rq.ring, resp.num_rvqe, qp->rq.buf, resp.rvq_used_off);
	if (vring_init_pool(&qp->rq, qp->num_rqe,
		sizeof(struct virtio_rdma_rq_req) + qp->num_rq_sge *
		sizeof(struct virtio_rdma_sge), false))
		goto fail_rq;

	return &qp->ibv_qp.qp;

fail_rq:
	munmap(qp->rq.buf, qp->rq.buf_size);
fail_sq:
	munmap(qp->sq.buf, qp->sq.buf_size);
fail:
	ibv_cmd_destroy_qp(&qp->ibv_qp.qp);
	free(qp);

	return NULL;
}

static int virtio_rdma_query_qp(struct ibv_qp *ibqp, struct ibv_qp_attr *attr,
				int attr_mask,
				struct ibv_qp_init_attr *init_attr)
{
	struct ibv_query_qp cmd = {};

	return ibv_cmd_query_qp(ibqp, attr, attr_mask, init_attr,
				&cmd, sizeof(cmd));
}

static int virtio_rdma_modify_qp(struct ibv_qp *ibqp, struct ibv_qp_attr *attr,
				 int attr_mask)
{
	struct ibv_modify_qp cmd = {};

	return ibv_cmd_modify_qp(ibqp, attr, attr_mask, &cmd, sizeof(cmd));
}

static int virtio_rdma_destroy_qp(struct ibv_qp *ibqp)
{
	struct virtio_rdma_qp *qp = to_vqp(ibqp);
	int rc;

	rc = ibv_cmd_destroy_qp(ibqp);
	if (rc)
		return rc;

	if (qp->sq.buf)
		munmap(qp->sq.buf, qp->sq.buf_size);
	if (qp->rq.buf)
		munmap(qp->rq.buf, qp->rq.buf_size);
	free(qp->sq.pool_table);
	free(qp->rq.pool_table);
	free(qp);
	return 0;
}

/* send a null WQE as a doorbell */
static int slow_doorbell(struct ibv_qp *ibqp, bool send)
{
	struct ibv_post_send cmd;
	struct ib_uverbs_post_send_resp resp;

	cmd.hdr.command	= send ? IB_USER_VERBS_CMD_POST_SEND :
			  IB_USER_VERBS_CMD_POST_RECV;
	cmd.hdr.in_words = sizeof(cmd) / 4;
	cmd.hdr.out_words = sizeof(resp) / 4;
	cmd.response	= (uintptr_t)&resp;
	cmd.qp_handle	= ibqp->handle;
	cmd.wr_count	= 0;
	cmd.sge_count	= 0;
	cmd.wqe_size	= sizeof(struct ibv_send_wr);

	if (write(ibqp->context->cmd_fd, &cmd, sizeof(cmd)) != sizeof(cmd))
		return errno;

	return 0;
}

static inline uint8_t to_virtio_send_flags(uint8_t flags)
{
	switch (flags) {
	case IBV_SEND_FENCE:
		return VIRTIO_IB_SEND_FENCE;
	case IBV_SEND_SIGNALED:
		return VIRTIO_IB_SEND_SIGNALED;
	case IBV_SEND_SOLICITED:
		return VIRTIO_IB_SEND_SOLICITED;
	case IBV_SEND_INLINE:
		return VIRTIO_IB_SEND_INLINE;
	}
	return -1;
}

static void copy_inline_data_to_wqe(struct virtio_rdma_sq_req *req,
                                    const struct ibv_send_wr *ibwr)
{
	struct ibv_sge *sge = ibwr->sg_list;
	char *p = (char *)req->inline_data;
	int i;

	for (i = 0; i < ibwr->num_sge; i++, sge++) {
		memcpy(p, (void *)(uintptr_t)sge->addr, sge->length);
		p += sge->length;
		req->inline_len += sge->length;
	}
}


static int virtio_rdma_post_send(struct ibv_qp *ibqp, struct ibv_send_wr *wr,
				 struct ibv_send_wr **bad_wr)
{
	struct virtio_rdma_qp *qp = to_vqp(ibqp);
	struct virtio_rdma_buf_pool_entry *buf_entry;
	struct virtio_rdma_sq_req *req;
	struct virtio_rdma_sge *sgl;
	uint32_t sgl_len;
	int rc = 0;

	pthread_spin_lock(&qp->slock);
	while (wr) {
		while ((buf_entry = vring_get_one(&qp->sq)) != NULL) {
			vring_flist_push(&qp->sq, buf_entry);
		}

		// TODO: more check
		buf_entry = vring_flist_pop(&qp->sq);
		if (!buf_entry) {
			rc = -ENOMEM;
			printf("error\n");
			goto out_err;
		}

		req = buf_entry->buf;
		sgl = req->sg_list;
		sgl_len = sizeof(*sgl) * wr->num_sge;

		req->num_sge = wr->num_sge;
		req->send_flags = to_virtio_send_flags(wr->send_flags);
		req->opcode = to_virtio_wr_opcode(wr->opcode);
		req->wr_id = wr->wr_id;
		req->imm_data = wr->imm_data;

		switch (ibqp->qp_type) {
		case IBV_QPT_UD:
			req->ud.remote_qpn = wr->wr.ud.remote_qpn;
			req->ud.remote_qkey = wr->wr.ud.remote_qkey;
			req->ud.ah = to_vah(wr->wr.ud.ah)->ah_num;
			break;
		case IBV_QPT_RC:
			switch (wr->opcode) {
			case IBV_WR_RDMA_READ:
			case IBV_WR_RDMA_WRITE:
			case IBV_WR_RDMA_WRITE_WITH_IMM:
				req->rdma.remote_addr = wr->wr.rdma.remote_addr;
				req->rdma.rkey = wr->wr.rdma.rkey;
				break;
			case IBV_WR_SEND:
			case IBV_WR_SEND_WITH_IMM:
				break;
			default:
				rc = -EOPNOTSUPP;
				goto out_err;
			}
			break;
		default:
			rc = -EINVAL;
			goto out_err;
		}
                // TODO: check max_inline_data
		if (unlikely(wr->send_flags & IBV_SEND_INLINE)) {
			req->inline_len = 0;
			sgl_len = 0;
			copy_inline_data_to_wqe(req, wr);
		} else
			memcpy(sgl, wr->sg_list, sgl_len);

		vring_add_one(&qp->sq, buf_entry, sizeof(*req) + sgl_len);

		wr = wr->next;
	}
	if (qp->sq.doorbell)
		vring_notify(&qp->sq);
	else
		slow_doorbell(ibqp, true);

out_err:
	*bad_wr = wr;
	pthread_spin_unlock(&qp->slock);
	return rc;
}

static int virtio_rdma_post_recv(struct ibv_qp *ibqp, struct ibv_recv_wr *wr,
				 struct ibv_recv_wr **bad_wr)
{
	struct virtio_rdma_qp *qp = to_vqp(ibqp);
	struct virtio_rdma_buf_pool_entry *buf_entry;
	struct virtio_rdma_rq_req *req;
	struct virtio_rdma_sge *sgl;
	uint32_t sgl_len;
	int rc = 0;

	pthread_spin_lock(&qp->rlock);
	while (wr) {
		while ((buf_entry = vring_get_one(&qp->rq)) != NULL) {
			vring_flist_push(&qp->rq, buf_entry);
		}

		// TODO: more check
		buf_entry = vring_flist_pop(&qp->rq);
		if (!buf_entry) {
			*bad_wr = wr;
			rc = -ENOMEM;
			printf("error\n");
			goto out;
		}

		req = buf_entry->buf;
		sgl = req->sg_list;
		sgl_len = sizeof(*sgl) * wr->num_sge;

		req->num_sge = wr->num_sge;
		req->wr_id = wr->wr_id;
		memcpy(sgl, wr->sg_list, sgl_len);

		vring_add_one(&qp->rq, buf_entry, sizeof(*req) + sgl_len);

		wr = wr->next;
	}
	if (qp->rq.doorbell)
		vring_notify(&qp->rq);
	else
		slow_doorbell(ibqp, false);

out:
	pthread_spin_unlock(&qp->rlock);
	return rc;
}

static struct ibv_ah* virtio_rdma_create_ah(struct ibv_pd *pd,
				struct ibv_ah_attr *attr)
{
	struct uvirtio_rdma_create_ah_resp resp;
	struct virtio_rdma_ah *ah;
	int err;

	ah = calloc(1, sizeof(*ah));
	if (!ah)
		return NULL;

	err = ibv_cmd_create_ah(pd, &ah->ibv_ah, attr,
				&resp.ibv_resp, sizeof(resp));
	if (err) {
		free(ah);
		errno = err;
		return NULL;
	}

	ah->ah_num = resp.ah;

	return &ah->ibv_ah;
}

static int virtio_rdma_destroy_ah(struct ibv_ah *ibvah)
{
	struct virtio_rdma_ah *ah;
	int err;

	ah = to_vah(ibvah);
	err = ibv_cmd_destroy_ah(ibvah);
	if (err)
		return err;
	free(ah);

	return 0;
}

static const struct verbs_context_ops virtio_rdma_ctx_ops = {
	.query_device_ex = virtio_rdma_query_device,
	.query_port = virtio_rdma_query_port,
	.alloc_pd = virtio_rdma_alloc_pd,
	.dealloc_pd = virtio_rdma_dealloc_pd,
	.reg_mr = virtio_rdma_reg_mr,
	.dereg_mr = virtio_rdma_dereg_mr,

	.create_cq = virtio_rdma_create_cq,
	.poll_cq = virtio_rdma_poll_cq,
	.req_notify_cq = ibv_cmd_req_notify_cq,
	.destroy_cq = virtio_rdma_destroy_cq,

	.create_qp = virtio_rdma_create_qp,
	.query_qp = virtio_rdma_query_qp,
	.modify_qp = virtio_rdma_modify_qp,
	.destroy_qp = virtio_rdma_destroy_qp,

	.post_send = virtio_rdma_post_send,
	.post_recv = virtio_rdma_post_recv,
	.create_ah = virtio_rdma_create_ah,
	.destroy_ah = virtio_rdma_destroy_ah,
	.free_context = virtio_rdma_free_context,
};

static struct verbs_context *virtio_rdma_alloc_context(struct ibv_device *ibdev,
					       int cmd_fd,
					       void *private_data)
{
	struct virtio_rdma_context *context;
	struct ibv_get_context cmd;
	struct ib_uverbs_get_context_resp resp;

	context = verbs_init_and_alloc_context(ibdev, cmd_fd, context, ibv_ctx,
					       RDMA_DRIVER_VIRTIO);
	if (!context)
		return NULL;

	if (ibv_cmd_get_context(&context->ibv_ctx, &cmd, sizeof(cmd),
				&resp, sizeof(resp)))
		goto out;

	verbs_set_ops(&context->ibv_ctx, &virtio_rdma_ctx_ops);

	return &context->ibv_ctx;

out:
	verbs_uninit_context(&context->ibv_ctx);
	free(context);
	return NULL;
}

static void virtio_rdma_free_context(struct ibv_context *ibctx)
{
	struct virtio_rdma_context *context = to_vctx(ibctx);

	verbs_uninit_context(&context->ibv_ctx);
	free(context);
}

static struct verbs_device *virtio_rdma_device_alloc(struct verbs_sysfs_dev *unused)
{
	struct virtio_rdma_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	return &dev->ibv_dev;
}

static void virtio_rdma_device_free(struct verbs_device *verbs_dev) {
	struct virtio_rdma_device *vdev = to_vdev(&verbs_dev->device);
	free(vdev);
}

static const struct verbs_device_ops virtio_rdma_dev_ops = {
	.name = "virtio_rdma",
	/*
	 * For 64 bit machines ABI version 1 and 2 are the same. Otherwise 32
	 * bit machines require ABI version 2 which guarentees the user and
	 * kernel use the same ABI.
	 */
	.match_min_abi_version = VIRTIO_RDMA_ABI_VERSION,
	.match_max_abi_version = VIRTIO_RDMA_ABI_VERSION,
	.match_table = hca_table,
	.alloc_device = virtio_rdma_device_alloc,
	.uninit_device = virtio_rdma_device_free,
	.alloc_context = virtio_rdma_alloc_context,
};
PROVIDER_DRIVER(virtio_rdma, virtio_rdma_dev_ops);
