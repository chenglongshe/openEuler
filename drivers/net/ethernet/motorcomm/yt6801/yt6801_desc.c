// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd. */

#include "yt6801_desc.h"

void fxgmac_desc_data_unmap(struct fxgmac_pdata *pdata,
			    struct fxgmac_desc_data *desc_data)
{
	if (desc_data->skb_dma) {
		if (desc_data->mapped_as_page) {
			dma_unmap_page(pdata->dev, desc_data->skb_dma,
				       desc_data->skb_dma_len, DMA_TO_DEVICE);
		} else {
			dma_unmap_single(pdata->dev, desc_data->skb_dma,
					 desc_data->skb_dma_len, DMA_TO_DEVICE);
		}
		desc_data->skb_dma = 0;
		desc_data->skb_dma_len = 0;
	}

	if (desc_data->skb) {
		dev_kfree_skb_any(desc_data->skb);
		desc_data->skb = NULL;
	}

	if (desc_data->rx.hdr.pa.pages)
		put_page(desc_data->rx.hdr.pa.pages);

	if (desc_data->rx.hdr.pa_unmap.pages) {
		dma_unmap_page(pdata->dev, desc_data->rx.hdr.pa_unmap.pages_dma,
			       desc_data->rx.hdr.pa_unmap.pages_len,
			       DMA_FROM_DEVICE);
		put_page(desc_data->rx.hdr.pa_unmap.pages);
	}

	if (desc_data->rx.buf.pa.pages)
		put_page(desc_data->rx.buf.pa.pages);

	if (desc_data->rx.buf.pa_unmap.pages) {
		dma_unmap_page(pdata->dev, desc_data->rx.buf.pa_unmap.pages_dma,
			       desc_data->rx.buf.pa_unmap.pages_len,
			       DMA_FROM_DEVICE);
		put_page(desc_data->rx.buf.pa_unmap.pages);
	}
	memset(&desc_data->tx, 0, sizeof(desc_data->tx));
	memset(&desc_data->rx, 0, sizeof(desc_data->rx));

	desc_data->mapped_as_page = 0;
}

static void fxgmac_ring_free(struct fxgmac_pdata *pdata,
			     struct fxgmac_ring *ring)
{
	if (!ring)
		return;

	if (ring->desc_data_head) {
		for (u32 i = 0; i < ring->dma_desc_count; i++)
			fxgmac_desc_data_unmap(pdata,
					       FXGMAC_GET_DESC_DATA(ring, i));

		kfree(ring->desc_data_head);
		ring->desc_data_head = NULL;
	}

	if (ring->rx_hdr_pa.pages) {
		dma_unmap_page(pdata->dev, ring->rx_hdr_pa.pages_dma,
			       ring->rx_hdr_pa.pages_len, DMA_FROM_DEVICE);
		put_page(ring->rx_hdr_pa.pages);

		ring->rx_hdr_pa.pages = NULL;
		ring->rx_hdr_pa.pages_len = 0;
		ring->rx_hdr_pa.pages_offset = 0;
		ring->rx_hdr_pa.pages_dma = 0;
	}

	if (ring->rx_buf_pa.pages) {
		dma_unmap_page(pdata->dev, ring->rx_buf_pa.pages_dma,
			       ring->rx_buf_pa.pages_len, DMA_FROM_DEVICE);
		put_page(ring->rx_buf_pa.pages);

		ring->rx_buf_pa.pages = NULL;
		ring->rx_buf_pa.pages_len = 0;
		ring->rx_buf_pa.pages_offset = 0;
		ring->rx_buf_pa.pages_dma = 0;
	}
	if (ring->dma_desc_head) {
		dma_free_coherent(pdata->dev, (sizeof(struct fxgmac_dma_desc) *
				  ring->dma_desc_count), ring->dma_desc_head,
				  ring->dma_desc_head_addr);
		ring->dma_desc_head = NULL;
	}
}

static int fxgmac_ring_init(struct fxgmac_pdata *pdata,
			    struct fxgmac_ring *ring,
			    unsigned int dma_desc_count)
{
	/* Descriptors */
	ring->dma_desc_count = dma_desc_count;
	ring->dma_desc_head =
		dma_alloc_coherent(pdata->dev, (sizeof(struct fxgmac_dma_desc) *
				   dma_desc_count),
				   &ring->dma_desc_head_addr, GFP_KERNEL);
	if (!ring->dma_desc_head)
		return -ENOMEM;

	/* Array of descriptor data */
	ring->desc_data_head = kcalloc(dma_desc_count,
				       sizeof(struct fxgmac_desc_data),
				       GFP_KERNEL);
	if (!ring->desc_data_head)
		return -ENOMEM;

	yt_dbg(pdata,
	       "dma_desc_head=%p, dma_desc_head_addr=%pad, desc_data_head=%p\n",
	       ring->dma_desc_head, &ring->dma_desc_head_addr,
	       ring->desc_data_head);
	return 0;
}

static void fxgmac_rings_free(struct fxgmac_pdata *pdata)
{
	struct fxgmac_channel *channel = pdata->channel_head;

	fxgmac_ring_free(pdata, channel->tx_ring);

	for (u32 i = 0; i < pdata->channel_count; i++, channel++)
		fxgmac_ring_free(pdata, channel->rx_ring);
}

static int fxgmac_rings_alloc(struct fxgmac_pdata *pdata)
{
	struct fxgmac_channel *channel = pdata->channel_head;
	int ret;

	yt_dbg(pdata, "%s - Tx ring:\n", channel->name);
	ret = fxgmac_ring_init(pdata, channel->tx_ring,
			       pdata->tx_desc_count);
	if (ret < 0) {
		yt_err(pdata, "error initializing Tx ring");
		goto err_init_ring;
	}
	if (netif_msg_drv(pdata))
		yt_dbg(pdata,
		       "%s, ch=%u,tx_desc_cnt=%u\n",
		       __func__, 0, pdata->tx_desc_count);

	for (u32 i = 0; i < pdata->channel_count; i++, channel++) {
		yt_dbg(pdata, "%s - Rx ring:\n", channel->name);
		ret = fxgmac_ring_init(pdata, channel->rx_ring,
				       pdata->rx_desc_count);
		if (ret < 0) {
			yt_err(pdata, "error initializing Rx ring\n");
			goto err_init_ring;
		}
		if (netif_msg_drv(pdata))
			yt_dbg(pdata,
			       "%s, ch=%u,rx_desc_cnt=%u\n",
			       __func__, i,
			       pdata->rx_desc_count);
	}

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s ok\n", __func__);

	return 0;

err_init_ring:
	fxgmac_rings_free(pdata);
	return ret;
}

static void fxgmac_channels_free(struct fxgmac_pdata *pdata)
{
	struct fxgmac_channel *channel = pdata->channel_head;

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "free_channels,tx_ring=%p\n", channel->tx_ring);

	kfree(channel->tx_ring);
	channel->tx_ring = NULL;
	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "free_channels,rx_ring=%p\n", channel->rx_ring);

	kfree(channel->rx_ring);
	channel->rx_ring = NULL;
	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "free_channels,channel=%p\n", channel);

	kfree(channel);
	pdata->channel_head = NULL;
}

#ifdef CONFIG_PCI_MSI
static void fxgmac_set_msix_tx_irq(struct fxgmac_pdata *pdata,
				   struct fxgmac_channel *channel, u32 i)
{
	if (!FXGMAC_IS_CHANNEL_WITH_TX_IRQ(i))
		return;

	pdata->channel_irq[FXGMAC_MAX_DMA_RX_CHANNELS] =
		pdata->msix_entries[FXGMAC_MAX_DMA_RX_CHANNELS].vector;
	channel->dma_irq_tx = pdata->channel_irq[FXGMAC_MAX_DMA_RX_CHANNELS];

	yt_dbg(pdata, "%s, for MSIx, channel %d dma_irq_tx=%u\n", __func__, i,
	       channel->dma_irq_tx);
}
#endif

static int fxgmac_channels_alloc(struct fxgmac_pdata *pdata)
{
	struct fxgmac_channel *channel_head, *channel;
	struct fxgmac_ring *tx_ring, *rx_ring;
	int ret = -ENOMEM;

	channel_head = kcalloc(pdata->channel_count,
			       sizeof(struct fxgmac_channel), GFP_KERNEL);
	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "alloc_channels,channel_head=%p,size=%d*%lu\n",
		       channel_head, pdata->channel_count,
		       sizeof(struct fxgmac_channel));

	if (!channel_head)
		return ret;

	tx_ring = kcalloc(FXGMAC_TX_1_RING, sizeof(struct fxgmac_ring),
			  GFP_KERNEL);
	if (!tx_ring)
		goto err_tx_ring;

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "alloc_channels,tx_ring=%p,size=%d*%lu\n",
		       tx_ring, FXGMAC_TX_1_RING,
		       sizeof(struct fxgmac_ring));

	rx_ring = kcalloc(pdata->rx_ring_count, sizeof(struct fxgmac_ring),
			  GFP_KERNEL);
	if (!rx_ring)
		goto err_rx_ring;

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "alloc_channels,rx_ring=%p,size=%d*%lu\n",
		       rx_ring, pdata->rx_ring_count,
		       sizeof(struct fxgmac_ring));

	channel = channel_head;
	for (u32 i = 0; i < pdata->channel_count; i++, channel++) {
		snprintf(channel->name, sizeof(channel->name), "channel-%u", i);
		channel->pdata = pdata;
		channel->queue_index = i;
		channel->dma_reg_offset = DMA_CH_BASE + (DMA_CH_INC * i);

		if (pdata->per_channel_irq) {
			pdata->channel_irq[i] = pdata->msix_entries[i].vector;
#ifdef CONFIG_PCI_MSI
			fxgmac_set_msix_tx_irq(pdata, channel, i);
#endif

			/* Get the per DMA rx interrupt */
			ret = pdata->channel_irq[i];
			if (ret < 0) {
				yt_err(pdata, "get_irq %u err\n", i + 1);
				goto err_irq;
			}

			channel->dma_irq_rx = ret;
			yt_dbg(pdata,
			       "%s, for MSIx, channel %d dma_irq_rx=%u\n",
			       __func__, i, channel->dma_irq_rx);
		}

		if (i < FXGMAC_TX_1_RING)
			channel->tx_ring = tx_ring++;

		if (i < pdata->rx_ring_count)
			channel->rx_ring = rx_ring++;
	}

	pdata->channel_head = channel_head;

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s ok\n", __func__);
	return 0;

err_irq:
	kfree(rx_ring);

err_rx_ring:
	kfree(tx_ring);

err_tx_ring:
	kfree(channel_head);

	yt_err(pdata, "%s err:%d\n", __func__, ret);
	return ret;
}

void fxgmac_channels_rings_free(struct fxgmac_pdata *pdata)
{
	fxgmac_rings_free(pdata);
	fxgmac_channels_free(pdata);
}

int fxgmac_channels_rings_alloc(struct fxgmac_pdata *pdata)
{
	int ret;

	ret = fxgmac_channels_alloc(pdata);
	if (ret < 0)
		goto err_alloc;

	ret = fxgmac_rings_alloc(pdata);
	if (ret < 0)
		goto err_alloc;

	return 0;

err_alloc:
	fxgmac_channels_rings_free(pdata);
	return ret;
}

static void fxgmac_set_buffer_data(struct fxgmac_buffer_data *bd,
				   struct fxgmac_page_alloc *pa,
				   unsigned int len)
{
	get_page(pa->pages);
	bd->pa = *pa;

	bd->dma_base = pa->pages_dma;
	bd->dma_off = pa->pages_offset;
	bd->dma_len = len;

	pa->pages_offset += len;
	if ((pa->pages_offset + len) > pa->pages_len) {
		/* This data descriptor is responsible for unmapping page(s) */
		bd->pa_unmap = *pa;

		/* Get a new allocation next time */
		pa->pages = NULL;
		pa->pages_len = 0;
		pa->pages_offset = 0;
		pa->pages_dma = 0;
	}
}

static int fxgmac_alloc_pages(struct fxgmac_pdata *pdata,
			      struct fxgmac_page_alloc *pa, gfp_t gfp,
			      int order)
{
	struct page *pages = NULL;
	dma_addr_t pages_dma;

	/* Try to obtain pages, decreasing order if necessary */
	gfp |= __GFP_COMP | __GFP_NOWARN;
	while (order >= 0) {
		pages = alloc_pages(gfp, order);
		if (pages)
			break;

		order--;
	}

	if (!pages)
		return -ENOMEM;

	/* Map the pages */
	pages_dma = dma_map_page(pdata->dev, pages, 0, PAGE_SIZE << order,
				 DMA_FROM_DEVICE);
	if (dma_mapping_error(pdata->dev, pages_dma)) {
		put_page(pages);
		return -ENOMEM;
	}

	pa->pages = pages;
	pa->pages_len = PAGE_SIZE << order;
	pa->pages_offset = 0;
	pa->pages_dma = pages_dma;

	return 0;
}

#define FXGMAC_SKB_ALLOC_SIZE 512

int fxgmac_rx_buffe_map(struct fxgmac_pdata *pdata, struct fxgmac_ring *ring,
			struct fxgmac_desc_data *desc_data)
{
	int ret;

	if (!ring->rx_hdr_pa.pages) {
		ret = fxgmac_alloc_pages(pdata, &ring->rx_hdr_pa, GFP_ATOMIC,
					 0);
		if (ret)
			return ret;
	}
	/* Set up the header page info */
	fxgmac_set_buffer_data(&desc_data->rx.hdr, &ring->rx_hdr_pa,
			       pdata->rx_buf_size);

	return 0;
}

void fxgmac_desc_tx_reset(struct fxgmac_desc_data *desc_data)
{
	struct fxgmac_dma_desc *dma_desc = desc_data->dma_desc;

	/* Reset the Tx descriptor
	 * Set buffer 1 (lo) address to zero
	 * Set buffer 1 (hi) address to zero
	 * Reset all other control bits (IC, TTSE, B2L & B1L)
	 * Reset all other control bits (OWN, CTXT, FD, LD, CPC, CIC, etc)
	 */
	dma_desc->desc0 = 0;
	dma_desc->desc1 = 0;
	dma_desc->desc2 = 0;
	dma_desc->desc3 = 0;

	/* Make sure ownership is written to the descriptor */
	dma_wmb();
}

void fxgmac_desc_rx_reset(struct fxgmac_desc_data *desc_data)
{
	struct fxgmac_dma_desc *dma_desc = desc_data->dma_desc;
	dma_addr_t hdr_dma;

	/* Reset the Rx descriptor
	 * Set buffer 1 (lo) address to header dma address (lo)
	 * Set buffer 1 (hi) address to header dma address (hi)
	 * set control bits OWN and INTE
	 */
	hdr_dma = desc_data->rx.hdr.dma_base + desc_data->rx.hdr.dma_off;
	dma_desc->desc0 = cpu_to_le32(lower_32_bits(hdr_dma));
	dma_desc->desc1 = cpu_to_le32(upper_32_bits(hdr_dma));
	dma_desc->desc2 = 0;
	dma_desc->desc3 = 0;
	fxgmac_set_bits_le(&dma_desc->desc3, RX_NORMAL_DESC3_INTE_POS,
			   RX_NORMAL_DESC3_INTE_LEN, 1);
	fxgmac_set_bits_le(&dma_desc->desc3, RX_NORMAL_DESC3_BUF2V_POS,
			   RX_NORMAL_DESC3_BUF2V_LEN, 0);
	fxgmac_set_bits_le(&dma_desc->desc3, RX_NORMAL_DESC3_BUF1V_POS,
			   RX_NORMAL_DESC3_BUF1V_LEN, 1);

	/* Since the Rx DMA engine is likely running, make sure everything
	 * is written to the descriptor(s) before setting the OWN bit
	 * for the descriptor
	 */
	dma_wmb();

	fxgmac_set_bits_le(&dma_desc->desc3, RX_NORMAL_DESC3_OWN_POS,
			   RX_NORMAL_DESC3_OWN_LEN, 1);

	/* Make sure ownership is written to the descriptor */
	dma_wmb();
}

int fxgmac_tx_skb_map(struct fxgmac_channel *channel, struct sk_buff *skb)
{
	struct fxgmac_pdata *pdata = channel->pdata;
	struct fxgmac_ring *ring = channel->tx_ring;
	unsigned int start_index, cur_index;
	struct fxgmac_desc_data *desc_data;
	unsigned int offset, datalen, len;
	struct fxgmac_pkt_info *pkt_info;
	unsigned int tso, vlan;
	dma_addr_t skb_dma;
	skb_frag_t *frag;

	offset = 0;
	start_index = ring->cur;
	cur_index = ring->cur;
	pkt_info = &ring->pkt_info;
	pkt_info->desc_count = 0;
	pkt_info->length = 0;

	tso = FXGMAC_GET_BITS(pkt_info->attributes, TX_PKT_ATTR_TSO_ENABLE_POS,
			      TX_PKT_ATTR_TSO_ENABLE_LEN);
	vlan = FXGMAC_GET_BITS(pkt_info->attributes, TX_PKT_ATTR_VLAN_CTAG_POS,
			       TX_PKT_ATTR_VLAN_CTAG_LEN);

	/* Save space for a context descriptor if needed */
	if ((tso && pkt_info->mss != ring->tx.cur_mss) ||
	    (vlan && pkt_info->vlan_ctag != ring->tx.cur_vlan_ctag))
		cur_index = FXGMAC_GET_ENTRY(cur_index, ring->dma_desc_count);

	desc_data = FXGMAC_GET_DESC_DATA(ring, cur_index);

	if (tso) {
		/* Map the TSO header */
		skb_dma = dma_map_single(pdata->dev, skb->data,
					 pkt_info->header_len, DMA_TO_DEVICE);
		if (dma_mapping_error(pdata->dev, skb_dma)) {
			yt_err(pdata, "dma_map_single err\n");
			goto err_out;
		}
		desc_data->skb_dma = skb_dma;
		desc_data->skb_dma_len = pkt_info->header_len;
		yt_dbg(pdata, "skb header: index=%u, dma=%pad, len=%u\n",
		       cur_index, &skb_dma, pkt_info->header_len);

		offset = pkt_info->header_len;
		pkt_info->length += pkt_info->header_len;

		cur_index = FXGMAC_GET_ENTRY(cur_index, ring->dma_desc_count);
		desc_data = FXGMAC_GET_DESC_DATA(ring, cur_index);
	}

	/* Map the (remainder of the) packet */
	for (datalen = skb_headlen(skb) - offset; datalen;) {
		len = min_t(unsigned int, datalen, FXGMAC_TX_MAX_BUF_SIZE);
		skb_dma = dma_map_single(pdata->dev, skb->data + offset, len,
					 DMA_TO_DEVICE);
		if (dma_mapping_error(pdata->dev, skb_dma)) {
			yt_err(pdata, "dma_map_single err\n");
			goto err_out;
		}
		desc_data->skb_dma = skb_dma;
		desc_data->skb_dma_len = len;
		yt_dbg(pdata, "skb data: index=%u, dma=%pad, len=%u\n",
		       cur_index, &skb_dma, len);

		datalen -= len;
		offset += len;
		pkt_info->length += len;

		cur_index = FXGMAC_GET_ENTRY(cur_index, ring->dma_desc_count);
		desc_data = FXGMAC_GET_DESC_DATA(ring, cur_index);
	}

	for (u32 i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		yt_dbg(pdata, "mapping frag %u\n", i);

		frag = &skb_shinfo(skb)->frags[i];
		offset = 0;

		for (datalen = skb_frag_size(frag); datalen;) {
			len = min_t(unsigned int, datalen,
				    FXGMAC_TX_MAX_BUF_SIZE);
			skb_dma = skb_frag_dma_map(pdata->dev, frag, offset,
						   len, DMA_TO_DEVICE);
			if (dma_mapping_error(pdata->dev, skb_dma)) {
				yt_err(pdata, "skb_frag_dma_map err\n");
				goto err_out;
			}
			desc_data->skb_dma = skb_dma;
			desc_data->skb_dma_len = len;
			desc_data->mapped_as_page = 1;

			yt_dbg(pdata, "skb frag: index=%u, dma=%pad, len=%u\n",
			       cur_index, &skb_dma, len);

			datalen -= len;
			offset += len;
			pkt_info->length += len;

			cur_index = FXGMAC_GET_ENTRY(cur_index,
						     ring->dma_desc_count);
			desc_data = FXGMAC_GET_DESC_DATA(ring, cur_index);
		}
	}

	/* Save the skb address in the last entry. We always have some data
	 * that has been mapped so desc_data is always advanced past the last
	 * piece of mapped data - use the entry pointed to by cur_index - 1.
	 */
	desc_data = FXGMAC_GET_DESC_DATA(ring, (cur_index - 1) &
					 (ring->dma_desc_count - 1));
	desc_data->skb = skb;

	/* Save the number of descriptor entries used */
	if (start_index <= cur_index)
		pkt_info->desc_count = cur_index - start_index;
	else
		pkt_info->desc_count =
			ring->dma_desc_count - start_index + cur_index;

	return pkt_info->desc_count;

err_out:
	while (start_index < cur_index) {
		desc_data = FXGMAC_GET_DESC_DATA(ring, start_index);
		start_index =
			FXGMAC_GET_ENTRY(start_index, ring->dma_desc_count);
		fxgmac_desc_data_unmap(pdata, desc_data);
	}

	return 0;
}

void fxgmac_dump_rx_desc(struct fxgmac_pdata *pdata, struct fxgmac_ring *ring,
			 unsigned int idx)
{
	struct fxgmac_desc_data *desc_data;
	struct fxgmac_dma_desc *dma_desc;

	desc_data = FXGMAC_GET_DESC_DATA(ring, idx);
	dma_desc = desc_data->dma_desc;
	yt_dbg(pdata,
	       "RX: dma_desc=%p, dma_desc_addr=%pad, RX_NORMAL_DESC[%d RX BY DEVICE] = %08x:%08x:%08x:%08x\n\n",
	       dma_desc, &desc_data->dma_desc_addr, idx,
	       le32_to_cpu(dma_desc->desc0), le32_to_cpu(dma_desc->desc1),
	       le32_to_cpu(dma_desc->desc2), le32_to_cpu(dma_desc->desc3));
}

void fxgmac_dump_tx_desc(struct fxgmac_pdata *pdata, struct fxgmac_ring *ring,
			 unsigned int idx, unsigned int count,
			 unsigned int flag)
{
	struct fxgmac_desc_data *desc_data;

	while (count--) {
		desc_data = FXGMAC_GET_DESC_DATA(ring, idx);
		yt_dbg(pdata,
		       "TX: dma_desc=%p, dma_desc_addr=%pad, TX_NORMAL_DESC[%d %s] = %08x:%08x:%08x:%08x\n",
		       desc_data->dma_desc, &desc_data->dma_desc_addr, idx,
		       (flag == 1) ? "QUEUED FOR TX" : "TX BY DEVICE",
		       le32_to_cpu(desc_data->dma_desc->desc0),
		       le32_to_cpu(desc_data->dma_desc->desc1),
		       le32_to_cpu(desc_data->dma_desc->desc2),
		       le32_to_cpu(desc_data->dma_desc->desc3));

		idx++;
	}
}

int fxgmac_is_tx_complete(struct fxgmac_dma_desc *dma_desc)
{
	return !FXGMAC_GET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3_OWN_POS,
				   TX_NORMAL_DESC3_OWN_LEN);
}

int fxgmac_is_last_desc(struct fxgmac_dma_desc *dma_desc)
{
	/* Rx and Tx share LD bit, so check TDES3.LD bit */
	return FXGMAC_GET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3_LD_POS,
				  TX_NORMAL_DESC3_LD_LEN);
}
