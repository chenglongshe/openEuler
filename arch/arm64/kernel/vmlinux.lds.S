/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ld script to make ARM Linux kernel
 * taken from the i386 version by Russell King
 * Written by Martin Mares <mj@atrey.karlin.mff.cuni.cz>
 */

#include <asm/hyp_image.h>
#ifdef CONFIG_KVM
#define HYPERVISOR_EXTABLE					\
	. = ALIGN(SZ_8);					\
	__start___kvm_ex_table = .;				\
	*(__kvm_ex_table)					\
	__stop___kvm_ex_table = .;

#define HYPERVISOR_DATA_SECTIONS				\
	HYP_SECTION_NAME(.rodata) : {				\
		. = ALIGN(PAGE_SIZE);				\
		__hyp_rodata_start = .;				\
		*(HYP_SECTION_NAME(.data..ro_after_init))	\
		*(HYP_SECTION_NAME(.rodata))			\
		. = ALIGN(PAGE_SIZE);				\
		__hyp_rodata_end = .;				\
	}

#define HYPERVISOR_PERCPU_SECTION				\
	. = ALIGN(PAGE_SIZE);					\
	HYP_SECTION_NAME(.data..percpu) : {			\
		*(HYP_SECTION_NAME(.data..percpu))		\
	}

#define HYPERVISOR_RELOC_SECTION				\
	.hyp.reloc : ALIGN(4) {					\
		__hyp_reloc_begin = .;				\
		*(.hyp.reloc)					\
		__hyp_reloc_end = .;				\
	}

#define BSS_FIRST_SECTIONS					\
	__hyp_bss_start = .;					\
	*(HYP_SECTION_NAME(.bss))				\
	. = ALIGN(PAGE_SIZE);					\
	__hyp_bss_end = .;

/*
 * We require that __hyp_bss_start and __bss_start are aligned, and enforce it
 * with an assertion. But the BSS_SECTION macro places an empty .sbss section
 * between them, which can in some cases cause the linker to misalign them. To
 * work around the issue, force a page alignment for __bss_start.
 */
#define SBSS_ALIGN			PAGE_SIZE
#else /* CONFIG_KVM */
#define HYPERVISOR_EXTABLE
#define HYPERVISOR_DATA_SECTIONS
#define HYPERVISOR_PERCPU_SECTION
#define HYPERVISOR_RELOC_SECTION
#define SBSS_ALIGN			0
#endif

#define RO_EXCEPTION_TABLE_ALIGN	4
#define RUNTIME_DISCARD_EXIT

#include <asm-generic/vmlinux.lds.h>
#include <asm/cache.h>
#include <asm/kernel-pgtable.h>
#include <asm/kexec.h>
#include <asm/memory.h>
#include <asm/page.h>

#include "image.h"

OUTPUT_ARCH(aarch64)
ENTRY(_text)

jiffies = jiffies_64;

#define HYPERVISOR_TEXT					\
	. = ALIGN(PAGE_SIZE);				\
	__hyp_idmap_text_start = .;			\
	*(.hyp.idmap.text)				\
	__hyp_idmap_text_end = .;			\
	__hyp_text_start = .;				\
	*(.hyp.text)					\
	HYPERVISOR_EXTABLE				\
	. = ALIGN(PAGE_SIZE);				\
	__hyp_text_end = .;

#define IDMAP_TEXT					\
	. = ALIGN(SZ_4K);				\
	__idmap_text_start = .;				\
	*(.idmap.text)					\
	__idmap_text_end = .;

#ifdef CONFIG_HIBERNATION
#define HIBERNATE_TEXT					\
	ALIGN_FUNCTION();				\
	__hibernate_exit_text_start = .;		\
	*(.hibernate_exit.text)				\
	__hibernate_exit_text_end = .;
#else
#define HIBERNATE_TEXT
#endif

#ifdef CONFIG_KEXEC_CORE
#define KEXEC_TEXT					\
	ALIGN_FUNCTION();				\
	__relocate_new_kernel_start = .;		\
	*(.kexec_relocate.text)				\
	__relocate_new_kernel_end = .;
#else
#define KEXEC_TEXT
#endif

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
#define TRAMP_TEXT					\
	. = ALIGN(PAGE_SIZE);				\
	__entry_tramp_text_start = .;			\
	*(.entry.tramp.text)				\
	. = ALIGN(PAGE_SIZE);				\
	__entry_tramp_text_end = .;			\
	*(.entry.tramp.rodata)
#else
#define TRAMP_TEXT
#endif

#ifdef CONFIG_UNWIND_TABLES
#define UNWIND_DATA_SECTIONS				\
	.eh_frame : {					\
		__eh_frame_start = .;			\
		*(.eh_frame)				\
		__eh_frame_end = .;			\
	}
#else
#define UNWIND_DATA_SECTIONS
#endif

/*
 * The size of the PE/COFF section that covers the kernel image, which
 * runs from _stext to _edata, must be a round multiple of the PE/COFF
 * FileAlignment, which we set to its minimum value of 0x200. '_stext'
 * itself is 4 KB aligned, so padding out _edata to a 0x200 aligned
 * boundary should be sufficient.
 */
PECOFF_FILE_ALIGNMENT = 0x200;

#ifdef CONFIG_EFI
#define PECOFF_EDATA_PADDING	\
	.pecoff_edata_padding : { BYTE(0); . = ALIGN(PECOFF_FILE_ALIGNMENT); }
#else
#define PECOFF_EDATA_PADDING
#endif

SECTIONS
{
	/*
	 * XXX: The linker does not define how output sections are
	 * assigned to input sections when there are multiple statements
	 * matching the same input section name.  There is no documented
	 * order of matching.
	 */
	DISCARDS
	/DISCARD/ : {
		*(.interp .dynamic)
		*(.dynsym .dynstr .hash .gnu.hash)
	}

	. = KIMAGE_VADDR;

	.head.text : {
		_text = .;
		HEAD_TEXT
	}
	.text : ALIGN(SEGMENT_ALIGN) {	/* Real text segment		*/
		_stext = .;		/* Text and read-only data	*/
			IRQENTRY_TEXT
			SOFTIRQENTRY_TEXT
			ENTRY_TEXT
			TEXT_TEXT
			SCHED_TEXT
			LOCK_TEXT
			KPROBES_TEXT
			HYPERVISOR_TEXT
			*(.gnu.warning)
	}

	. = ALIGN(SEGMENT_ALIGN);
	_etext = .;			/* End of text section */

	/* everything from this point to __init_begin will be marked RO NX */
	RO_DATA(PAGE_SIZE)

	HYPERVISOR_DATA_SECTIONS

	.got : { *(.got) }
	/*
	 * Make sure that the .got.plt is either completely empty or it
	 * contains only the lazy dispatch entries.
	 */
	.got.plt : { *(.got.plt) }
	ASSERT(SIZEOF(.got.plt) == 0 || SIZEOF(.got.plt) == 0x18,
	       "Unexpected GOT/PLT entries detected!")

	/* code sections that are never executed via the kernel mapping */
	.rodata.text : {
		TRAMP_TEXT
		HIBERNATE_TEXT
		KEXEC_TEXT
		IDMAP_TEXT
		. = ALIGN(PAGE_SIZE);
	}

	idmap_pg_dir = .;
	. += PAGE_SIZE;

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	tramp_pg_dir = .;
	. += PAGE_SIZE;
#endif

	reserved_pg_dir = .;
	. += PAGE_SIZE;

	swapper_pg_dir = .;
	. += PAGE_SIZE;

	. = ALIGN(SEGMENT_ALIGN);
	__init_begin = .;
	__inittext_begin = .;

	INIT_TEXT_SECTION(8)

	__exittext_begin = .;
	.exit.text : {
		EXIT_TEXT
	}
	__exittext_end = .;

	. = ALIGN(4);
	.altinstructions : {
		__alt_instructions = .;
		*(.altinstructions)
		__alt_instructions_end = .;
	}

	UNWIND_DATA_SECTIONS

	. = ALIGN(SEGMENT_ALIGN);
	__inittext_end = .;
	__initdata_begin = .;

	init_idmap_pg_dir = .;
	. += INIT_IDMAP_DIR_SIZE;
	init_idmap_pg_end = .;

	.init.data : {
		INIT_DATA
		INIT_SETUP(16)
		INIT_CALLS
		CON_INITCALL
		INIT_RAM_FS
		*(.init.altinstructions .init.bss)	/* from the EFI stub */
	}
	.exit.data : {
		EXIT_DATA
	}

	PERCPU_SECTION(L1_CACHE_BYTES)
	HYPERVISOR_PERCPU_SECTION

	HYPERVISOR_RELOC_SECTION

	.rela.dyn : ALIGN(8) {
		__rela_start = .;
		*(.rela .rela*)
		__rela_end = .;
	}

	.relr.dyn : ALIGN(8) {
		__relr_start = .;
		*(.relr.dyn)
		__relr_end = .;
	}

	. = ALIGN(SEGMENT_ALIGN);
	__initdata_end = .;
	__init_end = .;

	_data = .;
	_sdata = .;
	RW_DATA(L1_CACHE_BYTES, PAGE_SIZE, THREAD_ALIGN)

	/*
	 * Data written with the MMU off but read with the MMU on requires
	 * cache lines to be invalidated, discarding up to a Cache Writeback
	 * Granule (CWG) of data from the cache. Keep the section that
	 * requires this type of maintenance to be in its own Cache Writeback
	 * Granule (CWG) area so the cache maintenance operations don't
	 * interfere with adjacent data.
	 */
	.mmuoff.data.write : ALIGN(SZ_2K) {
		__mmuoff_data_start = .;
		*(.mmuoff.data.write)
	}
	. = ALIGN(SZ_2K);
	.mmuoff.data.read : {
		*(.mmuoff.data.read)
		__mmuoff_data_end = .;
	}

	PECOFF_EDATA_PADDING
	__pecoff_data_rawsize = ABSOLUTE(. - __initdata_begin);
	_edata = .;

	BSS_SECTION(SBSS_ALIGN, 0, 0)

	. = ALIGN(PAGE_SIZE);
	init_pg_dir = .;
	. += INIT_DIR_SIZE;
	init_pg_end = .;

	. = ALIGN(SEGMENT_ALIGN);
	__pecoff_data_size = ABSOLUTE(. - __initdata_begin);
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG
	ELF_DETAILS

	HEAD_SYMBOLS

	/*
	 * Sections that should stay zero sized, which is safer to
	 * explicitly check instead of blindly discarding.
	 */
	.plt : {
		*(.plt) *(.plt.*) *(.iplt) *(.igot .igot.plt)
	}
	ASSERT(SIZEOF(.plt) == 0, "Unexpected run-time procedure linkages detected!")

	.data.rel.ro : { *(.data.rel.ro) }
	ASSERT(SIZEOF(.data.rel.ro) == 0, "Unexpected RELRO detected!")
}

#include "image-vars.h"

/*
 * The HYP init code and ID map text can't be longer than a page each. The
 * former is page-aligned, but the latter may not be with 16K or 64K pages, so
 * it should also not cross a page boundary.
 */
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec0, "HYP init code 0xec0")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec1, "HYP init code 0xec1")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec2, "HYP init code 0xec2")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec3, "HYP init code 0xec3")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec4, "HYP init code 0xec4")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec5, "HYP init code 0xec5")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec6, "HYP init code 0xec6")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec7, "HYP init code 0xec7")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec8, "HYP init code 0xec8")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xec9, "HYP init code 0xec9")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeca, "HYP init code 0xeca")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xecb, "HYP init code 0xecb")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xecc, "HYP init code 0xecc")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xecd, "HYP init code 0xecd")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xece, "HYP init code 0xece")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xecf, "HYP init code 0xecf")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed0, "HYP init code 0xed0")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed1, "HYP init code 0xed1")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed2, "HYP init code 0xed2")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed3, "HYP init code 0xed3")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed4, "HYP init code 0xed4")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed5, "HYP init code 0xed5")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed6, "HYP init code 0xed6")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed7, "HYP init code 0xed7")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed8, "HYP init code 0xed8")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xed9, "HYP init code 0xed9")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeda, "HYP init code 0xeda")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xedb, "HYP init code 0xedb")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xedc, "HYP init code 0xedc")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xedd, "HYP init code 0xedd")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xede, "HYP init code 0xede")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xedf, "HYP init code 0xedf")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee0, "HYP init code 0xee0")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee1, "HYP init code 0xee1")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee2, "HYP init code 0xee2")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee3, "HYP init code 0xee3")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee4, "HYP init code 0xee4")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee5, "HYP init code 0xee5")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee6, "HYP init code 0xee6")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee7, "HYP init code 0xee7")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee8, "HYP init code 0xee8")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xee9, "HYP init code 0xee9")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeea, "HYP init code 0xeea")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeeb, "HYP init code 0xeeb")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeec, "HYP init code 0xeec")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeed, "HYP init code 0xeed")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeee, "HYP init code 0xeee")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeef, "HYP init code 0xeef")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef0, "HYP init code 0xef0")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef1, "HYP init code 0xef1")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef2, "HYP init code 0xef2")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef3, "HYP init code 0xef3")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef4, "HYP init code 0xef4")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef5, "HYP init code 0xef5")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef6, "HYP init code 0xef6")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef7, "HYP init code 0xef7")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef8, "HYP init code 0xef8")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xef9, "HYP init code 0xef9")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xefa, "HYP init code 0xefa")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xefb, "HYP init code 0xefb")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xefc, "HYP init code 0xefc")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xefd, "HYP init code 0xefd")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xefe, "HYP init code 0xefe")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xeff, "HYP init code 0xeff")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf00, "HYP init code 0xf00")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf01, "HYP init code 0xf01")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf02, "HYP init code 0xf02")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf03, "HYP init code 0xf03")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf04, "HYP init code 0xf04")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf05, "HYP init code 0xf05")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf06, "HYP init code 0xf06")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf07, "HYP init code 0xf07")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf08, "HYP init code 0xf08")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf09, "HYP init code 0xf09")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf0a, "HYP init code 0xf0a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf0b, "HYP init code 0xf0b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf0c, "HYP init code 0xf0c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf0d, "HYP init code 0xf0d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf0e, "HYP init code 0xf0e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf0f, "HYP init code 0xf0f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf10, "HYP init code 0xf10")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf11, "HYP init code 0xf11")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf12, "HYP init code 0xf12")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf13, "HYP init code 0xf13")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf14, "HYP init code 0xf14")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf15, "HYP init code 0xf15")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf16, "HYP init code 0xf16")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf17, "HYP init code 0xf17")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf18, "HYP init code 0xf18")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf19, "HYP init code 0xf19")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf1a, "HYP init code 0xf1a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf1b, "HYP init code 0xf1b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf1c, "HYP init code 0xf1c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf1d, "HYP init code 0xf1d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf1e, "HYP init code 0xf1e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf1f, "HYP init code 0xf1f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf20, "HYP init code 0xf20")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf21, "HYP init code 0xf21")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf22, "HYP init code 0xf22")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf23, "HYP init code 0xf23")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf24, "HYP init code 0xf24")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf25, "HYP init code 0xf25")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf26, "HYP init code 0xf26")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf27, "HYP init code 0xf27")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf28, "HYP init code 0xf28")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf29, "HYP init code 0xf29")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf2a, "HYP init code 0xf2a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf2b, "HYP init code 0xf2b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf2c, "HYP init code 0xf2c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf2d, "HYP init code 0xf2d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf2e, "HYP init code 0xf2e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf2f, "HYP init code 0xf2f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf30, "HYP init code 0xf30")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf31, "HYP init code 0xf31")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf32, "HYP init code 0xf32")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf33, "HYP init code 0xf33")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf34, "HYP init code 0xf34")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf35, "HYP init code 0xf35")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf36, "HYP init code 0xf36")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf37, "HYP init code 0xf37")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf38, "HYP init code 0xf38")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf39, "HYP init code 0xf39")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf3a, "HYP init code 0xf3a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf3b, "HYP init code 0xf3b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf3c, "HYP init code 0xf3c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf3d, "HYP init code 0xf3d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf3e, "HYP init code 0xf3e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf3f, "HYP init code 0xf3f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf40, "HYP init code 0xf40")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf41, "HYP init code 0xf41")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf42, "HYP init code 0xf42")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf43, "HYP init code 0xf43")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf44, "HYP init code 0xf44")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf45, "HYP init code 0xf45")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf46, "HYP init code 0xf46")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf47, "HYP init code 0xf47")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf48, "HYP init code 0xf48")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf49, "HYP init code 0xf49")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf4a, "HYP init code 0xf4a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf4b, "HYP init code 0xf4b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf4c, "HYP init code 0xf4c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf4d, "HYP init code 0xf4d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf4e, "HYP init code 0xf4e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf4f, "HYP init code 0xf4f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf50, "HYP init code 0xf50")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf51, "HYP init code 0xf51")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf52, "HYP init code 0xf52")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf53, "HYP init code 0xf53")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf54, "HYP init code 0xf54")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf55, "HYP init code 0xf55")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf56, "HYP init code 0xf56")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf57, "HYP init code 0xf57")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf58, "HYP init code 0xf58")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf59, "HYP init code 0xf59")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf5a, "HYP init code 0xf5a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf5b, "HYP init code 0xf5b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf5c, "HYP init code 0xf5c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf5d, "HYP init code 0xf5d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf5e, "HYP init code 0xf5e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf5f, "HYP init code 0xf5f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf60, "HYP init code 0xf60")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf61, "HYP init code 0xf61")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf62, "HYP init code 0xf62")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf63, "HYP init code 0xf63")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf64, "HYP init code 0xf64")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf65, "HYP init code 0xf65")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf66, "HYP init code 0xf66")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf67, "HYP init code 0xf67")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf68, "HYP init code 0xf68")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf69, "HYP init code 0xf69")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf6a, "HYP init code 0xf6a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf6b, "HYP init code 0xf6b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf6c, "HYP init code 0xf6c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf6d, "HYP init code 0xf6d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf6e, "HYP init code 0xf6e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf6f, "HYP init code 0xf6f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf70, "HYP init code 0xf70")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf71, "HYP init code 0xf71")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf72, "HYP init code 0xf72")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf73, "HYP init code 0xf73")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf74, "HYP init code 0xf74")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf75, "HYP init code 0xf75")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start  == 0xf76, "HYP init code 0xf76")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf77, "HYP init code 0xf77")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf78, "HYP init code 0xf78")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf79, "HYP init code 0xf79")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf7a, "HYP init code 0xf7a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf7b, "HYP init code 0xf7b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf7c, "HYP init code 0xf7c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf7d, "HYP init code 0xf7d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf7e, "HYP init code 0xf7e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf7f, "HYP init code 0xf7f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf80, "HYP init code 0xf80")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf81, "HYP init code 0xf81")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf82, "HYP init code 0xf82")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf83, "HYP init code 0xf83")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf84, "HYP init code 0xf84")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf85, "HYP init code 0xf85")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf86, "HYP init code 0xf86")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf87, "HYP init code 0xf87")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf88, "HYP init code 0xf88")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf89, "HYP init code 0xf89")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf8a, "HYP init code 0xf8a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf8b, "HYP init code 0xf8b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf8c, "HYP init code 0xf8c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf8d, "HYP init code 0xf8d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf8e, "HYP init code 0xf8e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf8f, "HYP init code 0xf8f")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf90, "HYP init code 0xf90")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf91, "HYP init code 0xf91")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf92, "HYP init code 0xf92")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf93, "HYP init code 0xf93")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf94, "HYP init code 0xf94")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf95, "HYP init code 0xf95")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf96, "HYP init code 0xf96")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf97, "HYP init code 0xf97")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf98, "HYP init code 0xf98")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf99, "HYP init code 0xf99")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf9a, "HYP init code 0xf9a")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf9b, "HYP init code 0xf9b")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf9c, "HYP init code 0xf9c")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf9d, "HYP init code 0xf9d")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf9e, "HYP init code 0xf9e")
ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start == 0xf9f, "HYP init code 0xf9f")

ASSERT(__hyp_idmap_text_end - __hyp_idmap_text_start <= PAGE_SIZE,
	"HYP init code too big")
ASSERT(__idmap_text_end - (__idmap_text_start & ~(SZ_4K - 1)) <= SZ_4K,
	"ID map text too big or misaligned")
#ifdef CONFIG_HIBERNATION
ASSERT(__hibernate_exit_text_end - __hibernate_exit_text_start <= SZ_4K,
       "Hibernate exit text is bigger than 4 KiB")
ASSERT(__hibernate_exit_text_start == swsusp_arch_suspend_exit,
       "Hibernate exit text does not start with swsusp_arch_suspend_exit")
#endif
#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
ASSERT((__entry_tramp_text_end - __entry_tramp_text_start) <= 3*PAGE_SIZE,
	"Entry trampoline text too big")
#endif
#ifdef CONFIG_KVM
ASSERT(__hyp_bss_start == __bss_start, "HYP and Host BSS are misaligned")
#endif
/*
 * If padding is applied before .head.text, virt<->phys conversions will fail.
 */
ASSERT(_text == KIMAGE_VADDR, "HEAD is misaligned")

ASSERT(swapper_pg_dir - reserved_pg_dir == RESERVED_SWAPPER_OFFSET,
       "RESERVED_SWAPPER_OFFSET is wrong!")

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
ASSERT(swapper_pg_dir - tramp_pg_dir == TRAMP_SWAPPER_OFFSET,
       "TRAMP_SWAPPER_OFFSET is wrong!")
#endif

#ifdef CONFIG_KEXEC_CORE
/* kexec relocation code should fit into one KEXEC_CONTROL_PAGE_SIZE */
ASSERT(__relocate_new_kernel_end - __relocate_new_kernel_start <= SZ_4K,
       "kexec relocation code is bigger than 4 KiB")
ASSERT(KEXEC_CONTROL_PAGE_SIZE >= SZ_4K, "KEXEC_CONTROL_PAGE_SIZE is broken")
ASSERT(__relocate_new_kernel_start == arm64_relocate_new_kernel,
       "kexec control page does not start with arm64_relocate_new_kernel")
#endif
