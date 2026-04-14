# US（User Story，用户故事）

## 基于专利《一种虚拟机绑核方法及计算设备》—— 发明人：张海亮

---

## US-01：查看宿主机 NUMA 拓扑

### [名称]
查看宿主机 NUMA 拓扑信息

### [US描述]
作为**运维管理员**，我希望**能够查看宿主机的 NUMA 拓扑结构**，以便**在规划虚拟机绑核策略时有准确的物理资源视图**。

### [依赖]
- 宿主机已安装并运行 Linux 内核（≥ 5.10）

### [用户接口]
- **CLI**：`vm-bindcore topology [--format json|table]`

### [验收准则]

**AC-US-01-01**：
- GIVEN 运维管理员已通过 SSH 登录宿主机
- WHEN 执行 `vm-bindcore topology`
- THEN 系统以表格形式输出各 NUMA 节点的 CPU 列表、在线状态、节点间距离矩阵

**AC-US-01-02**：
- GIVEN 运维管理员需要将拓扑信息传递给自动化脚本
- WHEN 执行 `vm-bindcore topology --format json`
- THEN 系统以 JSON 格式输出结构化的 NUMA 拓扑数据

### [备注]
```bash
# CLI 示例
$ vm-bindcore topology
NUMA Node   CPUs              Online
---------   ----              ------
node0       0-31              yes
node1       32-63             yes

Distance Matrix:
        node0   node1
node0   10      21
node1   21      10

$ vm-bindcore topology --format json
{
  "nodes": [
    {"id": 0, "cpus": [0,1,...,31], "online": true},
    {"id": 1, "cpus": [32,33,...,63], "online": true}
  ],
  "distances": [[10,21],[21,10]]
}
```

---

## US-02：自动绑核（NUMA 感知策略）

### [名称]
以 NUMA 感知策略自动绑定虚拟机 vCPU

### [US描述]
作为**运维管理员**，我希望**能够以 NUMA 感知策略自动为虚拟机设置 vCPU 绑核**，以便**减少虚拟机跨 NUMA 节点的内存访问延迟，提升虚拟机性能**。

### [依赖]
- SR：虚拟机 vCPU 绑核管理系统
- 虚拟机已通过 libvirt 管理，且处于运行状态
- 宿主机有 ≥ 2 个 NUMA 节点

### [用户接口]
- **CLI**：`vm-bindcore pin --vm <domain> --strategy numa-aware [--node <node_id>]`

### [验收准则]

**AC-US-02-01**：
- GIVEN 宿主机有 2 个 NUMA 节点，虚拟机 test-vm 正在运行且有 4 个 vCPU
- WHEN 执行 `vm-bindcore pin --vm test-vm --strategy numa-aware`
- THEN 系统自动选择空闲资源最多的 NUMA 节点，将 4 个 vCPU 绑定到该节点的 CPU 上

**AC-US-02-02**：
- GIVEN 用户指定了特定 NUMA 节点
- WHEN 执行 `vm-bindcore pin --vm test-vm --strategy numa-aware --node 1`
- THEN 系统将 vCPU 绑定到 node1 的 CPU 上

**AC-US-02-03**：
- GIVEN 指定 NUMA 节点的空闲 CPU 不足以容纳所有 vCPU
- WHEN 执行 `vm-bindcore pin --vm large-vm --strategy numa-aware --node 0`
- THEN 系统将溢出的 vCPU 绑定到 NUMA 距离最近的相邻节点

### [备注]
```bash
$ vm-bindcore pin --vm test-vm --strategy numa-aware
[INFO] Detected 2 NUMA nodes
[INFO] Selected node0 (28 free CPUs) for test-vm (4 vCPUs)
[INFO] Pinned vcpu0 -> pCPU 4
[INFO] Pinned vcpu1 -> pCPU 5
[INFO] Pinned vcpu2 -> pCPU 6
[INFO] Pinned vcpu3 -> pCPU 7
[OK] Successfully pinned 4 vCPUs for test-vm
```

---

## US-03：自动绑核（分散策略）

### [名称]
以分散策略自动绑定虚拟机 vCPU

### [US描述]
作为**运维管理员**，我希望**能够以分散策略将虚拟机 vCPU 均匀分布到多个 NUMA 节点**，以便**最大化利用各节点的内存带宽和计算资源**。

### [依赖]
- SR：虚拟机 vCPU 绑核管理系统

### [用户接口]
- **CLI**：`vm-bindcore pin --vm <domain> --strategy spread`

### [验收准则]

**AC-US-03-01**：
- GIVEN 宿主机有 4 个 NUMA 节点，虚拟机 db-vm 有 8 个 vCPU
- WHEN 执行 `vm-bindcore pin --vm db-vm --strategy spread`
- THEN 每个 NUMA 节点分配 2 个 vCPU

### [备注]
```bash
$ vm-bindcore pin --vm db-vm --strategy spread
[INFO] Spreading 8 vCPUs across 4 NUMA nodes
[INFO] Pinned vcpu0 -> node0, pCPU 2
[INFO] Pinned vcpu1 -> node1, pCPU 34
[INFO] Pinned vcpu2 -> node2, pCPU 66
[INFO] Pinned vcpu3 -> node3, pCPU 98
[INFO] Pinned vcpu4 -> node0, pCPU 3
[INFO] Pinned vcpu5 -> node1, pCPU 35
[INFO] Pinned vcpu6 -> node2, pCPU 67
[INFO] Pinned vcpu7 -> node3, pCPU 99
[OK] Successfully pinned 8 vCPUs for db-vm
```

---

## US-04：自动绑核（紧凑策略）

### [名称]
以紧凑策略自动绑定虚拟机 vCPU

### [US描述]
作为**运维管理员**，我希望**能够以紧凑策略将虚拟机 vCPU 绑定到尽量少的物理核心上**，以便**降低 CPU 资源占用面积，为其他虚拟机留出更多物理核心**。

### [依赖]
- SR：虚拟机 vCPU 绑核管理系统

### [用户接口]
- **CLI**：`vm-bindcore pin --vm <domain> --strategy compact`

### [验收准则]

**AC-US-04-01**：
- GIVEN 宿主机开启 SMT（每核 2 线程），虚拟机 small-vm 有 2 个 vCPU
- WHEN 执行 `vm-bindcore pin --vm small-vm --strategy compact`
- THEN 2 个 vCPU 绑定到同一物理核心的 2 个超线程上

### [备注]
```bash
$ vm-bindcore pin --vm small-vm --strategy compact
[INFO] SMT enabled: 2 threads per core
[INFO] Using compact strategy for 2 vCPUs
[INFO] Pinned vcpu0 -> pCPU 4 (core2/thread0)
[INFO] Pinned vcpu1 -> pCPU 36 (core2/thread1)
[OK] Successfully pinned 2 vCPUs for small-vm
```

---

## US-05：手动绑核

### [名称]
手动指定虚拟机 vCPU 到物理 CPU 的绑定关系

### [US描述]
作为**高级运维管理员**，我希望**能够手动指定每个 vCPU 绑定到哪些物理 CPU**，以便**在特殊场景下精确控制虚拟机的 CPU 资源分配**。

### [依赖]
- SR：虚拟机 vCPU 绑核管理系统

### [用户接口]
- **CLI**：`vm-bindcore pin --vm <domain> --manual <mapping>`

### [验收准则]

**AC-US-05-01**：
- GIVEN 虚拟机 custom-vm 有 2 个 vCPU
- WHEN 执行 `vm-bindcore pin --vm custom-vm --manual vcpu0:0-3,vcpu1:4-7`
- THEN vcpu0 被绑定到 pCPU 0、1、2、3，vcpu1 被绑定到 pCPU 4、5、6、7

**AC-US-05-02**：
- GIVEN 用户指定了不存在的 pCPU 编号（如 pCPU 256，但宿主机仅 64 核）
- WHEN 执行绑核命令
- THEN 系统返回错误信息 "pCPU 256 does not exist on this host"

### [备注]
```bash
$ vm-bindcore pin --vm custom-vm --manual vcpu0:0-3,vcpu1:4-7
[INFO] Manual pinning for custom-vm
[INFO] Pinned vcpu0 -> pCPU 0-3
[INFO] Pinned vcpu1 -> pCPU 4-7
[OK] Successfully pinned 2 vCPUs for custom-vm
```

---

## US-06：查看绑核状态

### [名称]
查看虚拟机当前的 vCPU 绑核状态

### [US描述]
作为**运维管理员**，我希望**能够查看虚拟机当前的 vCPU 绑核关系**，以便**确认绑核配置是否正确生效并进行日常巡检**。

### [依赖]
- SR：虚拟机 vCPU 绑核管理系统

### [用户接口]
- **CLI**：`vm-bindcore show --vm <domain> [--format json|table]`

### [验收准则]

**AC-US-06-01**：
- GIVEN 虚拟机 test-vm 已设置绑核
- WHEN 执行 `vm-bindcore show --vm test-vm`
- THEN 系统以表格显示每个 vCPU 绑定的 pCPU 列表及所在 NUMA 节点

**AC-US-06-02**：
- GIVEN 虚拟机 test-vm 未设置绑核
- WHEN 执行 `vm-bindcore show --vm test-vm`
- THEN 系统显示 "No pinning configured for test-vm (using system default scheduling)"

### [备注]
```bash
$ vm-bindcore show --vm test-vm
VM: test-vm (4 vCPUs)
vCPU    Pinned pCPUs    NUMA Node
----    ------------    ---------
vcpu0   4               node0
vcpu1   5               node0
vcpu2   6               node0
vcpu3   7               node0
Strategy: numa-aware
```

---

## US-07：解除绑核

### [名称]
解除虚拟机的 vCPU 绑核关系

### [US描述]
作为**运维管理员**，我希望**能够解除虚拟机的绑核配置**，以便**让虚拟机恢复系统默认调度，或为重新规划绑核做准备**。

### [依赖]
- SR：虚拟机 vCPU 绑核管理系统

### [用户接口]
- **CLI**：`vm-bindcore unpin --vm <domain>`

### [验收准则]

**AC-US-07-01**：
- GIVEN 虚拟机 test-vm 当前有绑核配置
- WHEN 执行 `vm-bindcore unpin --vm test-vm`
- THEN 所有 vCPU 的绑核关系被解除，`vm-bindcore show --vm test-vm` 显示无绑核状态

### [备注]
```bash
$ vm-bindcore unpin --vm test-vm
[INFO] Unpinning all vCPUs for test-vm
[INFO] Unpinned vcpu0
[INFO] Unpinned vcpu1
[INFO] Unpinned vcpu2
[INFO] Unpinned vcpu3
[INFO] Removed persistent config for test-vm
[OK] Successfully unpinned 4 vCPUs for test-vm
```

---

## US-08：绑核配置持久化与恢复

### [名称]
绑核配置持久化与自动恢复

### [US描述]
作为**运维管理员**，我希望**绑核配置能够持久化保存，并在宿主机重启后自动恢复**，以便**避免每次重启后手动重新配置绑核**。

### [依赖]
- SR：虚拟机 vCPU 绑核管理系统
- systemd 服务管理

### [用户接口]
- **CLI**：绑核操作默认自动持久化
- **配置项**：`/etc/vm-bindcore/pinning.d/<domain>.json`
- **CLI**：`vm-bindcore restore [--vm <domain>]`

### [验收准则]

**AC-US-08-01**：
- GIVEN 执行绑核操作成功后
- WHEN 查看 `/etc/vm-bindcore/pinning.d/test-vm.json`
- THEN 配置文件存在，且包含完整的 vCPU-to-pCPU 映射和策略信息

**AC-US-08-02**：
- GIVEN 宿主机重启后 vm-bindcore systemd 服务已启动
- WHEN 虚拟机 test-vm 启动完成
- THEN 绑核关系与重启前一致

### [备注]
```json
// /etc/vm-bindcore/pinning.d/test-vm.json
{
  "domain": "test-vm",
  "strategy": "numa-aware",
  "pinning": {
    "vcpu0": [4],
    "vcpu1": [5],
    "vcpu2": [6],
    "vcpu3": [7]
  },
  "created_at": "2026-04-14T08:00:00Z",
  "updated_at": "2026-04-14T08:00:00Z"
}
```
