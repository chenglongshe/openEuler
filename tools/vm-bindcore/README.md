# vm-bindcore — 虚拟机内业务绑核透传优化工具

基于专利《**一种优化虚拟机内业务绑核性能的方法**》（发明人：张海亮）

## 概述

在服务器虚拟化场景中，虚拟机 vCPU 与物理 CPU（pCPU）的绑核方式存在两种：

| 方式 | 优点 | 缺点 |
|------|------|------|
| **范围绑核** | 资源利用率高，支持超分配 | 性能不稳定 |
| **1:1 绑核** | 性能稳定 | 资源利用率低，管理复杂 |

`vm-bindcore` 实现了一种**透明的绑核透传优化机制**：当 VM 内业务应用通过
`sched_setaffinity` 绑核时，VMM 自动感知并在 Host 侧动态切换为 1:1 绑核；
业务解绑后自动恢复范围绑核。**兼顾资源利用率与性能稳定性**。

## 架构

```
┌──────────────────────────────────────────────────────────┐
│  Guest VM                                                 │
│  ┌─────────┐    ┌──────────────┐    ┌──────────────────┐ │
│  │ 业务 App │───>│ kprobe/eBPF  │───>│ hypercall/wrmsr  │─┼──┐
│  │ sched_   │    │ 截获模块     │    │ 或模拟设备通知   │ │  │
│  │ set-     │    └──────────────┘    └──────────────────┘ │  │
│  │ affinity │                                             │  │
│  └─────────┘                                              │  │
└──────────────────────────────────────────────────────────┘  │
                                                              │ VM-Exit
┌──────────────────────────────────────────────────────────┐  │
│  Host (VMM / KVM / QEMU)                                  │  │
│  ┌──────────────┐    ┌──────────────┐    ┌────────────┐  │  │
│  │ 通知处理模块 │<───│ KVM 模块     │<───│ VM-Exit    │<─┼──┘
│  │ vm-bindcore  │    │ 退出处理     │    │ 处理       │  │
│  └──────┬───────┘    └──────────────┘    └────────────┘  │
│         │                                                 │
│  ┌──────▼───────┐    ┌──────────────┐                    │
│  │ 全局 CPU 映射 │───>│ 绑核执行模块 │                    │
│  │ Global CPU   │    │ 1:1 绑定     │                    │
│  │ Map          │    │ /范围恢复    │                    │
│  └──────────────┘    └──────────────┘                    │
└──────────────────────────────────────────────────────────┘
```

## 核心功能

1. **Guest 侧截获**：kprobe（同步）或 eBPF CO-RE（异步，一次编译多版本运行）
2. **Guest→Host 通知**：hypercall / wrmsr / 模拟设备（PCI/ISA）
3. **VMM 动态 1:1 绑核**：Guest 业务绑核 → VMM 自动独占绑定 pCPU
4. **自动解绑恢复**：Guest 业务解绑 → VMM 自动恢复范围绑核
5. **全局 CPU 映射**：跨 VM 冲突检测与避免，确保 pCPU 不被多个 vCPU 独占
6. **持久化**：绑核状态持久化，重启自动恢复

## 安装

```bash
# Host 侧
yum install vm-bindcore

# Guest 侧（虚拟机内）
yum install vm-bindcore-guest
```

## 使用

### Host 侧（VMM 管理工具）

```bash
# 注册 VM（通常由 libvirt hook 自动完成）
vm-bindcore register --vm myvm --vcpus 4 --cpuset 0-15

# 模拟接收 Guest 绑核通知（用于测试）
vm-bindcore notify --vm myvm --vcpu 2 --action pin --pid 5678

# 查看 VM 绑核状态
vm-bindcore status --vm myvm

# 查看全局 CPU 映射
vm-bindcore map

# 恢复范围绑核
vm-bindcore unpin-all --vm myvm
```

### Guest 侧（截获代理）

```bash
# 启动 eBPF 截获代理
vm-bindcore-guest start

# 查看代理状态
vm-bindcore-guest status

# 停止代理
vm-bindcore-guest stop
```

## 文档

- [SR 系统需求](docs/SR.md)
- [US 用户故事](docs/US.md)
- `man vm-bindcore`

## 许可证

MulanPSL-2.0
