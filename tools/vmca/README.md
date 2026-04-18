# vmca (VMCoreAffinity) — 虚拟机 vCPU 亲和性透传优化框架

基于专利《**一种优化虚拟机内业务绑核性能的方法**》（发明人：张海亮）

## 概述

在服务器虚拟化场景中，虚拟机 vCPU 与物理 CPU（pCPU）的绑核方式存在两种：

| 方式 | 优点 | 缺点 |
|------|------|------|
| **范围绑核** | 资源利用率高，支持超分配 | 性能不稳定 |
| **1:1 绑核** | 性能稳定 | 资源利用率低，管理复杂 |

`vmca` 实现了一种**透明的绑核透传优化机制**：当 VM 内业务应用通过
`sched_setaffinity` 绑核时，VMM 自动感知并在 Host 侧动态切换为 1:1 绑核；
业务解绑后自动恢复范围绑核。**兼顾资源利用率与性能稳定性**。

## 架构

```
┌──────────────────────────────── Guest VM ────────────────────────────────┐
│                                                                          │
│  ┌──────────┐    ┌──────────────────┐    ┌──────────────────────────┐   │
│  │ 业务 App │───>│ eBPF tracepoint  │───>│ BPF ring buffer          │   │
│  │ sched_   │    │ (CO-RE .bpf.o)  │    │                          │   │
│  │ set-     │    └──────────────────┘    └──────────┬───────────────┘   │
│  │ affinity │                                       │                   │
│  └──────────┘                          ┌────────────▼────────────────┐  │
│                                        │ 用户态通知代理 (Python)      │  │
│                                        │  • 排空 ring buffer          │  │
│                                        │  • 分类 pin / unpin          │  │
│                                        └────────────┬────────────────┘  │
│                                                     │                   │
│                                        ┌────────────▼────────────────┐  │
│                                        │ 通知模块 (独立可插拔)        │  │
│                                        │  VSOCK / virtio-serial      │  │
│                                        └────────────┬────────────────┘  │
└─────────────────────────────────────────────────────┼───────────────────┘
                                                      │
┌─────────────────────────────────────────────────────▼───────────────────┐
│  Host (VMM)                                                              │
│  ┌────────────────┐    ┌──────────────┐    ┌─────────────────────────┐  │
│  │ VSOCK 监听守护  │───>│ 全局 CPU 映射 │───>│ 绑核执行模块            │  │
│  │ 进程 (listener)│    │ + VM cpuset  │    │ virsh vcpupin / cgroup  │  │
│  └────────────────┘    └──────────────┘    └─────────────────────────┘  │
│                                                                          │
│  ┌────────────────┐    ┌──────────────┐                                 │
│  │ 事件日志模块    │    │ 重启恢复模块  │                                │
│  │ (审计追溯)      │    │ (持久化)      │                                │
│  └────────────────┘    └──────────────┘                                 │
└──────────────────────────────────────────────────────────────────────────┘
```

## 两个 RPM 包

| RPM 包 | 安装位置 | 功能 |
|--------|----------|------|
| `vmca` | **宿主机** | VSOCK 监听守护进程、全局 CPU 映射管理、virsh vcpupin 执行、事件日志、重启恢复 |
| `vmca-guest` | **虚拟机内** | eBPF CO-RE 截获 `sched_setaffinity`、异步 ring buffer 处理、VSOCK/virtio-serial 通知 |

## 安装

```bash
# 宿主机上安装
yum install vmca

# 虚拟机内安装
yum install vmca-guest
```

## 使用

### 宿主机侧

```bash
# 启动通知监听守护进程（自动发现所有运行中的 VM）
systemctl start vmca-listener

# 手动发现所有运行中 VM 的 vCPU 绑核信息（从 virsh vcpupin 获取）
vmca discover

# 发现指定 VM 的 vCPU 绑核信息
vmca discover --vm FusionOS-23T10

# 手动注入绑核通知（测试用）
vmca notify --vm FusionOS-23T10 --vcpu 2 --action pin --pid 5678

# 查看 VM 绑核状态
vmca status --vm FusionOS-23T10

# 查看全局 CPU 映射
vmca map

# 查看绑核事件日志
vmca log --vm FusionOS-23T10

# 恢复范围绑核
vmca unpin-all --vm FusionOS-23T10
```

> **注意**：无需手动注册 VM。vCPU 的绑核范围在虚拟机启动时已由 libvirt 初始化，
> vmca 通过 `virsh vcpupin <domain>` 自动获取每个 vCPU 的 CPU 亲和性范围。

### 虚拟机内

```bash
# 启动 eBPF 截获代理
systemctl start vmca-guest

# 或手动前台运行
vmca-guest start --foreground --mode vsock

# 查看代理状态
vmca-guest status

# 停止代理
vmca-guest stop
```

## 文档

- [SR 系统需求](docs/SR.md)
- [US 用户故事](docs/US.md)
- `man vmca`

## 许可证

MulanPSL-2.0
