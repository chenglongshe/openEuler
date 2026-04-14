# SR（System Requirement，系统需求）

## 基于专利《一种虚拟机绑核方法及计算设备》—— 发明人：张海亮

---

### [名称]
虚拟机 vCPU 绑核管理系统（VM vCPU CPU-Pinning Management System）

### [类型]
功能

### [需求详情]

系统应提供一套虚拟机 vCPU 到物理 CPU 核心的绑定（Pinning）管理能力，具体包含以下功能行为：

1. **NUMA 拓扑感知**：系统应自动检测宿主机的 NUMA 拓扑结构（节点数量、每节点 CPU 列表、节点间距离），并以此作为绑核决策的基础数据。

2. **自动绑核策略**：当用户指定虚拟机名称/ID 和绑核策略（如 `numa-aware`、`spread`、`compact`）时，系统应根据以下规则自动计算并执行 vCPU 到 pCPU 的映射：
   - **`numa-aware` 策略**：将同一虚拟机的所有 vCPU 绑定到同一 NUMA 节点的物理 CPU 上，以减少跨节点内存访问延迟。若单节点可用 CPU 不足，则优先选择 NUMA 距离最近的相邻节点进行扩展。
   - **`spread` 策略**：将虚拟机的 vCPU 均匀分散到多个 NUMA 节点上，以最大化利用各节点的内存带宽。
   - **`compact` 策略**：将虚拟机的 vCPU 紧凑排布到最少数量的物理核心上，优先使用同一物理核心的超线程（Hyper-Threading/SMT siblings），以降低 CPU 资源占用面积。

3. **手动绑核**：系统应支持用户通过指定 vCPU 编号和目标 pCPU 列表手动设置绑定关系，格式如 `vcpu0:0-3,vcpu1:4-7`。

4. **绑核关系查询**：系统应支持查询指定虚拟机当前所有 vCPU 的绑核状态，输出格式应包含 vCPU 编号、绑定的 pCPU 列表以及所在 NUMA 节点。

5. **绑核关系解除**：系统应支持解除指定虚拟机的 vCPU 绑核关系，恢复为系统默认调度行为。

6. **冲突检测**：在执行绑核操作前，系统应检测目标 pCPU 是否已被其他虚拟机独占绑定（exclusive 模式），若存在冲突应返回明确的错误信息并拒绝操作。

7. **持久化**：绑核配置应支持持久化存储（配置文件），在宿主机或 libvirtd 重启后可自动恢复绑核关系。

8. **日志记录**：所有绑核操作（设置、解除、策略变更）应记录到系统日志（syslog/journald），包含时间戳、操作者、虚拟机标识和操作结果。

### [约束]

**法规/合规**：
- 绑核操作需具备审计日志，满足等保 2.0 日志留存要求。
- 不得修改内核调度器核心逻辑，仅通过用户态 API（cgroup/cpuset、sched_setaffinity、libvirt API）实现。

**平台兼容性**：
- 支持 x86_64 和 arm64（aarch64）架构。
- 支持 openEuler 22.03/24.03 LTS 版本。
- 依赖 libvirt ≥ 6.0.0、QEMU/KVM 虚拟化环境。
- 兼容 Linux 内核版本 ≥ 5.10。

**资源限制**：
- 绑核策略计算时间 ≤ 1 秒（128 核以内宿主机）。
- 工具运行时常驻内存 ≤ 10 MB。
- 单次绑核操作（含 libvirt API 调用）完成时间 ≤ 3 秒。

**部署约束**：
- 以 RPM 包形式交付。
- 默认不安装，用户按需安装。
- 依赖 libvirt-devel、python3 运行时环境。

### [范围]

**覆盖范围**：
- 宿主机 NUMA 拓扑检测模块
- vCPU 绑核策略计算引擎（numa-aware / spread / compact）
- vCPU-to-pCPU 绑定执行模块（通过 libvirt virDomainPinVcpu API 或 cgroup/cpuset）
- 绑核状态查询模块
- 绑核配置持久化模块
- CLI 命令行工具（vm-bindcore）

**不在范围内**：
- 虚拟机创建、删除、迁移等生命周期管理
- 内存绑定（memory binding / membind）策略（仅限 CPU 绑核）
- 内核态调度器修改
- GUI 图形界面
- 虚拟机 I/O 线程绑核（可作为后续扩展）

### [验收标准]

**AC-SR-01：NUMA 拓扑感知**
- GIVEN 宿主机为多 NUMA 节点服务器（≥2 个 NUMA 节点）
- WHEN 执行 `vm-bindcore topology` 命令
- THEN 系统输出完整的 NUMA 拓扑信息，包含各节点 CPU 列表和节点间距离矩阵，与 `numactl --hardware` 输出一致

**AC-SR-02：自动绑核 - numa-aware 策略**
- GIVEN 宿主机有 2 个 NUMA 节点（node0: CPU 0-31, node1: CPU 32-63），虚拟机 vm1 配置 4 个 vCPU
- WHEN 执行 `vm-bindcore pin --vm vm1 --strategy numa-aware`
- THEN vm1 的 4 个 vCPU 全部绑定到同一 NUMA 节点的连续 CPU 上，通过 `vm-bindcore show --vm vm1` 可验证所有 vCPU 位于同一 NUMA 节点

**AC-SR-03：自动绑核 - spread 策略**
- GIVEN 宿主机有 2 个 NUMA 节点，虚拟机 vm2 配置 4 个 vCPU
- WHEN 执行 `vm-bindcore pin --vm vm2 --strategy spread`
- THEN vm2 的 vCPU 均匀分布在两个 NUMA 节点上（每节点 2 个 vCPU）

**AC-SR-04：自动绑核 - compact 策略**
- GIVEN 宿主机开启 SMT（超线程），虚拟机 vm3 配置 2 个 vCPU
- WHEN 执行 `vm-bindcore pin --vm vm3 --strategy compact`
- THEN vm3 的 2 个 vCPU 绑定到同一物理核心的 2 个超线程上

**AC-SR-05：手动绑核**
- GIVEN 虚拟机 vm4 配置 2 个 vCPU
- WHEN 执行 `vm-bindcore pin --vm vm4 --manual vcpu0:0-3,vcpu1:4-7`
- THEN vcpu0 绑定到 pCPU 0-3，vcpu1 绑定到 pCPU 4-7，通过 `virsh vcpuinfo vm4` 可交叉验证

**AC-SR-06：冲突检测**
- GIVEN 虚拟机 vm5 的 vCPU 已独占绑定到 pCPU 0-3
- WHEN 执行 `vm-bindcore pin --vm vm6 --manual vcpu0:0-3 --exclusive`
- THEN 系统返回错误信息 "pCPU 0-3 已被 vm5 独占绑定，操作被拒绝"

**AC-SR-07：绑核解除**
- GIVEN 虚拟机 vm7 已设置绑核
- WHEN 执行 `vm-bindcore unpin --vm vm7`
- THEN vm7 的所有 vCPU 绑核关系被解除，恢复为系统默认调度

**AC-SR-08：持久化恢复**
- GIVEN 虚拟机 vm8 已设置绑核配置并已持久化
- WHEN 宿主机重启后 libvirtd 启动完成
- THEN 执行 `vm-bindcore show --vm vm8` 显示绑核关系与重启前一致

### [交付说明]

#### 交付清单
- 新增 RPM 包：`vm-bindcore`
- RPM 包集成方式：everything 镜像 / 独立 yum 源

#### 安装策略
- 默认不安装，用户通过 `yum install vm-bindcore` 按需安装

#### 环境与依赖
- 平台架构：x86_64 / arm64（aarch64）
- 交付机型：鲲鹏、海光、Intel、飞腾
- 运行依赖：libvirt ≥ 6.0.0、python3 ≥ 3.8、QEMU/KVM

#### 文档与培训资料
- 资料：用户手册（vm-bindcore 使用指南）、运维手册（部署与故障排查）
- 文档：CLI 命令参考手册（`vm-bindcore --help` 内嵌）
- 培训材料：快速入门指南、常见问题 FAQ

#### 版本策略
- GA 发布（随 openEuler 24.03 LTS SP1 或后续更新版本）
