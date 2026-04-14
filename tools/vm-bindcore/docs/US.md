# US（User Story，用户故事）

## 基于专利《一种优化虚拟机内业务绑核性能的方法》—— 发明人：张海亮

---

## US-01：Guest 侧业务绑核自动截获（kprobe 方式）

### [名称]
通过 kprobe 截获虚拟机内业务应用的绑核系统调用

### [US描述]
作为**虚拟化平台管理员**，我希望**虚拟机内的业务应用绑核动作能被自动截获并通知到 VMM**，以便**VMM 能够感知虚拟机内业务的 CPU 亲和性需求，进而在 Host 侧做出优化绑核决策**。

### [依赖]
- Guest OS 内核支持 kprobe（Linux ≥ 4.18）
- KVM/QEMU 虚拟化环境支持 hypercall 或 wrmsr

### [用户接口]
- **Guest 侧**：加载内核模块 `insmod vm_bindcore_kprobe.ko`
- **Guest 侧**：卸载内核模块 `rmmod vm_bindcore_kprobe`

### [验收准则]

**AC-US-01-01**：
- GIVEN 虚拟机 vm1 已加载 kprobe 截获内核模块
- WHEN vm1 内业务应用调用 `sched_setaffinity(0, sizeof(cpuset), &cpuset)` 将当前进程绑定到 vCPU 2
- THEN kprobe hook 触发，提取绑核目标信息（PID、目标 vCPU 掩码），并通过 hypercall 同步发送到 Host KVM 模块

**AC-US-01-02**：
- GIVEN kprobe 截获模块已加载
- WHEN 业务应用正常执行非绑核相关的系统调用
- THEN 截获模块不产生任何 VM-Exit 或通知开销，对正常业务性能影响 ≤ 1%

**AC-US-01-03**：
- GIVEN kprobe 截获模块已加载
- WHEN 业务应用连续高频调用 `sched_setaffinity`（如每秒 1000 次）
- THEN 截获模块能稳定工作，不造成 Guest OS 崩溃或死锁

### [备注]
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

### [名称]
通过 eBPF 截获虚拟机内业务应用的绑核系统调用（跨版本兼容）

### [US描述]
作为**虚拟化平台管理员**，我希望**能够使用 eBPF 技术在不修改 Guest 内核的情况下截获绑核动作，并支持一次编译多平台运行**，以便**减少每个 Guest OS 版本的适配维护工作量**。

### [依赖]
- Guest OS 内核支持 eBPF CO-RE（Linux ≥ 5.4，带 BTF 信息）
- Guest 侧安装 bpftool 或 libbpf 运行时

### [用户接口]
- **Guest 侧**：启动 eBPF 截获守护进程 `vm-bindcore-guest start`
- **Guest 侧**：停止 `vm-bindcore-guest stop`
- **Guest 侧**：查看状态 `vm-bindcore-guest status`

### [验收准则]

**AC-US-02-01**：
- GIVEN vm1 的 Guest OS 为 openEuler 22.03（内核 5.10）
- WHEN 启动 eBPF 截获守护进程后，业务应用调用 `sched_setaffinity`
- THEN eBPF 程序截获调用，通过 ring buffer 传递给用户态代理，用户态代理通过 hypercall 异步通知 VMM

**AC-US-02-02**：
- GIVEN 同一编译产物的 eBPF 程序
- WHEN 分别加载到 openEuler 22.03（内核 5.10）和 openEuler 24.03（内核 6.6）的 Guest OS
- THEN 两个版本都能正常截获 `sched_setaffinity`，无需重新编译

**AC-US-02-03**：
- GIVEN eBPF 截获守护进程运行中
- WHEN 业务应用解除绑核（恢复到所有 CPU）
- THEN eBPF 程序同样截获该解绑动作，异步通知 VMM 进行解绑恢复

### [备注]
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

### [名称]
VMM 接收 Guest 绑核通知后自动执行 Host 侧 1:1 绑核

### [US描述]
作为**虚拟化平台**，我希望**当 Guest 内业务绑核时，VMM 能自动将对应 vCPU 从范围绑核切换为 1:1 绑核**，以便**业务应用在超分配场景下也能获得稳定的 CPU 性能**。

### [依赖]
- US-01 或 US-02（Guest 侧截获与通知已就绪）
- VMM 侧全局 CPU 映射表已初始化

### [用户接口]
- **Host 侧**：自动处理，无需用户干预
- **Host CLI**：`vm-bindcore status --vm <domain>` 查看当前 vCPU 绑核状态

### [验收准则]

**AC-US-03-01**：
- GIVEN vm1 有 4 个 vCPU，cpuset 为 pCPU 0-15（范围绑核），当前所有 vCPU 在 pCPU 0-15 间自由调度
- WHEN vm1 内业务将任务绑定到 vcpu2
- THEN VMM 自动将 vcpu2 对应的 Host 任务通过 `sched_setaffinity` 1:1 绑定到一个空闲 pCPU（如 pCPU 8）

**AC-US-03-02**：
- GIVEN vm1 的 vcpu2 已被 1:1 绑定到 pCPU 8
- WHEN 使用 `vm-bindcore status --vm vm1` 查询
- THEN 输出显示 vcpu2 为 "exclusive: pCPU 8"，其他 vCPU 为 "range: pCPU 0-15"

**AC-US-03-03**：
- GIVEN vm1 有多个 vCPU 陆续被业务绑核
- WHEN vcpu0、vcpu1、vcpu2 分别被业务绑核
- THEN VMM 为每个 vCPU 选择不同的空闲 pCPU 进行 1:1 绑定，互不冲突

### [备注]
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

### [名称]
Guest 内业务解除绑核后 VMM 自动恢复范围绑核

### [US描述]
作为**虚拟化平台**，我希望**当 Guest 内业务解除绑核后，VMM 能自动将对应 vCPU 恢复为范围绑核**，以便**释放独占的 pCPU 资源，恢复资源共享与高利用率**。

### [依赖]
- US-03（VMM 侧 1:1 绑核已实现）

### [用户接口]
- **Host 侧**：自动处理，无需用户干预

### [验收准则]

**AC-US-04-01**：
- GIVEN vm1 的 vcpu2 当前被 VMM 1:1 绑定到 pCPU 8（因 Guest 内业务绑核触发）
- WHEN vm1 内业务调用 `sched_setaffinity` 恢复到全部 vCPU（解绑核）
- THEN VMM 收到解绑通知，将 vcpu2 恢复为范围绑核（cpuset 0-15），pCPU 8 的独占标记被释放

**AC-US-04-02**：
- GIVEN 多个 vCPU 陆续被 1:1 绑定后
- WHEN 所有业务均解除绑核
- THEN 所有 vCPU 恢复为范围绑核，全局 CPU 映射表中无独占记录

### [备注]
```
解绑恢复流程：
Guest App 解绑 → eBPF/kprobe 截获 → 通知 VMM → VMM 恢复范围绑核 → 释放独占 pCPU
```

---

## US-05：全局 CPU 映射管理与冲突避免

### [名称]
VMM 侧维护全局 CPU 映射表并自动避免绑核冲突

### [US描述]
作为**虚拟化平台管理员**，我希望**VMM 能维护一张全局 CPU 映射表，记录所有 VM 的绑核状态，在进行 1:1 绑核时自动避免多个 vCPU 绑定同一 pCPU**，以便**确保 1:1 绑核的性能隔离效果**。

### [依赖]
- US-03（VMM 侧 1:1 绑核功能）

### [用户接口]
- **Host CLI**：`vm-bindcore map` 显示全局 CPU 映射表
- **Host CLI**：`vm-bindcore map --format json` JSON 格式输出

### [验收准则]

**AC-US-05-01**：
- GIVEN pCPU 8 已被 vm1:vcpu2 独占绑定
- WHEN vm2 的 vcpu0 需要 1:1 绑核
- THEN VMM 跳过 pCPU 8，选择下一个空闲 pCPU（如 pCPU 9）进行绑定

**AC-US-05-02**：
- GIVEN 宿主机上运行多个 VM
- WHEN 执行 `vm-bindcore map`
- THEN 输出每个 pCPU 的占用状态和对应 VM/vCPU 信息

**AC-US-05-03**：
- GIVEN cpuset 范围内的所有 pCPU 都已被独占
- WHEN 新的 vCPU 需要 1:1 绑核
- THEN 系统记录警告日志，保持该 vCPU 为范围绑核模式（降级处理）

### [备注]
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

### [名称]
通过模拟 PCI/ISA 设备将 Guest 绑核信息传递到 QEMU 处理

### [US描述]
作为**虚拟化平台开发者**，我希望**提供一种基于模拟设备的替代通知方式**，以便**在 hypercall/wrmsr 不适用的场景下（如嵌套虚拟化或特定 Guest OS），仍能实现 Guest-to-Host 绑核信息传递**。

### [依赖]
- QEMU 侧实现模拟设备 X（PCI 或 ISA 设备）
- Guest 侧用户态程序能打开设备并写入绑核信息

### [用户接口]
- **Guest 侧**：`vm-bindcore-guest start --mode device`（使用模拟设备通知方式）
- **Host 侧 QEMU**：自动加载模拟设备 X

### [验收准则]

**AC-US-06-01**：
- GIVEN vm1 配置了模拟设备 X，Guest 侧使用 eBPF 截获 + 设备写入方式
- WHEN vm1 内业务绑核
- THEN eBPF 截获后，用户态代理通过 `write(fd, bindinfo, len)` 写入设备 X，QEMU 接收到绑核信息并执行 Host 侧绑核

**AC-US-06-02**：
- GIVEN 使用模拟设备方式
- WHEN 大量绑核/解绑操作
- THEN 设备 I/O 方式与 hypercall 方式功能一致，绑核结果正确

### [备注]
```
模拟设备流程（实施例二）：
Guest App → eBPF 截获 → 用户态打开设备X → write(绑核信息) → VM-Exit 到 QEMU
→ QEMU 设备模拟X处理 → 查询 cpuset/global maps → 执行绑核
```

---

## US-07：查看全局绑核状态

### [名称]
查看宿主机上所有虚拟机的 vCPU 绑核状态

### [US描述]
作为**虚拟化平台管理员**，我希望**能够在 Host 侧查看所有虚拟机的 vCPU 绑核状态**，以便**监控资源分配情况并排查性能问题**。

### [依赖]
- VMM 侧全局 CPU 映射表

### [用户接口]
- **Host CLI**：`vm-bindcore status [--vm <domain>] [--format json|table]`

### [验收准则]

**AC-US-07-01**：
- GIVEN 宿主机上运行 3 个 VM，各有不同绑核状态
- WHEN 执行 `vm-bindcore status`
- THEN 以表格形式输出每个 VM 的每个 vCPU 的绑核模式（range/exclusive）、绑定的 pCPU、触发来源

**AC-US-07-02**：
- GIVEN 无 VM 运行
- WHEN 执行 `vm-bindcore status`
- THEN 显示 "No active VMs found"

### [备注]
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

### [名称]
记录所有绑核事件到系统日志

### [US描述]
作为**虚拟化平台管理员**，我希望**所有由 Guest 内业务触发的绑核/解绑事件都被记录到系统日志**，以便**进行性能分析和安全审计**。

### [依赖]
- systemd/journald 日志服务

### [用户接口]
- **日志查看**：`journalctl -u vm-bindcore` 或 `vm-bindcore log [--vm <domain>]`

### [验收准则]

**AC-US-08-01**：
- GIVEN vm1 内业务触发绑核
- WHEN VMM 完成 1:1 绑核
- THEN journald 中记录包含：时间戳、VM 名称、vCPU 编号、绑核来源（guest-pin）、目标 pCPU、操作结果

**AC-US-08-02**：
- GIVEN vm1 内业务触发解绑
- WHEN VMM 恢复范围绑核
- THEN journald 中记录包含：时间戳、VM 名称、vCPU 编号、操作类型（unpin-restore）、恢复的 cpuset 范围

### [备注]
```
日志示例：
Apr 14 10:30:15 host vm-bindcore[1234]: [PIN] vm=vm1 vcpu=2 action=exclusive pcpu=8 source=guest-pin pid=5678
Apr 14 10:35:22 host vm-bindcore[1234]: [UNPIN] vm=vm1 vcpu=2 action=restore cpuset=0-15 source=guest-unpin
Apr 14 10:35:22 host vm-bindcore[1234]: [CONFLICT] vm=vm2 vcpu=0 requested=pcpu8 occupied_by=vm1:vcpu2 resolved=pcpu9
```

