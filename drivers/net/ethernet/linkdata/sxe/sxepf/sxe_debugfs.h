/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 - 2025 Intel Corporation. */
#ifndef __SXE_DEBUGFS_H__
#define __SXE_DEBUGFS_H__

struct sxe_adapter;

void sxe_debugfs_entries_init(struct sxe_adapter *adapter);

void sxe_debugfs_entries_exit(struct sxe_adapter *adapter);

void sxe_debugfs_init(void);

void sxe_debugfs_exit(void);

#endif