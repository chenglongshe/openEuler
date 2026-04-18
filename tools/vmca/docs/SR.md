# SR（System Requirement，系统需求）

## 基于专利《一种优化虚拟机内业务绑核性能的方法》—— 发明人：张海亮

---

### 【客户场景】

面向数据中心与私有云场景的运维管理员、虚拟化平台管理员，在日常运维中需要快速定位单台 VM 或特定 vCPU/线程引发的性能异常，并希望在一个统一入口完成计算资源的日常管理与变更。

当前，虚拟机 vCPU 与物理 CPU（pCPU）绑核方式存在两种模式——**范围绑核**（vCPU 可在一组 pCPU 上被调度器自由调度，资源利用率高但性能不稳定）与 **1:1 绑核**（vCPU 固定在一个 pCPU 上运行，性能稳定但资源利用率低）。在 CPU 超分配部署场景下，运维人员常常面临"不知道哪台 VM、哪个线程触发了绑核导致了 CPU 抖动"的困境，需要在多个工具之间反复切换才能完成定位与处置。

### 【问题陈述】

现状缺少按 VM 粒度的实时 vCPU 绑核观测能力与统一的绑核管理入口：

1. **定位困难**：遇到 CPU 抖动、卡顿或异常退出时，难以迅速归因到"哪台 VM、哪个 vCPU/线程"触发了绑核操作。
2. **感知缺失**：虚拟机内业务应用通过 `sched_setaffinity` 绑核后，宿主机侧无法自动感知，导致 vCPU 仍停留在性能不稳定的范围绑核模式。
3. **管理分散**：vCPU 绑核的初始范围已由 libvirt 在虚拟机启动时初始化（可通过 `virsh vcpupin <domain>` 获取），但后续动态切换、冲突避免、状态查询、日志审计等操作分散在不同工具中，缺乏统一管理口径。
4. **流程割裂**：绑核/解绑操作与状态回看、日志审计之间缺少串联，影响问题闭环效率。

### 【类型】

功能

### 【目标】

在 6.6 内核版本上孵化并交付一套面向 xVirt 的虚拟机 vCPU 绑核透传优化能力：

1. **观测可视化域**：提供按 VM/vCPU/线程归因的实时绑核观测能力，支撑现场快速锁定异常 VM 与线程。运维人员可通过 `vmca status --vm <domain>` 直接查看每个 vCPU 的绑核模式、绑定的 pCPU、操作来源等状态，通过 `vmca map` 查看全局 pCPU 独占/共享分布。

2. **透明绑核透传域**：实现 Guest 侧绑核感知 → Host 侧自动执行的端到端透传。当虚拟机内业务应用绑核时，系统自动将对应 vCPU 从范围绑核切换为 1:1 独占绑核；业务解绑后自动恢复。**无需手动注册 VM**——vCPU 绑核范围在虚拟机启动时已由 libvirt 初始化，`vmca` 通过 `virsh vcpupin <domain>` 自动发现每个 vCPU 的 CPU 亲和性范围。

3. **统一管理编排域**：提供覆盖 vCPU 绑核的一体化管理入口（`vmca` 命令行），支撑主机 CPU 资源的统一查看、变更与协同操作，包括自动发现（`discover`）、状态查询（`status`）、全局映射（`map`）、批量解绑（`unpin-all`）、事件日志（`log`）等。

实现后，用户能够在同一入口完成绑核相关的查询与操作，显著缩短定位路径、减少工具切换，提升运维与交付效率，并为后续自动化与规模化管理奠定基础。

### 【需求详情】

具体功能要求包括：

1. **VM 自动发现（无需手动注册）**：通过 `virsh vcpupin <domain>` 自动获取虚拟机每个 vCPU 的 CPU 亲和性范围，支持每个 vCPU 拥有不同的 cpuset（如 vcpu0-15 绑定 `5-47,53-63`，vcpu16-18 绑定 `0-63`）。监听守护进程启动时自动发现所有运行中的 VM，新 VM 连接时按需自动发现。

2. **Guest 侧绑核感知**：在虚拟机内核中，通过 eBPF CO-RE（一次编译多平台运行）非侵入式截获业务应用的 `sched_setaffinity` 操作，提取绑核目标 vCPU 范围等关键信息，经异步 ring buffer 处理后通过 VSOCK/virtio-serial 通知宿主机。

3. **Guest-to-Host 通知通道**：提供从虚拟机内部向 VMM 传递绑核信息的通信通道（VSOCK / virtio-serial），支持独立可插拔的传输模块设计。

4. **VMM 侧绑核执行**：VMM 接收到 Guest 内绑核通知后，解析绑核意图，结合全局 CPU 资源分配状态和虚拟机的每 vCPU cpuset 配置，通过 `virsh vcpupin` 为目标 vCPU 执行 1:1 独占绑定，避免多虚拟机之间的绑核冲突。

5. **自动解绑恢复**：当 Guest 内业务解除 CPU 亲和性绑定时，系统自动感知并通知 VMM，将对应 vCPU 恢复为该 vCPU 的原始范围绑核模式，同时释放原独占的 pCPU 资源。

6. **全局 CPU 映射管理**：VMM 侧维护全局 CPU 资源映射表，实时记录各虚拟机 vCPU 的绑核状态（独占/共享），在新绑核请求时进行冲突检测与自动调度。支持持久化与重启恢复。

7. **日志与审计**：对所有绑核/解绑事件及冲突处理过程进行结构化日志记录，支持按 VM 过滤查看，满足运维审计与故障追溯需求。

### 【约束】

1. **法规/合规**：绑核操作需具备审计日志，满足等保 2.0 日志留存要求。Guest 侧仅使用非侵入式内核探测机制（eBPF），不修改 Guest 内核源码。
2. **平台兼容性**：支持 x86_64 和 arm64（aarch64）架构；支持 openEuler 22.03/24.03 LTS（6.6 内核）版本作为 Host OS；Guest OS 支持主流 Linux 内核版本；依赖 QEMU/KVM 虚拟化环境。
3. **性能约束**：Guest 侧感知模块引入的额外延迟不超过微秒级；VMM 侧绑核处理端到端延迟不超过毫秒级；感知模块对虚拟机正常业务性能影响 ≤ 1%。
4. **部署约束**：Guest 侧以 eBPF CO-RE 程序 + 用户态通知代理形式部署；VMM 侧通过 `vmca` 命令行工具与 systemd 服务管理；支持一次编译、多版本 Guest 内核适配。

### 【范围】

该需求覆盖以下模块：
- VM 自动发现模块（通过 `virsh vcpupin` 获取每 vCPU 的 cpuset，无需手动注册）
- Guest 侧 CPU 亲和性感知模块（eBPF CO-RE 异步方式）
- Guest-to-Host 绑核信息通知通道（VSOCK / virtio-serial）
- VMM 侧通知接收与绑核意图解析模块
- VMM 侧绑核执行模块（动态 1:1 绑定 / 范围绑定恢复）
- 全局 CPU 映射管理模块（含冲突检测与调度、持久化与恢复）
- 日志审计模块

不在范围内：虚拟机创建、删除、迁移等生命周期管理；内存亲和性策略；Guest 内核调度器修改；GUI 图形界面；非 KVM 虚拟化平台；网络/存储虚拟化资源管理。

### 【验收标准】

> 以下验收标准均从**最终用户（虚拟化平台管理员）可执行、可观测**的角度编写。

#### 可用性

**场景 1：运维人员可在统一入口完成日常虚拟化绑核操作与状态查看**

- GIVEN 宿主机上运行虚拟机 FusionOS-23T10（19 vCPU，vcpu0-15 cpuset `5-47,53-63`，vcpu16-18 cpuset `0-63`），管理员在 Host 侧启动 `vmca-listener` 服务
- WHEN 管理员执行 `vmca discover --vm FusionOS-23T10`
- THEN **可观察到**系统自动获取该 VM 全部 19 个 vCPU 的 CPU 亲和性范围，每个 vCPU 的 cpuset 正确显示，无需手动注册

**场景 2：性能异常时，基于 VM 粒度视图直接定位到具体 vCPU 与线程**

- GIVEN 宿主机上运行多台 VM，管理员在 Host 侧执行 `vmca status` 确认所有 VM 均为 `range` 模式
- WHEN vm1 Guest 内某业务程序调用 `taskset -c 2 <workload>` 绑核到 vCPU 2
- THEN 管理员执行 `vmca status --vm vm1`，**可观察到** vcpu2 已变为 `exclusive` 模式并绑定到具体 pCPU，`source` 字段标记为 `guest-pin`，可直接定位到触发绑核的 VM 与 vCPU

#### 一致性

**场景 3：不同主机上通过同一管理入口操作呈现一致体验**

- GIVEN 在不同宿主机上均部署了 `vmca` 工具与 `vmca-listener` 服务
- WHEN 管理员在各主机上分别执行 `vmca discover`、`vmca status`、`vmca map` 等命令
- THEN **可观察到**命令行接口、输出格式、状态标识（`range`/`exclusive`/`shared`）在所有主机上保持一致，无需额外适配

#### 完整性

**场景 4：绑核 → 查看 → 解绑 → 回看全流程可串联完成**

- GIVEN 宿主机运行 vm1（vCPU 已被自动发现），管理员确认 vcpu2 为 `range` 模式
- WHEN 管理员在 Guest 内触发绑核（`taskset -c 2`），随后在 Host 侧执行 `vmca status --vm vm1` 查看状态，再在 Guest 内终止业务进程，最后在 Host 侧执行 `vmca status --vm vm1` 和 `vmca log --vm vm1`
- THEN **可观察到**完整的操作轨迹：绑核后 vcpu2 变为 `exclusive`，解绑后恢复为 `range`，日志中包含 `[PIN]` 和 `[UNPIN]` 事件记录，具备清晰的结果可见性

**场景 5：多 VM 绑核冲突自动避免**

- GIVEN 宿主机运行 vm1 和 vm2（cpuset 有重叠），vm1 的 vcpu0 已被 1:1 绑定到某 pCPU
- WHEN 管理员在 vm2 Guest 内触发业务绑核
- THEN 管理员执行 `vmca map`，**可观察到** vm2 的 vCPU 绑定到了另一个不冲突的 pCPU，系统自动避免了资源争用

#### 可学习性

**场景 6：运维人员可按文档独立完成部署与使用的基本闭环**

- GIVEN 运维人员首次接触 `vmca` 工具
- WHEN 运维人员参照 README.md 和 `man vmca` 文档，按步骤完成宿主机侧安装、启动监听服务、执行自动发现、查看 VM 状态
- THEN 运维人员能够独立完成从部署到使用的基本闭环，无需依赖额外系统或临时脚本

### 【交付说明】

#### 交付清单
- Guest 侧 eBPF CO-RE 截获程序（`.bpf.c` 源码 + `.bpf.o` 编译产物）
- Guest 侧用户态通知代理（`vmca-guest`，Python + VSOCK/virtio-serial 传输）
- Guest 侧 systemd 服务（`vmca-guest.service`）
- VMM 侧管理工具（`vmca`，含自动发现、状态查询、全局映射、绑核执行、日志审计）
- VMM 侧 systemd 服务（`vmca-listener.service`、`vmca-restore.service`）
- 是否涉及新增 RPM 包：是（`vmca`（宿主机侧）、`vmca-guest`（虚拟机侧））

#### 环境与依赖
- 平台架构：x86_64、arm64（aarch64）
- Host OS：openEuler 22.03/24.03 LTS（6.6 内核）
- Host 运行依赖：QEMU/KVM、libvirt ≥ 6.0.0
- Guest 运行依赖：Linux 内核 ≥ 5.4（eBPF CO-RE）、bpftool

