#include <linux/atomic.h>
#include <linux/kvm_host.h>
#include <linux/kvm.h>
#include <linux/vfio.h>
#include <linux/vfio_pci_core.h>
#include <asm/kvm_tmi.h>
#include <asm/kvm_pgtable.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <asm/stage2_pgtable.h>
#include <asm/virtcca_cvm_host.h>
#include <linux/arm-smccc.h>
#include <kvm/arm_hypercalls.h>
#include <kvm/arm_psci.h>
#include <asm/kvm_tmm.h>
#include <asm/virtcca_coda.h>
#include <uapi/linux/vm_sockets.h>
#include <net/sock.h>
#include <net/af_vsock.h>
#include <linux/nmi.h>
#include <virtcca_mig.h>

#define MAX_TRIES 100

#define SEC_CRC_PATH	"/tmp/sec_memory_check"
#define NS_CRC_PATH		"/tmp/ns_memory_check"
#define CRC_DUMP_CHUNK_SIZE 512

uint64_t g_sec_crc_start;
uint64_t g_sec_crc_end;
uint64_t g_sec_crc_granularity;

uint64_t g_ns_crc_start;
uint64_t g_ns_crc_end;
uint64_t g_ns_crc_granularity;

#define FILE_NAME_LEN	256
#define SRC_CRC_PATH	"/tmp/src_crc"
#define DST_CRC_PATH	"/tmp/dst_crc"
#define SWIOTLB_SRC_CRC_PATH	"/tmp/swiotlb_src_crc"
#define SWIOTLB_DST_CRC_PATH	"/tmp/swiotlb_dst_crc"

#define DEFAULT_IPA_START	0x40000000
#define SWIOTLB_CRC_LEN	512
#define CRC_POLYNOMIAL	0xEDB88320
#define CRC_LEN	512
#define CRC_SHIFT	8
#define MAX_MAC_PAGES_PER_ARR 256
#define MAX_BUF_PAGES 512
static struct virtcca_mig_capabilities virtcca_mig_caps;

/* now bypass the migCVM, config staightly 1 is source, 2 is dest*/
bool virtcca_is_migration_source(struct virtcca_cvm *cvm)
{
	if (!cvm || !cvm->mig_state) {
		pr_info("Error: cvm or cvm->params is NULL\n");
		return false;
	}

	pr_info("debug: virtcca_is_migration_source is calling!");

	if (cvm->mig_state->mig_src == VIRTCCA_MIG_SRC) {
		return true;
	}

	return false;
}

/* read the max-migs , max of rd/tec pages support */
int virtcca_mig_capabilities_setup(struct virtcca_cvm *cvm)
{
	uint64_t res;
	uint16_t immutable_state_pages, rd_state_pages, tec_state_pages;
	pr_info("debug: calling virtcca_mig_capabilities_setup \n");

	res = tmi_get_mig_config();
	crc32_init();

	virtcca_mig_caps.max_migs = (uint32_t)(res >> 48) & 0xFFFF;

	immutable_state_pages = (uint32_t)(res >> 32) & 0xFFFF;

	rd_state_pages = (uint32_t)(res >> 16) & 0xFFFF;

	tec_state_pages = (uint32_t)res & 0xFFFF;
	pr_info(KERN_INFO "immutable_state_pages: %u\n", immutable_state_pages);
	pr_info(KERN_INFO "rd_state_pages : %u\n", rd_state_pages);
	pr_info(KERN_INFO "tec_state_pages: %u\n", tec_state_pages);
	/*
	 * The minimal number of pages required. It hould be large enough to
	 * store all the non-memory states.
	 */
	virtcca_mig_caps.nonmem_state_pages = max3(immutable_state_pages, rd_state_pages, tec_state_pages);

	return 0;
}

static void virtcca_mig_stream_get_virtcca_mig_attr(struct virtcca_mig_stream *stream,
	struct kvm_dev_virtcca_mig_attr *attr)
{
	attr->version = KVM_DEV_VIRTCCA_MIG_ATTR_VERSION;
	attr->max_migs = virtcca_mig_caps.max_migs;
	attr->buf_list_pages = stream->buf_list_pages;
}

static int virtcca_mig_stream_get_attr(struct kvm_device *dev, struct kvm_device_attr *attr)
{
	struct virtcca_mig_stream *stream = dev->private;
	u64 __user *uaddr = (u64 __user *)(long)attr->addr;

	switch (attr->group) {
	case KVM_DEV_VIRTCCA_MIG_ATTR: {
		struct kvm_dev_virtcca_mig_attr virtcca_mig_attr;

		if (attr->attr != sizeof(struct kvm_dev_virtcca_mig_attr)) {
			pr_err("Incompatible kvm_dev_get_tdx_mig_attr\n");
			return -EINVAL;
		}

		virtcca_mig_stream_get_virtcca_mig_attr(stream, &virtcca_mig_attr);
		if (copy_to_user(uaddr, &virtcca_mig_attr, sizeof(virtcca_mig_attr)))
			return -EFAULT;
		break;
	}
	default:
		return -EINVAL;
	}

	return 0;
}
/*  this func is to check and cut the max page num of a stream */
static int virtcca_mig_stream_set_virtcca_mig_attr(struct virtcca_mig_stream *stream,
	struct kvm_dev_virtcca_mig_attr *attr)
{
	uint32_t req_pages = attr->buf_list_pages;
	uint32_t min_pages = virtcca_mig_caps.nonmem_state_pages;
	pr_info("debug: calling virtcca_mig_stream_set_virtcca_mig_attr\n");

	if (req_pages > VIRTCCA_MIG_BUF_LIST_PAGES_MAX) {
		stream->buf_list_pages = VIRTCCA_MIG_BUF_LIST_PAGES_MAX;
		pr_warn("Cut the buf_list_npages to the max supported num\n");
	} else if (req_pages < min_pages) {
		stream->buf_list_pages = min_pages;
	} else {
		stream->buf_list_pages = req_pages;
	}
	pr_info("buf_list_pages is %d",stream->buf_list_pages);

	return 0;
}

static uint32_t crc32_table[CRC_LEN];

void crc32_init(void)
{
	for (uint32_t i = 0; i < CRC_LEN; i++) {
		uint32_t c = i;
		for (size_t j = 0; j < CRC_SHIFT; j++) {
			if (c & 1) {
				c = CRC_POLYNOMIAL ^ (c >> 1);
			} else {
				c >>= 1;
			}
		}
		crc32_table[i] = c;
	}
}

uint32_t crc32_compute(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < len; i++) {
		uint8_t index = (crc ^ data[i]) & 0xFF;
		crc = crc32_table[index] ^ (crc >> CRC_SHIFT);
	}
	return crc ^ 0xFFFFFFFF;
}

void dump_array_to_file(struct virtcca_cvm *cvm, uint64_t *crc_result, uint64_t *gpa_list, int gpa_nums, char *file_name)
{
	struct file *filp;
	loff_t pos = 0;
	char *buf;
	int i, len;
	ssize_t ret;

	filp = filp_open(file_name, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (IS_ERR(filp)) {
		pr_err("dump_array_to_file: failed to open file\n");
		return;
	}

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		filp_close(filp, NULL);
		return;
	}

	for (i = 0; i < gpa_nums; i++) {
		len = snprintf(buf, PAGE_SIZE, "gpa = 0x%llx crc = 0x%llx\n",
						gpa_list[i], crc_result[i]);
		ret = kernel_write(filp, buf, len, &pos);
		if (ret < 0) {
			pr_err("dump_array_to_file: write error at %d\n", i);
			break;
		}
	}

	kfree(buf);
	filp_close(filp, NULL);
}

void swiotlb_checksum(struct kvm *kvm)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	uint64_t *crc_result = NULL;
	uint64_t *gpa_list = NULL;
	uint64_t swiotlb_size, swiotlb_nums;
	char file_name[FILE_NAME_LEN], base_name[FILE_NAME_LEN];
	void *data;
	struct file *filp;
	unsigned int total;
	int ret;

	swiotlb_size = cvm->swiotlb_end - cvm->swiotlb_start;
	swiotlb_nums = swiotlb_size / SZ_4K;
	total = swiotlb_nums / SWIOTLB_CRC_LEN;

	gpa_list = (uint64_t *)kmalloc(SWIOTLB_CRC_LEN * sizeof(uint64_t), GFP_KERNEL_ACCOUNT);
	crc_result = (uint64_t *)kmalloc(SWIOTLB_CRC_LEN * sizeof(uint64_t), GFP_KERNEL_ACCOUNT);
	data = kmalloc(SZ_4K, GFP_KERNEL_ACCOUNT);
	if (!crc_result || !gpa_list || !data) {
		pr_err("virtcca_dump_checksum: kmalloc failed");
		goto out;
	}

	if (cvm->mig_state) {
		if (cvm->mig_state->mig_src == VIRTCCA_MIG_SRC) {
			strcpy(base_name, SWIOTLB_SRC_CRC_PATH);
		} else {
			strcpy(base_name, SWIOTLB_DST_CRC_PATH);
		}
	} else {
		pr_info("Not a migration cvm, return");
		goto out;
	}

	int try_count = 0;
	 while (try_count < MAX_TRIES) {
		if (try_count == 0)
			snprintf(file_name, sizeof(file_name), "%s", base_name);
		else
			snprintf(file_name, sizeof(file_name), "%s-%d", base_name, try_count);

		filp = filp_open(file_name, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (IS_ERR(filp)) {
			if (PTR_ERR(filp) == -EEXIST) {
				try_count++;
				continue;
			} else {
				pr_err("Failed to open file: %ld\n", PTR_ERR(filp));
				ret = PTR_ERR(filp);
				break;
			}
		}
		pr_info("File created: %s\n", file_name);
		filp_close(filp, NULL);
		break;
	}

	for (uint64_t i = 0; i < total; i++) {
		for (int j = 0; j < SWIOTLB_CRC_LEN; j++) {
			gpa_list[j] = cvm->swiotlb_start + (i * SWIOTLB_CRC_LEN + j) * SZ_4K;
			gfn_t gfn = gpa_list[j] >> PAGE_SHIFT;
			ret = kvm_read_guest_page(kvm, gfn, data, 0, SZ_4K);
			if (ret < 0) {
				pr_info("read swiotlb page failed, ret = %d", ret);
			}
			crc_result[j] = crc32_compute(data, SZ_4K);
		}
		dump_array_to_file(cvm, crc_result, gpa_list, SWIOTLB_CRC_LEN, file_name);
		touch_softlockup_watchdog();
		cond_resched();
	}

	pr_info("swiotlb_checksum success");
out:
	if (crc_result)
		kfree(crc_result);
	if (gpa_list)
		kfree(gpa_list);
	if (data)
		kfree(data);
}

void virtcca_dump_checksum(struct kvm *kvm)
{
	return;
}

static int virtcca_mig_stream_mbmd_setup(struct virtcca_mig_mbmd *mbmd)
{
	struct page *page;
	unsigned long mbmd_size = PAGE_SIZE;
	int order = get_order(mbmd_size);
	pr_info("debug: calling virtcca_mig_stream_mbmd_setup\n");

	page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, order);
	if (!page)
		return -ENOMEM;

	mbmd->data = page_address(page);
	mbmd->hpa_and_size = page_to_phys(page) | (mbmd_size - 1) << 52;

	return 0;
}

static void virtcca_mig_stream_buf_list_cleanup(struct virtcca_mig_buf_list *buf_list)
{
	int i;
	kvm_pfn_t pfn;
	struct page *page;
	pr_info("debug: calling virtcca_mig_stream_buf_list_cleanup");

	if (!buf_list->entries)
		return;

	for (i = 0; i < MAX_BUF_PAGES; i++) {
		pfn = buf_list->entries[i].pfn;
		if (!pfn)
			break;
		page = pfn_to_page(pfn);
		__free_page(page);
	}
	free_page((unsigned long)buf_list->entries);
}

static int virtcca_mig_stream_buf_list_alloc(struct virtcca_mig_buf_list *buf_list)
{
	struct page *page;
	pr_info("debug: calling virtcca_mig_stream_buf_list_alloc\n");

	/*
	 * Allocate the buf list page, which has 512 entries pointing to up to
	 * 512 pages used as buffers to export/import migration data.
	 */
	page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	buf_list->entries = page_address(page);
	buf_list->hpa = page_to_phys(page);

	return 0;
}

static int virtcca_mig_stream_buf_list_setup(struct virtcca_mig_buf_list *buf_list, uint32_t npages)
{
	int i;
	struct page *page;
	pr_info("debug : calling virtcca_mig_stream_buf_list_setup\n");

	if (!npages) {
		pr_err("Userspace should set_attr on the device first\n");
		return -EINVAL;
	}

	if (virtcca_mig_stream_buf_list_alloc(buf_list))
		return -ENOMEM;

	for (i = 0; i < npages; i++) {
		page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!page) {
			virtcca_mig_stream_buf_list_cleanup(buf_list);
			return -ENOMEM;
		}
		buf_list->entries[i].pfn = page_to_pfn(page);
	}

	/* Mark unused entries as invalid */
	for (i = npages; i < MAX_BUF_PAGES; i++)
		buf_list->entries[i].invalid = true;

	return 0;
}

static int
virtcca_mig_stream_page_list_setup(struct virtcca_mig_page_list *page_list,
	struct virtcca_mig_buf_list *buf_list, uint32_t npages)
{
	struct page *page;
	uint32_t i;
	pr_info("debug : calling virtcca_mig_stream_page_list_setup!");

	page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	page_list->entries = page_address(page);
	page_list->info.pfn = page_to_pfn(page);

	/* Reuse the buffers from the buffer list for pages list */
	for (i = 0; i < npages; i++) {
		page_list->entries[i] = __pfn_to_phys(buf_list->entries[i].pfn);
	}
	page_list->info.last_entry = npages - 1;

	return 0;
}

/* this function is used to setup the page list for migration */
static int virtcca_mig_stream_gpa_list_setup(struct virtcca_mig_gpa_list *gpa_list)
{
	struct page *page;
	pr_info("debug : calling virtcca_mig_stream_gpa_list_setup \n");

	page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	gpa_list->info.pfn = page_to_pfn(page);
	gpa_list->entries = page_address(page);

	return 0;
}

static int virtcca_mig_stream_mac_list_setup(struct virtcca_mig_mac_list *mac_list)
{
	struct page *page;
	pr_info("debug : calling virtcca_mig_stream_mac_list_setup \n");

	page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, 0);
	if (!page)
		return -ENOMEM;

	mac_list->entries = page_address(page);
	mac_list->hpa = page_to_phys(page);

	return 0;
}

static int virtcca_mig_stream_setup(struct virtcca_mig_stream *stream, bool mig_src)
{
	int ret;

	ret = virtcca_mig_stream_mbmd_setup(&stream->mbmd);
	if (ret)
		goto err_mbmd;

	ret = virtcca_mig_stream_buf_list_setup(&stream->mem_buf_list, stream->buf_list_pages);
	if (ret)
		goto err_mem_buf_list;

	ret = virtcca_mig_stream_page_list_setup(&stream->page_list,
		&stream->mem_buf_list, stream->buf_list_pages);
	if (ret)
		goto err_page_list;

	ret = virtcca_mig_stream_gpa_list_setup(&stream->gpa_list);
	if (ret)
		goto err_gpa_list;

	ret = virtcca_mig_stream_mac_list_setup(&stream->mac_list[0]);
	if (ret)
		goto err_mac_list0;
	/*
	 * The 2nd mac list is needed only when the buf list uses more than
	 * 256 entries
	 */
	if (stream->buf_list_pages > MAX_MAC_PAGES_PER_ARR) {
		ret = virtcca_mig_stream_mac_list_setup(&stream->mac_list[1]);
		if (ret)
			goto err_mac_list1;
	}

	/* The lists used by the destination rd only */
	if (!mig_src) {
		ret = virtcca_mig_stream_buf_list_alloc(&stream->dst_buf_list);
		if (ret)
			goto err_dst_buf_list;
		ret = virtcca_mig_stream_buf_list_alloc(&stream->import_mem_buf_list);
		if (ret)
			goto err_import_mem_buf_list;
	}

	return 0;
err_import_mem_buf_list:
	free_page((unsigned long)stream->dst_buf_list.entries);
err_dst_buf_list:
	if (stream->mac_list[1].entries)
		free_page((unsigned long)stream->mac_list[1].entries);
err_mac_list1:
	free_page((unsigned long)stream->mac_list[0].entries);
err_mac_list0:
	free_page((unsigned long)stream->gpa_list.entries);
err_gpa_list:
	free_page((unsigned long)stream->page_list.entries);
err_page_list:
	virtcca_mig_stream_buf_list_cleanup(&stream->mem_buf_list);
err_mem_buf_list:
	free_page((unsigned long)stream->mbmd.data);
err_mbmd:
	pr_err("%s failed\n", __func__);
	return ret;
}

/* check the attr is enough */
static int virtcca_mig_stream_set_attr(struct kvm_device *dev, struct kvm_device_attr *attr)
{
	struct virtcca_cvm *cvm = dev->kvm->arch.virtcca_cvm;
	struct virtcca_mig_stream *stream = dev->private;
	u64 __user *uaddr = (u64 __user *)(long)attr->addr;
	int ret;
	pr_info("debug : calling virtcca_mig_stream_set_attr\n");

	switch (attr->group) {
	case KVM_DEV_VIRTCCA_MIG_ATTR: {
		struct kvm_dev_virtcca_mig_attr virtcca_mig_attr;

		if (copy_from_user(&virtcca_mig_attr, uaddr, sizeof(virtcca_mig_attr)))
			return -EFAULT;

		if (virtcca_mig_attr.version != KVM_DEV_VIRTCCA_MIG_ATTR_VERSION)
			return -EINVAL;

		ret = virtcca_mig_stream_set_virtcca_mig_attr(stream, &virtcca_mig_attr);
		if (ret)
			break;

		ret = virtcca_mig_stream_setup(stream,
					   virtcca_is_migration_source(cvm));
		break;
	}
	default:
		return -EINVAL;
	}

	return ret;
}

static bool virtcca_mig_stream_in_mig_buf_list(uint32_t i, uint32_t max_pages)
{
	if (i >= VIRTCCA_MIG_STREAM_BUF_LIST_MAP_OFFSET && i < VIRTCCA_MIG_STREAM_BUF_LIST_MAP_OFFSET + max_pages)
		return true;

	return false;
}

static vm_fault_t virtcca_mig_stream_fault(struct vm_fault *vmf)
{
	struct kvm_device *dev = vmf->vma->vm_file->private_data;
	struct virtcca_mig_stream *stream = dev->private;
	struct page *page;
	kvm_pfn_t pfn;
	uint32_t i;

	/* See linear_page_index for pgoff */
	if (vmf->pgoff == VIRTCCA_MIG_STREAM_MBMD_MAP_OFFSET) {
		page = virt_to_page(stream->mbmd.data);
	} else if (vmf->pgoff == VIRTCCA_MIG_STREAM_GPA_LIST_MAP_OFFSET) {
		page = virt_to_page(stream->gpa_list.entries);
	} else if (vmf->pgoff == VIRTCCA_MIG_STREAM_MAC_LIST_MAP_OFFSET ||
		   vmf->pgoff == VIRTCCA_MIG_STREAM_MAC_LIST_MAP_OFFSET + 1) {
		i = vmf->pgoff - VIRTCCA_MIG_STREAM_MAC_LIST_MAP_OFFSET;
		if (stream->mac_list[i].entries) {
			page = virt_to_page(stream->mac_list[i].entries);
		} else {
			pr_err("%s: mac list page %d not allocated\n",
				__func__, i);
			return VM_FAULT_SIGBUS;
		}
	} else if (virtcca_mig_stream_in_mig_buf_list(vmf->pgoff, stream->buf_list_pages)) {
		i = vmf->pgoff - VIRTCCA_MIG_STREAM_BUF_LIST_MAP_OFFSET;
		pfn = stream->mem_buf_list.entries[i].pfn;
		page = pfn_to_page(pfn);
	} else {
		pr_err("%s: VM_FAULT_SIGBUS\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	get_page(page);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct virtcca_mig_stream_ops = {
	.fault = virtcca_mig_stream_fault,
};

static int virtcca_mig_stream_mmap(struct kvm_device *dev, struct vm_area_struct *vma)
{
	vma->vm_ops = &virtcca_mig_stream_ops;
	return 0;
}

/* this function tmi call is just a dummy function for now */
static int virtcca_mig_export_state_immutable(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct virtcca_mig_page_list *page_list = &stream->page_list;
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	union virtcca_mig_stream_info stream_info = {.val = 0};
	struct arm_smccc_res ret;
	pr_info("debug: calling kvm ioctl virtcca_mig_export_state_immutable \n");

	ret = tmi_export_immutable(cvm->rd, stream->mbmd.hpa_and_size, page_list->info.val, stream_info.val);

	if (ret.a1 == TMI_SUCCESS) {
		stream->idx = stream->mbmd.data->migs_index;

		if (copy_to_user(data, &ret.a2, sizeof(uint64_t))) {
			return -EFAULT;
		}
	} else {
		pr_err("%s: failed, err=%lx\n", __func__, ret.a1);
		return -EIO;
	}

	ret.a1 = tmi_get_swiotlb(cvm->rd, (uint64_t)&cvm->swiotlb_start, (uint64_t)&cvm->swiotlb_end);
	if (ret.a1) {
		pr_err("tmi_get_swiotlb: failed, err=%lx\n", ret.a1);
		return -EIO;
	}

	return 0;
}

static int virtcca_mig_import_state_immutable(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct virtcca_mig_page_list *page_list = &stream->page_list;
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	union virtcca_mig_stream_info stream_info = {.val = 0};

	uint64_t ret, npages;

	if (copy_from_user(&npages, (void __user *)data, sizeof(uint64_t)))
		return -EFAULT;

	page_list->info.last_entry = npages - 1;

	ret = tmi_import_immutable(cvm->rd, stream->mbmd.hpa_and_size, page_list->info.val, stream_info.val);

	if (ret == TMI_SUCCESS) {
		stream->idx = stream->mbmd.data->migs_index;
	} else {
		pr_err("%s: failed, err=%llx\n", __func__, ret);
		return -EIO;
	}

	ret = kvm_cvm_mig_map_range(kvm);
	if (ret) {
		pr_err("kvm_cvm_mig_map_range: failed, err=%llx\n", ret);
		return -EIO;
	}

	ret = tmi_get_swiotlb(cvm->rd, (uint64_t)&cvm->swiotlb_start, (uint64_t)&cvm->swiotlb_end);
	if (ret) {
		pr_err("tmi_get_swiotlb: failed, err=%llx\n", ret);
		return -EIO;
	}

	return ret;
}

/* this function tmi call is just a dummy function for now */
static int virtcca_mig_export_state_mutable(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct virtcca_mig_page_list *page_list = &stream->page_list;
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	union virtcca_mig_stream_info stream_info = {.val = 0};

	struct arm_smccc_res ret;

	ret = tmi_export_mutable(cvm->rd, stream->mbmd.hpa_and_size, page_list->info.val, stream_info.val);

	if (ret.a1 == TMI_SUCCESS) {
		if (copy_to_user(data, &ret.a2, sizeof(uint64_t))) {
			return -EFAULT;
		}
	} else {
		pr_err("%s: failed, err=%lx\n", __func__, ret.a1);
		return -EIO;
	}

	return 0;
}

static int virtcca_mig_import_state_mutable(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct virtcca_mig_page_list *page_list = &stream->page_list;
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	union virtcca_mig_stream_info stream_info = {.val = 0};
	uint64_t ret, npages;

	if (copy_from_user(&npages, (void __user *)data, sizeof(uint64_t)))
		return -EFAULT;

	page_list->info.last_entry = npages - 1;

	ret = tmi_import_mutable(cvm->rd, stream->mbmd.hpa_and_size, page_list->info.val, stream_info.val);

	if (ret != TMI_SUCCESS) {
		pr_err("%s: failed, err=%llx\n", __func__, ret);
		return -EIO;
	}

	return 0;
}

static void virtcca_mig_buf_list_set_valid(struct virtcca_mig_buf_list *mem_buf_list,
						uint64_t num)
{
	int i;

	for (i = 0; i < num; i++)
		mem_buf_list->entries[i].invalid = false;

	for (i = num; i < MAX_BUF_PAGES; i++) {
		if (!mem_buf_list->entries[i].invalid)
			mem_buf_list->entries[i].invalid = true;
		else
			break;
	}
}

static int virtcca_mig_mem_param_setup(struct tmi_mig_mem *mig_mem_param)
{
	struct page *page;
	unsigned long mig_mem_param_size = PAGE_SIZE;
	int order = get_order(mig_mem_param_size);

	page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, order);
	if (!page)
		return -ENOMEM;

	mig_mem_param->data = page_address(page);
	mig_mem_param->addr_and_size = page_to_phys(page) | (mig_mem_param_size - 1) << 52;

	return 0;
}

static int64_t virtcca_mig_stream_export_mem(struct kvm *kvm,
					 struct virtcca_mig_stream *stream,
					 uint64_t __user *data)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct virtcca_mig_state *mig_state = cvm->mig_state;
	struct virtcca_mig_gpa_list *gpa_list = &stream->gpa_list;
	union virtcca_mig_stream_info stream_info = {.val = 0};

	struct tmi_mig_mem mig_mem_param = {0};
	struct tmi_mig_mem_data *mig_mem_param_data;
	uint64_t npages, gpa_list_info_val;
	int ret;

	struct arm_smccc_res tmi_res = { 0 };
	if (mig_state->bugged)
		return -EBADF;

	if (copy_from_user(&npages, (void __user *)data, sizeof(uint64_t)))
		return -EFAULT;

	if (npages > stream->buf_list_pages)
		return -EINVAL;

	ret = virtcca_mig_mem_param_setup(&mig_mem_param);
	if (ret) {
		goto out;
	}

	mig_mem_param_data = mig_mem_param.data;
	if (!mig_mem_param_data) {
		ret = -ENOMEM;
		goto out;
	}

	gpa_list->info.first_entry = 0;
	gpa_list->info.last_entry = npages - 1;
	virtcca_mig_buf_list_set_valid(&stream->mem_buf_list, npages);
	stream_info.index = stream->idx;

	mig_mem_param_data->gpa_list_info = gpa_list->info.val;
	mig_mem_param_data->mig_buff_list_pa = stream->mem_buf_list.hpa;
	mig_mem_param_data->mig_cmd = stream_info.val;
	mig_mem_param_data->mbmd_hpa_and_size = stream->mbmd.hpa_and_size;
	mig_mem_param_data->mac_pa0 = stream->mac_list[0].hpa;
	mig_mem_param_data->mac_pa1 = stream->mac_list[1].hpa;

	tmi_res = tmi_export_mem(cvm->rd, mig_mem_param.addr_and_size);

	ret = tmi_res.a1;
	gpa_list_info_val = tmi_res.a2;

	if (ret == TMI_SUCCESS) {
		if (copy_to_user(data, &gpa_list_info_val, sizeof(uint64_t)))
			return -EFAULT;
	} else {
		pr_err("%s: err=%d, gfn=%llx\n",
			__func__, ret, (uint64_t)gpa_list->entries[0].gfn);
		return -EIO;
	}

out:
	if (mig_mem_param.data) {
		free_pages((unsigned long)mig_mem_param.data, get_order(PAGE_SIZE));
	}

	return ret;
}

static int virtcca_mig_stream_import_mem(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct virtcca_mig_state *mig_state = cvm->mig_state;
	struct virtcca_mig_gpa_list *gpa_list = &stream->gpa_list;
	union virtcca_mig_stream_info stream_info = {.val = 0};

	struct tmi_mig_mem mig_mem_param = {0};
	struct tmi_mig_mem_data *mig_mem_param_data;

	uint64_t npages=0, gpa_list_info_val=0, ret=0;
	struct arm_smccc_res tmi_res;
	if (mig_state->bugged)
		return -EBADF;

	if (copy_from_user(&npages, (void __user *)data, sizeof(uint64_t))) {
		return -EFAULT;
	}

	if (npages > stream->buf_list_pages)
		return -EINVAL;

	ret = virtcca_mig_mem_param_setup(&mig_mem_param);
	if (ret) {
		goto out;
	}

	mig_mem_param_data = mig_mem_param.data;
	if (!mig_mem_param_data) {
		ret = -ENOMEM;
		goto out;
	}

	gpa_list->info.first_entry = 0;
	gpa_list->info.last_entry = npages - 1;
	virtcca_mig_buf_list_set_valid(&stream->mem_buf_list, npages);
	stream_info.index = stream->idx;

	mig_mem_param_data->gpa_list_info = gpa_list->info.val;
	mig_mem_param_data->mig_buff_list_pa = stream->mem_buf_list.hpa;
	mig_mem_param_data->mig_cmd = stream_info.val;
	mig_mem_param_data->mbmd_hpa_and_size = stream->mbmd.hpa_and_size;
	mig_mem_param_data->mac_pa0 = stream->mac_list[0].hpa;
	mig_mem_param_data->mac_pa1 = stream->mac_list[1].hpa;

 	tmi_res = tmi_import_mem(cvm->rd, mig_mem_param.addr_and_size);

	ret = tmi_res.a1;
	gpa_list_info_val = tmi_res.a2;

	if (ret == TMI_SUCCESS) {
		if (copy_to_user(data, &gpa_list_info_val, sizeof(uint64_t)))
			return -EFAULT;
	} else {
		pr_err("%s: err=%llx, gfn=%llx\n",
			__func__, ret, (uint64_t)gpa_list->entries[0].gfn);
		return -EIO;
	}

out:
	if (mig_mem_param.data) {
		free_pages((unsigned long)mig_mem_param.data, get_order(PAGE_SIZE));
	}

	return ret;
}

static int virtcca_mig_memslot_param_setup(struct tmi_mig_memslot *mig_mem_param)
{
	struct page *page;
	unsigned long mig_mem_param_size = PAGE_SIZE;
	int order = get_order(mig_mem_param_size);

	page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, order);
	if (!page)
		return -ENOMEM;

	mig_mem_param->data = page_address(page);
	mig_mem_param->addr_and_size = page_to_phys(page) | (mig_mem_param_size - 1) << 52;

	return 0;
}

void virtcca_set_tmm_memslot(struct kvm *kvm, struct kvm_memory_slot *memslot)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct tmi_mig_memslot mig_memslot_param = {0};
	struct tmi_mig_memslot_data *mig_memslot_param_data;
	struct page *dirty_bitmap_page;
	unsigned int dirty_bitmap_list_len;
	uint64_t dirty_bitmap_addr;
	int ret;

	if (memslot->base_gfn << PAGE_SHIFT < cvm->ipa_start) {
		return;
	}

	ret = virtcca_mig_memslot_param_setup(&mig_memslot_param);
	if (ret) {
		return;
	}

	mig_memslot_param_data = mig_memslot_param.data;
	if (!mig_memslot_param_data) {
		ret = -ENOMEM;
		return;
	}

	unsigned long bitmap_size_bytes = kvm_dirty_bitmap_bytes(memslot);
	dirty_bitmap_list_len = DIV_ROUND_UP(bitmap_size_bytes, SZ_2M);

	dirty_bitmap_addr = (uint64_t)memslot->dirty_bitmap;
	for (int i = 0; i < dirty_bitmap_list_len; i++) {
		dirty_bitmap_page = vmalloc_to_page((uint64_t *)dirty_bitmap_addr);
		mig_memslot_param_data->dirty_bitmap_list[i] = page_to_phys(dirty_bitmap_page);
		dirty_bitmap_addr += SZ_2M;
	}
	mig_memslot_param_data->base_gfn = memslot->base_gfn;
	mig_memslot_param_data->npages = memslot->npages;
	mig_memslot_param_data->memslot_id = memslot->id;

	tmi_set_tmm_memslot(cvm->rd, mig_memslot_param.addr_and_size);
}

static int virtcca_mig_export_track(struct kvm *kvm, struct virtcca_mig_stream *stream, uint64_t __user *data)
{
	union virtcca_mig_stream_info stream_info = {.val = 0};
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	uint64_t in_order, ret;

	if (copy_from_user(&in_order, (void __user *)data, sizeof(uint64_t)))
		return -EFAULT;

	/*
	 * Set the in_order bit if userspace requests to generate a start
	 * token by sending a non-0 value through tdx_cmd.data.
	 */
	stream_info.in_order = !!in_order;
	ret = tmi_export_track(cvm->rd, stream->mbmd.hpa_and_size, stream_info.val);
	if (ret != TMI_SUCCESS) {
		pr_err("%s: failed, err=%llx\n", __func__, ret);
		return -EIO;
	}
	return 0;
}

static inline bool
virtcca_mig_epoch_is_start_token(struct virtcca_mig_mbmd_data *data)
{
	return data->mig_epoch == VIRTCCA_MIG_EPOCH_START_TOKEN;
}

static int virtcca_mig_import_track(struct kvm *kvm,
				struct virtcca_mig_stream *stream)
{
	union virtcca_mig_stream_info stream_info = {.val = 0};
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	uint64_t ret;

	ret = tmi_import_track(cvm->rd, stream->mbmd.hpa_and_size, stream_info.val);
	if (ret != TMI_SUCCESS) {
		pr_err("tmi_import_track failed, err=%llx\n", ret);
		return -EIO;
	}
	return 0;
}

static int virtcca_mig_import_end(struct kvm *kvm)
{
	uint64_t ret;
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;

	if (!cvm) {
		pr_err("%s: cvm is not initialized\n", __func__);
		return -EINVAL;
	}

	ret = tmi_import_commit(cvm->rd);

	if (ret != TMI_SUCCESS) {
		pr_err("%s: failed, err=%llx\n", __func__, ret);
		return -EIO;
	}

	WRITE_ONCE(cvm->state, CVM_STATE_ACTIVE);

	return 0;
}

static int virtcca_mig_export_state_tec(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct kvm_vcpu *vcpu;
	struct virtcca_cvm_tec *tec;
	struct virtcca_mig_state *mig_state = kvm->arch.virtcca_cvm->mig_state;
	union virtcca_mig_stream_info stream_info = {.val = 0};
	struct arm_smccc_res ret;

	if (mig_state->vcpu_export_next_idx >= atomic_read(&kvm->online_vcpus)) {
		pr_err("%s: vcpu_export_next_idx %d >= online_vcpus %d\n",
			__func__, mig_state->vcpu_export_next_idx,
			atomic_read(&kvm->online_vcpus));
		return -EINVAL;
	}

	vcpu = kvm_get_vcpu(kvm, mig_state->vcpu_export_next_idx);
	tec = &vcpu->arch.tec;

	stream_info.index = stream->idx;


	ret = tmi_export_tec(tec->tec, stream->mbmd.hpa_and_size, stream->page_list.info.val, stream_info.val);

	if (ret.a1 == TMI_SUCCESS) {
		mig_state->vcpu_export_next_idx++;
		if (copy_to_user(data, &(ret.a2), sizeof(uint64_t))) {
			return -EFAULT;
		}
	} else {
		pr_err("%s: failed, err=%lx\n", __func__, ret.a1);
		return -EIO;
	}

	return 0;
}

static uint16_t tdx_mig_mbmd_get_vcpu_idx(struct virtcca_mig_mbmd_data *data)
{
	return *(uint16_t *)data->type_specific_info;
}

static int virtcca_mig_import_state_tec(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct kvm_vcpu *vcpu;
	struct virtcca_cvm_tec *tec;
	union virtcca_mig_stream_info stream_info = {.val = 0};
	uint64_t ret;
	uint64_t npages;
	uint16_t vcpu_idx;

	if (copy_from_user(&npages, (void __user *)data, sizeof(uint64_t)))
		return -EFAULT;

	stream->page_list.info.last_entry = npages - 1;

	vcpu_idx = tdx_mig_mbmd_get_vcpu_idx(stream->mbmd.data);
	vcpu = kvm_get_vcpu(kvm, vcpu_idx);
	tec = &vcpu->arch.tec;

	ret = tmi_import_tec(tec->tec, stream->mbmd.hpa_and_size, stream->page_list.info.val, stream_info.val);
	if (ret != TMI_SUCCESS) {
		pr_err("%s: failed, err=%llx\n", __func__, ret);
		return -EIO;
	}

	return 0;
}

static int virtcca_mig_get_mig_info(struct kvm *kvm, uint64_t __user *data)
{
	virtCCAMigInfo migInfo;
	struct virtcca_cvm *cvm;
	cvm = kvm->arch.virtcca_cvm;
	migInfo.swiotlb_start = cvm->swiotlb_start;
	migInfo.swiotlb_end = cvm->swiotlb_end;

	if (copy_to_user(data, &(migInfo), sizeof(virtCCAMigInfo))) {
		return -EFAULT;
	}

	return 0;
}

static int virtcca_mig_is_zero_page(struct kvm *kvm, 
		struct virtcca_mig_stream *stream, uint64_t __user *data)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct virtcca_mig_gpa_list *gpa_list = &stream->gpa_list;

	int ret;
	struct arm_smccc_res tmi_res = { 0 };
	bool is_zero_page = false;

	tmi_res = tmi_is_zero_page(cvm->rd, gpa_list->info.val);

	ret = tmi_res.a1;
	if (tmi_res.a2) {
		is_zero_page = true;
	}

	if (ret == TMI_SUCCESS) {
		if (copy_to_user(data, &is_zero_page, sizeof(bool)))
			return -EFAULT;
	} else {
		pr_err("%s: err=%d, gfn=%llx\n",
			__func__, ret, (uint64_t)gpa_list->entries[0].gfn);
		return -EIO;
	}

	return ret;
}

static int virtcca_mig_import_zero_page(struct kvm *kvm, 
		struct virtcca_mig_stream *stream, uint64_t __user *data)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	int ret;
	struct arm_smccc_res tmi_res = { 0 };
	uint64_t gpa;

	gpa = (uint64_t)data;

	tmi_res = tmi_import_zero_page(cvm->rd, gpa);

	ret = tmi_res.a1;

	if (ret) {
		pr_err("%s: err=%d\n",
			__func__, ret);
		return -EIO;
	}

	return ret;
}

/* add qemu ioctl struct to fit this func */
static long virtcca_mig_stream_ioctl(struct kvm_device *dev, unsigned int ioctl, unsigned long arg)
{
	struct kvm *kvm = dev->kvm;
	struct virtcca_mig_stream *stream = dev->private;
	void __user *argp = (void __user *)arg;
	struct kvm_virtcca_mig_cmd cvm_cmd;
	int r;

	if (copy_from_user(&cvm_cmd, argp, sizeof(struct kvm_virtcca_mig_cmd)))
		return -EFAULT;

	if (ioctl != KVM_CVM_MIG_IOCTL)
		return -EINVAL;

	switch (cvm_cmd.id) {
	case KVM_CVM_MIG_EXPORT_STATE_IMMUTABLE:
		r = virtcca_mig_export_state_immutable(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_IMPORT_STATE_IMMUTABLE:
		r = virtcca_mig_import_state_immutable(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_EXPORT_STATE_MUTABLE:
		r = virtcca_mig_export_state_mutable(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_IMPORT_STATE_MUTABLE:
		r = virtcca_mig_import_state_mutable(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_EXPORT_MEM:
		r = virtcca_mig_stream_export_mem(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_IMPORT_MEM:
		r = virtcca_mig_stream_import_mem(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_EXPORT_TRACK:
		r = virtcca_mig_export_track(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_IMPORT_TRACK:
		r = virtcca_mig_import_track(kvm, stream);
		break;
	case KVM_CVM_MIG_EXPORT_STATE_TEC:
		r = virtcca_mig_export_state_tec(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_IMPORT_STATE_TEC:
		r = virtcca_mig_import_state_tec(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_IMPORT_END:
		r = virtcca_mig_import_end(kvm);
		break;
	case KVM_CVM_MIG_CRC:
		r = 0;
		virtcca_dump_checksum(kvm);
		break;
	case KVM_CVM_MIG_GET_MIG_INFO:
		r = virtcca_mig_get_mig_info(kvm, (uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_IS_ZERO_PAGE:
		r = virtcca_mig_is_zero_page(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	case KVM_CVM_MIG_IMPORT_ZERO_PAGE:
		r = virtcca_mig_import_zero_page(kvm, stream,
					(uint64_t __user *)cvm_cmd.data);
		break;
	default:
		r = -EINVAL;
	}

	return r;
}

static int virtcca_mig_do_stream_create(struct kvm *kvm, struct virtcca_mig_stream *stream, hpa_t *migsc_addr)
{
	u64 numa_set = kvm_get_host_numa_set_by_vcpu(0, kvm);
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	hpa_t migsc_pa = 0;

	/*
	 * This migration stream has been created, e.g. the previous migration
	 * session is aborted and the migration stream is retained during the
	 * TD guest lifecycle (required by the TDX migration architecture for
	 * later re-migration). No need to proceed to the creation in this
	 * case.
	 */
	if (!migsc_addr) {
		pr_err("invalid migsc_addr!");
		return -1;
	}

	if (*migsc_addr)
		return 0;

	/* now just create stream in tmm */
	migsc_pa = tmi_mig_stream_create(cvm->rd, numa_set);
	if (!migsc_pa) {
		kvm_err("virtcca mig stream create failed!\n");
	}

	*migsc_addr = migsc_pa;
	return 0;
}

static int virtcca_mig_session_init(struct kvm *kvm)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct virtcca_mig_state *mig_state = cvm->mig_state;
	struct virtcca_mig_gpa_list *blockw_gpa_list = &mig_state->blockw_gpa_list;
	int ret = 0;

	if (virtcca_mig_do_stream_create(kvm, &mig_state->backward_stream, &mig_state->backward_migsc_paddr))
		return -EIO;

	if (virtcca_is_migration_source(cvm))
		ret = virtcca_mig_stream_gpa_list_setup(blockw_gpa_list);

	return ret;
}

static void virtcca_mig_session_exit(struct virtcca_mig_state *mig_state)
{
	if (mig_state->blockw_gpa_list.entries) {
		free_pages((uint64_t)mig_state->blockw_gpa_list.entries, 0);
		mig_state->blockw_gpa_list.entries = NULL;
		mig_state->blockw_gpa_list.info.pfn = 0;
	}

	return;
}

static int virtcca_mig_stream_create(struct kvm_device *dev, u32 type)
{
	struct kvm *kvm = dev->kvm;
	struct virtcca_cvm *cvm = dev->kvm->arch.virtcca_cvm;
	struct virtcca_mig_state *mig_state = cvm->mig_state;
	struct virtcca_mig_stream *stream;
	int ret;

	ret = tmi_get_swiotlb(cvm->rd, (uint64_t)&cvm->swiotlb_start, (uint64_t)&cvm->swiotlb_end);
	if (ret != TMI_SUCCESS) {
		kvm_err("%s: failed, err=%i\n", __func__, ret);
		return -EIO;
	}

	stream = (struct virtcca_mig_stream *)kzalloc(sizeof(struct virtcca_mig_stream), GFP_KERNEL_ACCOUNT);
	if (!stream)
		return -ENOMEM;

	dev->private = stream;
	stream->idx = atomic_inc_return(&mig_state->streams_created) - 1; /* set the stream idx of the cvm */

	if (!stream->idx) {
		ret = virtcca_mig_session_init(kvm); /* if is the first stream, call this func */
		if (ret)
			goto err_mig_session_init;

		WARN_ON_ONCE(mig_state->default_stream);
		mig_state->default_stream = stream;
	}

	ret = virtcca_mig_do_stream_create(kvm, stream, &mig_state->migsc_paddrs[stream->idx]);
	if (ret)
		goto err_stream_create;

	return 0;
err_stream_create:
	virtcca_mig_session_exit(mig_state);
err_mig_session_init:
	atomic_dec(&mig_state->streams_created);
	kfree(stream);
	return ret;
}

void virtcca_mig_state_release(struct virtcca_cvm *cvm)
{
	struct virtcca_mig_state *mig_state = cvm->mig_state;
	if (!mig_state) {
		return;
	}

	atomic_dec(&mig_state->streams_created);
	if (!atomic_read(&mig_state->streams_created))
		virtcca_mig_session_exit(mig_state);
}


static void virtcca_mig_stream_release(struct kvm_device *dev)
{
	struct virtcca_mig_stream *stream = dev->private;

	free_page((unsigned long)stream->mbmd.data);
	virtcca_mig_stream_buf_list_cleanup(&stream->mem_buf_list);
	free_page((unsigned long)stream->page_list.entries);
	free_page((unsigned long)stream->gpa_list.entries);
	free_page((unsigned long)stream->mac_list[0].entries);
	/*
	 * The 2nd mac list page is allocated conditionally when
	 * stream->buf_list_pages is larger than 256.
	 */
	if (stream->mac_list[1].entries)
		free_page((unsigned long)stream->mac_list[1].entries);
	if (stream->dst_buf_list.entries)
		free_page((unsigned long)stream->dst_buf_list.entries);
	if (stream->import_mem_buf_list.entries)
		free_page((unsigned long)stream->import_mem_buf_list.entries);
	/*print the elements of the stream*/
	kfree(stream);
}

int virtcca_mig_state_create(struct virtcca_cvm *cvm)
{
	struct virtcca_mig_state *mig_state = cvm->mig_state;
	mig_state = NULL;
	hpa_t *migsc_paddrs = NULL;;
	cvm->mig_cvm_info = NULL;
	pr_info("calling virtcca_mig_state_create\n");

	mig_state = kzalloc(sizeof(struct virtcca_mig_state), GFP_KERNEL_ACCOUNT);
	if (!mig_state) {
		goto out;
	}

	migsc_paddrs = kcalloc(virtcca_mig_caps.max_migs, sizeof(hpa_t), GFP_KERNEL_ACCOUNT);
	if (!migsc_paddrs) {
		goto out;
	}

	cvm->mig_cvm_info = (struct mig_cvm *)kzalloc(sizeof(struct mig_cvm), GFP_KERNEL_ACCOUNT);
	mig_state->mig_src = cvm->params->mig_src;
	if (!cvm->mig_cvm_info) {
		goto out;
	}

	mig_state->migsc_paddrs = migsc_paddrs;
	cvm->mig_state = mig_state;

	mig_state->crc_start = 0;
	mig_state->crc_end = 0;
	mig_state->crc_size = 0;

	return 0;

out:
	pr_err("virtcca_mig_state_create failed");
	if (mig_state) {
		kfree(mig_state);
	}
	if (migsc_paddrs) {
		kfree(migsc_paddrs);
	}
	if (cvm->mig_cvm_info) {
		kfree(cvm->mig_cvm_info);
	}
	return -ENOMEM;
}

void virtcca_crc_set(uint64_t ipa_start, uint64_t ipa_end, uint64_t size, int mem_type)
{
	if (ipa_start < ipa_end || ipa_end - ipa_start < size) {
		pr_warn("Failed to enable virtcca migration memory integrity check: invalid params");
		return;
	}

	if (mem_type) {
		g_sec_crc_start = ipa_start;
		g_sec_crc_end = ipa_end;
		g_sec_crc_granularity = size;
		pr_info("Virtcca migration secure memory integrity check enabled: 0x%llx - 0x%llx, granularity: 0x%llx",
			g_sec_crc_start, g_sec_crc_end, g_sec_crc_granularity);
	} else {
		g_ns_crc_start = ipa_start;
		g_ns_crc_end = ipa_end;
		g_ns_crc_granularity = size;
		pr_info("Virtcca migration no-secure memory integrity check enabled: 0x%llx - 0x%llx, granularity: 0x%llx",
			g_ns_crc_start, g_ns_crc_end, g_ns_crc_granularity);
	}
}
EXPORT_SYMBOL_GPL(virtcca_crc_set);

int virtcca_migvm_init(struct virtcca_cvm *cvm, uint64_t numa_set)
{
	uint64_t ret;
	pr_info("calling virtcca_migvm_init\n");
	if (cvm->params->migration_migvm_cap == 1) {
		ret = tmi_migvm_init(cvm->rd, numa_set);
		if (ret != TMI_SUCCESS) {
			kvm_err("%s: failed, err=%llx\n", __func__, ret);
			return 1; /* failed */
		}

		cvm->mig_cvm_info = kzalloc(sizeof(struct mig_cvm), GFP_KERNEL_ACCOUNT);
		if (!cvm->mig_cvm_info) {
			return -ENOMEM;
		}

		cvm->mig_cvm_info->is_migvm = true;
	}
	return 0;
}

int virtcca_migvm_destroy(struct virtcca_cvm *cvm)
{
	uint64_t ret;

	if (cvm->mig_cvm_info)
		kfree(cvm->mig_cvm_info);

	ret = tmi_migvm_clean(cvm->rd);
	if (ret != TMI_SUCCESS) {
		kvm_err("%s: failed, err=%llx\n", __func__, ret);
		return 1; /* failed */
	}
	return 0;
}

static int notify_migcvm_agent(uint64_t cid, uint64_t guest_rd) {
	struct socket *sock = NULL;
	int ret = 0;
	struct sockaddr_vm sa = {
		.svm_family = AF_VSOCK,
		.svm_cid = cid,
		.svm_port = MIGCVM_AGENT_PORT
	};

	printk("calling notify_migcvm_agent, the cid is %llu and the port is %d\n", cid, MIGCVM_AGENT_PORT);
	ret = sock_create_kern(&init_net, AF_VSOCK, SOCK_STREAM, 0, &sock);
	if (ret < 0) {
		pr_err("Failed to create socket, error: %d\n", ret);
		return ret;
	}

	ret = kernel_connect(sock, (struct sockaddr *)&sa, sizeof(sa), 0);
	if (ret < 0) {
		pr_err("Connect failed: CID=%llu, Port=%d, Error=%d\n", 
			cid, MIGCVM_AGENT_PORT, ret);
		goto out;
	}

    bind_msg_t bind_msg;
	memset(&bind_msg, 0, sizeof(bind_msg));
	strncpy(bind_msg.cmd, "BIND_COMPLETE", sizeof(bind_msg.cmd) - 1);
	bind_msg.cmd[sizeof(bind_msg.cmd) - 1] = '\0';
	bind_msg.payload_type = PAYLOAD_TYPE_ULL;
	bind_msg.payload.ull_payload = guest_rd;
	bind_msg.payload_len = sizeof(guest_rd);

    struct kvec bind_vec = {
        .iov_base = &bind_msg,
        .iov_len = sizeof(bind_msg)
     };
	
	struct msghdr bind_hdr = {
		.msg_flags = MSG_NOSIGNAL | MSG_DONTWAIT,
	};
    ret = kernel_sendmsg(sock, &bind_hdr, &bind_vec, 1, sizeof(bind_msg));

    if (ret != sizeof(bind_msg)) {
        pr_err("Sendmsg failed: Sent %d/%zu bytes, Error: %d\n", 
              ret, sizeof(bind_msg), ret < 0 ? ret : 0);
        if (ret >= 0) ret = -EMSGSIZE;
    } else {
		ret = 0;
    }
 
out:
    if (sock) {
		kernel_sock_shutdown(sock, SHUT_RDWR);
        sock_release(sock);
	}
 	return ret;
}

int virtcca_binding_with_migvm_pid(struct kvm *guest_kvm, struct kvm_virtcca_mig_cmd *cmd)
{
	struct kvm *migcvm_kvm;
	struct virtcca_cvm *guest_cvm = guest_kvm->arch.virtcca_cvm;
	struct virtcca_cvm *migcvm_cvm;
	struct mig_cvm *mig_cvm_info;
	int ret;
	struct mig_cvm *guest_mig_cvm_info = guest_cvm->mig_cvm_info;

	if (guest_mig_cvm_info == NULL) {
		return -EINVAL;
	}

	if (copy_from_user(guest_mig_cvm_info, (void __user *)cmd->data,
			sizeof(struct mig_cvm))) {
		return -EFAULT;
	}

	if (cmd->flags || guest_mig_cvm_info->version != KVM_CVM_MIGVM_VERSION) {
		return -EINVAL;
	}

	migcvm_kvm = kvm_get_target_kvm(guest_mig_cvm_info->migvm_pid);
	if (!migcvm_kvm || !(migcvm_kvm->arch.virtcca_cvm)) {
		pr_err("%s: servtd not found, pid=%d\n", __func__, guest_mig_cvm_info->migvm_pid);
		return -ENOENT;
	}

	migcvm_cvm = migcvm_kvm->arch.virtcca_cvm;
	mig_cvm_info = migcvm_cvm->mig_cvm_info;
	ret = tmi_bind_add(guest_cvm->rd, migcvm_cvm->rd);
	if (ret != TMI_SUCCESS) {
		pr_err("%s: tmi_bind_add failed, ret=%d\n", __func__, ret);
		return ret;
	}
	ret = notify_migcvm_agent(guest_mig_cvm_info->migvm_cid, guest_cvm->rd);
	if (ret != 0) {
		return 0;  /* now is 0, ignore the error */
	}
	return ret;
}


int virtcca_get_bind_info(struct kvm *kvm, struct kvm_virtcca_mig_cmd *cmd)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct virtcca_bind_info info;
	struct arm_smccc_res ret;

	if (copy_from_user(&info, (void __user *)cmd->data,
			sizeof(struct virtcca_bind_info))) {
		return -EFAULT;
	}

	if (cmd->flags || info.version != KVM_CVM_MIGVM_VERSION)
		return -EINVAL;

	ret = tmi_bind_peek(cvm->rd);
	if (ret.a1 == TMI_SUCCESS) {
		if (ret.a2 == SLOT_IS_READY) {
			info.premig_done = true;
		} else {
			info.premig_done = false;
		}
		if (copy_to_user((void __user *)cmd->data, &info,
			sizeof(struct virtcca_bind_info))) {
			return -EFAULT;
		}
		return ret.a1;
	} else {
		pr_err("%s: failed, err=%lx\n", __func__, ret.a1);
		return -EIO;
	}

	return ret.a1;
}

static struct kvm_device_ops kvm_virtcca_mig_stream_ops = {
	.name = "kvm-virtcca-mig-stream",
	.get_attr = virtcca_mig_stream_get_attr,
	.set_attr = virtcca_mig_stream_set_attr,
	.mmap = virtcca_mig_stream_mmap,
	.ioctl = virtcca_mig_stream_ioctl,
	.create = virtcca_mig_stream_create,
	.destroy = virtcca_mig_stream_release,
};

static atomic_t g_mig_streams_used = ATOMIC_INIT(0);

int kvm_virtcca_mig_stream_ops_init(void)
{
	int ret = 0;

	if (!atomic_read(&g_mig_streams_used))
		ret = kvm_register_device_ops(&kvm_virtcca_mig_stream_ops, KVM_DEV_TYPE_VIRTCCA_MIG_STREAM);

	if (!ret) {
		atomic_inc(&g_mig_streams_used);
	}
	pr_info("kvm_virtcca_mig_stream_ops_init g_mig_streams_used = %d", atomic_read(&g_mig_streams_used));
	return ret;
}

void kvm_virtcca_mig_stream_ops_exit(void)
{
	atomic_dec(&g_mig_streams_used);
	pr_info("kvm_virtcca_mig_stream_ops_exit g_mig_streams_used = %d", atomic_read(&g_mig_streams_used));

	if (!atomic_read(&g_mig_streams_used))
		kvm_unregister_device_ops(KVM_DEV_TYPE_VIRTCCA_MIG_STREAM);
}
