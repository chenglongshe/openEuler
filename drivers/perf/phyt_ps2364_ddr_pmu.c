// SPDX-License-Identifier: GPL-2.0
/*
 * Phytium PS2364 DDR PMU support
 */

#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/perf_event.h>
#include <linux/types.h>
#include <linux/acpi.h>
#include <linux/bug.h>
#include <linux/cpuhotplug.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/errno.h>

#include <asm/cputype.h>
#include <asm/local64.h>

#undef pr_fmt
#define pr_fmt(fmt)		"phyt_ps2364_ddrpmu: " fmt

#define PHYT_DDR_MAX_COUNTERS 8

#define DDR_START_TIMER			0x000
#define DDR_STOP_TIMER			0x004
#define DDR_CLEAR_EVENT			0x008
#define DDR_SET_TIMER_L			0x00c
#define DDR_SET_TIMER_H			0x010
#define DDR_TRIG_MODE			0x014
#define DDR_NOW_STATE			0x0e0
#define DDR_EVENT_CYCLES		0x0e4
#define DDR_TPOINT_END_L		0x0e4
#define DDR_TPOINT_END_H		0x0e8
#define DDR_STATE_STOP			0x0ec
#define DDR_EVENT_RXREQ			0x100
#define DDR_EVENT_RXDAT			0x104
#define DDR_EVENT_TXDAT			0x108
#define DDR_EVENT_RXREQ_RNS		0x10c
#define DDR_EVENT_RXREQ_WNSP	0x110
#define DDR_EVENT_RXREQ_WNSF	0x114
#define DDR_EVENT_BANDWIDTH		0x200
#define DDR_W_DATA_BASE			0x200
#define DDR_CLK_FRE				0xe00
#define DDR_DATA_WIDTH			0xe04

#define to_phyt_ps2364_ddrpmu(p)	(container_of(p, struct phyt_ps2364_ddrpmu, pmu))

struct phyt_ps2364_ddrpmu_hwevents {
	struct perf_event *hw_events[PHYT_DDR_MAX_COUNTERS];
	DECLARE_BITMAP(used_mask, PHYT_DDR_MAX_COUNTERS);
};

struct phyt_ps2364_ddrpmu {
	struct device *dev;
	void __iomem *base;
	void __iomem *csr_base;
	struct pmu pmu;
	struct phyt_ps2364_ddrpmu_hwevents pmu_events;
	u32 die_id;
	u32 ddr_id;
	u32 pmu_id;
	int bit_idx;
	int on_cpu;
	int irq;
	struct hlist_node node;
};

#define GET_DDR_EVENTID(hwc)	(hwc->config_base & 0x7)
#define EVENT_VALID(idx) ((idx >= 0) && (idx < PHYT_DDR_MAX_COUNTERS))

static const u32 ddr_counter_reg_offset[] = {
	DDR_EVENT_CYCLES,
	DDR_EVENT_RXREQ, DDR_EVENT_RXDAT, DDR_EVENT_TXDAT,
	DDR_EVENT_RXREQ_RNS, DDR_EVENT_RXREQ_WNSP, DDR_EVENT_RXREQ_WNSF,
	DDR_EVENT_BANDWIDTH
};

ssize_t phyt_ps2364_ddrpmu_format_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);

	return sprintf(buf, "%s\n", (char *)eattr->var);
}

ssize_t phyt_ps2364_ddrpmu_event_sysfs_show(struct device *dev,
				  struct device_attribute *attr, char *page)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);

	return sprintf(page, "config=0x%lx\n", (unsigned long)eattr->var);
}

static ssize_t cpumask_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = to_phyt_ps2364_ddrpmu(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, cpumask_of(ddr_pmu->on_cpu));
}

#define PHYT_PMU_ATTR(_name, _func, _config)				\
	(&((struct dev_ext_attribute[]) {				\
		{ __ATTR(_name, 0444, _func, NULL), (void *)_config }	\
	})[0].attr.attr)

#define PHYT_DDRPMU_FORMAT_ATTR(_name, _config)		\
	PHYT_PMU_ATTR(_name, phyt_ps2364_ddrpmu_format_sysfs_show, (void *)_config)

#define PHYT_DDRPMU_EVENT_ATTR(_name, _config)		\
	PHYT_PMU_ATTR(_name, phyt_ps2364_ddrpmu_event_sysfs_show, (unsigned long)_config)

static struct attribute *phyt_ps2364_ddrpmu_format_attr[] = {
	PHYT_DDRPMU_FORMAT_ATTR(event, "config:0-2"),
	PHYT_DDRPMU_FORMAT_ATTR(timer, "config1:0-31"),
	NULL,
};

static const struct attribute_group phyt_ps2364_ddrpmu_format_group = {
	.name = "format",
	.attrs = phyt_ps2364_ddrpmu_format_attr,
};

static struct attribute *phyt_ps2364_ddrpmu_events_attr[] = {
	PHYT_DDRPMU_EVENT_ATTR(cycles,		0x00),
	PHYT_DDRPMU_EVENT_ATTR(rxreq,		0x01),
	PHYT_DDRPMU_EVENT_ATTR(rxdat,		0x02),
	PHYT_DDRPMU_EVENT_ATTR(txdat,		0x03),
	PHYT_DDRPMU_EVENT_ATTR(rxreq_RNS,		0x04),
	PHYT_DDRPMU_EVENT_ATTR(rxreq_WNSP,		0x05),
	PHYT_DDRPMU_EVENT_ATTR(rxreq_WNSF,		0x06),
	PHYT_DDRPMU_EVENT_ATTR(bandwidth,		0x07),
	NULL,
};

static const struct attribute_group phyt_ps2364_ddrpmu_events_group = {
	.name = "events",
	.attrs = phyt_ps2364_ddrpmu_events_attr,
};

static DEVICE_ATTR_RO(cpumask);

static struct attribute *phyt_ps2364_ddrpmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group phyt_ps2364_ddrpmu_cpumask_attr_group = {
	.attrs = phyt_ps2364_ddrpmu_cpumask_attrs,
};

static const struct attribute_group *phyt_ps2364_ddrpmu_attr_groups[] = {
	&phyt_ps2364_ddrpmu_format_group,
	&phyt_ps2364_ddrpmu_events_group,
	&phyt_ps2364_ddrpmu_cpumask_attr_group,
	NULL,
};

static u32 phyt_ps2364_ddrpmu_get_event_timer(struct perf_event *event)
{
	return FIELD_GET(GENMASK(31, 0), event->attr.config1);
}

static u64 phyt_ps2364_ddrpmu_read_counter(struct phyt_ps2364_ddrpmu *ddr_pmu,
					  struct hw_perf_event *hwc)
{
	u32 idx = GET_DDR_EVENTID(hwc);
	u32 cycle_l, cycle_h, w_data, ddr_data_width;
	u64 val64 = 0;
	int i;
	u32 counter_offset = ddr_counter_reg_offset[idx];

	if (!EVENT_VALID(idx)) {
		dev_err(ddr_pmu->dev, "Unsupported event index:%d!\n", idx);
		return 0;
	}

	switch (idx) {
	case 0: //cycles
		cycle_l = readl(ddr_pmu->base + counter_offset);
		cycle_h = readl(ddr_pmu->base + counter_offset + 4);
		val64 = (u64)cycle_h << 32 | (u64)cycle_l;
	break;
	case 7: //w_data
		ddr_data_width = readl(ddr_pmu->base + DDR_DATA_WIDTH);
		for (i = 0; i < (ddr_data_width/8); i++) {
			w_data = readl(ddr_pmu->base + counter_offset + 4*i);
			val64 += w_data;
	}
	break;
	default:
		val64 = readl(ddr_pmu->base + counter_offset);
	break;
	}

	return val64;
}

static void phyt_ps2364_ddrpmu_enable_clk(struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	u32 val;

	val = readl(ddr_pmu->csr_base);
	val |= 0xF;
	writel(val, ddr_pmu->csr_base);
}

static void phyt_ps2364_ddrpmu_disable_clk(struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	u32 val;

	val = readl(ddr_pmu->csr_base);
	val &= ~(0xF);
	writel(val, ddr_pmu->csr_base);
}

static void phyt_ps2364_ddrpmu_clear_all_counters(struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	writel(0x1, ddr_pmu->base + DDR_CLEAR_EVENT);
}

static void phyt_ps2364_ddrpmu_start_all_counters(struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	writel(0x1, ddr_pmu->base + DDR_START_TIMER);
}

static void phyt_ps2364_ddrpmu_stop_all_counters(struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	writel(0x1, ddr_pmu->base + DDR_STOP_TIMER);
}

static void phyt_ps2364_ddrpmu_set_timer(struct phyt_ps2364_ddrpmu *ddr_pmu, u32 th_val)
{
	u32 val;

	val = readl(ddr_pmu->base + DDR_SET_TIMER_L);
	val = readl(ddr_pmu->base + DDR_SET_TIMER_H);

	writel(th_val, ddr_pmu->base + DDR_SET_TIMER_L);
	writel(0, ddr_pmu->base + DDR_SET_TIMER_H);
}

static void phyt_ps2364_ddrpmu_reset_timer(struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	u32 val;

	val = readl(ddr_pmu->base + DDR_SET_TIMER_L);
	val = readl(ddr_pmu->base + DDR_SET_TIMER_H);

	writel(0xFFFFFFFF, ddr_pmu->base + DDR_SET_TIMER_L);
	writel(0xFFFFFFFF, ddr_pmu->base + DDR_SET_TIMER_H);
}

static unsigned long phyt_ps2364_ddrpmu_get_stop_state(struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	unsigned long val;

	val = (unsigned long)readl(ddr_pmu->base + DDR_STATE_STOP);
	return val;
}

static unsigned long phyt_ps2364_ddrpmu_get_irq_flag(struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	unsigned long val;

	val = (unsigned long)readl(ddr_pmu->csr_base+4);
	return val;
}

static int phyt_ps2364_ddrpmu_mark_event(struct perf_event *event)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = to_phyt_ps2364_ddrpmu(event->pmu);
	unsigned long *used_mask = ddr_pmu->pmu_events.used_mask;
	struct hw_perf_event *hwc = &event->hw;

	int idx = GET_DDR_EVENTID(hwc);

	if (test_bit(idx, used_mask))
		return -EAGAIN;

	set_bit(idx, used_mask);

	return idx;
}

static void phyt_ps2364_ddrpmu_unmark_event(struct phyt_ps2364_ddrpmu *ddr_pmu, int idx)
{
	if (!EVENT_VALID(idx)) {
		dev_err(ddr_pmu->dev, "Unsupported event index:%d!\n", idx);
		return;
	}

	clear_bit(idx, ddr_pmu->pmu_events.used_mask);
}

int phyt_ps2364_ddrpmu_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct phyt_ps2364_ddrpmu *ddr_pmu;
	u32 event_timer;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (is_sampling_event(event) || event->attach_state & PERF_ATTACH_TASK)
		return -EOPNOTSUPP;

	ddr_pmu = to_phyt_ps2364_ddrpmu(event->pmu);

	if (event->cpu < 0) {
		dev_warn(ddr_pmu->dev, "Can't provide per-task data!\n");
		return -EINVAL;
	}

	if (event->attr.config > PHYT_DDR_MAX_COUNTERS)
		return -EINVAL;


	if (ddr_pmu->on_cpu == -1)
		return -EINVAL;

	event_timer = phyt_ps2364_ddrpmu_get_event_timer(event);
	if (event_timer != 0)
		phyt_ps2364_ddrpmu_set_timer(ddr_pmu, event_timer);

	hwc->idx		= -1;
	hwc->config_base	= event->attr.config;

	event->cpu = ddr_pmu->on_cpu;

	return 0;
}

void phyt_ps2364_ddrpmu_event_update(struct perf_event *event)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = to_phyt_ps2364_ddrpmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 delta;

	delta = phyt_ps2364_ddrpmu_read_counter(ddr_pmu, hwc);
	local64_add(delta, &event->count);
}

void phyt_ps2364_ddrpmu_event_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	hwc->state = 0;
	perf_event_update_userpage(event);
}

void phyt_ps2364_ddrpmu_event_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	hwc->state |= PERF_HES_STOPPED;

	if (flags & PERF_EF_UPDATE)
		phyt_ps2364_ddrpmu_event_update(event);

}

int phyt_ps2364_ddrpmu_event_add(struct perf_event *event, int flags)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = to_phyt_ps2364_ddrpmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx;

	hwc->state |= PERF_HES_STOPPED;

	idx = phyt_ps2364_ddrpmu_mark_event(event);
	if (idx < 0)
		return idx;

	event->hw.idx = idx;
	ddr_pmu->pmu_events.hw_events[idx] = event;

	return 0;
}

void phyt_ps2364_ddrpmu_event_del(struct perf_event *event, int flags)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = to_phyt_ps2364_ddrpmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	unsigned long val;
	u32 event_timer;

	phyt_ps2364_ddrpmu_event_stop(event, PERF_EF_UPDATE);
	val = phyt_ps2364_ddrpmu_get_irq_flag(ddr_pmu);
	val = phyt_ps2364_ddrpmu_get_stop_state(ddr_pmu);
	phyt_ps2364_ddrpmu_unmark_event(ddr_pmu, hwc->idx);

	event_timer = phyt_ps2364_ddrpmu_get_event_timer(event);
	if (event_timer != 0)
		phyt_ps2364_ddrpmu_reset_timer(ddr_pmu);

	perf_event_update_userpage(event);
	ddr_pmu->pmu_events.hw_events[hwc->idx] = NULL;
}

void phyt_ps2364_ddrpmu_enable(struct pmu *pmu)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = to_phyt_ps2364_ddrpmu(pmu);
	int event_added = bitmap_weight(ddr_pmu->pmu_events.used_mask, PHYT_DDR_MAX_COUNTERS);

	if (event_added) {
		phyt_ps2364_ddrpmu_clear_all_counters(ddr_pmu);
		phyt_ps2364_ddrpmu_start_all_counters(ddr_pmu);
	}
}

void phyt_ps2364_ddrpmu_disable(struct pmu *pmu)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = to_phyt_ps2364_ddrpmu(pmu);
	int event_added = bitmap_weight(ddr_pmu->pmu_events.used_mask, PHYT_DDR_MAX_COUNTERS);

	if (event_added)
		phyt_ps2364_ddrpmu_stop_all_counters(ddr_pmu);
}

static const struct acpi_device_id phyt_ps2364_ddrpmu_acpi_match[] = {
	{ "PHYT0043", },
	{},
};
MODULE_DEVICE_TABLE(acpi, phyt_ps2364_ddrpmu_acpi_match);

static irqreturn_t phyt_ps2364_ddrpmu_overflow_handler(int irq, void *dev_id)
{

	struct phyt_ps2364_ddrpmu *ddr_pmu = dev_id;
	struct perf_event *event;
	unsigned long overflown, stop_state;
	int idx;
	unsigned long *used_mask = ddr_pmu->pmu_events.used_mask;

	int event_added = bitmap_weight(used_mask,
					PHYT_DDR_MAX_COUNTERS);

	overflown = phyt_ps2364_ddrpmu_get_irq_flag(ddr_pmu);

	if (!test_bit(ddr_pmu->bit_idx, &overflown))
		return IRQ_NONE;

	stop_state = phyt_ps2364_ddrpmu_get_stop_state(ddr_pmu);

	if (bitmap_weight(&stop_state, 6)) {
		for_each_set_bit(idx, used_mask, PHYT_DDR_MAX_COUNTERS) {
			event = ddr_pmu->pmu_events.hw_events[idx];
			if (!event)
				continue;
			phyt_ps2364_ddrpmu_event_update(event);
		}
		phyt_ps2364_ddrpmu_clear_all_counters(ddr_pmu);
		phyt_ps2364_ddrpmu_start_all_counters(ddr_pmu);

		return IRQ_HANDLED;
	}

	if (!event_added) {
		phyt_ps2364_ddrpmu_clear_all_counters(ddr_pmu);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;

}

static int phyt_ps2364_ddrpmu_init_irq(struct phyt_ps2364_ddrpmu *ddr_pmu,
				  struct platform_device *pdev)
{
	int irq, ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(&pdev->dev, irq, phyt_ps2364_ddrpmu_overflow_handler,
				   IRQF_NOBALANCING | IRQF_NO_THREAD | IRQF_SHARED,
				   dev_name(&pdev->dev), ddr_pmu);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Fail to request IRQ:%d ret:%d\n", irq, ret);
		return ret;
	}

	ddr_pmu->irq = irq;

	return 0;
}

static int phyt_ps2364_ddrpmu_init_data(struct platform_device *pdev,
		struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	struct resource *res, *clkres;

	if (device_property_read_u32(&pdev->dev, "phytium,die-id", &ddr_pmu->die_id)) {
		dev_err(&pdev->dev, "Can not read phytium,die-id!\n");
		return -EINVAL;
	}

	if (device_property_read_u32(&pdev->dev, "phytium,ddr-id", &ddr_pmu->ddr_id)) {
		dev_err(&pdev->dev, "Can not read phytium,ddr-id!\n");
		return -EINVAL;
	}

	if (device_property_read_u32(&pdev->dev, "phytium,pmu-id", &ddr_pmu->pmu_id)) {
		dev_err(&pdev->dev, "Can not read ddr pmu-id!\n");
		return -EINVAL;
	}

	ddr_pmu->bit_idx = ddr_pmu->ddr_id * 2 + ddr_pmu->pmu_id;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ddr_pmu->base = devm_ioremap_resource(&pdev->dev, res);

	if (IS_ERR(ddr_pmu->base)) {
		dev_err(&pdev->dev, "ioremap failed for ddr_pmu base resource\n");
		return PTR_ERR(ddr_pmu->base);
	}

	clkres = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!clkres) {
		dev_err(&pdev->dev, "failed for get ddr_pmu clk resource.\n");
		return -EINVAL;
	}

	ddr_pmu->csr_base = devm_ioremap(&pdev->dev, clkres->start, resource_size(clkres));
	if (IS_ERR(ddr_pmu->csr_base)) {
		dev_err(&pdev->dev, "ioremap failed for ddr_pmu csr resource\n");
		return PTR_ERR(ddr_pmu->csr_base);
	}
	return 0;
}


static int phyt_ps2364_ddrpmu_dev_probe(struct platform_device *pdev,
		struct phyt_ps2364_ddrpmu *ddr_pmu)
{
	int ret;

	ret = phyt_ps2364_ddrpmu_init_data(pdev, ddr_pmu);
	if (ret)
		return ret;

	ret = phyt_ps2364_ddrpmu_init_irq(ddr_pmu, pdev);
	if (ret)
		return ret;
	ddr_pmu->dev = &pdev->dev;
	ddr_pmu->on_cpu = raw_smp_processor_id();

	return 0;
}

static int phyt_ps2364_ddrpmu_probe(struct platform_device *pdev)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu;
	char *name;
	int ret;

	ddr_pmu = devm_kzalloc(&pdev->dev, sizeof(*ddr_pmu), GFP_KERNEL);
	if (!ddr_pmu)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddr_pmu);

	ret = phyt_ps2364_ddrpmu_dev_probe(pdev, ddr_pmu);
	if (ret)
		return ret;

	ret = cpuhp_state_add_instance_nocalls(CPUHP_AP_PERF_ARM_PHYT_PS2364_DDRPMU_ONLINE,
					   &ddr_pmu->node);
	if (ret) {
		dev_err(&pdev->dev, "Error %d registering hotplug;\n", ret);
		return ret;
	}

	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "phyt%u_ddr%u_pmu%u",
				  ddr_pmu->die_id, ddr_pmu->ddr_id, ddr_pmu->pmu_id);
	ddr_pmu->pmu = (struct pmu) {
		.name			= name,
	.module	 = THIS_MODULE,
		.task_ctx_nr	= perf_invalid_context,
		.event_init		= phyt_ps2364_ddrpmu_event_init,
		.pmu_enable		= phyt_ps2364_ddrpmu_enable,
		.pmu_disable	= phyt_ps2364_ddrpmu_disable,
		.add			= phyt_ps2364_ddrpmu_event_add,
		.del			= phyt_ps2364_ddrpmu_event_del,
		.start			= phyt_ps2364_ddrpmu_event_start,
		.stop			= phyt_ps2364_ddrpmu_event_stop,
		.read			= phyt_ps2364_ddrpmu_event_update,
		.attr_groups	= phyt_ps2364_ddrpmu_attr_groups,
	.capabilities	= PERF_PMU_CAP_NO_EXCLUDE,
	};


	ret = perf_pmu_register(&ddr_pmu->pmu, name, -1);
	if (ret) {
		dev_err(ddr_pmu->dev, "DDR PMU register failed!\n");
	cpuhp_state_remove_instance_nocalls(CPUHP_AP_PERF_ARM_PHYT_PS2364_DDRPMU_ONLINE,
						&ddr_pmu->node);
	}

	phyt_ps2364_ddrpmu_enable_clk(ddr_pmu);

	pr_info("phyt ps2364 ddr pmu: ");
	pr_info(" die_id = %d ddr_id = %d pmu_id = %d.\n", ddr_pmu->die_id,
		ddr_pmu->ddr_id, ddr_pmu->pmu_id);

	return ret;
}

static int phyt_ps2364_ddrpmu_remove(struct platform_device *pdev)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = platform_get_drvdata(pdev);

	phyt_ps2364_ddrpmu_disable_clk(ddr_pmu);

	perf_pmu_unregister(&ddr_pmu->pmu);
	cpuhp_state_remove_instance_nocalls(CPUHP_AP_PERF_ARM_PHYT_PS2364_DDRPMU_ONLINE,
					&ddr_pmu->node);

	return 0;
}

static struct platform_driver phyt_ps2364_ddrpmu_driver = {
	.driver = {
		.name = "phyt_ps2364_ddrpmu",
		.acpi_match_table = ACPI_PTR(phyt_ps2364_ddrpmu_acpi_match),
	.suppress_bind_attrs = true,
	},
	.probe = phyt_ps2364_ddrpmu_probe,
	.remove = phyt_ps2364_ddrpmu_remove,
};

int phyt_ps2364_ddrpmu_offline_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct phyt_ps2364_ddrpmu *ddr_pmu = hlist_entry_safe(node, struct phyt_ps2364_ddrpmu,
							 node);
	unsigned int target;

	if (ddr_pmu->on_cpu != cpu)
		return 0;

	ddr_pmu->on_cpu = -1;

	target = cpumask_first(cpu_online_mask);
	if (target >= nr_cpu_ids)
		return 0;

	perf_pmu_migrate_context(&ddr_pmu->pmu, cpu, target);
	ddr_pmu->on_cpu = target;

	return 0;
}


static int __init phyt_ps2364_ddrpmu_module_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_PERF_ARM_PHYT_PS2364_DDRPMU_ONLINE,
					  "AP_PERF_ARM_PHYT_PS2364DDRPMU_ONLINE",
					  NULL,
					  phyt_ps2364_ddrpmu_offline_cpu);
	if (ret) {
		pr_err("DDR PMU: setup hotplug, ret = %d\n", ret);
		return ret;
	}

	ret = platform_driver_register(&phyt_ps2364_ddrpmu_driver);
	if (ret)
		cpuhp_remove_multi_state(CPUHP_AP_PERF_ARM_PHYT_PS2364_DDRPMU_ONLINE);

	return ret;
}
module_init(phyt_ps2364_ddrpmu_module_init);

static void __exit phyt_ps2364_ddrpmu_module_exit(void)
{
	platform_driver_unregister(&phyt_ps2364_ddrpmu_driver);
	cpuhp_remove_multi_state(CPUHP_AP_PERF_ARM_PHYT_PS2364_DDRPMU_ONLINE);

}
module_exit(phyt_ps2364_ddrpmu_module_exit);

MODULE_DESCRIPTION("Phytium PS2364 DDR-PMU driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Xianghua HU <huxianghua@phytium.com.cn>");

