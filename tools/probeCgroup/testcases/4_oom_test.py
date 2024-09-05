#!/usr/bin/env python
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

from cgroup_utils import create_cgroup, add_process_to_cgroup, get_process_memory_usage, remove_cgroup, check_memory_usage, cleanup, get_oom_process_memory_usage
import os
import subprocess
import time

def test_oom(num_procs):
    subprocess.check_call(['insmod', '../probeCgroup.ko'])
    time.sleep(1)

    cgroup_name = 'test'
    cgroup_path = create_cgroup(cgroup_name)

    with open(f"/sys/fs/cgroup/memory/{cgroup_name}/memory.limit_in_bytes", 'w') as limit_file:
        limit_file.write("5M")
    with open(f"/sys/fs/cgroup/memory/{cgroup_name}/memory.swappiness", 'w') as swap_file:
        swap_file.write("0")

    processes = []
    pids = []
    for i in range(num_procs):
        process = subprocess.Popen(['./simple-mem-allocate'])
        pid = process.pid
        add_process_to_cgroup(cgroup_path, pid)
        processes.append(process)
        pids.append(pid)

    time.sleep(6)

    try:
        for pid in pids:
            memory_usage = get_oom_process_memory_usage(pid, cgroup_name)
            assert memory_usage is not None, f"Memory usage(oom) not found for PID {pid}"
            assert memory_usage > 0, f"Memory usage should be greater than zero for PID {pid}"

        remove_cgroup(cgroup_path, pids)
        check_memory_usage(cgroup_name, pids, True)
        cleanup(processes)
        subprocess.check_call(['rmmod', 'probeCgroup'])

        print('pass oom test!')
    except AssertionError as e:
        print(f"Assertion failed: {e}")
        remove_cgroup(cgroup_path, pids)
        cleanup(processes)
        subprocess.check_call(['rmmod', 'probeCgroup'])

if __name__ == '__main__':
    test_oom(1)