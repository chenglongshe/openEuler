# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

import os
import subprocess
import time

def create_cgroup(cgroup_name):
    cgroup_path = f'/sys/fs/cgroup/memory/{cgroup_name}'

    try:
        os.makedirs(cgroup_path)
    except FileExistsError:
        pass

    return cgroup_path

def add_process_to_cgroup(cgroup_path, pid):
    with open(os.path.join(cgroup_path, 'cgroup.procs'), 'w') as procs_file:
        procs_file.write(str(pid))

def get_process_memory_usage(pid, cgroup_name):
    cur_name = ''
    with open('/proc/cgroup_memory_usage_per_process', 'r') as file:
        for line in file:
            parts = line.strip().split()
            if len(parts) >= 4 and parts[0] == 'cgroup':
                cur_name = parts[3]
            if len(parts) >= 3 and parts[0] != 'cgroup' and cur_name == cgroup_name and parts[0] != 'pid' and int(parts[0]) == pid:
                return int(parts[2])
    return None

def get_process_kmem_usage(pid, cgroup_name):
    cur_name = ''
    with open('/proc/cgroup_memory_usage_per_process', 'r') as file:
        for line in file:
            parts = line.strip().split()
            if len(parts) >= 4 and parts[0] == 'cgroup':
                cur_name = parts[3]
            if len(parts) >= 4 and parts[0] != 'cgroup' and cur_name == cgroup_name and parts[0] != 'pid' and int(parts[0]) == pid:
                return int(parts[3])
    return None

def remove_cgroup(cgroup_path, pids):
    for pid in pids:
        with open('/sys/fs/cgroup/memory/cgroup.procs', 'w') as backup_file:
            backup_file.write(str(pid))
    os.rmdir(cgroup_path)
    return

def check_memory_usage(cgroup_name, pids, delete):
    memory_sum = 0
    for pid in pids:
        memory_usage = get_process_memory_usage(pid, cgroup_name)
        if delete == False:
            assert memory_usage is not None, f"Memory usage not found for PID {pid}"
            assert memory_usage >= 0, f"Memory usage should be greater than zero for PID {pid}"
            memory_sum += memory_usage
        else:
            assert memory_usage is None, f"Error: Memory usage should not be available for PID {pid} after deleting the cgroup."
    if delete == False:
        with open(f"/sys/fs/cgroup/memory/{cgroup_name}/memory.usage_in_bytes", 'r') as file:
            content = file.readline().strip()
        memory_read = int(content)
        memory_sum *= 1024
        delta = abs(memory_read - memory_sum)
        # print(f"read: {memory_read}")
        # print(f"sum : {memory_sum}")
        if (delta > max(memory_read, memory_sum) * 0.1):
            return 1
        else:
            return 0
    else:
        return 0

def check_kmem_usage(cgroup_name, pids, delete):
    kmem_sum = 0
    for pid in pids:
        kmem_usage = get_process_kmem_usage(pid, cgroup_name)
        if delete == False:
            assert kmem_usage is not None, f"Kmem usage not found for PID {pid}"
            assert kmem_usage >= 0, f"Kmem usage should be greater than zero for PID {pid}"
            kmem_sum += kmem_usage
        else:
            assert kmem_usage is None, f"Error: Kmem usage should not be available for PID {pid} after deleting the cgroup."
    if delete == False:
        with open(f"/sys/fs/cgroup/memory/{cgroup_name}/memory.kmem.usage_in_bytes", 'r') as file:
            content = file.readline().strip()
        kmem_read = int(content)
        kmem_sum *= 1024
        delta = abs(kmem_read - kmem_sum)
        # print(f"kmem read: {kmem_read}")
        # print(f"kmem sum : {kmem_sum}")
        # assert delta <= max(kmem_read, kmem_sum) * 0.2, f"Kmem read by probeCgroup is not accurate, {kmem_read}, {kmem_sum}"

def cleanup(processes):
    for process in processes:
        process.terminate()
        process.wait()

def get_oom_process_memory_usage(pid, cgroup_name):
    cur_name = ''
    oom = False
    with open('/proc/cgroup_memory_usage_per_process', 'r') as file:
        for line in file:
            parts = line.strip().split()
            if len(parts) >= 4 and parts[0] == 'cgroup':
                cur_name = parts[3]
                oom = False
            if len(parts) >= 1 and parts[0] == 'oom:':
                oom = True
            if len(parts) >= 3 and parts[0] != 'cgroup' and cur_name == cgroup_name and parts[0] != 'pid' and int(parts[0]) == pid and oom == True:
                return int(parts[2])
    return None