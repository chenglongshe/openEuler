# TC（Test Case，测试用例）

## 基于专利《一种优化虚拟机内业务绑核性能的方法》—— 发明人：张海亮

---

> 以下测试用例均从**最终用户可执行、可观测**的角度编写，覆盖 SR 和 US 的核心验收场景。

---

## TC-01：单 VM 单 vCPU 绑核 — 端到端验证

| 项目 | 内容 |
|------|------|
| **关联** | SR 场景 1, US-01, US-03 |
| **前置条件** | 宿主机运行 vm1（4 vCPU，cpuset 0-15），Guest 内已加载截获模块（kprobe 或 eBPF），Host 侧 vm-bindcore 服务已启动 |

**步骤**：
1. 在 Host 侧执行 `vm-bindcore status --vm vm1`，确认所有 vCPU 均为 `range` 模式
2. 在 vm1 Guest 内执行 `taskset -c 2 stress --cpu 1 --timeout 60s`（将压测进程绑定到 vCPU 2）
3. 等待 2 秒后，在 Host 侧执行 `vm-bindcore status --vm vm1`

**预期结果**：
- 步骤 1：所有 vcpu0-vcpu3 显示 `range` 模式，`pCPU 0-15`
- 步骤 3：vcpu2 显示 `exclusive` 模式，绑定到具体 pCPU（如 `pCPU 8`），`source` 为 `guest-pin`；其余 vCPU 仍为 `range` 模式

---

## TC-02：单 VM 绑核后解绑 — 自动恢复验证

| 项目 | 内容 |
|------|------|
| **关联** | SR 场景 2, US-04 |
| **前置条件** | 承接 TC-01 执行完成，vm1 的 vcpu2 当前为 `exclusive` 模式 |

**步骤**：
1. 在 Host 侧执行 `vm-bindcore status --vm vm1`，确认 vcpu2 为 `exclusive` 模式
2. 在 Host 侧执行 `vm-bindcore map`，记录 vcpu2 独占的 pCPU 编号（如 pCPU 8）
3. 在 vm1 Guest 内终止 stress 进程（`kill` 或等待超时结束）
4. 等待 2 秒后，在 Host 侧再次执行 `vm-bindcore status --vm vm1`
5. 在 Host 侧执行 `vm-bindcore map`

**预期结果**：
- 步骤 4：vcpu2 恢复为 `range` 模式（`pCPU 0-15`）
- 步骤 5：之前被独占的 pCPU 8 状态恢复为 `shared`

---

## TC-03：多 VM 绑核冲突避免验证

| 项目 | 内容 |
|------|------|
| **关联** | US-05 |
| **前置条件** | 宿主机运行 vm1 和 vm2，两者 cpuset 有重叠（如均包含 pCPU 0-15），Guest 内均已加载截获模块 |

**步骤**：
1. 在 vm1 Guest 内执行 `taskset -c 0 stress --cpu 1 --timeout 120s`
2. 等待 2 秒，在 Host 侧执行 `vm-bindcore map`，记录 vm1:vcpu0 绑定的 pCPU（如 pCPU 0）
3. 在 vm2 Guest 内执行 `taskset -c 0 stress --cpu 1 --timeout 120s`
4. 等待 2 秒，在 Host 侧执行 `vm-bindcore map`

**预期结果**：
- 步骤 4：vm2:vcpu0 绑定到**另一个** pCPU（如 pCPU 1），**不与** vm1:vcpu0 的 pCPU 0 冲突
- 两个 pCPU 均标记为 `exclusive`，分别归属于不同的 VM

---

## TC-04：全局 CPU 映射表查询 — 状态正确性验证

| 项目 | 内容 |
|------|------|
| **关联** | US-05, US-07 |
| **前置条件** | 宿主机运行 vm1（2 个 vCPU 已绑核）、vm2（1 个 vCPU 已绑核）、vm3（无绑核） |

**步骤**：
1. 在 Host 侧执行 `vm-bindcore status`（查看所有 VM）
2. 在 Host 侧执行 `vm-bindcore map`（查看全局 pCPU 映射）
3. 在 Host 侧执行 `vm-bindcore status --vm vm1 --format json`

**预期结果**：
- 步骤 1：以表格形式输出 vm1（2 个 `exclusive` + 剩余 `range`）、vm2（1 个 `exclusive` + 剩余 `range`）、vm3（全部 `range`），格式清晰
- 步骤 2：输出每个 pCPU 的 `shared`/`exclusive` 状态，独占的 pCPU 显示对应 VM:vCPU
- 步骤 3：输出合法 JSON，包含 `mode`、`pcpu`、`source` 等字段

---

## TC-05：eBPF 跨内核版本兼容性验证

| 项目 | 内容 |
|------|------|
| **关联** | US-02 |
| **前置条件** | 宿主机分别运行 vm-a（openEuler 22.03，内核 5.10）和 vm-b（openEuler 24.03，内核 6.6），使用同一编译产物的 eBPF 截获程序 |

**步骤**：
1. 在 vm-a Guest 内执行 `vm-bindcore-guest start`，确认 `vm-bindcore-guest status` 显示 `running`
2. 在 vm-b Guest 内执行 `vm-bindcore-guest start`，确认 `vm-bindcore-guest status` 显示 `running`
3. 分别在 vm-a 和 vm-b 内执行 `taskset -c 1 stress --cpu 1 --timeout 60s`
4. 在 Host 侧执行 `vm-bindcore status`

**预期结果**：
- 步骤 1-2：两个不同内核版本的 Guest 均能成功加载同一 eBPF 截获程序
- 步骤 4：两个 VM 的 vcpu1 均显示为 `exclusive` 模式，eBPF CO-RE 方式跨版本正常工作

---

## TC-06：日志审计 — 绑核/解绑事件记录验证

| 项目 | 内容 |
|------|------|
| **关联** | US-08 |
| **前置条件** | Host 侧 vm-bindcore 服务已启动且日志正常输出到 journald |

**步骤**：
1. 在 Host 侧执行 `journalctl -u vm-bindcore --since "now"` 清空日志视图
2. 在 vm1 Guest 内执行 `taskset -c 2 stress --cpu 1 --timeout 30s`（触发绑核）
3. 等待 2 秒，在 Host 侧执行 `journalctl -u vm-bindcore --since "1 min ago"`
4. 等待 30 秒（stress 超时结束），在 Host 侧再次执行 `journalctl -u vm-bindcore --since "1 min ago"`

**预期结果**：
- 步骤 3：日志中可看到 `[PIN]` 事件记录，包含 `vm=vm1 vcpu=2 action=exclusive pcpu=<N> source=guest-pin`
- 步骤 4：日志中可看到 `[UNPIN]` 事件记录，包含 `vm=vm1 vcpu=2 action=restore cpuset=0-15`

---

## 测试用例覆盖矩阵

| 测试用例 | SR 场景 | US AC | 覆盖场景 |
|----------|---------|-------|----------|
| TC-01 | 场景 1 | AC-US-01-01, AC-US-03-01 | 单 VM 绑核端到端 |
| TC-02 | 场景 2 | AC-US-04-01, AC-US-04-02 | 自动解绑恢复 |
| TC-03 | — | AC-US-05-01 | 多 VM 绑核冲突避免 |
| TC-04 | — | AC-US-05-02, AC-US-07-01 | 全局映射与状态查询 |
| TC-05 | — | AC-US-02-01, AC-US-02-02 | eBPF 跨版本兼容 |
| TC-06 | — | AC-US-08-01, AC-US-08-02 | 日志审计记录 |
