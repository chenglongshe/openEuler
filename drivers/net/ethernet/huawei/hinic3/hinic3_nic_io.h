/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2021 Huawei Technologies Co., Ltd */

#ifndef HINIC3_NIC_IO_H
#define HINIC3_NIC_IO_H

#include "hinic3_crm.h"
#include "hinic3_common.h"
#include "hinic3_wq.h"

#define HINIC3_MAX_TX_QUEUE_DEPTH	65536
#define HINIC3_MAX_RX_QUEUE_DEPTH	16384

#define HINIC3_MIN_QUEUE_DEPTH		128

#define HINIC3_SQ_WQEBB_SHIFT		4
#define HINIC3_RQ_WQEBB_SHIFT		3

#define HINIC3_SQ_WQEBB_SIZE		BIT(HINIC3_SQ_WQEBB_SHIFT)
#define HINIC3_CQE_SIZE_SHIFT		4

#define HINIC3_Q_CTXT_MAX		31 /* (2048 - 8) / 64 */

#define RQ_CTXT_PI_IDX_SHIFT				0
#define RQ_CTXT_CI_IDX_SHIFT				16

#define RQ_CTXT_PI_IDX_MASK				0xFFFFU
#define RQ_CTXT_CI_IDX_MASK				0xFFFFU

#define RQ_CTXT_CI_PI_SET(val, member)			(((val) & \
					RQ_CTXT_##member##_MASK) \
					<< RQ_CTXT_##member##_SHIFT)

#define SQ_CTXT_SIZE(num_sqs)	((u16)(sizeof(struct hinic3_qp_ctxt_header) \
				+ (num_sqs) * sizeof(struct hinic3_sq_ctxt)))

#define RQ_CTXT_SIZE(num_rqs)	((u16)(sizeof(struct hinic3_qp_ctxt_header) \
				+ (num_rqs) * sizeof(struct hinic3_rq_ctxt)))

#define CI_IDX_HIGH_SHIFH				12

#define CI_HIGN_IDX(val)		((val) >> CI_IDX_HIGH_SHIFH)

#define SQ_CTXT_PI_IDX_SHIFT				0
#define SQ_CTXT_CI_IDX_SHIFT				16

#define SQ_CTXT_PI_IDX_MASK				0xFFFFU
#define SQ_CTXT_CI_IDX_MASK				0xFFFFU

#define SQ_CTXT_CI_PI_SET(val, member)			(((val) & \
					SQ_CTXT_##member##_MASK) \
					<< SQ_CTXT_##member##_SHIFT)

#define SQ_CTXT_MODE_SP_FLAG_SHIFT			0
#define SQ_CTXT_MODE_PKT_DROP_SHIFT			1

#define SQ_CTXT_MODE_SP_FLAG_MASK			0x1U
#define SQ_CTXT_MODE_PKT_DROP_MASK			0x1U

#define SQ_CTXT_MODE_SET(val, member)	(((val) & \
					SQ_CTXT_MODE_##member##_MASK) \
					<< SQ_CTXT_MODE_##member##_SHIFT)

#define SQ_CTXT_WQ_PAGE_HI_PFN_SHIFT			0
#define SQ_CTXT_WQ_PAGE_OWNER_SHIFT			23

#define SQ_CTXT_WQ_PAGE_HI_PFN_MASK			0xFFFFFU
#define SQ_CTXT_WQ_PAGE_OWNER_MASK			0x1U

#define SQ_CTXT_WQ_PAGE_SET(val, member)		(((val) & \
					SQ_CTXT_WQ_PAGE_##member##_MASK) \
					<< SQ_CTXT_WQ_PAGE_##member##_SHIFT)

#define SQ_CTXT_PKT_DROP_THD_ON_SHIFT			0
#define SQ_CTXT_PKT_DROP_THD_OFF_SHIFT			16

#define SQ_CTXT_PKT_DROP_THD_ON_MASK			0xFFFFU
#define SQ_CTXT_PKT_DROP_THD_OFF_MASK			0xFFFFU

#define SQ_CTXT_PKT_DROP_THD_SET(val, member)		(((val) & \
					SQ_CTXT_PKT_DROP_##member##_MASK) \
					<< SQ_CTXT_PKT_DROP_##member##_SHIFT)

#define SQ_CTXT_GLOBAL_SQ_ID_SHIFT			0

#define SQ_CTXT_GLOBAL_SQ_ID_MASK			0x1FFFU

#define SQ_CTXT_GLOBAL_QUEUE_ID_SET(val, member)		(((val) & \
					SQ_CTXT_##member##_MASK) \
					<< SQ_CTXT_##member##_SHIFT)

#define SQ_CTXT_VLAN_TAG_SHIFT				0
#define SQ_CTXT_VLAN_TYPE_SEL_SHIFT			16
#define SQ_CTXT_VLAN_INSERT_MODE_SHIFT			19
#define SQ_CTXT_VLAN_CEQ_EN_SHIFT			23

#define SQ_CTXT_VLAN_TAG_MASK				0xFFFFU
#define SQ_CTXT_VLAN_TYPE_SEL_MASK			0x7U
#define SQ_CTXT_VLAN_INSERT_MODE_MASK			0x3U
#define SQ_CTXT_VLAN_CEQ_EN_MASK			0x1U

#define SQ_CTXT_VLAN_CEQ_SET(val, member)		(((val) & \
					SQ_CTXT_VLAN_##member##_MASK) \
					<< SQ_CTXT_VLAN_##member##_SHIFT)

#define SQ_CTXT_PREF_CACHE_THRESHOLD_SHIFT		0
#define SQ_CTXT_PREF_CACHE_MAX_SHIFT			14
#define SQ_CTXT_PREF_CACHE_MIN_SHIFT			25

#define SQ_CTXT_PREF_CACHE_THRESHOLD_MASK		0x3FFFU
#define SQ_CTXT_PREF_CACHE_MAX_MASK			0x7FFU
#define SQ_CTXT_PREF_CACHE_MIN_MASK			0x7FU

#define SQ_CTXT_PREF_CI_HI_SHIFT			0
#define SQ_CTXT_PREF_OWNER_SHIFT			4

#define SQ_CTXT_PREF_CI_HI_MASK				0xFU
#define SQ_CTXT_PREF_OWNER_MASK				0x1U

#define SQ_CTXT_PREF_WQ_PFN_HI_SHIFT			0
#define SQ_CTXT_PREF_CI_LOW_SHIFT			20

#define SQ_CTXT_PREF_WQ_PFN_HI_MASK			0xFFFFFU
#define SQ_CTXT_PREF_CI_LOW_MASK			0xFFFU

#define SQ_CTXT_PREF_SET(val, member)			(((val) & \
					SQ_CTXT_PREF_##member##_MASK) \
					<< SQ_CTXT_PREF_##member##_SHIFT)

#define SQ_CTXT_WQ_BLOCK_PFN_HI_SHIFT			0

#define SQ_CTXT_WQ_BLOCK_PFN_HI_MASK			0x7FFFFFU

#define SQ_CTXT_WQ_BLOCK_SET(val, member)	(((val) & \
					SQ_CTXT_WQ_BLOCK_##member##_MASK) \
					<< SQ_CTXT_WQ_BLOCK_##member##_SHIFT)

#define RQ_CTXT_PI_IDX_SHIFT				0
#define RQ_CTXT_CI_IDX_SHIFT				16

#define RQ_CTXT_PI_IDX_MASK				0xFFFFU
#define RQ_CTXT_CI_IDX_MASK				0xFFFFU

#define RQ_CTXT_CI_PI_SET(val, member)			(((val) & \
					RQ_CTXT_##member##_MASK) \
					<< RQ_CTXT_##member##_SHIFT)

#define RQ_CTXT_CEQ_ATTR_INTR_SHIFT			21
#define RQ_CTXT_CEQ_ATTR_EN_SHIFT			31

#define RQ_CTXT_CEQ_ATTR_INTR_MASK			0x3FFU
#define RQ_CTXT_CEQ_ATTR_EN_MASK			0x1U

#define RQ_CTXT_CEQ_ATTR_ARM_SHIFT			30
#define RQ_CTXT_CEQ_ATTR_ARM_MASK			0x1U

#define RQ_CTXT_CEQ_ATTR_SET(val, member)		(((val) & \
					RQ_CTXT_CEQ_ATTR_##member##_MASK) \
					<< RQ_CTXT_CEQ_ATTR_##member##_SHIFT)

#define RQ_CTXT_WQ_PAGE_HI_PFN_SHIFT			0
#define RQ_CTXT_WQ_PAGE_WQE_TYPE_SHIFT			28
#define RQ_CTXT_WQ_PAGE_OWNER_SHIFT			31

#define RQ_CTXT_WQ_PAGE_HI_PFN_MASK			0xFFFFFU
#define RQ_CTXT_WQ_PAGE_WQE_TYPE_MASK			0x3U
#define RQ_CTXT_WQ_PAGE_OWNER_MASK			0x1U

#define RQ_CTXT_WQ_PAGE_SET(val, member)		(((val) & \
					RQ_CTXT_WQ_PAGE_##member##_MASK) << \
					RQ_CTXT_WQ_PAGE_##member##_SHIFT)

#define RQ_CTXT_CQE_LEN_SHIFT				28

#define RQ_CTXT_CQE_LEN_MASK				0x3U

#define RQ_CTXT_MAX_COUNT_SHIFT				18
#define RQ_CTXT_MAX_COUNT_MASK				0x3FFU

#define RQ_CTXT_CQE_LEN_SET(val, member)		(((val) & \
					RQ_CTXT_##member##_MASK) << \
					RQ_CTXT_##member##_SHIFT)

#define RQ_CTXT_PREF_CACHE_THRESHOLD_SHIFT		0
#define RQ_CTXT_PREF_CACHE_MAX_SHIFT			14
#define RQ_CTXT_PREF_CACHE_MIN_SHIFT			25

#define RQ_CTXT_PREF_CACHE_THRESHOLD_MASK		0x3FFFU
#define RQ_CTXT_PREF_CACHE_MAX_MASK			0x7FFU
#define RQ_CTXT_PREF_CACHE_MIN_MASK			0x7FU

#define RQ_CTXT_PREF_CI_HI_SHIFT			0
#define RQ_CTXT_PREF_OWNER_SHIFT			4

#define RQ_CTXT_PREF_CI_HI_MASK				0xFU
#define RQ_CTXT_PREF_OWNER_MASK				0x1U

#define RQ_CTXT_PREF_WQ_PFN_HI_SHIFT			0
#define RQ_CTXT_PREF_CI_LOW_SHIFT			20

#define RQ_CTXT_PREF_WQ_PFN_HI_MASK			0xFFFFFU
#define RQ_CTXT_PREF_CI_LOW_MASK			0xFFFU

#define RQ_CTXT_PREF_SET(val, member)			(((val) & \
					RQ_CTXT_PREF_##member##_MASK) << \
					RQ_CTXT_PREF_##member##_SHIFT)

#define RQ_CTXT_WQ_BLOCK_PFN_HI_SHIFT			0

#define RQ_CTXT_WQ_BLOCK_PFN_HI_MASK			0x7FFFFFU

#define RQ_CTXT_WQ_BLOCK_SET(val, member)		(((val) & \
					RQ_CTXT_WQ_BLOCK_##member##_MASK) << \
					RQ_CTXT_WQ_BLOCK_##member##_SHIFT)

#define WQ_PREFETCH_MAX			4
#define WQ_PREFETCH_MIN			1
#define WQ_PREFETCH_THRESHOLD		256

enum hinic3_rq_wqe_type {
	HINIC3_COMPACT_RQ_WQE,
	HINIC3_NORMAL_RQ_WQE,
	HINIC3_EXTEND_RQ_WQE,
};

struct hinic3_rq_ctxt {
	u32	ci_pi;
	u32	ceq_attr;
	u32	wq_pfn_hi_type_owner;
	u32	wq_pfn_lo;

	u32	ci_paddr_hi;
	u32	ci_paddr_lo;

	u32	rsvd;
	u32	cqe_sge_len;

	u32	pref_cache;
	u32	pref_ci_owner;
	u32	pref_wq_pfn_hi_ci;
	u32	pref_wq_pfn_lo;

	u32	pi_paddr_hi;
	u32	pi_paddr_lo;
	u32	wq_block_pfn_hi;
	u32	wq_block_pfn_lo;
};

struct hinic3_io_queue {
	struct hinic3_wq wq;
	union {
		u8 wqe_type; /* for rq */
		u8 owner; /* for sq */
	};
	u8			rsvd1;
	u16			rsvd2;

	u16 q_id;
	u16 msix_entry_idx;

	u8 __iomem *db_addr;

	union {
		struct {
			void *cons_idx_addr;
		} tx;

		struct {
			u16 *pi_virt_addr;
			dma_addr_t pi_dma_addr;
		} rx;
	};

	void *rx_cons_idx_addr;
	void *rx_ci_vaddr;
	dma_addr_t rx_ci_paddr;
	dma_addr_t cqe_start_paddr;
} ____cacheline_aligned;

struct hinic3_nic_db {
	u32 db_info;
	u32 pi_hi;
};

struct hinic3_tx_rx_ops {
	void (*tx_set_wqebb_cnt)(void *wqe_combo, u32 offload, u16 num_sge);
	void (*tx_set_wqe_task)(void *wqe_combo, void *offload_info);
	void (*rx_get_cqe_info)(void *rx_cqe, void *cqe_info, u8 cqe_mode);
	bool (*rx_cqe_done)(void *rxq, void **rx_cqe);
};

struct hinic3_rq_ci_wb {
	union {
		struct {
			u16 cqe_num;
			u16 hw_ci;
		} bs;
		u32 value;
	} dw0;

	u32 rsvd[3];
};

#ifdef static
#undef static
#define LLT_STATIC_DEF_SAVED
#endif

/* *
 * @brief hinic3_get_sq_free_wqebbs - get send queue free wqebb
 * @param sq: send queue
 * @retval : number of free wqebb
 */
static inline u16 hinic3_get_sq_free_wqebbs(struct hinic3_io_queue *sq)
{
	return hinic3_wq_free_wqebbs(&sq->wq);
}

/* *
 * @brief hinic3_update_sq_local_ci - update send queue local consumer index
 * @param sq: send queue
 * @param wqe_cnt: number of wqebb
 */
static inline void hinic3_update_sq_local_ci(struct hinic3_io_queue *sq,
					     u16 wqebb_cnt)
{
	hinic3_wq_put_wqebbs(&sq->wq, wqebb_cnt);
}

/* *
 * @brief hinic3_get_sq_local_ci - get send queue local consumer index
 * @param sq: send queue
 * @retval : local consumer index
 */
static inline u16 hinic3_get_sq_local_ci(const struct hinic3_io_queue *sq)
{
	return WQ_MASK_IDX(&sq->wq, sq->wq.cons_idx);
}

/* *
 * @brief hinic3_get_sq_local_pi - get send queue local producer index
 * @param sq: send queue
 * @retval : local producer index
 */
static inline u16 hinic3_get_sq_local_pi(const struct hinic3_io_queue *sq)
{
	return WQ_MASK_IDX(&sq->wq, sq->wq.prod_idx);
}

/* *
 * @brief hinic3_get_sq_hw_ci - get send queue hardware consumer index
 * @param sq: send queue
 * @retval : hardware consumer index
 */
static inline u16 hinic3_get_sq_hw_ci(const struct hinic3_io_queue *sq)
{
	return WQ_MASK_IDX(&sq->wq,
			   hinic3_hw_cpu16(*(u16 *)sq->tx.cons_idx_addr));
}

/* *
 * @brief hinic3_get_rq_hw_ci - get recv queue hardware consumer index
 * @param rq: recv queue
 * @retval : hardware consumer index
 */
static inline u16 hinic3_get_rq_hw_ci(const struct hinic3_io_queue *rq)
{
	u16 hw_ci;
	u32 rq_ci_wb;

	rq_ci_wb = hinic3_hw_cpu32(*(u32 *)rq->rx_cons_idx_addr);
	hw_ci = ((struct hinic3_rq_ci_wb *) &rq_ci_wb)->dw0.bs.hw_ci;

	return WQ_MASK_IDX(&rq->wq, hw_ci);
}

/* *
 * @brief hinic3_get_sq_one_wqebb - get send queue wqe with single wqebb
 * @param sq: send queue
 * @param pi: return current pi
 * @retval : wqe base address
 */
static inline void *hinic3_get_sq_one_wqebb(struct hinic3_io_queue *sq, u16 *pi)
{
	return hinic3_wq_get_one_wqebb(&sq->wq, pi);
}

/* *
 * @brief hinic3_get_sq_multi_wqebb - get send queue wqe with multiple wqebbs
 * @param sq: send queue
 * @param wqebb_cnt: wqebb counter
 * @param pi: return current pi
 * @param second_part_wqebbs_addr: second part wqebbs base address
 * @param first_part_wqebbs_num: number wqebbs of first part
 * @retval : first part wqebbs base address
 */
static inline void *hinic3_get_sq_multi_wqebbs(struct hinic3_io_queue *sq,
					       u16 wqebb_cnt, u16 *pi,
					       void **second_part_wqebbs_addr,
					       u16 *first_part_wqebbs_num)
{
	return hinic3_wq_get_multi_wqebbs(&sq->wq, wqebb_cnt, pi,
					  second_part_wqebbs_addr,
					  first_part_wqebbs_num);
}

/* *
 * @brief hinic3_get_and_update_sq_owner - get and update send queue owner bit
 * @param sq: send queue
 * @param curr_pi: current pi
 * @param wqebb_cnt: wqebb counter
 * @retval : owner bit
 */
static inline u16 hinic3_get_and_update_sq_owner(struct hinic3_io_queue *sq,
						 u16 curr_pi, u16 wqebb_cnt)
{
	u16 owner = sq->owner;

	if (unlikely(curr_pi + wqebb_cnt >= sq->wq.q_depth))
		sq->owner = !sq->owner;

	return owner;
}

/* *
 * @brief hinic3_get_sq_wqe_with_owner - get send queue wqe with owner
 * @param sq: send queue
 * @param wqebb_cnt: wqebb counter
 * @param pi: return current pi
 * @param owner: return owner bit
 * @param second_part_wqebbs_addr: second part wqebbs base address
 * @param first_part_wqebbs_num: number wqebbs of first part
 * @retval : first part wqebbs base address
 */
static inline void *hinic3_get_sq_wqe_with_owner(struct hinic3_io_queue *sq,
						 u16 wqebb_cnt, u16 *pi,
						 u16 *owner,
						 void **second_part_wqebbs_addr,
						 u16 *first_part_wqebbs_num)
{
	void *wqe = hinic3_wq_get_multi_wqebbs(&sq->wq, wqebb_cnt, pi,
					       second_part_wqebbs_addr,
					       first_part_wqebbs_num);

	*owner = sq->owner;
	if (unlikely(*pi + wqebb_cnt >= sq->wq.q_depth))
		sq->owner = !sq->owner;

	return wqe;
}

/* *
 * @brief hinic3_rollback_sq_wqebbs - rollback send queue wqe
 * @param sq: send queue
 * @param wqebb_cnt: wqebb counter
 * @param owner: owner bit
 */
static inline void hinic3_rollback_sq_wqebbs(struct hinic3_io_queue *sq,
					     u16 wqebb_cnt, u16 owner)
{
	if (owner != sq->owner)
		sq->owner = (u8)owner;
	sq->wq.prod_idx -= wqebb_cnt;
}

/* *
 * @brief hinic3_rq_wqe_addr - get receive queue wqe address by queue index
 * @param rq: receive queue
 * @param idx: wq index
 * @retval: wqe base address
 */
static inline void *hinic3_rq_wqe_addr(struct hinic3_io_queue *rq, u16 idx)
{
	return hinic3_wq_wqebb_addr(&rq->wq, idx);
}

/* *
 * @brief hinic3_update_rq_local_ci - update receive queue local consumer index
 * @param sq: receive queue
 * @param wqe_cnt: number of wqebb
 */
static inline void hinic3_update_rq_local_ci(struct hinic3_io_queue *rq,
					     u16 wqebb_cnt)
{
	hinic3_wq_put_wqebbs(&rq->wq, wqebb_cnt);
}

/* *
 * @brief hinic3_get_rq_local_ci - get receive queue local ci
 * @param rq: receive queue
 * @retval: receive queue local ci
 */
static inline u16 hinic3_get_rq_local_ci(const struct hinic3_io_queue *rq)
{
	return WQ_MASK_IDX(&rq->wq, rq->wq.cons_idx);
}

/* *
 * @brief hinic3_get_rq_local_pi - get receive queue local pi
 * @param rq: receive queue
 * @retval: receive queue local pi
 */
static inline u16 hinic3_get_rq_local_pi(const struct hinic3_io_queue *rq)
{
	return WQ_MASK_IDX(&rq->wq, rq->wq.prod_idx);
}

/* ******************** DB INFO ******************** */
#define DB_INFO_QID_SHIFT		0
#define DB_INFO_NON_FILTER_SHIFT	22
#define DB_INFO_CFLAG_SHIFT		23
#define DB_INFO_COS_SHIFT		24
#define DB_INFO_TYPE_SHIFT		27

#define DB_INFO_QID_MASK		0x1FFFU
#define DB_INFO_NON_FILTER_MASK		0x1U
#define DB_INFO_CFLAG_MASK		0x1U
#define DB_INFO_COS_MASK		0x7U
#define DB_INFO_TYPE_MASK		0x1FU
#define DB_INFO_SET(val, member)	\
		(((u32)(val) & DB_INFO_##member##_MASK) << \
		 DB_INFO_##member##_SHIFT)

#define DB_PI_LOW_MASK			0xFFU
#define DB_PI_HIGH_MASK			0xFFU
#define DB_PI_LOW(pi)			((pi) & DB_PI_LOW_MASK)
#define DB_PI_HI_SHIFT			8
#define DB_PI_HIGH(pi)		(((pi) >> DB_PI_HI_SHIFT) & DB_PI_HIGH_MASK)
#define DB_ADDR(queue, pi)	((u64 *)((queue)->db_addr) + DB_PI_LOW(pi))
#define SRC_TYPE			1

/* CFLAG_DATA_PATH */
#define SQ_CFLAG_DP			0
#define RQ_CFLAG_DP			1
/* *
 * @brief hinic3_write_db - write doorbell
 * @param queue: nic io queue
 * @param cos: cos index
 * @param cflag: 0--sq, 1--rq
 * @param pi: product index
 */
static inline void hinic3_write_db(struct hinic3_io_queue *queue, int cos,
				   u8 cflag, u16 pi)
{
	struct hinic3_nic_db db;

	db.db_info = DB_INFO_SET(SRC_TYPE, TYPE) | DB_INFO_SET(cflag, CFLAG) |
			DB_INFO_SET(cos, COS) | DB_INFO_SET(queue->q_id, QID);
	db.pi_hi = DB_PI_HIGH(pi);
	/* Data should be written to HW in Big Endian Format */
	db.db_info = hinic3_hw_be32(db.db_info);
	db.pi_hi = hinic3_hw_be32(db.pi_hi);

	wmb(); /* Write all before the doorbell */

	writeq(*((u64 *)(u8 *)&db), DB_ADDR(queue, pi));
}

struct hinic3_dyna_qp_params {
	u16	num_qps;
	u32	sq_depth;
	u32	rq_depth;

	struct hinic3_io_queue *sqs;
	struct hinic3_io_queue *rqs;
};

int hinic3_alloc_qps(void *hwdev, struct irq_info *qps_msix_arry,
		     struct hinic3_dyna_qp_params *qp_params);
void hinic3_free_qps(void *hwdev, struct hinic3_dyna_qp_params *qp_params);
int hinic3_init_qps(void *hwdev, struct hinic3_dyna_qp_params *qp_params);
void hinic3_deinit_qps(void *hwdev, struct hinic3_dyna_qp_params *qp_params);
int hinic3_init_nicio_res(void *hwdev);
void hinic3_deinit_nicio_res(void *hwdev);
u8 hinic3_get_nic_io_cqe_coal_state(void *hwdev);
int hinic3_get_rq_wqe_type(void *hwdev);
void hinic3_rq_prepare_ctxt_get_wq_info(struct hinic3_io_queue *rq,
					       u32 *wq_page_pfn_hi,
					       u32 *wq_page_pfn_lo,
					       u32 *wq_block_pfn_hi,
					       u32 *wq_block_pfn_lo);
#endif
