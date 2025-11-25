// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2021 Huawei Technologies Co., Ltd */

#include "hinic3_nic_cmdq.h"
#include "hw_cmdq_ops.h"
#include "hinic3_nic_io.h"

static void hinic3_hw_rq_prepare_ctxt(struct hinic3_io_queue *rq,
				      struct hinic3_rq_ctxt *rq_ctxt)
{
	u32 wq_page_pfn_hi, wq_page_pfn_lo;
	u32 wq_block_pfn_hi, wq_block_pfn_lo;
	u16 pi_start, ci_start;
	u16 wqe_type = rq->wqe_type;

	/* RQ depth is in unit of 8Bytes */
	ci_start = (u16)((u32)hinic3_get_rq_local_ci(rq) << wqe_type);
	pi_start = (u16)((u32)hinic3_get_rq_local_pi(rq) << wqe_type);

	hinic3_rq_prepare_ctxt_get_wq_info(rq, &wq_page_pfn_hi, &wq_page_pfn_lo,
					   &wq_block_pfn_hi, &wq_block_pfn_lo);

	rq_ctxt->ci_pi =
		RQ_CTXT_CI_PI_SET(ci_start, CI_IDX) |
		RQ_CTXT_CI_PI_SET(pi_start, PI_IDX);

	rq_ctxt->ceq_attr = RQ_CTXT_CEQ_ATTR_SET(0, EN) |
			    RQ_CTXT_CEQ_ATTR_SET(rq->msix_entry_idx, INTR);

	rq_ctxt->wq_pfn_hi_type_owner =
		RQ_CTXT_WQ_PAGE_SET(wq_page_pfn_hi, HI_PFN) |
		RQ_CTXT_WQ_PAGE_SET(1, OWNER);

	switch (wqe_type) {
	case HINIC3_EXTEND_RQ_WQE:
		/* use 32Byte WQE with SGE for CQE */
		rq_ctxt->wq_pfn_hi_type_owner |=
			RQ_CTXT_WQ_PAGE_SET(0, WQE_TYPE);
		break;
	case HINIC3_NORMAL_RQ_WQE:
		/* use 16Byte WQE with 32Bytes SGE for CQE */
		rq_ctxt->wq_pfn_hi_type_owner |=
			RQ_CTXT_WQ_PAGE_SET(2, WQE_TYPE);
		rq_ctxt->cqe_sge_len = RQ_CTXT_CQE_LEN_SET(1, CQE_LEN);
		break;
	case HINIC3_COMPACT_RQ_WQE:
		/* use 8Byte WQE */
		rq_ctxt->wq_pfn_hi_type_owner |=
			RQ_CTXT_WQ_PAGE_SET(3, WQE_TYPE);
		break;
	default:
		pr_err("Invalid rq wqe type: %u", wqe_type);
	}

	rq_ctxt->wq_pfn_lo = wq_page_pfn_lo;

	rq_ctxt->pref_cache =
		RQ_CTXT_PREF_SET(WQ_PREFETCH_MIN, CACHE_MIN) |
		RQ_CTXT_PREF_SET(WQ_PREFETCH_MAX, CACHE_MAX) |
		RQ_CTXT_PREF_SET(WQ_PREFETCH_THRESHOLD, CACHE_THRESHOLD);

	rq_ctxt->pref_ci_owner =
		RQ_CTXT_PREF_SET(CI_HIGN_IDX(ci_start), CI_HI) |
		RQ_CTXT_PREF_SET(1, OWNER);

	rq_ctxt->pref_wq_pfn_hi_ci =
		RQ_CTXT_PREF_SET(wq_page_pfn_hi, WQ_PFN_HI) |
		RQ_CTXT_PREF_SET(ci_start, CI_LOW);

	rq_ctxt->pref_wq_pfn_lo = wq_page_pfn_lo;


	rq_ctxt->wq_block_pfn_hi =
		RQ_CTXT_WQ_BLOCK_SET(wq_block_pfn_hi, PFN_HI);

	rq_ctxt->wq_block_pfn_lo = wq_block_pfn_lo;

	hinic3_cpu_to_be32(rq_ctxt, sizeof(*rq_ctxt));
}

static void hinic3_qp_prepare_cmdq_header(struct hinic3_qp_ctxt_header *qp_ctxt_hdr,
					  enum hinic3_qp_ctxt_type ctxt_type, u16 num_queues,
					  u16 q_id, u16 func_id)
{
	qp_ctxt_hdr->queue_type = ctxt_type;
	qp_ctxt_hdr->num_queues = num_queues;
	qp_ctxt_hdr->start_qid = q_id;
	qp_ctxt_hdr->dest_func_id = func_id;

	hinic3_cpu_to_be32(qp_ctxt_hdr, sizeof(*qp_ctxt_hdr));
}

static u8 prepare_cmd_buf_qp_context_multi_store(struct hinic3_nic_io *nic_io,
						 struct hinic3_cmd_buf *cmd_buf,
						 enum hinic3_qp_ctxt_type ctxt_type,
	u16 start_qid, u16 max_ctxts)
{
	struct hinic3_qp_ctxt_block *qp_ctxt_block = NULL;
	u16 func_id;
	u16 i;

	qp_ctxt_block = cmd_buf->buf;
	func_id = hinic3_global_func_id(nic_io->hwdev);
	hinic3_qp_prepare_cmdq_header(&qp_ctxt_block->cmdq_hdr, ctxt_type,
				      max_ctxts, start_qid, func_id);

	for (i = 0; i < max_ctxts; i++) {
		if (ctxt_type == HINIC3_QP_CTXT_TYPE_RQ)
			hinic3_hw_rq_prepare_ctxt(&nic_io->rq[start_qid + i],
					       &qp_ctxt_block->rq_ctxt[i]);
		else
			hinic3_sq_prepare_ctxt(&nic_io->sq[start_qid + i],
					       start_qid + i,
					       &qp_ctxt_block->sq_ctxt[i]);
	}

	return (u8)HINIC3_HTN_CMD_SQ_RQ_CONTEXT_MULTI_ST;
}

static u8 prepare_cmd_buf_clean_tso_lro_space(struct hinic3_nic_io *nic_io,
					      struct hinic3_cmd_buf *cmd_buf,
	enum hinic3_qp_ctxt_type ctxt_type)
{
	struct hinic3_clean_queue_ctxt *ctxt_block = NULL;

	ctxt_block = cmd_buf->buf;
	ctxt_block->cmdq_hdr.dest_func_id = hinic3_global_func_id(nic_io->hwdev);
	ctxt_block->cmdq_hdr.num_queues = nic_io->max_qps;
	ctxt_block->cmdq_hdr.queue_type = ctxt_type;
	ctxt_block->cmdq_hdr.start_qid = 0;

	hinic3_cpu_to_be32(ctxt_block, sizeof(*ctxt_block));

	cmd_buf->size = sizeof(*ctxt_block);
	return (u8)HINIC3_HTN_CMD_TSO_LRO_SPACE_CLEAN;
}

static void prepare_rss_indir_table_cmd_header(const struct hinic3_nic_io *nic_io,
					       const struct hinic3_cmd_buf *cmd_buf)
{
	struct hinic3_rss_cmd_header *header = cmd_buf->buf;

	header->dest_func_id = hinic3_global_func_id(nic_io->hwdev);
	hinic3_cpu_to_be32(header, sizeof(*header));
}

static u8 prepare_cmd_buf_set_rss_indir_table(const struct hinic3_nic_io *nic_io,
					      const u32 *indir_table,
					      struct hinic3_cmd_buf *cmd_buf)
{
	u32 i;
	u8 *indir_tbl = NULL;

	indir_tbl = (u8 *)cmd_buf->buf + sizeof(struct hinic3_rss_cmd_header);
	cmd_buf->size = sizeof(struct hinic3_rss_cmd_header) + NIC_RSS_INDIR_SIZE;
	memset(indir_tbl, 0, NIC_RSS_INDIR_SIZE);

	prepare_rss_indir_table_cmd_header(nic_io, cmd_buf);

	for (i = 0; i < NIC_RSS_INDIR_SIZE; i++)
		indir_tbl[i] = (u8)(*(indir_table + i));

	hinic3_cpu_to_be32(indir_tbl, NIC_RSS_INDIR_SIZE);

	return (u8)HINIC3_HTN_CMD_SET_RSS_INDIR_TABLE;
}

static u8 prepare_cmd_buf_get_rss_indir_table(const struct hinic3_nic_io *nic_io,
					      const struct hinic3_cmd_buf *cmd_buf)
{
	memset(cmd_buf->buf, 0, cmd_buf->size);
	prepare_rss_indir_table_cmd_header(nic_io, cmd_buf);

	return (u8)HINIC3_HTN_CMD_GET_RSS_INDIR_TABLE;
}

static void cmd_buf_to_rss_indir_table(const struct hinic3_cmd_buf *cmd_buf, u32 *indir_table)
{
	u32 i;
	u8 *indir_tbl = NULL;

	indir_tbl = (u8 *)cmd_buf->buf;
	hinic3_be32_to_cpu(cmd_buf->buf, NIC_RSS_INDIR_SIZE);
	for (i = 0; i < NIC_RSS_INDIR_SIZE; i++)
		indir_table[i] = *(indir_tbl + i);
}

static u8 prepare_cmd_buf_modify_svlan(struct hinic3_cmd_buf *cmd_buf,
				       u16 func_id, u16 vlan_tag, u16 q_id, u8 vlan_mode)
{
	struct hinic3_vlan_ctx *vlan_ctx = NULL;

	cmd_buf->size = sizeof(struct hinic3_vlan_ctx);
	vlan_ctx = (struct hinic3_vlan_ctx *)cmd_buf->buf;

	vlan_ctx->dest_func_id = func_id;
	vlan_ctx->start_qid = q_id;
	vlan_ctx->vlan_tag = vlan_tag;
	vlan_ctx->vlan_sel = 0; /* TPID0 in IPSU */
	vlan_ctx->vlan_mode = vlan_mode;

	hinic3_cpu_to_be32(vlan_ctx, sizeof(struct hinic3_vlan_ctx));
	return (u8)HINIC3_HTN_CMD_SVLAN_MODIFY;
}

struct hinic3_nic_cmdq_ops *hinic3_nic_cmdq_get_hw_ops(void)
{
	static struct hinic3_nic_cmdq_ops cmdq_hw_ops = {
		.prepare_cmd_buf_clean_tso_lro_space = prepare_cmd_buf_clean_tso_lro_space,
		.prepare_cmd_buf_qp_context_multi_store = prepare_cmd_buf_qp_context_multi_store,
		.prepare_cmd_buf_modify_svlan = prepare_cmd_buf_modify_svlan,
		.prepare_cmd_buf_set_rss_indir_table = prepare_cmd_buf_set_rss_indir_table,
		.prepare_cmd_buf_get_rss_indir_table = prepare_cmd_buf_get_rss_indir_table,
		.cmd_buf_to_rss_indir_table = cmd_buf_to_rss_indir_table,
	};

	return &cmdq_hw_ops;
}
