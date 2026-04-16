# US（User Story，用户故事）

## 基于专利《一种优化虚拟机内业务绑核性能的方法》—— 发明人：张海亮

---

## US-01：VM vCPU 绑核状态自动发现与实时观测

### 【US描述】

作为**虚拟化平台管理员**，我希望**系统能自动发现所有运行中 VM 的 vCPU 绑核信息（从 `virsh vcpupin` 获取每个 vCPU 的 CPU 亲和性范围），并提供按 VM/vCPU 粒度的实时绑核状态查看能力**，以便**在发生性能异常时，能够基于 VM 粒度视图直接定位到具体虚拟机与相关 vCPU，无需在多个工具之间切换**。

### 【依赖】

- QEMU/KVM 虚拟化环境
- libvirt（提供 `virsh vcpupin` 命令）

### 【用户接口】

| 位置 | 接口 | 说明 |
|------|------|------|
| Host 侧 | `vaffinity discover [--vm <domain>]` | 自动发现 VM 的 vCPU 绑核信息（从 virsh vcpupin 获取） |
| Host 侧 | `vaffinity status [--vm <domain>] [--format json\|table]` | 查看 vCPU 绑核状态（模式、pCPU、来源） |
| Host 侧 | `vaffinity map [--format json\|table]` | 查看全局 CPU 映射表 |

### 【验收准则】

**AC-01-01：自动发现 VM 的每 vCPU 绑核范围**

- GIVEN 宿主机上运行虚拟机 FusionOS-23T10（19 vCPU），其绑核配置为 vcpu0-15 绑定 `5-47,53-63`，vcpu16-18 绑定 `0-63`
- WHEN 管理员执行 `vaffinity discover --vm FusionOS-23T10`
- THEN **可观察到**输出显示 19 个 vCPU 的各自 cpuset，vcpu0-15 的 cpuset 为 `5-47,53-63`，vcpu16-18 的 cpuset 为 `0-63`

**AC-01-02：listener 启动时自动发现所有运行中的 VM**

- GIVEN 宿主机上运行多台 VM
- WHEN 管理员启动 `vaffinity-listener` 服务
- THEN **可观察到**服务启动日志中包含各 VM 的自动发现记录，`vaffinity status` 可查看所有已发现 VM 的状态

**AC-01-03：状态查询支持 VM 粒度定位**

- GIVEN 宿主机运行 vm1 和 vm2，vm1 的 vcpu2 为 `exclusive` 模式
- WHEN 管理员执行 `vaffinity status --vm vm1`
- THEN **可观察到** vcpu2 显示 `exclusive` 模式、绑定的具体 pCPU、来源 `guest-pin`，其余 vCPU 显示 `range` 模式

---

## US-02：Guest 侧业务绑核透明感知与 Host 侧自动执行

### 【US描述】

作为**虚拟化平台管理员**，我希望**当虚拟机内的业务应用进行 CPU 亲和性绑定时，系统能够自动感知并在宿主机侧将对应 vCPU 从范围绑核动态切换为 1:1 独占绑核；当业务解除绑核后自动恢复为范围绑核**，以便**在 CPU 超分配部署场景下，无需人工干预即可兼顾资源利用率与业务性能稳定性**。

### 【依赖】

- Guest 内安装 `vaffinity-guest` 包
- Host 侧 `vaffinity-listener` 服务运行中
- VSOCK 或 virtio-serial 通信通道可用

### 【用户接口】

| 位置 | 接口 | 说明 |
|------|------|------|
| Guest 侧 | `vaffinity-guest start [--mode vsock\|virtio-serial]` | 启动 eBPF 绑核感知模块 |
| Guest 侧 | `vaffinity-guest stop` | 停止感知模块 |
| Guest 侧 | `vaffinity-guest status` | 查看感知模块运行状态 |

### 【验收准则】

**AC-02-01：Guest 内业务绑核 → Host 侧 vCPU 自动切换为 1:1 独占绑核**

- GIVEN 宿主机运行 vm1，Guest 内启动 `vaffinity-guest start`，Host 侧 `vaffinity status --vm vm1` 确认所有 vCPU 为 `range` 模式
- WHEN 管理员在 vm1 Guest 内运行 `taskset -c 2 <workload>`
- THEN 管理员在 Host 侧执行 `vaffinity status --vm vm1`，**可观察到** vcpu2 变为 `exclusive` 模式并绑定到具体 pCPU

**AC-02-02：Guest 内业务解除绑核 → vCPU 自动恢复为范围绑核**

- GIVEN 承接 AC-02-01，vm1 的 vcpu2 为 `exclusive` 模式
- WHEN 管理员在 Guest 内终止该业务进程
- THEN Host 侧执行 `vaffinity status --vm vm1`，**可观察到** vcpu2 恢复为 `range` 模式；`vaffinity map` 显示原独占 pCPU 恢复为 `shared`

**AC-02-03：eBPF CO-RE 跨 Guest 内核版本兼容**

- GIVEN 同一编译产物的 eBPF 截获程序分别部署到 openEuler 22.03（5.10）和 24.03（6.6）的 Guest
- WHEN 在各 Guest 内启动 `vaffinity-guest start` 并运行绑核业务
- THEN 各 Guest 均正常工作，Host 侧可观察到 vCPU 状态正确切换

**AC-02-04：感知模块对非绑核业务性能无显著影响**

- GIVEN 感知模块已在 Guest 内启动，vm1 内运行非绑核的普通业务
- WHEN 管理员测量业务性能
- THEN **可观察到**业务吞吐量与未加载感知模块时差异 ≤ 1%

---

## US-03：多 VM 绑核冲突自动避免与全局 CPU 资源管理

### 【US描述】

作为**虚拟化平台管理员**，我希望**系统在执行 1:1 独占绑核时能自动避免多虚拟机之间的 pCPU 冲突，并维护全局 CPU 映射表实时反映资源分配状态**，以便**通过全局资源映射避免绑核冲突，支撑主机 CPU 资源的统一查看与协同操作**。

### 【依赖】

- Host 侧 `vaffinity` 管理工具
- 全局 CPU 映射持久化存储

### 【用户接口】

| 位置 | 接口 | 说明 |
|------|------|------|
| Host 侧 | `vaffinity map [--format json\|table] [--limit N]` | 查看全局 CPU 映射表（每个 pCPU 的状态与归属） |
| Host 侧 | `vaffinity unpin-all --vm <domain>` | 批量恢复指定 VM 所有 vCPU 为范围绑核 |

### 【验收准则】

**AC-03-01：多 VM 绑核冲突自动避免**

- GIVEN 宿主机运行 vm1 和 vm2（cpuset 有重叠），vm1 的 vcpu0 已被 1:1 绑定到 pCPU N
- WHEN vm2 Guest 内触发业务绑核
- THEN `vaffinity map` 显示 vm2 的 vCPU 绑定到另一个不冲突的 pCPU，**不与** vm1 的 pCPU N 冲突

**AC-03-02：全局映射表正确反映多 VM 状态**

- GIVEN 宿主机运行 vm1（2 个 vCPU 已绑核）、vm2（1 个 vCPU 已绑核）、vm3（无绑核）
- WHEN 管理员执行 `vaffinity map`
- THEN **可观察到**每个 pCPU 的 `shared`/`exclusive` 状态正确，独占的 pCPU 显示对应 VM:vCPU

**AC-03-03：批量解绑恢复**

- GIVEN vm1 有多个 vCPU 为 `exclusive` 模式
- WHEN 管理员执行 `vaffinity unpin-all --vm vm1`
- THEN 所有 vCPU 恢复为 `range` 模式，对应的 pCPU 恢复为 `shared`

---

## US-04：绑核事件日志审计

### 【US描述】

作为**虚拟化平台管理员**，我希望**所有绑核/解绑事件都有结构化日志记录，支持按 VM 过滤查看**，以便**在运维审计与故障追溯时有清晰的操作轨迹与结果可见性**。

### 【依赖】

- Host 侧 `vaffinity` 管理工具

### 【用户接口】

| 位置 | 接口 | 说明 |
|------|------|------|
| Host 侧 | `vaffinity log [--vm <domain>] [--tail N]` | 查看绑核事件日志 |

### 【验收准则】

**AC-04-01：绑核事件日志包含完整信息**

- GIVEN vm1 内业务触发绑核后又解除绑核
- WHEN 管理员执行 `vaffinity log --vm vm1`
- THEN **可看到**包含 `[PIN]` 和 `[UNPIN]` 标记的结构化日志记录，内含 VM 名称、vCPU 编号、目标/释放的 pCPU、操作来源等关键信息

**AC-04-02：日志支持过滤与限制**

- GIVEN 系统运行一段时间后积累了大量日志
- WHEN 管理员执行 `vaffinity log --vm vm1 --tail 20`
- THEN 仅显示 vm1 相关的最近 20 条日志记录

---

### 【备注】

**整体工作流程：**

```
┌─────────────────── Guest 侧 ───────────────────┐     ┌──────────── Host / VMM 侧 ────────────┐
│                                                  │     │                                        │
│  业务应用绑核 → eBPF 截获 → ring buffer          │     │  vaffinity-listener 启动时               │
│  → 用户态代理 → VSOCK 通知                       │────>│  自动发现所有 VM (virsh vcpupin)         │
│                                                  │     │  → 接收通知 → 解析意图                   │
│                                                  │     │  → 查询全局映射 (per-vCPU cpuset)       │
│                                                  │     │  → 执行 1:1 绑定 → 更新映射 → 记录日志  │
│                                                  │     │                                        │
│  业务应用解绑 → eBPF 截获 → ring buffer          │     │                                        │
│  → 用户态代理 → VSOCK 通知                       │────>│  接收通知 → 恢复范围绑核 → 释放 pCPU    │
│                                                  │     │  → 更新映射 → 记录日志                  │
└──────────────────────────────────────────────────┘     └────────────────────────────────────────┘
```

**关键设计决策：** 无需手动注册 VM。vCPU 的绑核范围在虚拟机启动时已由 libvirt 初始化，`vaffinity` 通过 `virsh vcpupin <domain>` 自动获取每个 vCPU 的 CPU 亲和性范围（每个 vCPU 可以有不同的 cpuset），后续只需基于此进行动态修改。

