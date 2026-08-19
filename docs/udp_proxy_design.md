# udp-proxy 设计说明

UDP 弱网仿真代理的内部设计与实现机制，覆盖模块结构、报文处理管线、带宽调度、队列重排、L4S 仿真、周期卡顿与统计。

源码：`src/main.cpp`（入口 + 参数解析）、`src/udp_proxy.hpp`（`UdpProxy` / `DisturbConfig` / `PktQueue`）、`src/udp_proxy.cpp`（实现）。

---

## 1. 总体架构

```mermaid
flowchart TB
    subgraph Sockets["Socket 层"]
        S["主 UDP socket (listen)"]
        CS["control socket (127.0.0.1:PORT)"]
    end

    subgraph Threads["线程"]
        R["recvLoop<br/>recvfrom → 方向分流 → process()"]
        F["senderLoop(forward)<br/>到期 sendto target"]
        V["senderLoop(reverse)<br/>到期 sendto lastClient"]
        ST["statsLoop<br/>周期打印统计"]
        CT["controlLoop<br/>控制命令 + stall 调度"]
        SC["scenarioLoop<br/>时间轴损伤脚本"]
    end

    subgraph Core["核心状态"]
        CFG["cfg_ (DisturbConfig)<br/>cfgMutex_ 保护"]
        FQ["fwdQ_ 优先级队列<br/>(按 due 时间排序, cap=queue_cap)"]
        RQ["revQ_ 优先级队列"]
        FS["fwdStats_ / revStats_ 计数器"]
        ENG["Engine 波形生成器<br/>bandwidth(cfg)"]
        STALL["StallSched 周期卡顿调度"]
        SCEN["ScenarioStep[] 场景段"]
    end

    S --> R
    CS --> CT
    R --> CFG
    R --> FQ
    R --> RQ
    FQ --> F
    RQ --> V
    FQ --> FS
    RQ --> FS
    CT --> CFG
    ST --> CFG
    SC --> CFG
    STALL --> CFG
    CFG --> ENG
```

线程分工：

| 线程 | 职责 |
| --- | --- |
| `recvLoop` | 唯一收包点。按源地址分流：target → reverse 队列；其他 → forward 队列并记录 `lastClient` |
| `senderLoop`（×2） | 阻塞在队列上，到报文 `due` 时间才 `sendto`（天然实现延迟 / 带宽调度 / 乱序） |
| `statsLoop` | 周期打印双向流量与损伤统计 |
| `controlLoop` | 读取控制命令并改配置；同时执行周期卡顿调度 |
| `scenarioLoop` | 按段应用场景脚本命令，段间休眠，循环回第一段时恢复启动参数 |

## 2. 报文处理管线

### 2.1 处理流程图

```mermaid
flowchart LR
    P["收包<br/>recvfrom"] --> D{"源 == target ?"}
    D -- "是" --> RD{"clean_reverse ?"}
    RD -- "是" --> RQ["入 reverse 队列<br/>(无损伤)"]
    RD -- "否" --> RP["process(reverse)<br/>施加 reverse 损伤"]
    D -- "否" --> LC["记录 lastClient"]
    LC --> FP["process(forward)<br/>施加 forward 损伤"]
    FP --> FQ["入 forward 队列"]
    RP --> RQ
```

### 2.2 process() 单包损伤顺序

```mermaid
sequenceDiagram
    participant R as recvLoop
    participant P as process()
    participant E as Engine
    participant Q as PktQueue

    R->>P: data, dst, disturb标志
    P->>P: 带宽重排标志检查 (rescale?)
    alt L4S 启用
        P->>P: 所有报文按 L4S 处理, l4s_seen++
    else 非 L4S
        P->>P: loss 判定 → 丢弃(lost++)
        P->>P: block 判定 → 丢弃(blocked++)
    end
    P->>E: delay-normal? N(mean,σ)裁剪±2σ + tail 长尾 : 均匀[min,max]
    P->>P: reorder 判定 → 额外 hold-off
    P->>E: 当前带宽 bw
    P->>P: 服务时间 t = size×8/bw<br/>due = max(now, drain) + t<br/>drain = due
    alt L4S 且积压 > 阈值
        P->>P: 发送 CE 标记包(替换丢包)
    end
    P->>P: dup/corrupt 判定
    P->>Q: push(due)
    alt 队列满
        P->>P: qfull++
    end
```

### 2.3 带宽限速模型（令牌桶等效）

`PktQueue` 是**按到期时间排序的优先级队列**，`drain` 记录"链路排队积压的虚拟完成时间"：

```
服务时间 tms = 报文大小(bytes) × 8 × 1000 / 当前带宽(bps)
due = max(当前时间, drain) + tms
drain = due
```

- 流量超过带宽 → `due` 不断后移 → 队列堆积（积压时延 = `drain - now`）；
- 队列满（`queue_cap`，默认 4096）→ 新报文丢弃（`qfull`），模拟拥塞丢弃；
- 带宽恢复 → 排队报文按新速率快速清空。

### 2.4 带宽波形（Engine::bandwidth）

```mermaid
flowchart TD
    B["bandwidth(cfg)"] --> W{"wave 类型"}
    W -- "rect" --> WR["ph < duty ? max : min"]
    W -- "sine" --> WS["min + (max-min)×0.5×(1+sin(2π·ph))"]
    W -- "sawtooth" --> WA["min + (max-min)×p<br/>p = (ph + 随机相位) mod 1<br/>每周期随机起点"]
    W -- "random" --> WD["uniform[min,max] 每包随机"]
    W -- "off" --> WO["不启用带宽"]
    WR --> OUT["返回当前带宽 bps"]
    WS --> OUT
    WA --> OUT
    WD --> OUT
```

其中 `ph = elapsed_ms mod period / period`。Sawtooth 的周期起点用 `splitmix64(cycle + seed)` 随机化，避免可预测的锯齿重复。

## 3. 带宽变化时的队列重排（rescale）

**问题**：带宽下降 → 旧积压按旧速率排程，链路被旧积压以旧速率长时间占用，带宽恢复后发送端测得的 btl（瓶颈带宽）长期不回升。

**机制**（`rescaleDirection` / `PktQueue::rescale`）：`controlLoop` 或场景切换导致带宽配置变化时，记录变化前后的有效速率，置位 `rescaleFwd_/rescaleRev_`；`process()` 下次收包时对队列内所有未到期报文按速率比压缩 `due`：

```mermaid
flowchart LR
    A["带宽配置变化<br/>markBwChanged()"] --> B["记录 before/after<br/>置位 rescale 标志"]
    B --> C["process() 检查标志"]
    C --> D{"was 启用 && now 启用?"}
    D -- "是" --> E["ratio = before/after"]
    E --> F["对每个 due>now 的报文:<br/>due = now + (due-now)×ratio"]
    D -- "否(关闭/开启)" --> G["rebase():<br/>所有 due=now, 立即发送"]
    F --> H["drain 同步按 ratio 缩放"]
    G --> H
```

效果：带宽从 4M 降到 400K 时 `ratio=10`，旧积压的排程时间整体拉长 10 倍（等价于按新速率重新排程）；带宽恢复时 `ratio<1`，积压被压缩快速清空，btl 迅速回升。

## 4. 延迟模型

### 4.1 旧模式（uniform）

`delay-prob P` 概率命中后，在 `[delay-min, delay-max]` 均匀取延迟。

### 4.2 正态 + 长尾模式（delay-normal，推荐）

模拟真实网络的陡峭延迟曲线：

```
主体: N(mean, σ)，裁剪到 [mean-2σ, mean+2σ]
长尾: 另以 tail_prob 概率叠加 uniform[tail_min, tail_max] 的偶发大延迟
```

```mermaid
flowchart LR
    A["采样 N(mean,σ)"] --> B{"v < mean-2σ ?"}
    B -- "是" --> C["v = mean-2σ"]
    B -- "否" --> D{"v > mean+2σ ?"}
    D -- "是" --> E["v = mean+2σ"]
    D -- "否" --> F["u01 < tail_prob ?"]
    C --> F
    E --> F
    F -- "是" --> G["v += uniform[tail_min, tail_max]"]
    F -- "否" --> H["dms = v"]
    G --> H
```

设计意图：主体延迟模拟常态排队抖动，长尾模拟偶发的拥塞/重传事件 —— 正是传输层 FEC 需要覆盖的报文。

## 5. L4S 仿真（RFC 9331）

Windows 上读取不到 IP TOS，因此当前实现**把入包一律视为 L4S 流**（单条 L4S 流的仿真场景）：

```mermaid
flowchart TD
    A["报文入队"] --> B{"积压 = drain - now"}
    B --> C{"积压 > l4s_threshold_ms ?"}
    C -- "是" --> D["向接收方发 CE 标记包<br/>(tight_detail::ecn::kCeMarkMagic)"]
    C -- "否" --> E["正常入队, 不标记"]
    D --> F["ce_marked++"]
    F --> E
```

闭环：带宽下降 → 队列积压 → CE 标记 → 传输协议收到 CE 提前降速 → 队列清空 → 标记自动停止。标记替代丢包（`l4s` 启用时跳过 loss/block 丢弃），配合 tight 的 ECN 处理实现无丢包拥塞控制。L4S 模式下 `clean_reverse` 仅控制是否做延迟/带宽等扰动。

## 6. 周期卡顿调度（stall）

`controlLoop` 每次迭代调用 `applyStallSchedule()`，独立于带宽波形周期性制造"瞬间卡死"：

```
命令: stall <low_bps> <dur_ms> <period_ms>
行为: 每 period_ms 周期内, 前 dur_ms 带宽强制为 low_bps, 之后恢复 normal_bps
      (normal_bps = 卡顿前的 bw_max)
```

```mermaid
stateDiagram-v2
    [*] --> Normal: stall 命令解析, next_start = now + period
    Normal --> Stalled: now >= next_start<br/>bw = low_bps, restore_at = now + dur
    Stalled --> Normal: now >= restore_at<br/>bw = normal_bps, next_start = now + period
```

每次切换都会走 `markBwChanged` → 触发队列重排，保证卡顿开始/结束都立即反映到调度上。

## 7. 场景脚本执行器

```mermaid
sequenceDiagram
    participant M as main
    participant S as scenarioLoop
    participant C as apply_command
    participant P as process

    M->>S: set_scenario() 解析文件 → ScenarioStep[]
    loop 循环执行
        S->>S: idx==0 时 resetToBaseline()<br/>(恢复启动参数快照)
        S->>C: 逐条应用本段命令 (cmd1; cmd2; ...)
        S->>S: 分段休眠 100ms, 期间响应 stop / 控制命令
        S->>S: idx = (idx+1) % N, 时间到进入下一段
    end
    C->>P: 配置变更生效 (带宽变更会触发 rescale)
```

文件格式（`scenarios/*.txt`）：

```
<seconds> : cmd1 ; cmd2 ; ...
# 注释
```

- `#` 截断、首尾空白清除、空行跳过；
- 每段 `duration_s` 后进入下一段，**循环回第一段时恢复 baseline**（启动参数快照）；
- 段内命令语法与 `apply_command` 完全一致。

## 8. 并发与一致性

```mermaid
flowchart LR
    subgraph Lock["锁"]
        L1["cfgMutex_<br/>保护 DisturbConfig + stall 配置"]
        L2["PktQueue::m_<br/>保护队列 + cv"]
        L3["clientM_<br/>保护 lastClient"]
    end
    subgraph Atomic["原子量"]
        A1["rescaleFwd_/rescaleRev_"]
        A2["bwWas/NowEnabled_, bwBefore_/bwAfter_"]
        A3["各 DirStats 计数器"]
        A4["stop_ / started_"]
    end
```

- `process()` 内先快照 `cfg_`（持锁拷贝），随后整个损伤计算不使用锁，避免长持锁；
- 带宽重排用原子标志"置位 + 消费"模式：`controlLoop` 置位，`process()` 下次收包消费执行，无需额外同步；
- 两个 `senderLoop` 在队列上 `cv_.wait_until(due)`，`stop()` 时 `wake()` 唤醒退出。

## 9. 统计口径

`statsLoop` 实测带宽 = `Δbytes × 8 / statsInterval`，与配置带宽（`bandwidth wave=...`）对照，可判断发送端是否充分利用链路。`qfull` 是拥塞的直接信号（发送速率 > 当前带宽），`ce` 是 L4S 标记计数。
