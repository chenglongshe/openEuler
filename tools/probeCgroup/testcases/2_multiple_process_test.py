#!/usr/bin/env python
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

from cgroup_utils import create_cgroup, add_process_to_cgroup, get_process_memory_usage, remove_cgroup, check_memory_usage, cleanup, check_kmem_usage
import os
import subprocess
import time

def test_multiple_process(num_procs):
    subprocess.check_call(['insmod', '../probeCgroup.ko'])
    time.sleep(1)

    cgroup_name = 'test'
    cgroup_path = create_cgroup(cgroup_name)

    processes = []
    pids = []

    for i in range(num_procs):
        process = subprocess.Popen(['./mem-allocate'])
        pid = process.pid
        add_process_to_cgroup(cgroup_path, pid)
        processes.append(process)
        pids.append(pid)

    time.sleep(0.1)
    try:
        count = 0
        for i in range (2000):
            count += check_memory_usage(cgroup_name, pids, False)
            time.sleep(0.01)
            assert count <= 50, f"Memory read by probeCgroup is not accurate"

        remove_cgroup(cgroup_path, pids)
        check_memory_usage(cgroup_name, pids, True)
        cleanup(processes)
        subprocess.check_call(['rmmod', 'probeCgroup'])

        print('pass multiple process test!')
    except AssertionError as e:
        print(f"Assertion failed: {e}")
        remove_cgroup(cgroup_path, pids)
        cleanup(processes)
        subprocess.check_call(['rmmod', 'probeCgroup'])

if __name__ == '__main__':
    test_multiple_process(3)