#!/bin/bash
source /home/name/common/env.sh

nr=$1
mode=${2:-"sequential"}  # 默认顺序模式，可选：sequential(顺序)或interleave(交错)
[[ -z $nr ]] && { echo "Usage: $0 <number> [sequential|interleave]"; exit 1; }

docker stop $(docker ps -aq) 2>/dev/null
docker rm $(docker ps -aq) 2>/dev/null

mkdir -p /var/run/netns
systemctl stop irqbalance

if [[ $mode == "interleave" ]]; then
    deployment_order=()
    for i in $(seq 0 $((nr - 1))); do
        for j in $(seq $numa_start $numa_end); do
            deployment_order+=("$j $i")
        done
    done
else
    deployment_order=()
    for j in $(seq $numa_start $numa_end); do
        for i in $(seq 0 $((nr - 1))); do
            deployment_order+=("$j $i")
        done
    done
fi

ip_count=0
for deployment in "${deployment_order[@]}"; do
    j=$(echo $deployment | awk '{print $1}')
    i=$(echo $deployment | awk '{print $2}')
    
    docker run --cpus=2 --cpuset-cpus=${cpus_numa[$j]} --cpuset-mems=$j -m 8g \
        --cap-add CAP_SYS_ADMIN --privileged=true -itd \
        --name redis-docker-bridge-numa$j-$i \
        -v /home/name/redis_image:/home -v /usr:/usr -v /mnt:/mnt \
        -v /lib/modules:/lib/modules -v /data:/data -v /etc:/etc \
        openeuler-22.03-lts-sp4 /bin/bash

    pid=$(docker inspect -f '{{.State.Pid}}' redis-docker-bridge-numa$j-$i)
    vf=$((j * nr + i))
    ip_count=$((ip_count + 1))
    IP=${UNREACHABLE_IPS[$((ip_count))]}

    echo "vf: ${vf} IP: ${IP}"   
 
    ln -sf /proc/$pid/ns/net /var/run/netns/redis-docker-bridge-numa$j-$i
    ip link set enp24s0f0v$vf netns redis-docker-bridge-numa$j-$i
    docker exec redis-docker-bridge-numa$j-$i ifconfig enp24s0f0v$vf $IP/16

    docker exec -d redis-docker-bridge-numa$j-$i \
        /home/c00838100/redis-origin/src/redis-server \
        /home/c00838100/redis.conf --bind 0.0.0.0 --port 6379

    while ! docker exec redis-docker-bridge-numa$j-$i ps -ef | grep -q "[r]edis-server"; do
        sleep 0.5
    done
    
    pid=$(docker exec redis-docker-bridge-numa$j-$i pgrep -f redis-server)
    core_array=(${cpus[$j]})
    cpu_core=${core_array[$((i * CPU_DIST))]}
    docker exec redis-docker-bridge-numa$j-$i taskset -pc $cpu_core $pid

    vf_num=$vf
    vf_dev=$(ls -la /sys/class/net/enp24s0f0np0/device/virtfn${vf_num} | awk '{print $11}' | sed "s|../||g")
    irq_core=$((cpu_core + 1))

    for irq in `cat /proc/interrupts | grep $vf_dev | awk -F ':' '{print $1}'`; do    
        echo $irq_core > /proc/irq/$irq/smp_affinity_list
    done
done

echo "部署完成! 模式: $mode, 容器数量: $((nr * (numa_end - numa_start + 1)))"
