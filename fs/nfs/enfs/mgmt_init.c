/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: mgmt component init
 * Author: y00583252
 * Create: 2023-07-31
 */

#include "mgmt_init.h"
#include
#include "enfs_errcode.h"
#include "enfs_config.h"

int32_t mgmt_init(void)
{
	return enfs_config_timer_init();
}

void mgmt_fini(void)
{
	enfs_config_timer_exit();
	return;
}


