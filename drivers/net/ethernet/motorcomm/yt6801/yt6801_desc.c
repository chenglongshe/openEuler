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
