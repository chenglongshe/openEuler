/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * Description: ubagg jetty ops header
 * Author: Weicheng Zhang
 * Create: 2025-08-13
 * Note:
 * History: 2025-08-13: Create file
 */

#ifndef UBAGG_JETTY_H
#define UBAGG_JETTY_H

#include "ubagg_types.h"

struct ubcore_tjetty *ubagg_import_jfr(struct ubcore_device *dev,
				       struct ubcore_tjetty_cfg *cfg,
				       struct ubcore_udata *udata);

int ubagg_unimport_jfr(struct ubcore_tjetty *tjfr);

struct ubcore_tjetty *ubagg_import_jetty(struct ubcore_device *dev,
					 struct ubcore_tjetty_cfg *cfg,
					 struct ubcore_udata *udata);

int ubagg_unimport_jetty(struct ubcore_tjetty *tjetty);

#endif // UBAGG_SEG_H
