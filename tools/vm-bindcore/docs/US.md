# US（User Story，用户故事）

## 基于专利《一种优化虚拟机内业务绑核性能的方法》—— 发明人：张海亮

---

## US-01：Guest 侧业务绑核自动截获（kprobe 方式）

### 【US描述】
作为**虚拟化平台管理员**，我希望**虚拟机内的业务应用绑核动作能被自动截获并通知到 VMM**，以便**VMM 能够感知虚拟机内业务的 CPU 亲和性需求，进而在 Host 侧做出优化绑核决策**。

### 【依赖】
- Guest OS 内核支持 kprobe（Linux ≥ 4.18）
- KVM/QEMU 虚拟化环境支持 hypercall 或 wrmsr

### 【用户接口】
- **Guest 侧**：加载内核模块 `insmod vm_bindcore_kprobe.ko`
- **Guest 侧**：卸载内核模块 `rmmod vm_bindcore_kprobe`

### 【验收准则】

> 以下验收标准从最终用户可执行、可观测角度编写。

**AC-US-01-01**：
- GIVEN 管理员在 vm1 Guest 内执行 `insmod vm_bindcore_kprobe.ko` 成功加载截获模块
- WHEN 管理员在 vm1 内运行 `taskset -c 2 stress --cpu 1`（将压测进程绑定到 vCPU 2）
- THEN 管理员在 Host 侧执行 `vm-bindcore status --vm vm1`，**可看到** vcpu2 已变为 `exclusive` 模式

**AC-US-01-02**：
- GIVEN kprobe 截获模块已加载，vm1 内正在运行非绑核的普通业务
- WHEN 管理员在 Guest 内通过 `perf stat` 测量业务性能
- THEN **可观察到**业务吞吐量与未加载截获模块时的差异 ≤ 1%（截获模块对非绑核操作无感知开销）

### 【备注】
```
kprobe 截获流程（同步方式）：
┌──────────────┐    ┌─────────────┐    ┌──────────────┐    ┌──────────┐
│ App 调用      │───>│ kprobe hook │───>│ hypercall/   │───>│ KVM 模块  │
│ sched_set-   │    │ 截获并提取   │    │ wrmsr 通知   │    │ 处理绑核  │
│ affinity()   │    │ 绑核信息     │    │ Host         │    │ 请求      │
└──────────────┘    └─────────────┘    └──────────────┘    └──────────┘
```

---

## US-02：Guest 侧业务绑核自动截获（eBPF 方式）

### 【US描述】
作为**虚拟化平台管理员**，我希望**能够使用 eBPF 技术在不修改 Guest 内核的情况下截获绑核动作，并支持一次编译多平台运行**，以便**减少每个 Guest OS 版本的适配维护工作量**。

### 【依赖】
- Guest OS 内核支持 eBPF CO-RE（Linux ≥ 5.4，带 BTF 信息）
- Guest 侧安装 bpftool 或 libbpf 运行时

### 【用户接口】
- **Guest 侧**：启动 eBPF 截获守护进程 `vm-bindcore-guest start`
- **Guest 侧**：停止 `vm-bindcore-guest stop`
- **Guest 侧**：查看状态 `vm-bindcore-guest status`

### 【验收准则】

> 以下验收标准从最终用户可执行、可观测角度编写。

**AC-US-02-01**：
- GIVEN 管理员在 vm1（openEuler 22.03）内执行 `vm-bindcore-guest start` 启动 eBPF 截获守护进程，执行 `vm-bindcore-guest status` 确认状态为 `running`
- WHEN 管理员在 vm1 内运行 `taskset -c 1 stress --cpu 1`（绑核到 vCPU 1）
- THEN 管理员在 Host 侧执行 `vm-bindcore status --vm vm1`，**可看到** vcpu1 已变为 `exclusive` 模式

**AC-US-02-02**：
- GIVEN 同一编译产物的 eBPF 截获程序分别部署到 openEuler 22.03（内核 5.10）和 openEuler 24.03（内核 6.6）的 Guest OS
- WHEN 管理员在两个 Guest 内分别执行 `vm-bindcore-guest start` 并运行绑核业务
- THEN 两个 Guest 均能正常工作：Host 侧 `vm-bindcore status` **可看到**两个 VM 都有 vCPU 变为 `exclusive` 模式，无需为不同内核版本重新编译

### 【备注】
```
eBPF 截获流程（异步方式）：
┌───────────┐    ┌───────────┐    ┌───────────┐    ┌────────────┐    ┌──────────┐
│ App 调用   │───>│ eBPF hook │───>│ ring buf  │───>│ 用户态代理  │───>│ hypercall│
│ sched_set- │    │ 截获绑核  │    │ 传递信息  │    │ 异步通知   │    │ → KVM    │
│ affinity() │    │ 信息      │    │           │    │ VMM        │    │ 处理     │
└───────────┘    └───────────┘    └───────────┘    └────────────┘    └──────────┘
```

---

## US-03：VMM 侧自动 1:1 绑核

### 【US描述】
作为**虚拟化平台**，我希望**当 Guest 内业务绑核时，VMM 能自动将对应 vCPU 从范围绑核切换为 1:1 绑核**，以便**业务应用在超分配场景下也能获得稳定的 CPU 性能**。

### 【依赖】
- US-01 或 US-02（Guest 侧截获与通知已就绪）
- VMM 侧全局 CPU 映射表已初始化

### 【用户接口】
- **Host 侧**：自动处理，无需用户干预
- **Host CLI**：`vm-bindcore status --vm <domain>` 查看当前 vCPU 绑核状态

### 【验收准则】

> 以下验收标准从最终用户可执行、可观测角度编写。

**AC-US-03-01**：
- GIVEN vm1 有 4 个 vCPU（cpuset 0-15，范围绑核），管理员执行 `vm-bindcore status --vm vm1` 确认所有 vCPU 均为 `range` 模式
- WHEN 管理员在 vm1 Guest 内分别运行 `taskset -c 0 app1`、`taskset -c 2 app2`（两个业务分别绑核到 vCPU 0 和 vCPU 2）
- THEN 管理员在 Host 侧执行 `vm-bindcore status --vm vm1`，**可看到** vcpu0 和 vcpu2 均为 `exclusive` 模式且绑定到**不同的** pCPU，vcpu1 和 vcpu3 仍为 `range` 模式

**AC-US-03-02**：
- GIVEN vm1 的 vcpu2 已被自动 1:1 绑定到某 pCPU
- WHEN 管理员执行 `vm-bindcore status --vm vm1 --format json`
- THEN 输出的 JSON 中 **可看到** vcpu2 的 `mode` 为 `"exclusive"`、`pcpu` 字段为具体数字、`source` 为 `"guest-pin"`

### 【备注】
```bash
$ vm-bindcore status --vm vm1
VM: vm1 (4 vCPUs, cpuset: 0-15)
vCPU    Mode        pCPU         Source
----    ----        ----         ------
vcpu0   exclusive   pCPU 4       guest-pin (PID 1234)
vcpu1   range       pCPU 0-15    default
vcpu2   exclusive   pCPU 8       guest-pin (PID 5678)
vcpu3   range       pCPU 0-15    default
```

---

## US-04：自动解绑恢复

### 【US描述】
作为**虚拟化平台**，我希望**当 Guest 内业务解除绑核后，VMM 能自动将对应 vCPU 恢复为范围绑核**，以便**释放独占的 pCPU 资源，恢复资源共享与高利用率**。

### 【依赖】
- US-03（VMM 侧 1:1 绑核已实现）

### 【用户接口】
- **Host 侧**：自动处理，无需用户干预

### 【验收准则】

> 以下验收标准从最终用户可执行、可观测角度编写。

**AC-US-04-01**：
- GIVEN vm1 的 vcpu2 当前为 `exclusive` 模式（管理员可通过 `vm-bindcore status --vm vm1` 确认）
- WHEN 管理员在 vm1 Guest 内终止该绑核业务进程（`kill <pid>`），或执行 `taskset -c 0-3 <pid>` 恢复到全部 vCPU
- THEN 管理员在 Host 侧再次执行 `vm-bindcore status --vm vm1`，**可看到** vcpu2 已恢复为 `range` 模式

**AC-US-04-02**：
- GIVEN vm1 的 vcpu0、vcpu2 均为 `exclusive` 模式，`vm-bindcore map` 显示 2 个 pCPU 被独占
- WHEN 管理员在 Guest 内终止所有绑核业务
- THEN 管理员执行 `vm-bindcore map`，**可看到**之前被独占的 2 个 pCPU 均已恢复为 `shared` 状态

### 【备注】
```
解绑恢复流程：
Guest App 解绑 → eBPF/kprobe 截获 → 通知 VMM → VMM 恢复范围绑核 → 释放独占 pCPU
```

---

## US-05：全局 CPU 映射管理与冲突避免

### 【US描述】
作为**虚拟化平台管理员**，我希望**VMM 能维护一张全局 CPU 映射表，记录所有 VM 的绑核状态，在进行 1:1 绑核时自动避免多个 vCPU 绑定同一 pCPU**，以便**确保 1:1 绑核的性能隔离效果**。

### 【依赖】
- US-03（VMM 侧 1:1 绑核功能）

### 【用户接口】
- **Host CLI**：`vm-bindcore map` 显示全局 CPU 映射表
- **Host CLI**：`vm-bindcore map --format json` JSON 格式输出

### 【验收准则】

> 以下验收标准从最终用户可执行、可观测角度编写。

**AC-US-05-01**：
- GIVEN 宿主机上运行 vm1 和 vm2（cpuset 有重叠），vm1 的 vcpu2 已被 1:1 绑定到 pCPU 8（通过 `vm-bindcore map` 可见）
- WHEN 管理员在 vm2 Guest 内运行 `taskset -c 0 stress --cpu 1`（触发 vm2 的 vcpu0 绑核）
- THEN 管理员执行 `vm-bindcore map`，**可看到** vm2:vcpu0 绑定到了**另一个** pCPU（如 pCPU 9），而非 pCPU 8，系统自动避免了冲突

**AC-US-05-02**：
- GIVEN 宿主机运行多个 VM
- WHEN 管理员执行 `vm-bindcore map`
- THEN **可看到**每个 pCPU 的状态（`shared` / `exclusive`）和对应的 VM:vCPU 归属信息，格式清晰可读

### 【备注】
```bash
$ vm-bindcore map
Global CPU Pinning Map (64 pCPUs)
pCPU    Status      Owner
----    ------      -----
0       shared      (vm1, vm2 range)
1       shared      (vm1, vm2 range)
...
8       exclusive   vm1:vcpu2 (guest-pin)
9       exclusive   vm2:vcpu0 (guest-pin)
10      shared      (vm1, vm3 range)
...
```

---

## US-06：模拟设备方式传递绑核信息（实施例二）

### 【US描述】
作为**虚拟化平台开发者**，我希望**提供一种基于模拟设备的替代通知方式**，以便**在 hypercall/wrmsr 不适用的场景下（如嵌套虚拟化或特定 Guest OS），仍能实现 Guest-to-Host 绑核信息传递**。

### 【依赖】
- QEMU 侧实现模拟设备 X（PCI 或 ISA 设备）
- Guest 侧用户态程序能打开设备并写入绑核信息

### 【用户接口】
- **Guest 侧**：`vm-bindcore-guest start --mode device`（使用模拟设备通知方式）
- **Host 侧 QEMU**：自动加载模拟设备 X

### 【验收准则】

> 以下验收标准从最终用户可执行、可观测角度编写。

**AC-US-06-01**：
- GIVEN vm1 配置了模拟设备 X，管理员在 Guest 内执行 `vm-bindcore-guest start --mode device` 启动设备通知方式
- WHEN 管理员在 vm1 Guest 内运行 `taskset -c 2 stress --cpu 1`
- THEN 管理员在 Host 侧执行 `vm-bindcore status --vm vm1`，**可看到** vcpu2 变为 `exclusive` 模式（与 hypercall 方式功能一致）

**AC-US-06-02**：
- GIVEN 使用模拟设备方式，vm1 内业务先绑核再解绑
- WHEN 管理员在 Host 侧分别查看绑核后和解绑后的状态
- THEN **可看到**状态先变为 `exclusive` 后恢复为 `range`，与 hypercall 方式表现一致

### 【备注】
```
模拟设备流程（实施例二）：
Guest App → eBPF 截获 → 用户态打开设备X → write(绑核信息) → VM-Exit 到 QEMU
→ QEMU 设备模拟X处理 → 查询 cpuset/global maps → 执行绑核
```

---

## US-07：查看全局绑核状态

### 【US描述】
作为**虚拟化平台管理员**，我希望**能够在 Host 侧查看所有虚拟机的 vCPU 绑核状态**，以便**监控资源分配情况并排查性能问题**。

### 【依赖】
- VMM 侧全局 CPU 映射表

### 【用户接口】
- **Host CLI**：`vm-bindcore status [--vm <domain>] [--format json|table]`

### 【验收准则】

> 以下验收标准从最终用户可执行、可观测角度编写。

**AC-US-07-01**：
- GIVEN 宿主机上运行 3 个 VM，其中 vm1 有 2 个 vCPU 为 `exclusive`，vm2 有 1 个 vCPU 为 `exclusive`，vm3 全部为 `range`
- WHEN 管理员执行 `vm-bindcore status`
- THEN **可看到**以表格形式输出所有 VM 的所有 vCPU 的绑核模式、绑定的 pCPU、触发来源，信息完整且易读

**AC-US-07-02**：
- GIVEN 当前无任何 VM 运行
- WHEN 管理员执行 `vm-bindcore status`
- THEN **可看到**输出提示 "No active VMs found"，程序正常退出（退出码 0）

### 【备注】
```bash
$ vm-bindcore status
Host: 64 pCPUs, 2 NUMA nodes

VM: vm1 (4 vCPUs, cpuset: 0-15)
  vcpu0  exclusive  pCPU 4   guest-pin
  vcpu1  range      pCPU 0-15
  vcpu2  exclusive  pCPU 8   guest-pin
  vcpu3  range      pCPU 0-15

VM: vm2 (2 vCPUs, cpuset: 16-31)
  vcpu0  exclusive  pCPU 20  guest-pin
  vcpu1  range      pCPU 16-31
```

---

## US-08：日志与审计

### 【US描述】
作为**虚拟化平台管理员**，我希望**所有由 Guest 内业务触发的绑核/解绑事件都被记录到系统日志**，以便**进行性能分析和安全审计**。

### 【依赖】
- systemd/journald 日志服务

### 【用户接口】
- **日志查看**：`journalctl -u vm-bindcore` 或 `vm-bindcore log [--vm <domain>]`

### 【验收准则】

> 以下验收标准从最终用户可执行、可观测角度编写。

**AC-US-08-01**：
- GIVEN vm1 内业务触发绑核，VMM 完成 1:1 绑核
- WHEN 管理员在 Host 侧执行 `journalctl -u vm-bindcore --since "1 min ago"`
- THEN **可看到**包含 `[PIN]` 标记的日志行，内含：VM 名称、vCPU 编号、目标 pCPU、操作来源（`guest-pin`）

**AC-US-08-02**：
- GIVEN vm1 内业务解除绑核，VMM 恢复范围绑核
- WHEN 管理员在 Host 侧执行 `journalctl -u vm-bindcore --since "1 min ago"`
- THEN **可看到**包含 `[UNPIN]` 标记的日志行，内含：VM 名称、vCPU 编号、恢复的 cpuset 范围

### 【备注】
```
日志示例：
Apr 14 10:30:15 host vm-bindcore[1234]: [PIN] vm=vm1 vcpu=2 action=exclusive pcpu=8 source=guest-pin pid=5678
Apr 14 10:35:22 host vm-bindcore[1234]: [UNPIN] vm=vm1 vcpu=2 action=restore cpuset=0-15 source=guest-unpin
Apr 14 10:35:22 host vm-bindcore[1234]: [CONFLICT] vm=vm2 vcpu=0 requested=pcpu8 occupied_by=vm1:vcpu2 resolved=pcpu9
```

