// SPDX-License-Identifier: GPL-2.0-only
/*
 * HYGON Memory Encryption Support
 *
 * Copyright (C) 2024 Hygon Info Technologies Ltd.
 *
 * Author: Liyang Han <hanliyang@hygon.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DISABLE_BRANCH_PROFILING

#include <linux/cc_platform.h>
#include <linux/mem_encrypt.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/cma.h>
#include <linux/minmax.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <asm/io.h>
#include <asm/cacheflush.h>
#include <asm/set_memory.h>
#include <asm/csv.h>
#include <asm/processor-hygon.h>

#define NUM_SMR_ENTRIES			(8 * 1024)
#define CSV_CMA_SHIFT			PUD_SHIFT
#define CSV_CMA_SIZE			(1 << CSV_CMA_SHIFT)
#define MIN_SMR_ENTRY_SHIFT		23
#define CSV_SMR_INFO_SIZE		(nr_node_ids * sizeof(struct csv_mem))
#define DEFAULT_MAX_CSV_NUMBER		113

u32 vendor_ebx __section(".data") = 0;
u32 vendor_ecx __section(".data") = 0;
u32 vendor_edx __section(".data") = 0;

void print_hygon_cc_feature_info(void)
{
	/* Secure Memory Encryption */
	if (cc_platform_has(CC_ATTR_HOST_MEM_ENCRYPT)) {
		/*
		 * HYGON SME is mutually exclusive with any of the
		 * HYGON CSV features below.
		 */
		pr_info(" HYGON SME");
		return;
	}

	/* Secure Encrypted Virtualization */
	if (cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT))
		pr_info(" HYGON CSV");

	/* Encrypted Register State */
	if (cc_platform_has(CC_ATTR_GUEST_STATE_ENCRYPT))
		pr_info(" HYGON CSV2");

	if (csv3_active())
		pr_info(" HYGON CSV3");
}

/*
 * Check whether host supports CSV3 in hygon platform.
 * Called in the guest, it always returns false.
 */
static bool __init __maybe_unused csv3_check_cpu_support(void)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned long me_mask;
	u64 msr;
	bool csv3_enabled;

	if (!is_x86_vendor_hygon())
		return false;

	if (sev_status)
		return false;

	/* Check for the SME/CSV support leaf */
	eax = 0x80000000;
	ecx = 0;
	native_cpuid(&eax, &ebx, &ecx, &edx);
	if (eax < 0x8000001f)
		return false;

#define HYGON_SME_BIT	BIT(0)
#define HYGON_CSV3_BIT	BIT(30)
	/*
	 * Check for the CSV feature:
	 * CPUID Fn8000_001F[EAX]
	 * - Bit 0  - SME support
	 * - Bit 1  - CSV support
	 * - Bit 3  - CSV2 support
	 * - Bit 30 - CSV3 support
	 */
	eax = 0x8000001f;
	ecx = 0;
	native_cpuid(&eax, &ebx, &ecx, &edx);
	if (!(eax & HYGON_SME_BIT))
		return false;

	csv3_enabled = !!(eax & HYGON_CSV3_BIT);

	me_mask = 1UL << (ebx & 0x3f);

	/* No SME if Hypervisor bit is set */
	eax = 1;
	ecx = 0;
	native_cpuid(&eax, &ebx, &ecx, &edx);
	if (ecx & BIT(31))
		return false;

	/* For SME, check the SYSCFG MSR */
	msr = __rdmsr(MSR_AMD64_SYSCFG);
	if (!(msr & MSR_AMD64_SYSCFG_MEM_ENCRYPT))
		return false;

	return !!me_mask && csv3_enabled;
}

/* csv3_active() indicate whether the guest is protected by CSV3 */
bool csv3_active(void)
{
	if (vendor_ebx == 0 || vendor_ecx == 0 || vendor_edx == 0) {
		u32 eax = 0;

		native_cpuid(&eax, &vendor_ebx, &vendor_ecx, &vendor_edx);
	}

	/* HygonGenuine */
	if (vendor_ebx == CPUID_VENDOR_HygonGenuine_ebx &&
	    vendor_ecx == CPUID_VENDOR_HygonGenuine_ecx &&
	    vendor_edx == CPUID_VENDOR_HygonGenuine_edx)
		return !!(sev_status & MSR_CSV3_ENABLED);
	else
		return false;
}
EXPORT_SYMBOL_GPL(csv3_active);

/* 0 percent of total memory by default*/
static unsigned char csv_mem_percentage;
static unsigned long csv_mem_size;
static unsigned int csv_use_hugetlb;

static LIST_HEAD(csv_metadata_list);
DEFINE_SPINLOCK(csv_metadata_lock);

struct csv_metadata {
	struct list_head list;
	unsigned long hpa;
	bool used;
};

static int __init cmdline_parse_csv_mem_size(char *str)
{
	unsigned long size;
	char *endp;

	if (str) {
		size  = memparse(str, &endp);
		csv_mem_size = size;
		if (!csv_mem_size)
			csv_mem_percentage = 0;
	}

	return 0;
}
early_param("csv_mem_size", cmdline_parse_csv_mem_size);

static int __init cmdline_parse_csv_mem_percentage(char *str)
{
	unsigned char percentage;
	int ret;

	if (!str)
		return 0;

	ret  = kstrtou8(str, 10, &percentage);
	if (!ret) {
		csv_mem_percentage = min_t(unsigned char, percentage, 80);
		if (csv_mem_percentage != percentage)
			pr_warn("csv_mem_percentage is limited to 80.\n");
	} else {
		/* Disable CSV CMA. */
		csv_mem_percentage = 0;
		pr_err("csv_mem_percentage is invalid. (0 - 80) is expected.\n");
	}

	return ret;
}
early_param("csv_mem_percentage", cmdline_parse_csv_mem_percentage);

static int __init cmdline_parse_csv_smcr_size(char *str)
{
#define CSV_SMCR_MAX_ENTRIES		64 /* 16GB SMCR */
	unsigned long size;
	char *endp;

	if (str) {
		size = memparse(str, &endp);
		if (size) {
			csv_smcr_num = size >> CSV_MR_ALIGN_BITS;
			if (csv_smcr_num < 2) {
				csv_smcr_num = 0;
				pr_err("CSV-SMCR: csv_smcr_size must be greater than 512m\n");
			}
			if (csv_smcr_num > CSV_SMCR_MAX_ENTRIES) {
				csv_smcr_num = CSV_SMCR_MAX_ENTRIES;
				pr_warn("CSV-SMCR: csv_smcr_size is limited to 16g\n");
			}
		} else
			pr_err("CSV-SMCR: csv_smcr_size is invalid\n");
	}

	return 0;
}
early_param("csv_smcr_size", cmdline_parse_csv_smcr_size);

static int __init cmdline_parse_csv_use_hugetlb(char *str)
{
	unsigned int count;
	int ret;

	if (!str) {
		csv_use_hugetlb = DEFAULT_MAX_CSV_NUMBER;
		return 0;
	}

	ret  = kstrtou32(str, 10, &count);
	if (!ret) {
		csv_use_hugetlb = count;
	} else {
		/* Disable CSV hugetlb. */
		csv_use_hugetlb = 0;
		pr_err("csv_use_hugetlb is invalid. (0 - 65535) is expected.\n");
	}

	return ret;
}
early_param("csv_use_hugetlb", cmdline_parse_csv_use_hugetlb);

struct csv_mem *csv_smr;
EXPORT_SYMBOL_GPL(csv_smr);

unsigned int csv_smr_num;
EXPORT_SYMBOL_GPL(csv_smr_num);

struct csv_mem *csv_smcr;
EXPORT_SYMBOL_GPL(csv_smcr);

unsigned int csv_smcr_num;
EXPORT_SYMBOL_GPL(csv_smcr_num);

static unsigned int smr_entry_shift;

static void csv_set_smr_entry_shift(unsigned int shift)
{
	smr_entry_shift = max_t(unsigned int, shift, MIN_SMR_ENTRY_SHIFT);
	pr_info("CSV-%s: SMR entry size is 0x%x\n",
		csv_smcr ? "SMCR" : "CMA", 1 << smr_entry_shift);
}

unsigned int csv_get_smr_entry_shift(void)
{
	return smr_entry_shift;
}
EXPORT_SYMBOL_GPL(csv_get_smr_entry_shift);

static int __init csv_smcr_reserve_mem(void)
{
	unsigned int i;
	int ret = -1;

	if (!csv_smcr_num)
		goto exit;

	csv_smcr = memblock_alloc_node(sizeof(struct csv_mem) * csv_smcr_num,
					SMP_CACHE_BYTES, NUMA_NO_NODE);
	if (!csv_smcr) {
		pr_err("CSV-SMCR: Fail to allocate memory\n");
		goto exit;
	}

	memset(csv_smcr, 0, sizeof(struct csv_mem) * csv_smcr_num);
	for (i = 0; i < csv_smcr_num; i++) {
		csv_smcr[i].size = 1UL << CSV_MR_ALIGN_BITS;
		csv_smcr[i].start = memblock_phys_alloc_try_nid(csv_smcr[i].size,
								csv_smcr[i].size,
								NUMA_NO_NODE);
		if (csv_smcr[i].start == 0) {
			csv_smcr[i].size = 0;
			pr_err("CSV-SMCR: Fail to reserve memory\n");
			goto failure;
		}
		csv_smcr[i].nid = phys_to_target_node(csv_smcr[i].start);
	}

	for (i = 0; i < csv_smcr_num; i++)
		pr_info("CSV-SMCR: reserve mem - paddr 0x%016llx, size 0x%016llx\n",
			csv_smcr[i].start, csv_smcr[i].size);

	ret = 0;
	goto exit;

failure:
	for (i = 0; i < csv_smcr_num; i++) {
		if (csv_smcr[i].start && csv_smcr[i].size)
			memblock_phys_free(csv_smcr[i].start, csv_smcr[i].size);
	}

	if (csv_smcr) {
		memblock_free(csv_smcr, sizeof(struct csv_mem) * csv_smcr_num);
		csv_smcr = NULL;
	}

exit:
	return ret;
}

static struct cma_array *csv_contiguous_pernuma_area[MAX_NUMNODES];

static unsigned long __init present_pages_in_node(int nid)
{
	unsigned long range_start_pfn, range_end_pfn;
	unsigned long nr_present = 0;
	int i;

	for_each_mem_pfn_range(i, nid, &range_start_pfn, &range_end_pfn, NULL)
		nr_present += range_end_pfn - range_start_pfn;

	return nr_present;
}

static unsigned long __init smallest_pfn_in_node(int nid)
{
	unsigned long range_start_pfn, range_end_pfn;
	unsigned long smallest = -1;
	int i;

	for_each_mem_pfn_range(i, nid, &range_start_pfn, &range_end_pfn, NULL) {
		if (range_start_pfn < smallest)
			smallest = range_start_pfn;
	}

	return smallest;
}

static unsigned long __init largest_pfn_in_node(int nid)
{
	unsigned long range_start_pfn, range_end_pfn;
	unsigned long largest = 0;
	int i;

	for_each_mem_pfn_range(i, nid, &range_start_pfn, &range_end_pfn, NULL) {
		if (range_end_pfn > largest)
			largest = range_end_pfn;
	}

	return largest;
}

static unsigned long __init largest_pfn(void)
{
	unsigned long range_start_pfn, range_end_pfn;
	unsigned long largest = 0;
	int node, i;

	for_each_node_state(node, N_ONLINE) {
		for_each_mem_pfn_range(i, node, &range_start_pfn, &range_end_pfn, NULL) {
			if (range_end_pfn > largest)
				largest = range_end_pfn;
		}
	}

	return largest;
}

static struct csv_mem * __init find_csv_smcr_mem_nid(int nid)
{
	int i;
	struct csv_mem *smcr = NULL;

	if (!csv_smcr)
		goto exit;

	for (i = 0; i < csv_smcr_num; i++) {
		if (csv_smcr[i].nid == nid) {
			smcr = &csv_smcr[i];
			goto exit;
		}
	}

exit:
	return smcr;
}

static phys_addr_t __init csv_early_percent_memory_on_node(int nid)
{
	return (present_pages_in_node(nid) * csv_mem_percentage / 100) << PAGE_SHIFT;
}

/******************************************************************************/
/**************************** CSV3 CMA interfaces *****************************/
/******************************************************************************/
#ifdef CONFIG_CMA
struct csv_cma {
	int nid;
	int fast;
	struct cma *cma;
};

struct cma_array {
	unsigned long count;
	atomic64_t csv_free_size;
	struct csv_cma csv_cma[];
};

static void __init csv_cma_reserve_mem(void)
{
	int node, i;
	unsigned long size;
	int idx = 0;
	int count;
	int cma_array_size;
	unsigned long max_spanned_size = 0;

	if (!csv_smr) {
		csv_smr = memblock_alloc_node(CSV_SMR_INFO_SIZE, SMP_CACHE_BYTES, NUMA_NO_NODE);
		if (!csv_smr) {
			pr_err("CSV-CMA: Fail to allocate csv_smr\n");
			return;
		}
	}

	for_each_node_state(node, N_ONLINE) {
		int ret;
		char name[CMA_MAX_NAME];
		struct cma_array *array;
		unsigned long spanned_size;
		unsigned long start = 0, end = 0;
		struct csv_cma *csv_cma;

		size = csv_early_percent_memory_on_node(node);
		count = DIV_ROUND_UP(size, 1 << CSV_CMA_SHIFT);
		if (!count)
			continue;

		cma_array_size = count * sizeof(*csv_cma) + sizeof(*array);
		array = memblock_alloc_node(cma_array_size, SMP_CACHE_BYTES, NUMA_NO_NODE);
		if (!array) {
			pr_err("CSV-CMA: Fail to allocate cma_array\n");
			continue;
		}

		array->count = 0;
		atomic64_set(&array->csv_free_size, 0);
		csv_contiguous_pernuma_area[node] = array;

		for (i = 0; i < count; i++) {
			csv_cma = &array->csv_cma[i];
			csv_cma->fast = 1;
			csv_cma->nid = node;
			snprintf(name, sizeof(name), "csv-n%dc%d", node, i);
			ret = cma_declare_contiguous_nid(0, CSV_CMA_SIZE, 0,
					1 << CSV_MR_ALIGN_BITS, PMD_SHIFT - PAGE_SHIFT,
					false, name, &(csv_cma->cma), node);
			if (ret) {
				pr_warn("CSV-CMA: Fail to reserve memory size 0x%x node %d\n",
					1 << CSV_CMA_SHIFT, node);
				break;
			}

			atomic64_add(CSV_CMA_SIZE, &array->csv_free_size);

			if (start > cma_get_base(csv_cma->cma) || !start)
				start = cma_get_base(csv_cma->cma);

			if (end < cma_get_base(csv_cma->cma) + cma_get_size(csv_cma->cma))
				end = cma_get_base(csv_cma->cma) + cma_get_size(csv_cma->cma);
		}

		if (!i)
			continue;

		array->count = i;

		if (find_csv_smcr_mem_nid(node)) {
			pr_info("CSV-CMA: Node %d has smcr reserved,set all mem as SMR\n", node);
			start = ALIGN(smallest_pfn_in_node(node) << PAGE_SHIFT,
					1ull << CSV_MR_ALIGN_BITS);
			end = ALIGN_DOWN(largest_pfn_in_node(node) << PAGE_SHIFT,
							1ull << CSV_MR_ALIGN_BITS);
		}

		spanned_size = end - start;
		if (spanned_size > max_spanned_size)
			max_spanned_size = spanned_size;

		csv_smr[idx].start = start;
		csv_smr[idx].size  = end - start;
		idx++;

		pr_info("CSV-CMA: Node %d - reserve size 0x%016lx, (expected size 0x%016lx)\n",
			node, (unsigned long)i * CSV_CMA_SIZE, size);
	}

	csv_smr_num = idx;
	WARN_ON((max_spanned_size / NUM_SMR_ENTRIES) < 1);
	if (likely((max_spanned_size / NUM_SMR_ENTRIES) >= 1))
		csv_set_smr_entry_shift(ilog2(max_spanned_size / NUM_SMR_ENTRIES - 1) + 1);
}

phys_addr_t csv_alloc_from_contiguous(size_t size, nodemask_t *nodes_allowed,
				      unsigned int align)
{
	int nid;
	int nr_nodes;
	struct page *page = NULL;
	struct cma_array *array = NULL;
	phys_addr_t phys_addr;
	int count;
	struct csv_cma *csv_cma;
	int fast = 1;

	if (!nodes_allowed || size > CSV_CMA_SIZE) {
		pr_err("CSV-CMA: Invalid params, size = 0x%lx, nodes_allowed = %p\n",
			size, nodes_allowed);
		return 0;
	}

	align = min_t(unsigned int, align, get_order(CSV_CMA_SIZE));
retry:
	nr_nodes = nodes_weight(*nodes_allowed);

	/* Traverse from current node */
	nid = numa_node_id();
	if (!node_isset(nid, *nodes_allowed))
		nid = next_node_in(nid, *nodes_allowed);

	for (; nr_nodes > 0; nid = next_node_in(nid, *nodes_allowed), nr_nodes--) {
		array = csv_contiguous_pernuma_area[nid];

		if (!array)
			continue;

		count = array->count;
		while (count) {
			csv_cma = &array->csv_cma[count - 1];

			/*
			 * The value check of csv_cma->fast is lockless, but
			 * that's ok as this don't affect functional correntness
			 * whatever the value of csv_cma->fast.
			 */
			if (fast && !csv_cma->fast) {
				count--;
				continue;
			}
			page = cma_alloc(csv_cma->cma, PAGE_ALIGN(size) >> PAGE_SHIFT,
							align, true);
			if (page) {
				page->private = (unsigned long)csv_cma;
				if (!csv_cma->fast)
					csv_cma->fast = 1;
				goto success;
			} else
				csv_cma->fast = 0;

			count--;
		}
	}

	if (fast) {
		fast = 0;
		goto retry;
	} else {
		pr_err("CSV-CMA: Fail to alloc secure memory(size = 0x%lx)\n", size);
		return 0;
	}

success:
	atomic64_sub(PAGE_ALIGN(size), &array->csv_free_size);
	phys_addr = page_to_phys(page);
	clflush_cache_range(__va(phys_addr), size);

	return phys_addr;
}
EXPORT_SYMBOL_GPL(csv_alloc_from_contiguous);

void csv_release_to_contiguous(phys_addr_t pa, size_t size)
{
	struct csv_cma *csv_cma;
	struct cma_array *array = NULL;
	struct page *page = pfn_to_page(pa >> PAGE_SHIFT);

	WARN_ON(!page);
	if (likely(page)) {
		csv_cma = (struct csv_cma *)page->private;
		WARN_ON(!csv_cma);
		if (likely(csv_cma)) {
			page->private = 0;
			csv_cma->fast = 1;
			cma_release(csv_cma->cma, page, PAGE_ALIGN(size) >> PAGE_SHIFT);
			array = csv_contiguous_pernuma_area[csv_cma->nid];
			atomic64_add(PAGE_ALIGN(size), &array->csv_free_size);
		}
	}
}
EXPORT_SYMBOL_GPL(csv_release_to_contiguous);

/*
 * The "free_size" file where the free size of csv cma is read from.
 */
static ssize_t free_size_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	int node;
	int offset = 0;
	unsigned long free_size, total_free_size = 0;
	unsigned long csv_size, total_csv_size = 0;
	struct cma_array *array = NULL;

	for_each_node_state(node, N_ONLINE) {
		array = csv_contiguous_pernuma_area[node];
		if (array == NULL) {
			csv_size = 0;
			free_size = 0;

			offset += snprintf(buf + offset, PAGE_SIZE - offset, "Node%d:\n", node);
			offset += snprintf(buf + offset, PAGE_SIZE - offset,
						" total: %8lu MiB\n", csv_size);
			offset += snprintf(buf + offset, PAGE_SIZE - offset,
						" free:  %8lu MiB\n", free_size);
			continue;
		}

		free_size = atomic64_read(&array->csv_free_size);
		csv_size = array->count * CSV_CMA_SIZE;
		offset += snprintf(buf + offset, PAGE_SIZE - offset, "Node%d:\n", node);
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
					" total: %8lu MiB\n", csv_size >> 20);
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
					" free:  %8lu MiB\n", free_size >> 20);
		total_free_size += free_size;
		total_csv_size += csv_size;
	}

	offset += snprintf(buf + offset, PAGE_SIZE - offset, "All Nodes:\n");
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
				" total: %8lu MiB\n", total_csv_size >> 20);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
				" free:  %8lu MiB\n", total_free_size >> 20);

	return offset;
}

static struct kobj_attribute csv_cma_attr = __ATTR(free_size, 0444,	free_size_show, NULL);

/*
 * Create a group of attributes so that we can create and destroy them all
 * at once.
 */
static struct attribute *csv_cma_attrs[] = {
	&csv_cma_attr.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static const struct attribute_group csv_cma_attr_group = {
	.attrs = csv_cma_attrs,
};

static struct kobject *csv_cma_kobj_root;

static int __init csv_cma_sysfs_init(void)
{
	int err;

	if (!is_x86_vendor_hygon() || !boot_cpu_has(X86_FEATURE_CSV3))
		return 0;

	csv_cma_kobj_root = kobject_create_and_add("csv3_cma", mm_kobj);
	if (!csv_cma_kobj_root)
		return -ENOMEM;

	err = sysfs_create_group(csv_cma_kobj_root, &csv_cma_attr_group);
	if (err)
		goto out;

	return 0;

out:
	kobject_put(csv_cma_kobj_root);
	return err;
}

static void __exit csv_cma_sysfs_exit(void)
{
	if (!is_x86_vendor_hygon() || !boot_cpu_has(X86_FEATURE_CSV3))
		return;

	if (csv_cma_kobj_root != NULL)
		kobject_put(csv_cma_kobj_root);
}

module_init(csv_cma_sysfs_init);
module_exit(csv_cma_sysfs_exit);

#else /* !CONFIG_CMA */

phys_addr_t csv_alloc_from_contiguous(size_t size, nodemask_t *nodes_allowed,
				      unsigned int align)
{
	return 0;
}
EXPORT_SYMBOL_GPL(csv_alloc_from_contiguous);

void csv_release_to_contiguous(phys_addr_t pa, size_t size)
{
}
EXPORT_SYMBOL_GPL(csv_release_to_contiguous);

#endif /* CONFIG_CMA */

static void __init csv_mark_secure_mem_region(void)
{
	int node;
	int idx = 0;
	unsigned long max_spanned_size = 0;

	csv_smr = memblock_alloc_node(CSV_SMR_INFO_SIZE, SMP_CACHE_BYTES, NUMA_NO_NODE);
	if (!csv_smr) {
		pr_err("CSV: Fail to allocate csv_smr\n");
		return;
	}

	for_each_node_state(node, N_ONLINE) {
		unsigned long spanned_size;
		unsigned long start = 0, end = 0;

		start = ALIGN(smallest_pfn_in_node(node) << PAGE_SHIFT,
						1ull << CSV_MR_ALIGN_BITS);
		end = ALIGN_DOWN(largest_pfn_in_node(node) << PAGE_SHIFT,
						1ull << CSV_MR_ALIGN_BITS);

		spanned_size = end - start;
		if (spanned_size > max_spanned_size)
			max_spanned_size = spanned_size;

		csv_smr[idx].start = start;
		csv_smr[idx].size  = end - start;
		idx++;

		pr_info("CSV: Node %d - secure range 0x%016lx ~ 0x%016lx\n",
			node, start, end);
	}

	csv_smr_num = idx;
	WARN_ON((max_spanned_size / NUM_SMR_ENTRIES) < 1);
	if (likely((max_spanned_size / NUM_SMR_ENTRIES) >= 1))
		csv_set_smr_entry_shift(ilog2(max_spanned_size / NUM_SMR_ENTRIES - 1) + 1);
}

static void __init csv_reserve_metadata(void)
{
	unsigned int i;
	struct csv_metadata *metadata;
	u64 hpa;
	u64 smr_size;
	struct list_head *pos, *q;

	smr_size = 1 << smr_entry_shift;
	for (i = 0; i < csv_use_hugetlb; i++) {
		hpa = memblock_phys_alloc_range(smr_size, smr_size, 0,
				ALIGN_DOWN((largest_pfn() << PAGE_SHIFT) - PUD_SIZE,
				PUD_SIZE));
		if (WARN_ON(!hpa))
			goto err;

		metadata = memblock_alloc_node(sizeof(*metadata), SMP_CACHE_BYTES,
						NUMA_NO_NODE);
		if (WARN_ON(!metadata)) {
			memblock_phys_free(hpa, 1 << smr_entry_shift);
			goto err;
		}

		metadata->hpa = hpa;
		metadata->used = false;
		list_add_tail(&metadata->list, &csv_metadata_list);
	}

	goto exit;
err:
	list_for_each_safe(pos, q, &csv_metadata_list) {
		metadata = list_entry(pos, struct csv_metadata, list);
		if (metadata) {
			memblock_phys_free(metadata->hpa, 1 << smr_entry_shift);
			list_del(&metadata->list);
			memblock_free(metadata, sizeof(*metadata));
		}
	}

	pr_warn("CSV: Fail to reserve metadata.\n");

exit:
	return;
}

phys_addr_t csv_alloc_metadata(void)
{
	struct csv_metadata *metadata;
	struct list_head *pos, *q;
	u64 hpa = 0;

	spin_lock(&csv_metadata_lock);

	list_for_each_safe(pos, q, &csv_metadata_list) {
		metadata = list_entry(pos, struct csv_metadata, list);
		if (metadata) {
			if (!metadata->used) {
				metadata->used = true;
				hpa = metadata->hpa;
				break;
			}
		}
	}

	spin_unlock(&csv_metadata_lock);

	return hpa;
}
EXPORT_SYMBOL_GPL(csv_alloc_metadata);

void csv_free_metadata(u64 hpa)
{
	struct csv_metadata *metadata;
	struct list_head *pos, *q;

	spin_lock(&csv_metadata_lock);

	list_for_each_safe(pos, q, &csv_metadata_list) {
		metadata = list_entry(pos, struct csv_metadata, list);
		if (metadata) {
			if (metadata->hpa == hpa) {
				WARN_ON(metadata->used != true);
				metadata->used = false;
				break;
			}
		}
	}

	spin_unlock(&csv_metadata_lock);
}
EXPORT_SYMBOL_GPL(csv_free_metadata);

#define CSV_CMA_AREAS		2458

void __init early_csv_reserve_mem(void)
{
	unsigned long total_pages;

	/* Only reserve memory on the host that enabled CSV3 feature */
	if (!csv3_check_cpu_support())
		return;

	/* SMCR memory for CSV3 NPT/context. */
	if (csv_smcr_reserve_mem())
		pr_warn("CSV: Fail to reserve NPT/context!\n");

#ifdef CONFIG_CMA
	if (cma_alloc_areas(CSV_CMA_AREAS))
		return;

	total_pages = PHYS_PFN(memblock_phys_mem_size());
	if (csv_mem_size) {
		if (csv_mem_size < (total_pages << PAGE_SHIFT)) {
			csv_mem_percentage = csv_mem_size * 100 / (total_pages << PAGE_SHIFT);
			if (csv_mem_percentage > 80)
				csv_mem_percentage = 80; /* Maximum percentage */
		} else
			csv_mem_percentage = 80; /* Maximum percentage */
	}

	if (csv_mem_percentage)
		csv_cma_reserve_mem();
#endif

	if (!csv_mem_percentage && csv_use_hugetlb) {
		csv_mark_secure_mem_region();
		csv_reserve_metadata();
	}

	if (!(csv_mem_percentage || csv_use_hugetlb))
		pr_warn("CSV: Configuration of either csv_mem_percentage or csv_use_hugetlb is required.\n");

}

enum csv_smr_source get_csv_smr_source(void)
{
	if (csv_mem_percentage)
		return USE_CMA;

	if (csv_use_hugetlb)
		return USE_HUGETLB;

	return NOT_SUPPORTED;
}
EXPORT_SYMBOL_GPL(get_csv_smr_source);
