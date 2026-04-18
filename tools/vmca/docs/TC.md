# TC（Test Case，测试用例）

## 基于专利《一种优化虚拟机内业务绑核性能的方法》—— 发明人：张海亮

---

> 以下测试用例均从**最终用户可执行、可观测**的角度编写，覆盖 SR 和 US 的核心验收场景。

---

## TC-01：VM 自动发现 — vCPU 绑核范围获取验证

| 项目 | 内容 |
|------|------|
| **关联** | SR 场景 1, US-01 AC-01-01 |
| **前置条件** | 宿主机运行 VM FusionOS-23T10（19 vCPU），vcpu0-15 绑定 `5-47,53-63`，vcpu16-18 绑定 `0-63`，Host 侧 vmca 服务已启动 |

**步骤**：
1. 在 Host 侧执行 `vmca discover --vm FusionOS-23T10`
2. 检查输出中每个 vCPU 的 cpuset 信息

**预期结果**：
- 步骤 1-2：输出显示 19 个 vCPU，vcpu0-15 的 cpuset 为 `5-47,53-63`，vcpu16-18 的 cpuset 为 `0-63`，无需手动注册

---

## TC-02：单 VM 单 vCPU 绑核 — 端到端验证

| 项目 | 内容 |
|------|------|
| **关联** | SR 场景 2, US-02 AC-02-01 |
| **前置条件** | 宿主机运行 vm1，Guest 内已启动 `vmca-guest`，vCPU 已被自动发现 |

**步骤**：
1. 在 Host 侧执行 `vmca status --vm vm1`，确认所有 vCPU 均为 `range` 模式
2. 在 vm1 Guest 内执行 `taskset -c 2 stress --cpu 1 --timeout 60s`（将压测进程绑定到 vCPU 2）
3. 等待 2 秒后，在 Host 侧执行 `vmca status --vm vm1`

**预期结果**：
- 步骤 1：所有 vCPU 显示 `range` 模式
- 步骤 3：vcpu2 显示 `exclusive` 模式，绑定到具体 pCPU，`source` 为 `guest-pin`；其余 vCPU 仍为 `range` 模式

---

## TC-03：单 VM 绑核后解绑 — 自动恢复验证

| 项目 | 内容 |
|------|------|
| **关联** | SR 场景 4, US-02 AC-02-02 |
| **前置条件** | 承接 TC-02 执行完成，vm1 的 vcpu2 当前为 `exclusive` 模式 |

**步骤**：
1. 在 Host 侧执行 `vmca status --vm vm1`，确认 vcpu2 为 `exclusive` 模式
2. 在 Host 侧执行 `vmca map`，记录 vcpu2 独占的 pCPU 编号（如 pCPU 8）
3. 在 vm1 Guest 内终止 stress 进程（`kill` 或等待超时结束）
4. 等待 2 秒后，在 Host 侧再次执行 `vmca status --vm vm1`
5. 在 Host 侧执行 `vmca map`

**预期结果**：
- 步骤 4：vcpu2 恢复为 `range` 模式（cpuset 恢复为该 vCPU 的原始亲和性范围）
- 步骤 5：之前被独占的 pCPU 状态恢复为 `shared`

---

## TC-04：多 VM 绑核冲突避免验证

| 项目 | 内容 |
|------|------|
| **关联** | SR 场景 5, US-03 AC-03-01 |
| **前置条件** | 宿主机运行 vm1 和 vm2，两者 cpuset 有重叠，Guest 内均已启动 `vmca-guest` |

**步骤**：
1. 在 vm1 Guest 内执行 `taskset -c 0 stress --cpu 1 --timeout 120s`
2. 等待 2 秒，在 Host 侧执行 `vmca map`，记录 vm1:vcpu0 绑定的 pCPU（如 pCPU 5）
3. 在 vm2 Guest 内执行 `taskset -c 0 stress --cpu 1 --timeout 120s`
4. 等待 2 秒，在 Host 侧执行 `vmca map`

**预期结果**：
- 步骤 4：vm2:vcpu0 绑定到**另一个** pCPU（如 pCPU 6），**不与** vm1:vcpu0 的 pCPU 5 冲突
- 两个 pCPU 均标记为 `exclusive`，分别归属于不同的 VM

---

## TC-05：全局 CPU 映射表查询 — 状态正确性验证

| 项目 | 内容 |
|------|------|
| **关联** | US-03 AC-03-02 |
| **前置条件** | 宿主机运行 vm1（2 个 vCPU 已绑核）、vm2（1 个 vCPU 已绑核）、vm3（无绑核） |

**步骤**：
1. 在 Host 侧执行 `vmca status`（查看所有 VM）
2. 在 Host 侧执行 `vmca map`（查看全局 pCPU 映射）
3. 在 Host 侧执行 `vmca status --vm vm1 --format json`

**预期结果**：
- 步骤 1：以表格形式输出 vm1（2 个 `exclusive` + 剩余 `range`）、vm2（1 个 `exclusive` + 剩余 `range`）、vm3（全部 `range`），格式清晰
- 步骤 2：输出每个 pCPU 的 `shared`/`exclusive` 状态，独占的 pCPU 显示对应 VM:vCPU
- 步骤 3：输出合法 JSON，包含 `mode`、`pcpu`、`source` 等字段

---

## TC-06：eBPF 跨内核版本兼容性验证

| 项目 | 内容 |
|------|------|
| **关联** | US-02 AC-02-03 |
| **前置条件** | 宿主机分别运行 vm-a（openEuler 22.03，内核 5.10）和 vm-b（openEuler 24.03，内核 6.6），使用同一编译产物的 eBPF 截获程序 |

**步骤**：
1. 在 vm-a Guest 内执行 `vmca-guest start`，确认 `vmca-guest status` 显示 `running`
2. 在 vm-b Guest 内执行 `vmca-guest start`，确认 `vmca-guest status` 显示 `running`
3. 分别在 vm-a 和 vm-b 内执行 `taskset -c 1 stress --cpu 1 --timeout 60s`
4. 在 Host 侧执行 `vmca status`

**预期结果**：
- 步骤 1-2：两个不同内核版本的 Guest 均能成功加载同一 eBPF 截获程序
- 步骤 4：两个 VM 的 vcpu1 均显示为 `exclusive` 模式，eBPF CO-RE 方式跨版本正常工作

---

## TC-07：日志审计 — 绑核/解绑事件记录验证

| 项目 | 内容 |
|------|------|
| **关联** | US-04 AC-04-01, AC-04-02 |
| **前置条件** | Host 侧 vmca 服务已启动且日志正常输出 |

**步骤**：
1. 在 vm1 Guest 内执行 `taskset -c 2 stress --cpu 1 --timeout 30s`（触发绑核）
2. 等待 2 秒，在 Host 侧执行 `vmca log --vm vm1`
3. 等待 30 秒（stress 超时结束），在 Host 侧再次执行 `vmca log --vm vm1`

**预期结果**：
- 步骤 2：日志中可看到 `[PIN]` 事件记录，包含 `vm=vm1 vcpu=2 action=exclusive pcpu=<N> source=guest-pin`
- 步骤 3：日志中可看到 `[UNPIN]` 事件记录，包含 `vm=vm1 vcpu=2 action=restore`

---

## 测试用例覆盖矩阵

| 测试用例 | SR 验收场景 | US AC | 覆盖场景 |
|----------|-------------|-------|----------|
| TC-01 | 场景 1（可用性） | AC-01-01 | VM 自动发现（无需注册） |
| TC-02 | 场景 2（可用性） | AC-02-01 | 单 VM 绑核端到端 |
| TC-03 | 场景 4（完整性） | AC-02-02 | 自动解绑恢复 |
| TC-04 | 场景 5（完整性） | AC-03-01 | 多 VM 绑核冲突避免 |
| TC-05 | — | AC-03-02 | 全局映射与状态查询 |
| TC-06 | — | AC-02-03 | eBPF 跨版本兼容 |
| TC-07 | 场景 4（完整性） | AC-04-01, AC-04-02 | 日志审计记录 |
