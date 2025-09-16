#!/bin/bash

CPU_DIST=4

cpus_numa=(
"0-79" // todo
)

vf_core=317 // todo
redis_core=316

numa_start=0
numa_end=3

cpus=(
"0 1"// todo
"80 81"
)

CLIENT=(
ip1 // todo
ip2 // todo
)

count=80
CLIENT_OBSERVE=ip3 // todo

UNREACHABLE_IPS=(
ip1 // todo
ip2 // todo
)
