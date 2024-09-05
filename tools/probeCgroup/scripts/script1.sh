#! /bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

cd ..
make
insmod probeCgroup.ko

cd testcases
gcc simple-mem-allocate.c -o simple-mem-allocate
