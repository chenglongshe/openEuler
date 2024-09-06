# probeCgroup

#### Description
probeCgroup is a process-level cgroup memory monitoring tool based on dynamic tracing (kprobe/kretprobe) technology. By inserting kprobes and kretprobes at the entry and exit points of relevant cgroup functions, this tool can track the memory usage of individual processes within each cgroup in real time.

#### Software Architecture
1. Dynamic Tracing : Insert kprobes and kretprobes at critical points in cgroup functions to capture memory allocation and release events.
2. Hash Table Recording : Record the addresses of pages currently used by each process in a hash table, so that when a page is released, the process it belongs to can be identified.
3. Real-Time Statistics : Provide real-time statistics showing the memory usage of individual processes within each cgroup.

#### Instruction
1. Compile and Load the Module
    a. In the 'probeCgroup' directory, run the 'make' command to compile the module.
    b. Load the module: 'insmod probeCgroup.ko'.
    c. View memory statistics: 'cat /proc/cgroup_memory_usage_per_process'.
        If an OOM (Out of Memory) event occurs in a cgroup, you can see "oom:" followed by the process that experienced the OOM and its memory usage at the time.

2. Automate OOM Scenario
    In the 'probeCgroup' directory, run './run.sh'. This script will automatically set up an OOM scenario and output the content of '/proc/cgroup_memory_usage_per_process' after execution.

3. Perform More Tests
    a. After compiling the module, in the 'testcases' directory, run './run.py'.
    b. This script will perform various tests, including:
        - Loading and unloading the module
        - Each cgroup containing multiple processes
        - Creating multiple cgroups
        - OOM scenarios
        - Multithreading
    c. The tests will take approximately one minute to complete.
