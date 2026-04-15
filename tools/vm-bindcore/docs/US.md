# US（User Story，用户故事）

## 基于专利《一种优化虚拟机内业务绑核性能的方法》—— 发明人：张海亮

---

## US-01：虚拟机内业务绑核透明感知与宿主机侧自动优化

### 【US描述】

作为**虚拟化平台管理员**，我希望**当虚拟机内的业务应用进行 CPU 亲和性绑定时，系统能够自动感知并在宿主机侧将对应 vCPU 从范围绑核动态切换为 1:1 独占绑核；当业务解除绑核后自动恢复为范围绑核**，以便**在 CPU 超分配部署场景下，无需人工干预即可兼顾资源利用率与业务性能稳定性，同时通过全局资源映射避免绑核冲突，并保留完整的操作日志供运维审计**。

### 【依赖】

- QEMU/KVM 虚拟化环境
- Guest OS 支持非侵入式内核探测机制（内核模块或轻量级探测程序）
- Host 侧 `vm-bindcore` 管理工具

### 【用户接口】

| 位置 | 接口 | 说明 |
|------|------|------|
| Guest 侧 | `vm-bindcore-guest start [--mode auto\|device]` | 启动绑核感知模块 |
| Guest 侧 | `vm-bindcore-guest stop` | 停止绑核感知模块 |
| Guest 侧 | `vm-bindcore-guest status` | 查看感知模块运行状态 |
| Host 侧 | `vm-bindcore status [--vm <domain>] [--format json\|table]` | 查看 vCPU 绑核状态 |
| Host 侧 | `vm-bindcore map [--format json\|table]` | 查看全局 CPU 映射表 |
| Host 侧 | `vm-bindcore log [--vm <domain>]` | 查看绑核事件日志 |

### 【验收准则】

> 以下验收标准均从**最终用户（虚拟化平台管理员）可执行、可观测**的角度编写。

**AC-01：Guest 内业务绑核 → Host 侧 vCPU 自动切换为 1:1 独占绑核**

- GIVEN 宿主机上运行虚拟机 vm1（4 vCPU，cpuset 0-15，范围绑核模式），管理员在 Guest 内启动感知模块（`vm-bindcore-guest start`），在 Host 侧执行 `vm-bindcore status --vm vm1` 确认所有 vCPU 均为 `range` 模式
- WHEN 管理员在 vm1 Guest 内运行 `taskset -c 2 <workload>`（绑核到 vCPU 2）
- THEN 管理员在 Host 侧执行 `vm-bindcore status --vm vm1`，**可观察到** vcpu2 已变为 `exclusive` 模式并绑定到一个具体的 pCPU，其余 vCPU 仍为 `range` 模式

**AC-02：Guest 内业务解除绑核 → vCPU 自动恢复为范围绑核，独占 pCPU 释放**

- GIVEN 承接 AC-01，vm1 的 vcpu2 当前为 `exclusive` 模式
- WHEN 管理员在 vm1 Guest 内终止该业务进程（或恢复到全部 vCPU 的亲和性设置）
- THEN 管理员在 Host 侧执行 `vm-bindcore status --vm vm1`，**可观察到** vcpu2 已恢复为 `range` 模式；执行 `vm-bindcore map` **可观察到**原独占 pCPU 已恢复为 `shared` 状态

**AC-03：多 VM 绑核冲突自动避免**

- GIVEN 宿主机上运行 vm1 和 vm2（cpuset 有重叠），vm1 的 vcpu2 已被 1:1 绑定到某 pCPU
- WHEN 管理员在 vm2 Guest 内触发业务绑核
- THEN 管理员执行 `vm-bindcore map`，**可观察到** vm2 的 vCPU 绑定到了另一个不冲突的 pCPU，系统自动避免了资源争用

**AC-04：感知模块对非绑核业务性能无显著影响**

- GIVEN 感知模块已在 Guest 内启动，vm1 内正在运行非绑核的普通业务
- WHEN 管理员在 Guest 内测量业务性能
- THEN **可观察到**业务吞吐量与未加载感知模块时的差异 ≤ 1%

**AC-05：绑核事件日志可审计**

- GIVEN vm1 内业务触发绑核后又解除绑核
- WHEN 管理员在 Host 侧执行 `vm-bindcore log --vm vm1`
- THEN **可看到**包含绑核（PIN）和解绑（UNPIN）标记的结构化日志记录，内含 VM 名称、vCPU 编号、目标/释放的 pCPU、操作来源等关键信息

**AC-06：跨 Guest 内核版本兼容（异步模式）**

- GIVEN 同一编译产物的感知模块分别部署到不同内核版本的 Guest OS
- WHEN 管理员在各 Guest 内启动感知模块并运行绑核业务
- THEN 各 Guest 均能正常工作，Host 侧可观察到 vCPU 状态正确切换，无需为不同内核版本重新编译

### 【备注】

**整体工作流程：**

```
┌─────────────────── Guest 侧 ───────────────────┐     ┌──────────── Host / VMM 侧 ────────────┐
│                                                  │     │                                        │
│  业务应用绑核 → 感知模块截获 → 通知通道传递       │────>│  接收通知 → 解析意图 → 查询全局映射     │
│                                                  │     │  → 执行 1:1 绑定 → 更新映射 → 记录日志  │
│  业务应用解绑 → 感知模块截获 → 通知通道传递       │────>│  接收通知 → 恢复范围绑核 → 释放 pCPU    │
│                                                  │     │  → 更新映射 → 记录日志                  │
└──────────────────────────────────────────────────┘     └────────────────────────────────────────┘
```

**功能覆盖说明：** 本 US 涵盖 Guest 侧绑核感知（同步/异步模式、多种通知通道）、VMM 侧自动绑核/解绑执行、全局 CPU 映射管理与冲突避免、状态查询与日志审计等完整端到端功能。

