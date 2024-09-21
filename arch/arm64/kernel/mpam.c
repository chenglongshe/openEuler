/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2021 Arm Ltd. */

#include <asm/mpam.h>

#include <linux/arm_mpam.h>
#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <linux/crash_dump.h>

DEFINE_STATIC_KEY_FALSE(arm64_mpam_has_hcr);
DEFINE_STATIC_KEY_FALSE(mpam_enabled);
DEFINE_PER_CPU(u64, arm64_mpam_default);
DEFINE_PER_CPU(u64, arm64_mpam_current);

static const struct midr_range mpam_disable_list[] = {
	MIDR_ALL_VERSIONS(MIDR_HISI_TSV110),
	{ /* sentinel */ }
};

static bool __read_mostly mpam_detect_enabled;
static int __init mpam_setup(char *str)
{
	if (!is_midr_in_range_list(read_cpuid_id(), mpam_disable_list))
		mpam_detect_enabled = true;

	if (!strcmp(str, "acpi"))
		mpam_detect_enabled = true;

	return 0;
}
early_param("mpam", mpam_setup);

bool mpam_detect_is_enabled(void)
{
	return mpam_detect_enabled;
}

static int __init arm64_mpam_register_cpus(void)
{
	u16 partid_max;
	u64 mpamidr;
	u8 pmg_max;

	if (is_kdump_kernel())
		return 0;

	if (!mpam_detect_is_enabled())
		return 0;

	mpamidr = read_sanitised_ftr_reg(SYS_MPAMIDR_EL1);
	partid_max = FIELD_GET(MPAMIDR_PARTID_MAX, mpamidr);
	pmg_max = FIELD_GET(MPAMIDR_PMG_MAX, mpamidr);

	return mpam_register_requestor(partid_max, pmg_max);
}
arch_initcall(arm64_mpam_register_cpus);
