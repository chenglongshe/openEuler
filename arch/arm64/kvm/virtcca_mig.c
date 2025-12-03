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

#define SEC_CRC_PATH	"/tmp/sec_memory_check"
#define NS_CRC_PATH		"/tmp/ns_memory_check"
#define CRC_DUMP_CHUNK_SIZE 512
#define FILE_NAME_LEN	256

#define DEFAULT_IPA_START	0x40000000
#define CRC_POLYNOMIAL	0xEDB88320
#define CRC_LEN	512
#define CRC_SHIFT	8
#define MAX_MAC_PAGES_PER_ARR 256
#define MAX_BUF_PAGES 512
/* migvm vsock retry times */
#define SEND_RETRY_LIMIT 5
#define RECV_RETRY_LIMIT 5
#define CONNECT_RETRY_LIMIT 3
#define TMI_IMPORT_TIMEOUT_MS   600000

static struct virtcca_mig_capabilities g_virtcca_mig_caps;
static struct migcvm_agent_listen_cids g_migcvm_agent_listen_cid;

static crc_config_t g_crc_configs[2] = {
	[0] = { .is_secure = false, .enabled = false }, /* non-secure mem */
	[1] = { .is_secure = true, .enabled = false }  /* secure mem */
};

/* now bypass the migCVM, config staightly 1 is source, 2 is dest*/
bool virtcca_is_migration_source(struct virtcca_cvm *cvm)
{
	if (!cvm || !cvm->mig_state) {
		pr_err("Error: cvm or cvm->params is NULL\n");
		return false;
	}

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

	res = tmi_get_mig_config();
	crc32_init();

	g_virtcca_mig_caps.max_migs = (uint32_t)(res >> 48) & 0xFFFF;

	immutable_state_pages = (uint32_t)(res >> 32) & 0xFFFF;

	rd_state_pages = (uint32_t)(res >> 16) & 0xFFFF;

	tec_state_pages = (uint32_t)res & 0xFFFF;
	/*
	 * The minimal number of pages required. It hould be large enough to
	 * store all the non-memory states.
	 */
	g_virtcca_mig_caps.nonmem_state_pages = max3(immutable_state_pages, rd_state_pages, tec_state_pages);

	return 0;
}

static void virtcca_mig_stream_get_virtcca_mig_attr(struct virtcca_mig_stream *stream,
	struct kvm_dev_virtcca_mig_attr *attr)
{
	attr->version = KVM_DEV_VIRTCCA_MIG_ATTR_VERSION;
	attr->max_migs = g_virtcca_mig_caps.max_migs;
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
	uint32_t min_pages = g_virtcca_mig_caps.nonmem_state_pages;

	if (req_pages > VIRTCCA_MIG_BUF_LIST_PAGES_MAX) {
		stream->buf_list_pages = VIRTCCA_MIG_BUF_LIST_PAGES_MAX;
		pr_warn("Cut the buf_list_npages to the max supported num\n");
	} else if (req_pages < min_pages) {
		stream->buf_list_pages = min_pages;
	} else {
		stream->buf_list_pages = req_pages;
	}

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

int virtcca_config_crc(uint64_t crc_addr_start, uint64_t crc_addr_end,
					  uint64_t crc_granularity, bool is_secure)
{
	crc_config_t *config = &g_crc_configs[is_secure];
	const char *mem_type = is_secure ? SEC_MEM : NON_SEC_MEM;

	/* disable crc check */
	if (crc_granularity == 0) {
		memset(config, 0, sizeof(*config));
		pr_info("Virtcca migration %s memory crc check disabled", mem_type);
		return 0;
	}

	if (crc_addr_start >= crc_addr_end ||
		(crc_granularity != SZ_2M && crc_granularity != SZ_4K) ||
		crc_addr_end - crc_addr_start < crc_granularity) {
		pr_err("virtcca_config_crc: invalid input parameters");
		return -EINVAL;
	}

	config->ipa_start = ALIGN(crc_addr_start, crc_granularity);
	config->ipa_end = ALIGN_DOWN(crc_addr_end, crc_granularity);
	config->granularity = crc_granularity;
	config->enabled = true;

	pr_info("Virtcca migration %s memory crc check enabled", mem_type);
	return 0;
}
EXPORT_SYMBOL_GPL(virtcca_config_crc);

bool is_valid_crc_params_for_cvm(struct kvm *kvm, bool is_secure)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	crc_config_t *config = &g_crc_configs[is_secure];
	uint64_t cvm_addr_start = is_secure ? cvm->ipa_start : cvm->swiotlb_start;
	uint64_t cvm_addr_end = is_secure ? (DEFAULT_IPA_START + cvm->ram_size) : cvm->swiotlb_end;

	if (config->ipa_start < cvm_addr_start || config->ipa_end > cvm_addr_end)
		return false;
	return true;
}

static int virtcca_prepare_crc_file(struct virtcca_cvm *cvm, char *file_name,
						   size_t name_size, bool is_secure)
{
	struct file *file_p = NULL;
	loff_t pos = 0;
	int ret = 0;
	const char *crc_file_path = is_secure ? SEC_CRC_PATH : NS_CRC_PATH;

	if (!file_name) {
		pr_err("Invalid file name");
		return -EINVAL;
	}

	snprintf(file_name, name_size, "%s_%u", crc_file_path, cvm->cvm_vmid);
	file_p = filp_open(file_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(file_p)) {
		ret = PTR_ERR(file_p);
		pr_err("Failed to open file %s: %d\n", file_name, ret);
		return -EIO;
	}

	ret = kernel_write(file_p, "=== crc check start ===\n", 
					  strlen("=== crc check start ===\n"), &pos);
	if (ret < 0) {
		pr_err("Failed to write file header: %d\n", ret);
	}

	filp_close(file_p, NULL);
	return ret;
}

int virtcca_dump_array_to_file(uint64_t *gpa_list, uint64_t *crc_result, int gpa_nums, char *file_name)
{
	loff_t pos = 0;
	char *buf = NULL;
	int i, len;
	int ret = 0;
	struct file *file_p = NULL;

	if (file_name == NULL) {
		pr_err("dump_array_to_file error: invalid input.");
		return -EINVAL;
	}

	file_p = filp_open(file_name, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (IS_ERR(file_p)) {
		pr_err("dump_array_to_file: failed to open file");
		return -EIO;
	}

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto cleanup;
	}

	for (i = 0; i < gpa_nums; i++) {
		len = snprintf(buf, PAGE_SIZE, "gpa = 0x%llx crc = 0x%llx\n",
						gpa_list[i], crc_result[i]);
		ret = kernel_write(file_p, buf, len, &pos);
		if (ret < 0) {
			pr_err("dump_array_to_file: write error at %d\n", i);
			ret = -EIO;
			goto cleanup;
		}
	}

cleanup:
	if (buf)
		kfree(buf);
	if (file_p)
		filp_close(file_p, NULL);
	return ret;
}

uint32_t __execute_ns_crc_dump(struct kvm *kvm, uint64_t target_ipa, unsigned char *crc_buf, uint64_t crc_granularity)
{
	uint64_t crc_buf_offset;
	int ret;
	crc_buf_offset = 0;
	while (crc_buf_offset < crc_granularity) {
		gfn_t gfn = target_ipa >> PAGE_SHIFT;
		/* The default granularity of the swiotlb range is 4K. */
		ret = kvm_read_guest_page(kvm, gfn, crc_buf + crc_buf_offset, 0, SZ_4K);
		if (ret < 0) {
			pr_err("read swiotlb page failed, ret = %d", ret);
			return 0;
		}
		crc_buf_offset += SZ_4K;
	}

	return crc32_compute((uint8_t *)crc_buf, crc_granularity);
}

static int virtcca_execute_crc_dump(struct kvm *kvm, crc_config_t *config, char *file_name)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	uint64_t crc_granularity = config->granularity;
	uint64_t gpa_start = config->ipa_start;
	uint64_t gpa_end = config->ipa_end;
	uint64_t crc_addr = gpa_start;
	uint64_t *gpa_list = NULL;
	uint64_t *crc_result = NULL;
	unsigned char *crc_buf = NULL;
	uint64_t actual_chunk_size = 0;
	uint64_t valid_count = 0;
	int ret = 0;

	if (!file_name || !config) {
		pr_err("execute_crc_dump_new: invalid input");
		return -EINVAL;
	}

	gpa_list = kzalloc(CRC_DUMP_CHUNK_SIZE * sizeof(uint64_t), GFP_KERNEL);
	crc_result = kzalloc(CRC_DUMP_CHUNK_SIZE * sizeof(uint64_t), GFP_KERNEL);
	crc_buf = kzalloc(crc_granularity, GFP_KERNEL);
	if (!crc_result || !gpa_list || !crc_buf) {
		pr_err("execute_crc_dump_new: memory allocation failed");
		ret = -ENOMEM;
		goto cleanup;
	}

	while (crc_addr < gpa_end) {
		valid_count = 0;
		actual_chunk_size = min_t(uint64_t, CRC_DUMP_CHUNK_SIZE,
								(gpa_end - crc_addr) / crc_granularity);
		if (actual_chunk_size <= 0)
			break;
		if (config->is_secure) {
			for (int i = 0; i < actual_chunk_size; i++) {
				uint64_t addr = crc_addr + i * crc_granularity;
				if (addr >= UEFI_SIZE && addr < DEFAULT_IPA_START)
					continue; /* skip the uefi reversed area */
				gpa_list[valid_count++] = addr;
			}

			if (valid_count == 0) {
				crc_addr += actual_chunk_size * crc_granularity;
				continue;
			}
			ret = tmi_dump_checksum(cvm->rd, virt_to_phys(gpa_list),
								virt_to_phys(crc_result), crc_granularity);
			if (ret) {
				pr_err("tmi_dump_checksum failed: %d", ret);
				ret = -EIO;
				goto cleanup;
			}
		} else {
			for (int i = 0; i < actual_chunk_size; i++) {
				gpa_list[i] = crc_addr + i * crc_granularity;
				crc_result[i] = __execute_ns_crc_dump(kvm, gpa_list[i], crc_buf, crc_granularity);
				valid_count++;
			}
		}

		ret = virtcca_dump_array_to_file(gpa_list, crc_result, valid_count, file_name);
		if (ret < 0) {
			pr_err("dump crc to file failed: %d", ret);
			ret = -EIO;
			goto cleanup;
		}

		memset(gpa_list, 0, actual_chunk_size * sizeof(uint64_t));
		memset(crc_result, 0, actual_chunk_size * sizeof(uint64_t));
		crc_addr += actual_chunk_size * crc_granularity;
		touch_softlockup_watchdog();
	}

	ret = 0;
cleanup:
	if (crc_result)
		kfree(crc_result);
	if (gpa_list)
		kfree(gpa_list);
	if (crc_buf)
		kfree(crc_buf);
	return ret;
}

int virtcca_dump_crc(struct kvm *kvm, bool is_secure)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	crc_config_t *config = &g_crc_configs[is_secure];
	char file_name[FILE_NAME_LEN];
	const char *mem_type = is_secure ? SEC_MEM : NON_SEC_MEM;
	int ret = 0;

	if (!cvm->mig_state) {
		pr_err("virtcca_dump_crc: invalid mig_state");
		return -EINVAL;
	}

	if (!config->enabled) {
		pr_err("virtcca_dump_crc disabled!");
		return 0;
	}

	if (!is_valid_crc_params_for_cvm(kvm, config->is_secure)) {
		pr_err("virtcca_dump_crc: invalid input parameters");
		return -EINVAL;
	}

	ret = virtcca_prepare_crc_file(cvm, file_name, sizeof(file_name), config->is_secure);
	if (ret < 0) {
		pr_err("virtcca_dump_crc: create file failed");
		return -EIO;
	}

	ret = virtcca_execute_crc_dump(kvm, config, file_name);
	if (ret) {
		pr_err("virtcca_dump_crc: CRC dump execution failed");
		return -EIO;
	}

	pr_info("virtcca dump %s crc success", mem_type);
	return 0;
}

static int virtcca_mig_stream_mbmd_setup(struct virtcca_mig_mbmd *mbmd)
{
	struct page *page;
	unsigned long mbmd_size = PAGE_SIZE;
	int order = get_order(mbmd_size);

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

static int virtcca_mig_export_state_immutable(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct virtcca_mig_page_list *page_list = &stream->page_list;
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	union virtcca_mig_stream_info stream_info = {.val = 0};
	struct arm_smccc_res ret;

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
	return 0;
}

static int virtcca_mig_import_state_immutable(struct kvm *kvm, struct virtcca_mig_stream *stream,
	uint64_t __user *data)
{
	struct virtcca_mig_page_list *page_list = &stream->page_list;
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	union virtcca_mig_stream_info stream_info = {.val = 0};
	struct mig_cvm_update_info *update_info = NULL;
	uint64_t ret, npages;
	int res = 0;

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

	update_info = kmalloc(sizeof(struct mig_cvm_update_info), GFP_KERNEL);
	if (!update_info) {
		pr_err("virtcca_mig_export_state_immutable: kmalloc failed.");
		return -ENOMEM;
	}

	ret = tmi_update_cvm_info(cvm->rd, (uint64_t)update_info);
	if (ret) {
		pr_err("tmi_update_cvm_info failed, err=%llx", ret);
		res = -EIO;
		goto out;
	}

	cvm->swiotlb_start = update_info->swiotlb_start;
	cvm->swiotlb_end = update_info->swiotlb_end;
	cvm->ipa_start = update_info->ipa_start;

	ret = kvm_cvm_mig_map_range(kvm);
	if (ret) {
		pr_err("kvm_cvm_mig_map_range: failed, err=%llx\n", ret);
		res = -EIO;
	}

out:
	if (update_info)
		kfree(update_info);
	return res;
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
	unsigned long timeout = jiffies + msecs_to_jiffies(TMI_IMPORT_TIMEOUT_MS);
	uint64_t in_order, ret;

	if (copy_from_user(&in_order, (void __user *)data, sizeof(uint64_t)))
		return -EFAULT;

	/*
	 * Set the in_order bit if userspace requests to generate a start
	 * token by sending a non-0 value through tdx_cmd.data.
	 */
	stream_info.in_order = !!in_order;
	do {
		ret = tmi_export_track(cvm->rd, stream->mbmd.hpa_and_size, stream_info.val);
		msleep(1);

		if (time_after(jiffies, timeout)) {
			pr_err("tmi_export_track timeout (%d ms)", TMI_IMPORT_TIMEOUT_MS);
			ret = ETIMEDOUT;
			break;
		}
	} while (ret == TMI_IMPORT_INCOMPLETE);

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
	unsigned long timeout = jiffies + msecs_to_jiffies(TMI_IMPORT_TIMEOUT_MS);

	if (!cvm) {
		pr_err("%s: cvm is not initialized\n", __func__);
		return -EINVAL;
	}

	do {
		ret = tmi_import_commit(cvm->rd);
		msleep(1);

		if (time_after(jiffies, timeout)) {
			pr_err("tmi_import_commit timeout (%d ms)", TMI_IMPORT_TIMEOUT_MS);
			ret = ETIMEDOUT;
			break;
		}
	} while (ret == TMI_IMPORT_INCOMPLETE);

	if (ret != TMI_SUCCESS) {
		pr_err("%s: failed, err=%llx\n", __func__, ret);
		return -EIO;
	}

	virtcca_mig_state_release(cvm);

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

static int virtcca_mig_export_pause(struct kvm *kvm,
		struct virtcca_mig_stream *stream, uint64_t __user *data)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct arm_smccc_res tmi_res = { 0 };

	tmi_res = tmi_export_pause(cvm->rd);
	if (tmi_res.a1) {
		pr_err("%s: err=%lu\n",
			__func__, tmi_res.a1);
		return -EIO;
	}

	return tmi_res.a1;
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
		virtcca_dump_crc(kvm, true);
		virtcca_dump_crc(kvm, false);
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
	case KVM_CVM_MIG_EXPORT_PAUSE:
		r = virtcca_mig_export_pause(kvm, stream,
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
	struct mig_cvm_update_info *update_info = NULL;
	uint64_t ret = 0;

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

	/* now just create stream in tmm */
	migsc_pa = tmi_mig_stream_create(cvm->rd, numa_set);
	if (!migsc_pa) {
		kvm_err("virtcca mig stream create failed!\n");
	}

	*migsc_addr = migsc_pa;

	update_info = kmalloc(sizeof(struct mig_cvm_update_info), GFP_KERNEL);
	if (!update_info) {
		pr_err("virtcca_mig_export_state_immutable: kmalloc failed.");
		return -ENOMEM;
	}

	ret = tmi_update_cvm_info(cvm->rd, (uint64_t)update_info);
	if (ret) {
		pr_err("tmi_update_cvm_info failed, err=%llx", ret);
		kfree(update_info);
		return -EIO;
	}
	cvm->swiotlb_start = update_info->swiotlb_start;
	cvm->swiotlb_end = update_info->swiotlb_end;

	kfree(update_info);
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

	stream = (struct virtcca_mig_stream *)kzalloc(sizeof(struct virtcca_mig_stream), GFP_KERNEL_ACCOUNT);
	if (!stream)
		return -ENOMEM;

	dev->private = stream;
	stream->idx = atomic_inc_return(&mig_state->streams_created) - 1; /* set the stream idx of the cvm */

	if (!stream->idx) {
		ret = virtcca_mig_session_init(kvm); /* if is the first stream, call this func */
		if (ret)
			goto err_mig_session_init;
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

	mig_state->vcpu_export_next_idx = 0;
	mig_state->backward_migsc_paddr = 0;

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

	mig_state = kzalloc(sizeof(struct virtcca_mig_state), GFP_KERNEL_ACCOUNT);
	if (!mig_state) {
		goto out;
	}

	migsc_paddrs = kcalloc(g_virtcca_mig_caps.max_migs, sizeof(hpa_t), GFP_KERNEL_ACCOUNT);
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

int virtcca_save_migvm_cid(struct kvm *kvm, struct kvm_virtcca_mig_cmd *cmd)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct mig_cvm *mig_cvm_usr;
	struct mig_cvm *mig_cvm_info = cvm->mig_cvm_info;

	pr_info("calling virtcca_save_migvm_cid\n");
	if (mig_cvm_info == NULL) {
		pr_info("guest_mig_cvm_info is NULL\n");
		return -EINVAL;
	}

	mig_cvm_usr = kmalloc(sizeof(struct mig_cvm), GFP_KERNEL);
    if (!mig_cvm_usr) {
		pr_info("cannot allocate memory buffer for get user data\n");
        return -ENOMEM;
    }
	if (copy_from_user(mig_cvm_usr, (void __user *)cmd->data,
			sizeof(struct mig_cvm))) {
		kfree(mig_cvm_usr);
		return -EFAULT;
	}

	if (cmd->flags || mig_cvm_usr->version != KVM_CVM_MIGVM_VERSION) {
		kfree(mig_cvm_usr);
		return -EINVAL;
	}

	memcpy(&mig_cvm_info->migvm_cid, &mig_cvm_usr->migvm_cid,
			sizeof(uint64_t));
	g_migcvm_agent_listen_cid.cid = mig_cvm_info->migvm_cid;

	kfree(mig_cvm_usr);
	return 0;
}

static int send_and_wait_ack(struct socket *sock, bind_msg_t *req_msg)
{
    int ret;
    struct kvec vec;
    struct msghdr hdr;
    bind_msg_t ack_msg = {0};
	int retry = 0;

	memset(&hdr, 0, sizeof(hdr));
    vec.iov_base = req_msg;
    vec.iov_len = sizeof(*req_msg);
	/* set vsock hdr */
    hdr.msg_flags = MSG_NOSIGNAL;
	iov_iter_kvec(&hdr.msg_iter, WRITE, &vec, 1, vec.iov_len);

	if (req_msg->payload_len > MAX_PAYLOAD_SIZE) {
		pr_err("Payload size %u exceeds limit\n", req_msg->payload_len);
		ret = -EINVAL;
		goto out;
	}

    /* send request to migcvm agent, expect ack from migcvm agent */
    retry = 0;
    do {
        ret = kernel_sendmsg(sock, &hdr, &vec, 1, vec.iov_len);
        if (ret == -EINTR) {
            pr_warn("sendmsg interrupted by signal, retry %d\n", retry);
            retry++;
            continue;
        }
        break;
    } while (retry < SEND_RETRY_LIMIT);

    if (ret < 0) {
        pr_err("Failed to send request, ret=%d\n", ret);
        goto out;
    } else if (ret != sizeof(*req_msg)) {
        pr_err("Partial send, ret=%d\n", ret);
        ret = -EIO;
        goto out;
    }

	/* reset ack buffer */
    vec.iov_base = &ack_msg;
    vec.iov_len = sizeof(ack_msg);

    retry = 0;
    do {
        ret = kernel_recvmsg(sock, &hdr, &vec, 1, sizeof(ack_msg), hdr.msg_flags);
        if (ret == -EINTR) {
            pr_warn("recvmsg interrupted by signal, retry %d\n", retry);
            retry++;
            continue;
        }
        break;
    } while (retry < RECV_RETRY_LIMIT);

    if (ret < 0) {
        pr_err("Failed to recv ack, ret=%d\n", ret);
        goto out;
    } else if (ret != sizeof(ack_msg)) {
        pr_err("Partial ack recv ret=%d\n", ret);
        ret = -EIO;
        goto out;
    }

    /* validate ack message */
    if (ack_msg.payload_type != VSOCK_MSG_ACK ||
        ack_msg.session_id != req_msg->session_id ||
		ack_msg.success == 0) {
        pr_err("ACK validation failed, the payload_type=%d, session_id=%llu, success=%d\n",
               ack_msg.payload_type, ack_msg.session_id, ack_msg.success);
        ret = -EPROTO;
        goto out;
    }
	pr_info("ACK validation passed\n");
	ret = 0;

out:
    return ret;
}

/* vsock connection*/
/* step 1: send to mig-cvm agent: the migrated rd, the destination platform ip*/
/* step 2: wait for mig-cvm agent's response */
static int notify_migcvm_agent(uint64_t cid, virtcca_dst_host_info_t *dst_host_info,
							   uint64_t guest_rd, bool is_src)
{
	struct socket *sock = NULL;
	int ret = 0;
	int retry_count = 3;
	int error = 0, len = sizeof(error);
	int connect_retry = 0;
    long old_sndtimeo = 0, old_rcvtimeo = 0;
	const unsigned long timeout = 5 * HZ;

	struct sockaddr_vm sa = {
		.svm_family = AF_VSOCK,
		.svm_cid = cid,
		.svm_port = is_src ? MIGCVM_AGENT_PORT_SRC : MIGCVM_AGENT_PORT_DST
	};
	bind_msg_t bind_msg = {0};

	printk("calling notify_migcvm_agent, cid=%llu, port=%d\n", cid, sa.svm_port);
	ret = sock_create_kern(&init_net, AF_VSOCK, SOCK_STREAM, 0, &sock);
	if (ret < 0) {
		pr_err("Failed to create socket, error: %d\n", ret);
		return ret;
	}

    /* save original receive timeout, and set 5s timeout for migcvm ack */
    if (sock->sk) {
        old_sndtimeo = sock->sk->sk_sndtimeo;
        old_rcvtimeo = sock->sk->sk_rcvtimeo;
        sock->sk->sk_sndtimeo = timeout;
        sock->sk->sk_rcvtimeo = timeout;
    }

    connect_retry = 0;
    do {
        ret = kernel_connect(sock, (struct sockaddr *)&sa, sizeof(sa), O_NONBLOCK);
        if (ret == -EINTR) {
            pr_warn("connect interrupted by signal, retry %d\n", connect_retry);
			schedule_timeout_uninterruptible(HZ / 10);
            connect_retry++;
            continue;
        }
        break;
    } while (connect_retry < CONNECT_RETRY_LIMIT);

    if (ret < 0) {
        if (sock->ops && sock->ops->getsockopt)
            sock->ops->getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
        if (error) {
            pr_err("Connect failed (cid=%llu, port=%d, err=%d)\n",
                   cid, sa.svm_port, error);
            ret = -error;
            goto cleanup;
        }
    }

	if (strscpy(bind_msg.cmd, is_src ? "START_CLIENT" : "START_SERVER",
	            sizeof(bind_msg.cmd)) < 0) {
        pr_err("Command string too long\n");
        ret = -EINVAL;
        goto cleanup;
    }

	bind_msg.session_id = get_jiffies_64();
	bind_msg.payload_type = is_src ? PAYLOAD_TYPE_ALL : PAYLOAD_TYPE_ULL;
	bind_msg.payload.ull_payload = guest_rd;
	bind_msg.payload_len = MAX_PAYLOAD_SIZE;
	if (is_src) {
        if (!dst_host_info) {
            pr_err("No destination host info provided for source\n");
            ret = -EINVAL;
            goto cleanup;
        }
		bind_msg.payload_len = strlen(dst_host_info->dst_ip) + 1;
        if (strscpy(bind_msg.payload.char_payload, dst_host_info->dst_ip,
		    sizeof(bind_msg.payload.char_payload)) < 0) {
            pr_err("Destination IP too long\n");
            ret = -EINVAL;
            goto cleanup;
        }
	}

    do {
        ret = send_and_wait_ack(sock, &bind_msg);
        if (!ret) break;
        pr_warn("Send/ACK failed, retrying (%d left)\n", retry_count - 1);
        retry_count--;
		/* a delay time */
        schedule_timeout_uninterruptible(HZ / 10);
    } while (retry_count > 0);

    if (ret) {
        pr_err("Failed to get bind info after retries, error=%d\n", ret);
    }
 
cleanup:
    if (sock) {
		if (sock->sk) {
			sock->sk->sk_sndtimeo = old_sndtimeo;
			sock->sk->sk_rcvtimeo = old_rcvtimeo;
		}
		if (ret ==0)
			kernel_sock_shutdown(sock, SHUT_RDWR);
        sock_release(sock);
	}
 	return ret;
}

int virtcca_migvm_agent_ratstls_dst(struct kvm *kvm, struct kvm_virtcca_mig_cmd *cmd)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct virtcca_dst_host_info dst_info;
	struct arm_smccc_res tmi_ret;
	int ret = 0;
	pr_info("virtcca_migvm_agent_ratstls_dst called\n");
	if (g_migcvm_agent_listen_cid.cid == 0) {
		pr_err("there is no cid of migcvm, cannot migrate virtCCA cVM\n");
		return -EINVAL;
	}

	if (copy_from_user(&dst_info, (void __user *)cmd->data,
		sizeof(virtcca_dst_host_info_t))) {
		return -EFAULT;
	}

	if (cmd->flags || dst_info.version != KVM_CVM_MIGVM_VERSION) {
		pr_err("invalid flags or version, flags is %x, version is %x\n", cmd->flags, dst_info.version);
		return -EINVAL;
	}
	pr_info("calling tmi_bind_peek");
	/* check if the slot is binded*/
	tmi_ret = tmi_bind_peek(cvm->rd);
	if (tmi_ret.a1 == TMI_SUCCESS) {
		if (tmi_ret.a2 <= SLOT_NOT_BINDED) {
			pr_err("%s: failed, err=%lx\n", __func__, tmi_ret.a1);
			return -EINVAL;
		}
	} else {
		pr_err("%s: failed, err=%lx\n", __func__, tmi_ret.a1);
		return -EINVAL;
	}

	ret = notify_migcvm_agent(g_migcvm_agent_listen_cid.cid, NULL, cvm->rd, false);
	if (ret != 0) {
		pr_err("%s: notify_migcvm_agent failed, ret=%d\n", __func__, ret);
		return -EINVAL;
	}

	return ret;
}

int virtcca_migvm_agent_ratstls(struct kvm *kvm, struct kvm_virtcca_mig_cmd *cmd)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	struct virtcca_dst_host_info dst_info;
	struct arm_smccc_res tmi_ret;
	int ret = 0;
	pr_info("virtcca_migvm_agent_ratstls called\n");
	if (g_migcvm_agent_listen_cid.cid == 0) {
		pr_err("there is no cid of migcvm, cannot migrate virtCCA cVM\n");
		return -EINVAL;
	}

	if (copy_from_user(&dst_info, (void __user *)cmd->data,
		sizeof(virtcca_dst_host_info_t))) {
		return -EFAULT;
	}

	if (cmd->flags || dst_info.version != KVM_CVM_MIGVM_VERSION) {
		pr_err("invalid flags or version, flags is %x, version is %x\n", cmd->flags, dst_info.version);
		return -EINVAL;
	}

	/* now the dst ip is none, and dst port is 0, it should be add check into this (after qemu input)*/
    if (strscpy(cvm->mig_cvm_info->dst_ip, dst_info.dst_ip, sizeof(cvm->mig_cvm_info->dst_ip)) < 0) {
		pr_err("save dst_ip failed\n");
        return -EINVAL;
    }
	cvm->mig_cvm_info->dst_port = dst_info.dst_port;
	/* check if the slot is binded*/
	tmi_ret = tmi_bind_peek(cvm->rd);
	if (tmi_ret.a1 == TMI_SUCCESS) {
		if (tmi_ret.a2 <= SLOT_NOT_BINDED) {
			pr_err("%s: failed, err=%lx\n", __func__, tmi_ret.a1);
			return -EINVAL;
		}
	} else {
		pr_err("%s: failed, err=%lx\n", __func__, tmi_ret.a1);
		return -EINVAL;
	}

	ret = notify_migcvm_agent(g_migcvm_agent_listen_cid.cid, &dst_info, cvm->rd, true);
	if (ret != 0) {
		pr_info("%s: notify_migcvm_agent failed, ret=%d\n", __func__, ret);
		return -EINVAL;
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

	return ret;
}

void kvm_virtcca_mig_stream_ops_exit(void)
{
	atomic_dec(&g_mig_streams_used);
	if (!atomic_read(&g_mig_streams_used))
		kvm_unregister_device_ops(KVM_DEV_TYPE_VIRTCCA_MIG_STREAM);
}

void virtcca_enable_log_dirty(struct kvm *kvm, uint64_t start, uint64_t end)
{
	struct arm_smccc_res res;
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	uint64_t s_start = cvm->ipa_start;
	uint64_t s_end = cvm->ipa_start + cvm->ram_size;

	if (end <= s_start || start >= s_end) {
		return;
	}

	res = tmi_mem_region_protect(cvm->rd, start, end);
	if (res.a1 != 0) {
		pr_err("tmi_mem_region_protect failed!\n");
	}
}

int virtcca_mig_export_abort(struct kvm *kvm)
{
	struct virtcca_cvm *cvm = kvm->arch.virtcca_cvm;
	phys_addr_t target_ipa = cvm->swiotlb_start;
	struct kvm_pgtable *pgt = kvm->arch.mmu.pgt;
	int level;
	uint64_t granule;
	uint64_t ret = 0;

	while (target_ipa != 0 && target_ipa < cvm->swiotlb_end) {
		ret = kvm_pgtable_get_leaf(pgt, target_ipa, NULL, &level);
		if (ret) {
			pr_err("%s: err=%llx\n", __func__, ret);
			ret = -EIO;
		}

		granule = kvm_granule_size(level);
		ret = virtcca_stage2_update_leaf_attrs(pgt, target_ipa, granule,
		KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R | KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W, 0, NULL, NULL, 0);
		if (ret) {
			pr_err("%s: err=%llx\n", __func__, ret);
			ret = -EIO;
		}

		kvm_call_hyp(__kvm_tlb_flush_vmid_ipa_nsh, pgt->mmu, target_ipa, level);
		target_ipa += granule;
	}

	ret = tmi_export_abort(cvm->rd);
	if (ret) {
		pr_err("%s: err=%llx\n", __func__, ret);
		ret = -EIO;
	}

	virtcca_mig_state_release(cvm);
	return ret;
}