// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
// Copyright (C) 2025 KylinSoft Co., Ltd. All rights reserved.

#ifndef __IOGLITCH_H
#define __IOGLITCH_H

#define BLK_NS_TO_US		1000
#define BLK_DEFAULT_A2D_US	1000
#define BLK_DEFAULT_A2G_US	600
#define MAX_SLOTS		64
#define DISK_PATH_LEN		32

#define MINORBITS	20
#define MINORMASK	((1U << MINORBITS) - 1)

#define MAJOR(dev)	((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)	((unsigned int) ((dev) & MINORMASK))
#define MKDEV(ma, mi)	(((ma) << MINORBITS) | (mi))

enum latency_group {
	A2T = 0,
	T2R,
	R2G,
	G2I,
	I2D,
	G2D,
	D2C,
	A2C,
	LATENCY_NR,
};

char *latency_name[] = {
	"A2T",
	"T2R",
	"R2G",
	"G2I",
	"I2D",
	"G2D",
	"D2C",
	"A2C"
};

struct hist_key {
	char rwbs[8];
	__u32 dev;
};

struct latency_info {
	char rwbs[8];
	__u32 max_latency;
	__u32 sector;
	__u32 nr_sector;
};

struct hist {
	__u32   alloc_to_throend[MAX_SLOTS];
	__u32   throend_to_rqsend[MAX_SLOTS];
	__u32   rqsend_to_getrq[MAX_SLOTS];
	__u32   getrq_to_insert[MAX_SLOTS];
	__u32   insert_to_issue[MAX_SLOTS];
	__u32	getrq_to_issue[MAX_SLOTS];
	__u32   issue_to_complete[MAX_SLOTS];
	__u32   alloc_to_complete[MAX_SLOTS];
	/* a2t/t2r/r2g/g2i/i2d/g2d/d2c/a2c*/
	struct latency_info latency_info[LATENCY_NR];
};

#endif
