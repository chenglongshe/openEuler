# SR（System Requirement，系统需求）

## 基于专利《一种优化虚拟机内业务绑核性能的方法》—— 发明人：张海亮

---

### [名称]
虚拟机内业务绑核透传优化系统（VM In-Guest Pinning Passthrough Optimization System）

### [类型]
功能

### [需求详情]

在服务器虚拟化场景中，虚拟机的 vCPU 与物理 CPU（pCPU）存在两种绑核模式：**范围绑核**（vCPU 可在一组 pCPU 上被调度器自由调度）和 **1:1 绑核**（vCPU 固定运行在一个 pCPU 上）。范围绑核资源利用率高但性能不稳定，1:1 绑核性能稳定但资源利用率低。

本系统实现一种**透明的虚拟机内业务绑核感知与透传优化机制**：当虚拟机内的业务应用通过 `sched_setaffinity` 系统调用进行绑核时，VMM 侧能够自动感知并将该绑核意图透传到宿主机侧，动态地将对应 vCPU 从范围绑核切换为 1:1 绑核；当业务解除绑核后，VMM 自动恢复为范围绑核。从而在超分配部署场景下，兼顾资源利用率与业务性能稳定性。

系统应提供以下核心功能：

1. **Guest 侧绑核截获**：在虚拟机 Guest OS 内核中，通过 kprobe 或 eBPF 技术截获业务应用对 `sched_setaffinity` 系统调用的调用，获取绑核目标 vCPU 范围等信息。
   - **kprobe 方式**（同步）：在内核 kprobe hook 中直接通过 hypercall 或 wrmsr 将绑核信息同步传递到 VMM。
   - **eBPF 方式**（异步）：通过 eBPF 程序截获后传递给用户态处理程序，由用户态异步通知 VMM。eBPF 方式支持一次编译多平台运行，减少每个 Guest OS 版本的适配工作量。

2. **Guest-to-Host 通知机制**：提供从虚拟机内部向 VMM/Hypervisor 传递绑核信息的通道，支持以下方式：
   - **hypercall 方式**：直接调用虚拟机管理程序调用接口，触发 VM-Exit 进入 KVM 模块处理。
   - **wrmsr/rdmsr 方式**：通过写入特定 MSR 寄存器触发 VM-Exit 传递信息。
   - **模拟设备方式**（实施例二）：通过模拟 PCI/ISA 设备的 write 操作传递信息，VM-Exit 到 QEMU 用户态处理。

3. **VMM 侧绑核处理**：VMM（KVM 或 QEMU）接收到 Guest 内绑核通知后：
   - 解析绑核信息（绑核/解绑核动作、目标 vCPU 范围）。
   - 查询 **全局 CPU 映射表（Global CPU Maps）** 和该 VM 的 **cpuset 配置**，确定可用的物理 CPU。
   - 对需要绑核的 vCPU 执行 Host 侧 1:1 绑定（`sched_setaffinity` 到指定 pCPU）。
   - 尽可能避免多个 VM 的 1:1 绑核范围重叠（同一 pCPU 不被多个 vCPU 独占绑定）。

4. **自动解绑恢复**：当 Guest 内业务应用解除绑核（调用 `sched_setaffinity` 恢复到全范围）时：
   - Guest 侧截获模块感知解绑核动作。
   - 通知 VMM 侧恢复该 vCPU 为范围绑核（允许在原 cpuset 范围内调度）。
   - 更新全局 CPU 映射表，释放原独占 pCPU。

5. **全局 CPU 映射管理**：VMM 侧维护一张全局 CPU 映射表，记录所有 VM 的 vCPU 绑核状态：
   - 哪些 pCPU 已被哪个 VM 的哪个 vCPU 1:1 独占绑定。
   - 哪些 pCPU 处于空闲或范围绑核共享状态。
   - 绑核冲突检测：新绑核请求时检查目标 pCPU 是否已被占用，若冲突则选择最近可用 pCPU。

6. **日志与审计**：所有绑核事件（Guest 内截获、VMM 侧绑定/解绑、冲突检测结果）记录到系统日志。

### [约束]

**法规/合规**：
- 绑核操作需具备审计日志，满足等保 2.0 日志留存要求。
- Guest 侧仅使用 kprobe/eBPF 等非侵入式内核机制，不修改 Guest 内核源码（eBPF 方案可在不修改 VM 内核的情况下支持旧版本 OS）。

**平台兼容性**：
- 支持 x86_64 和 arm64（aarch64）架构。
- 支持 openEuler 22.03/24.03 LTS 版本作为 Host OS。
- Guest OS 支持 Linux 内核 ≥ 4.18（kprobe）或 ≥ 5.4（eBPF CO-RE）。
- 依赖 QEMU/KVM 虚拟化环境。

**性能约束**：
- Guest 侧截获模块对 `sched_setaffinity` 调用的额外延迟 ≤ 50μs。
- VMM 侧绑核处理（从 VM-Exit 到绑核完成）延迟 ≤ 1ms。
- eBPF 程序加载时间 ≤ 500ms。
- 截获模块对虚拟机正常业务性能影响 ≤ 1%。

**部署约束**：
- Guest 侧截获模块以内核模块（kprobe）或 eBPF 程序形式部署。
- VMM 侧处理逻辑集成在 KVM 模块或 QEMU 设备模拟中。
- eBPF 方案支持 CO-RE（Compile Once – Run Everywhere），一次编译适配多种 Guest 内核版本。

### [范围]

**覆盖范围**：
- Guest 侧 sched_setaffinity 截获模块（kprobe 方式 + eBPF 方式）
- Guest-to-Host 绑核信息通知通道（hypercall / wrmsr / 模拟设备）
- VMM 侧通知接收与解析模块
- VMM 侧绑核执行模块（动态 1:1 绑定 / 范围绑定恢复）
- 全局 CPU 映射表管理模块
- 绑核冲突检测与解决模块
- 日志审计模块

**不在范围内**：
- 虚拟机创建、删除、迁移等生命周期管理
- 内存绑定（memory binding / membind）策略
- Guest 内核 CFS 调度器修改
- GUI 图形界面
- 非 KVM 虚拟化平台（如 Xen、Hyper-V）

### [验收标准]

> 以下验收标准均从**最终用户（虚拟化平台管理员）可执行、可观测**的角度编写。

**AC-SR-01：虚拟机内业务绑核后，管理员能在宿主机侧观察到对应 vCPU 已被自动切换为 1:1 独占绑核**
- GIVEN 宿主机上运行虚拟机 vm1（4 vCPU，cpuset 0-15，范围绑核模式），管理员在 Host 侧执行 `vm-bindcore status --vm vm1` 确认所有 vCPU 均为 `range` 模式
- WHEN 管理员在 vm1 Guest 内运行业务程序，该程序调用 `taskset -c 2 <workload>`（即绑核到 vCPU 2）
- THEN 管理员在 Host 侧再次执行 `vm-bindcore status --vm vm1`，**可观察到** vcpu2 已变为 `exclusive` 模式并绑定到一个具体的 pCPU（如 pCPU 8），其余 vCPU 仍为 `range` 模式；同时 `vm-bindcore map` 输出中 pCPU 8 标记为 `exclusive: vm1:vcpu2`

**AC-SR-02：虚拟机内业务解除绑核后，管理员能在宿主机侧观察到对应 vCPU 已自动恢复为范围绑核，独占 pCPU 被释放**
- GIVEN 承接 AC-SR-01，vm1 的 vcpu2 当前为 `exclusive` 模式（1:1 绑定到 pCPU 8）
- WHEN 管理员在 vm1 Guest 内终止该业务进程（或显式调用 `taskset -c 0-3 <workload>` 恢复到全部 vCPU）
- THEN 管理员在 Host 侧执行 `vm-bindcore status --vm vm1`，**可观察到** vcpu2 已恢复为 `range` 模式（cpuset 0-15）；执行 `vm-bindcore map` **可观察到** pCPU 8 不再标记为独占，状态恢复为 `shared`

### [交付说明]

#### 交付清单
- Guest 侧截获模块：kprobe 内核模块（`.ko`）+ eBPF 程序（CO-RE `.bpf.o`）
- Guest 侧用户态通知代理（eBPF 方案）
- VMM 侧绑核处理补丁（KVM 模块 patch 或 QEMU 设备模拟）
- 全局 CPU 映射管理工具（`vm-bindcore`）
- RPM 包：`vm-bindcore`（宿主机侧）、`vm-bindcore-guest`（虚拟机侧）

#### 安装策略
- Host 侧：`yum install vm-bindcore` 安装 VMM 侧处理模块和管理工具
- Guest 侧：`yum install vm-bindcore-guest` 安装 eBPF 截获程序和通知代理

#### 环境与依赖
- 平台架构：x86_64 / arm64（aarch64）
- Host 运行依赖：QEMU/KVM、libvirt ≥ 6.0.0
- Guest 运行依赖：Linux 内核 ≥ 4.18（kprobe）或 ≥ 5.4（eBPF CO-RE）、bpftool
- 交付机型：鲲鹏、海光、Intel、飞腾

#### 文档与培训资料
- 技术白皮书：透明虚拟机绑核优化原理与架构
- 部署指南：Host/Guest 双侧安装配置手册
- CLI 命令参考手册（`vm-bindcore --help`）

#### 版本策略
- GA 发布（随 openEuler 24.03 LTS SP1 或后续更新版本）

