# TCP 改造研究报告:spdm_tool ↔ cxl_test_tool 交互

**日期**: 2026-08-11
**目标**: 分析将 MCTP/DOE 两条路径的 UDP 报文交互改为 TCP 的可行性、影响面与设计
**状态**: 研究分析完成,未实现
**依据**: 三个并行 explore 对 `transport_doe.c`/`spdm_responder_udp.c`/`cxl_test_tool.c`/`bridge.c` 的代码事实核验

---

## 1. 现状(三段 UDP 交互,已核实)

```
MCTP 路径: spdm_tool(AF_MCTP)→ 内核 → bridge ──UDP A──▶ cxl_test_tool -u → VU vuid=0x11 → 固件
DOE 路径:  spdm_tool --trans doe ──UDP B──▶ cxl_test_tool -U → /dev/doe0 ioctl → 设备 DOE
验证对端:  spdm_responder_udp(测试用 DOE responder)──UDP── 接收 spdm_tool
```

| 段 | 发送方 socket | 接收方 socket | payload 格式 | framing |
|---|---|---|---|---|
| bridge → cxl_test_tool `-u` | connected UDP(`send`) | UDP bind+O_NONBLOCK busy-poll(`recvfrom/sendto`) | 裸 MCTP 帧(4B mctp_hdr + 1B type + payload) | **零 framing**,数据报=一帧 |
| spdm_tool `--trans doe` → cxl_test_tool `-U` | connected UDP(`send/recv`,SO_RCVTIMEO 5000ms) | UDP bind+O_NONBLOCK busy-poll | 裸 DOE 数据对象(8B DOE 头 + payload) | **零 framing**,响应长度自描述(`length*4`) |
| spdm_responder_udp ↔ spdm_tool | UDP sendto/recvfrom | 同上 | 同上 | 零 framing |

关键事实:
- DOE 响应长度**自描述**(DOE 头 length 字段 ×4),请求/响应都是完整数据对象
- MCTP 帧**无长度字段**(mctp_hdr 4B 无 len),帧边界全靠 UDP 数据报
- cxl_test_tool 两个 loop 都是 **busy-poll**(usleep(1000)),VU 模式有 **AER 线程**推设备事件到 `last_peer`
- transport_doe.c 的 discovery 用 `doe_udp_exchange`(send+recv,**无自身超时,继承 socket 选项**——潜在隐患)

## 2. 改 TCP 的核心变化

### 2.1 帧边界(TCP 是流,必须加 framing)——最大改动点

| 段 | UDP 现状 | TCP 需求 |
|---|---|---|
| DOE | 数据报=一帧,响应长度自描述 | 请求需长度前缀(4B);响应可仅依赖 DOE 头 length(但仍建议统一前缀) |
| MCTP | 数据报=一帧,无长度字段 | **必须加长度前缀**(2B 或 4B) |

**framing 方案**:
- **方案 A(推荐)**:统一 4B u32 长度前缀(载荷字节数)+ 原载荷。最简,与 DOE 头 LE 约定一致。MCTP 段可用 2B(帧 < 64KB)但统一 4B 更省心。
- **方案 B**:复用 spdm-emu platform framing `[cmd u32 BE][type u32 BE][len u32 BE][payload]`(已有互通先例,transport_tcp.c 已验证)。若未来需要区分 MCTP/DOE 类型,type 字段现成。
- **方案 C**:libspdm TCP binding header(4B,含 type)——仅当直接套 DSP0287 语义。

### 2.2 连接模型

- **DOE 段**:client connect → server accept,单连接(SPDM 单 requester 足够)。`listen()+accept()` 是新增状态(UDP 无此概念)。
- **MCTP 段(bridge)**:bridge 当前"单 connected UDP socket 双向"(connect 到 --udp 目标 + bind --listen-port 收对端)。**TCP 无法单 socket 服务两个端点**:
  - 方案:bridge 只 `connect` 到 receiver(去掉 --listen-port 需求),receiver accept 后双向走连接。简化。
  - 或:bridge 同时 listen+connect(两 socket),复杂,不建议。

### 2.3 语义变化

| 项 | UDP | TCP |
|---|---|---|
| `recv()==0` | 死分支(UDP 永不返回 0) | **EOF = 对端关闭**,必须处理(断连重连) |
| send | 不阻塞,整包 | 可能部分写/阻塞(需循环写 + MSG_NOSIGNAL) |
| 丢包 | 有(recv timeout 边界) | **无(TCP 保证),消除 UDP 丢包超时问题** |
| 超时 | SO_RCVTIMEO | 对流式 socket **同样有效**,策略不变 |
| 多 peer | 天然多客户端(每数据报 peer) | 单连接;多客户端需 accept 循环(SPDM 场景不需要) |

### 2.4 AER 线程适配(cxl_test_tool VU 模式)

- UDP:全局 `last_peer`(mutex 保护),AER 推设备事件到该 peer
- TCP:连接后 peer 固定 → `last_peer` 变为连接 fd;AER 与主循环**并发写同一 fd 需写锁**(现有 peer_mutex 可复用);`sec_mb_aer_drive` 的 `sendto` 改 `write`

## 3. 改动面清单

| 段 | 文件 | 改动 |
|---|---|---|
| DOE host | `transport_doe.c` | `SOCK_DGRAM→SOCK_STREAM`;getaddrinfo hints 改;删 `g_dst` 死代码;`send→write` 循环(MSG_NOSIGNAL);`recv→读长度前缀+循环读`;`doe_udp_exchange` 加显式超时(修隐患) |
| DOE receiver | `cxl_test_tool.c udp_doe_forward_loop` | `bind+listen+accept`;`recvfrom/sendto → 连接 fd 读写`;framing 解析;busy-poll → 阻塞读(或 poll) |
| DOE 测试对端 | `spdm_responder_udp.c` | 同 receiver(`accept` 单连接);`handle_discovery` 走连接 fd;peer 跟踪(g_peer)简化 |
| MCTP bridge | `bridge.c` | `connect` 到 receiver(send 侧近 drop-in);**--listen-port 语义变化**(bind→listen+accept 或去掉);加长度前缀;`recv==0=EOF` 处理 |
| MCTP receiver | `cxl_test_tool.c udp_vu_forward_loop` | 同 DOE receiver;AER 线程:last_peer→连接 fd + 写锁 |
| CLI | 两者 | `-u/-U` 端口语义不变(监听端口);bridge `--listen-port` 需求调整 |

**参考实现**:`spdm_tool/transport_tcp.c` 的 `read_bytes`/`write_bytes`(EINTR-continue、MSG_NOSIGNAL、recv==0→peer closed)可直接复用为 framing 助手。

## 4. 工作量与风险

| 段 | 工作量 | 风险 | 验证 |
|---|---|---|---|
| DOE 段(transport_doe + udp_doe_forward_loop + responder) | 中(~3 文件) | framing 边界错误是最大风险点 | **当前环境可端到端验证**(spdm_tool → TCP → TCP 版 responder,7 步 SPDM) |
| MCTP 段(bridge + udp_vu_forward_loop + AER) | 中偏高(bridge 角色变化 + AER 并发) | bridge 双端点语义、AER 写锁 | 需真实 MCTP 环境(硬件) |

## 5. 建议路径

**分两阶段**:
1. **阶段 1(当前环境可验证)**:DOE 段改 TCP——transport_doe.c(4B 长度前缀)+ cxl_test_tool `udp_doe_forward_loop`(accept + framing)+ spdm_responder_udp.c(accept)。本地端到端 7 步回归。
2. **阶段 2(需硬件)**:MCTP 段改 TCP——bridge.c(connect + framing,简化 --listen-port)+ cxl_test_tool `udp_vu_forward_loop`(accept + framing + AER 适配)。

**统一 framing 规范**:两段都用 4B u32 长度前缀(载荷字节数),MCTP/DOE 段各走独立 TCP 端口(-u/-U 不变),避免 type 多路复用复杂度;未来如需单端口合并再加 1B type 前缀。

## 6. 收益

1. **消除 UDP 丢包边界**(此前 DOE recv timeout 需 5s 超时容忍)
2. 可靠有序,SPDM 同步请求-响应语义天然匹配
3. 连接状态明确(对端关闭可检测),错误处理更清晰
4. 有 transport_tcp.c 现成读写助手与 spdm-emu framing 先例

## 7. 结论

- **可行**,且 DOE 段可在当前环境完成实现与端到端验证;MCTP 段需硬件。
- **最大风险是 TCP 流式 framing 边界**,建议统一 4B 长度前缀并复用 transport_tcp.c 的 read_bytes/write_bytes 模式。
- 分两阶段推进:先 DOE(可验证),后 MCTP(需硬件)。
