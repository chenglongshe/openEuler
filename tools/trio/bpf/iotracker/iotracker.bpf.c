// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2025 Huawei Technologies Co., Ltd
 */

#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#define PAGE_SZ 4096
#define PAGE_ST 12

extern void bpf_get_dpath_mark(unsigned long addr, unsigned long off,
			       unsigned long len) __ksym;

char LICENSE[] SEC("license") = "Dual BSD/GPL";

static int _read_request(struct pt_regs *ctx, struct kiocb *iocb, struct iov_iter *to)
{
	struct file *filp;
	unsigned long foff, len, count;
	loff_t offset;

	bpf_core_read(&offset, sizeof(loff_t), &(iocb->ki_pos));
	bpf_core_read(&filp, sizeof(struct file *), &(iocb->ki_filp));
	bpf_core_read(&count, sizeof(size_t), &(to->count));

	/* enlarge to the 4k-aligned(page-based) */
	foff = (offset >> PAGE_ST) << PAGE_ST;
	len  = ((count >> PAGE_ST) + 1) << PAGE_ST;

	bpf_get_dpath_mark((unsigned long)filp, foff, len);
	return 0;
}

SEC("kprobe/erofs_file_read_iter")
int BPF_KPROBE(erofs_file_read_iter_entry, struct kiocb *iocb,
		struct iov_iter *to)
{
	return _read_request(ctx, iocb, to);
}

SEC("kprobe/filemap_fault")
int BPF_KPROBE(filemap_fault_entry, struct vm_fault *vmf)
{
	struct file  *file;
	unsigned long foff, len;

	bpf_core_read(&foff, sizeof(unsigned long), &vmf->pgoff);
	file = BPF_CORE_READ(vmf, vma, vm_file);
	if (!file)
		return 0;

	foff = foff * PAGE_SZ;
	len  = PAGE_SZ;

	bpf_get_dpath_mark((unsigned long)file, foff, len);
	return 0;
}
