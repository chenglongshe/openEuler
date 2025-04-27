/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: enfs log
 * Author: y00583252
 * Create: 2023-07-31
 */

#ifndef ENFS_LOG_H
#define ENFS_LOG_H

#include

extern unsigned int enfs_debug;

#define enfs_log_info(fmt, ...) printk(KERN_INFO "enfs:[%s]" pr_fmt(fmt), __func__, ##__VA_ARGS__)
#define enfs_log_error(fmt, ...) printk(KERN_ERR "enfs:[%s]" pr_fmt(fmt), __func__, ##__VA_ARGS__)
#define enfs_log_debug(fmt, ...)                                \
	do {                                                        \
		if (enfs_debug != 0) {                                  \
			printk(KERN_INFO "enfs:[%s]" pr_fmt(fmt), __func__, \
				   ##__VA_ARGS__);                              \
		}                                                       \
	} while (0)

#endif // ENFS_ERRCODE_H
