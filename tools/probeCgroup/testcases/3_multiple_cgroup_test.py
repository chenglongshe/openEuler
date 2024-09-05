#!/usr/bin/env python
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

from cgroup_utils import create_cgroup, add_process_to_cgroup, get_process_memory_usage, remove_cgroup, check_memory_usage, cleanup
import os
import subprocess
import time

def test_multiple_cgroup(num_procs, num_cgroups):
    subprocess.check_call(['insmod', '../probeCgroup.ko'])
    time.sleep(1)

    cgroups = []
    processes = {}
    pids = {}
    for i in range(num_cgroups):
        cgroup_name = f'test_{i}'
        cgroup_path = create_cgroup(cgroup_name)
        cgroups.append((cgroup_name, cgroup_path))

        for j in range(num_procs):
            process = subprocess.Popen(['./mem-allocate'])
            pid = process.pid
            add_process_to_cgroup(cgroup_path, pid)
            if cgroup_path not in processes:
                processes[cgroup_path] = []
            processes[cgroup_path].append(process)
            if cgroup_path not in pids:
                pids[cgroup_path] = []
            pids[cgroup_path].append(pid)

    time.sleep(0.1)
    try:
        for i in range (100):
            for cgroup_name, cgroup_path in cgroups:
                check_memory_usage(cgroup_name, pids[cgroup_path], False)
            time.sleep(0.01)

        for cgroup_name, cgroup_path in cgroups:
            remove_cgroup(cgroup_path, pids[cgroup_path])
            check_memory_usage(cgroup_name, pids[cgroup_path], True)
            cleanup(processes[cgroup_path])
        subprocess.check_call(['rmmod', 'probeCgroup'])

        print('pass multiple cgroup test!')
    except AssertionError as e:
        print(f"Assertion failed: {e}")
        for cgroup_name, cgroup_path in cgroups:
            remove_cgroup(cgroup_path, pids[cgroup_path])
            cleanup(processes[cgroup_path])
        subprocess.check_call(['rmmod', 'probeCgroup'])

if __name__ == '__main__':
    test_multiple_cgroup(2,2)