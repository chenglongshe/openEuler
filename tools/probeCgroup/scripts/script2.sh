#! /bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

current_dir=$(pwd)
cd /sys/fs/cgroup/memory
mkdir test
cd test
sh -c "echo $$ >> cgroup.procs"
sh -c "echo 5M > memory.limit_in_bytes"
sh -c "echo 0 > memory.swappiness"
cd "$current_dir"
cd ../testcases
./simple-mem-allocate
