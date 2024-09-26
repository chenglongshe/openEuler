/* SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0 */
/*
 * Copyright (C) 2024 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by MercyHeart <2384268568@qq.com>
 * Created by Feynman G <gxnorz@gmail.com>
 */
#ifndef __EROFS_BCJ_H
#define __EROFS_BCJ_H

#ifdef __cplusplus
extern "C"
{
#endif

int erofs_bcj_filedecode(char *filepath, int bcj_type);
int erofs_bcj_fileread(int fd, void *buf, size_t nbytes, off_t offset);
int bcj_code(uint8_t *buf, uint32_t startpos, size_t size, int bcj_type, bool is_encode);
int page_bcj_decode(struct page *page, size_t startpos);

#ifdef __cplusplus
}
#endif

#endif
