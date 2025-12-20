#! /bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

cd /proc
cat cgroup_memory_usage_per_process

cat /sys/fs/cgroup/memory/test/cgroup.procs > /sys/fs/cgroup/memory/cgroup.procs
rmdir /sys/fs/cgroup/memory/test
# cat cgroup_memory_usage_per_process
rmmod probeCgroup
