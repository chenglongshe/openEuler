镜像下载：wget https://dl-cdn.openeuler.openatom.cn/openEuler-22.03-LTS-SP4/docker_img/aarch64/openEuler-docker.aarch64.tar.xz
导入容器镜像：docker load < ./openEuler-docker.aarch64.tar.xz
redis下载：https://redis.io/downloads/  版本6.2
修改点：
protected-mode no
daemonize yes
cd redis-xx && make && make install

# 1. runc
基线测试方法： 6.6.0-92.0.0.96.oe2403sp2.aarch64
1. 硬件：服务器，CX5双网口网卡共160个vf 
2. redis容器：每个2u8g容器1个核绑定redis实例，另1个核绑定VF中断；且两个核同属一个SMT 
3. 部署方式：numa 3的最后一个cluster部署1个观察容器，其他背景容器按顺序部署在numa 0-3 
4. 客户端：一台客户端单独加压观察容器，保证客户端不是瓶颈；其他四台客户端加压背景容器 
5. 组网方式：每个容器一个VF

echo 80 > /sys/class/net/enp24s0f0np0/device/sriov_numvfs
echo 80 > /sys/class/net/enp24s0f1np1/device/sriov_numvfs
cd /home/name/redis_image/test_inbalance_refactor
sh create_runc_passthrough.sh 20

# 同时跑4个redis
sh runc_parallel.sh 4
# 同时跑80个redis
sh runc_parallel.sh 80

# 2. vm：6.6.0-95.0.0
1. BIOS配置：开启SMMU、开启GiCv4.1 
2. 服务器内核配置：开启中断直通kvm-arm.vgic_v4_enable=1、1G大页：default_hugepagesz=1G hugepagesz=1G 
3. 虚拟机规格：2台虚机，单虚机2个numa，160U，240G内存，40个VF直通网卡 
4. host和虚拟机内开启调度策略优化（echo IRQ_AVG > /sys/kernel/debug/sched/features）


smt开关：Advanced -> AMD CBS-> CPU Common Options  -> performance -> SMU Common Options
IOMMU开关：Advanced -> AMD CBS-> NBIO Common Options  -> IOMMU/security -> IOMMU

分配大页：  
echo 120 > /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages  
echo 120 > /sys/devices/system/node/node1/hugepages/hugepages-1048576kB/nr_hugepages  
echo 120 > /sys/devices/system/node/node2/hugepages/hugepages-1048576kB/nr_hugepages  
echo 120 > /sys/devices/system/node/node3/hugepages/hugepages-1048576kB/nr_hugepages

virsh create /home/name/vms/vm1/vm1.xml
virsh create /home/name/vms/vm2/vm2.xml

ssh root@vm_ip1
cd /home/name/redis_image/test_inbalance_refactor
sh create_runc_passthrough.sh 20

ssh root@vm_ip2
cd /home/name/redis_image/test_inbalance_refactor
sh create_runc_passthrough.sh 20

# host上
cd /home/name/redis_image/test_inbalance_refactor
sh runc_parallel.sh 80
