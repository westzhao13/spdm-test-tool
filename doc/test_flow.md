# MCTP Bridge 测试流文档 (Test Flow)

> ## ⚠️ 本文档描述的是**外部仓库**，内容不在本仓库里
>
> `mctp_bridge.ko`、`bridge` 守护进程、UDP 收端进程都来自
> **[whou-sfx/cxl_sideband](https://github.com/whou-sfx/cxl_sideband)**。
> 本仓库（spdm-test-tool）只提供 **host 侧 SPDM 工具**，它通过 UDP 接到那套
> bridge 上——MCTP 路径的环境准备必须先 clone 那个仓库。
>
> 只跑 **DOE 路径**不需要它：本仓库自包含（驱动 + 脚本 + 工具都在这里），
> 见 [doe_transport.md](doe_transport.md)。

**位置**: [cxl_sideband](https://github.com/whou-sfx/cxl_sideband) 的 `sfx/driver/`（外部仓库）
**日期**: 2026-08-07
**目标**: 总结 `mctp_bridge.ko` + `bridge` 守护进程的机制与测试流程——一套"提取内核 MCTP 报文、由 bridge 操作、转发到 UDP"的系统,并给出 SPDM 场景的接入点。

---

## 1. 系统概述

本套件用于在 Host 侧**提取发往指定 MCTP EID 的报文**,交给用户态处理或经 UDP 转发到远端(如 CXL 设备侧模拟器),并将远端响应**注入回内核 MCTP 协议栈**,使上层 AF_MCTP socket 应用无感知。

```
┌────────────── Host ──────────────┐          ┌─── 远端 (如 CXL 设备模拟器) ───┐
│ AF_MCTP app (EID 19)             │          │                                 │
│   socket(AF_MCTP)                │          │   UDP 对端进程                 │
│        │ sendto(EID 8)           │          │                                 │
│        ▼                         │          │                                 │
│ 内核 MCTP 栈 (路由: 8 via mctp_bridge0)     │                                 │
│        ▼                         │  UDP     │                                 │
│ mctp_bridge.ko ──char dev──▶ bridge ───────▶│  (转发 MCTP 帧)                │
│        ▲                         │  ◀───────│                                 │
│        └────── netif_rx ◀── char dev ◀──────┘                                 │
└──────────────────────────────────┘          └─────────────────────────────────┘
```

## 2. 组件与职责

| 文件 | 角色 | 关键点 |
|---|---|---|
| `mctp_bridge.c` | 内核模块 | 虚拟网卡 `mctp_bridge0`(ARPHRD_MCTP, MTU 1024)+ 字符设备 `/dev/mctp_bridge`;`start_xmit` 将 TX skb 入队,`chr_write` 经 `netif_rx()` 注入 RX |
| `bridge.c` | 用户态 bridge 守护进程 | 双向转发:dev→UDP(发远端)、UDP→dev(注入内核);可选本地 PLDM/CCI 应答;支持 `--control` 注入 |
| `pldm.c/h` | PLDM 应答 | GetTID / GetPLDMVersion,构造 MCTP 响应帧写回 |
| `cxl_cci.h` | CXL r3.1 CCI 格式 | `handle_cci_req` 应答 CCI 命令(category/tag/command/command_set/return_code) |
| `demo.c` | 简化 bridge(无 UDP) | 本地应答 PLDM/CCI;stdin 可输入 hex 构造帧发送 |
| `send.c` | AF_MCTP 测试客户端 | EID 19→8;MCTP control(Get/Set EID)或 PLDM 消息 |
| `setup.sh` | 一键装载配置 | insmod + dev up + route/address |
| `frace.sh` `mctp_net_trac.sh` | 内核符号跟踪 | kprobe:mctp_sendmsg / mctp_route_lookup / mctp_route_output |

## 3. 数据流

### 3.1 请求方向(内核 → UDP)

```
send.c / SPDM app: sendto(AF_MCTP, dst EID 8, type 0x05/0x06/0x01/0x00)
  → 内核 mctp_route_lookup: 路由表命中 "8 via mctp_bridge0"
  → mctp_bridge.ko mbridge_start_xmit(): clone skb → tx_to_daemon 队列 → wake_up
  → bridge: poll(/dev/mctp_bridge) → read() 取整帧 MCTP
  → [UDP 模式] send(UDP) 到对端        | [本地模式] handle_pldm_req / handle_cci_req
```

### 3.2 响应方向(UDP → 内核)

```
远端 UDP → bridge: recv(UDP)
  → [no-bridge] 丢弃/打印 | [正常] write(/dev/mctp_bridge)
  → mctp_bridge.ko mbridge_chr_write(): skb 置 ETH_P_MCTP → netif_rx()
  → 内核 MCTP RX → 按 EID/type 投递 → AF_MCTP socket recvfrom() 返回
```

## 4. 关键机制与注意事项

1. **拦截原理 = 路由重定向,非 hook**:`mctp route add 8 via mctp_bridge0`,目标 EID 8 的报文全部被内核 MCTP 路由导向虚拟网卡,`start_xmit` 完成提取。
2. **注入原理**:字符设备 write → `netif_rx()` 走标准 MCTP RX 路径,上层协议栈无感知。
3. **⚠️ dev 必须 up**:否则 `mctp_route_output` 因 outdev link down **静默丢弃**报文(UDP 无提示)。
4. **⚠️ 先加 route 再绑 address**,否则 route 添加失败。
5. **EID 约定**:远端 EID 8(被提取对象)、本地 EID 19(绑在 `mctp_bridge0`,bind/recvfrom 依赖它)。
6. **char dev 队列**:`CHAR_DEV_BUF_MAX = 64KB`,skb 队列 + waitqueue;O_NONBLOCK 时无数据返回 `-EAGAIN`。
7. **UDP 模式参数**:`--udp host:port` 与 `--listen-port N` 必须成对出现;`--no-bridge` 纯 UDP 测试;`--once` 单次。

## 5. 测试流程

### 5.1 构建

```bash
make            # 内核模块 mctp_bridge.ko
make bridge     # gcc bridge.c pldm.c -o bridge
make demo       # gcc demo.c pldm.c -o demo
make send       # gcc send.c -o send
```

### 5.2 装载与配置

```bash
sudo insmod mctp_bridge.ko
sudo ip link set mctp_bridge0 up          # 必须 up!
sudo mctp route add 8 via mctp_bridge0    # 先 route
sudo mctp address add 19 dev mctp_bridge0 # 后 address
sudo mctp link show mctp_bridge0          # 查看
# 或直接: sudo ./setup.sh
```

### 5.3 测试用例

| 用例 | 命令 | 预期 |
|---|---|---|
| 本地 PLDM 应答 | 终端1 `sudo ./demo`;终端2 `sudo ./send` | send 收到 "hello, world!" 回显;demo 打印 PLDM 请求与响应 |
| MCTP control | `sudo ./send --get-eid` / `--set-eid <n> --op <0-3>` | 打印 EID 分配状态/完成码 |
| UDP 双向转发 | 远端起 UDP 对端;本机 `sudo ./bridge --udp host:port --listen-port N` | dev→UDP 帧、UDP→dev 帧均双向打印 |
| UDP 纯转发(无桥) | `./bridge --udp 127.0.0.1:P --listen-port P+1 --no-bridge` | stdin 输入 hex 直接发 UDP |
| Control 注入 | `./bridge --control 02` | 启动即发 MCTP control 帧;`test_bridge_control.sh` 验证字节 `01 08 13 C8 00 80 02` |
| 自动测试 | `sudo ./test_bridge_control.sh` | 输出 "bridge control startup injection test passed" |

### 5.4 调试

```bash
sudo strace -e trace=sendto ./send                 # 用户态 sendto 跟踪
sudo ./frace.sh mctp_sendmsg                        # 内核发送路径
sudo ./frace.sh mctp_route_lookup / mctp_route_output
sudo mctp link show / route show / address show mctp_bridge0
```

## 6. SPDM 场景接入点

Host 侧 SPDM app 复用本套件作为传输通道:

1. **发送**:SPDM app 的 `spdm_device_send_message` 使用 AF_MCTP socket(dst EID 8,`smctp_type` = `0x05` SPDM / `0x06` secured)——即 `send.c` 的现成模式,libspdm mctp transport 输出 "1 字节 message_type + SPDM 消息" 后直接 `sendto`。
2. **接收**:`spdm_device_receive_message` 从同 socket `recvfrom`(EID 8 回包)。
3. **设备侧**:EID 8 的报文被 bridge 提取并 UDP 转发到 CXL 设备侧进程;设备侧应答经 UDP → bridge → `netif_rx()` 注入,host 侧无感。
4. **bridge 扩展**:`handle_req_from_host()` 中的消息类型分派(msg_type == PLDM/CXLCCI)照此模式增加 `0x05/0x06` 透传分支(或干脆仅作透传,不做协议解析)。
5. **分片注意**:内核 MCTP 栈自动处理 64B packet 分片/重组,SPDM app 与 libspdm 均无感知。

## 7. 已知限制

- 仅示例代码级别:无健壮性/fuzz 验证(同 spdm-emu 定位)。
- `demo.c`/`bridge.c` 对 PLDM/CCI 仅实现少量命令,其余返回错误码。
- UDP 转发为明文无认证,仅适合测试环境。

## 8. 方案汇总:双 port 分流 + DOE 驱动(2026-08-07)

### 8.1 拓扑

```
Host SPDM app (libspdm)                        接收端进程 (单进程, 双 UDP socket)
┌────────────────────────────┐                 ┌──────────────────────────────────────┐
│  mctp binding              │                 │  socket A (MCTP port)                 │
│   AF_MCTP EID8             │──bridge──UDP A─▶│    MCTP 帧原样                        │
│                            │                 │    → mailbox VU vuid=0x11 → 设备固件  │
│  pci_doe binding           │                 │    ← 响应 → sendto 回 peer            │
│   device I/O 回调 = UDP    │──UDP B─────────▶│  socket B (DOE port)                 │
│   (DOE 数据对象原样发送)    │                 │    DOE 数据对象原样                    │
└────────────────────────────┘                 │    → /dev/doe0 ioctl(DOE_IOCTL_MBOX_CMD)│
                                               │      → doe.ko → 设备 DOE 硬件         │
                                               │    ← 响应 → sendto 回 peer            │
                                               └──────────────────────────────────────┘
```

**核心原则:按 port 分流,不做内容剥离**——MCTP 帧与 DOE 数据对象原样透传到各自通道,协议识别全部在设备侧。

### 8.2 端口区分(传参)

```
--mctp-port <n>   # socket A: 收 bridge 转发来的 MCTP 帧 → VU 通道
--doe-port  <n>   # socket B: 收 host 直发的 DOE 数据对象 → DOE 驱动
```

两个 socket 独立 poll/recvfrom、独立 peer 缓存。传参确认端口归属,天然解决多协议剥离问题。

### 8.3 DOE 通道:DOE 驱动收发

参考本仓库的驱动实现（`driver/doe/`，ioctl 定义见 `driver/doe/doe_api.h`）：

```c
/* 接收端收到 UDP DOE 数据对象后 */
uint32_t buf[DOE_MAX_DW + 1];
buf[0] = cap_offset;                        /* NORMAL_DOE_CAP_OFF / SECURITY_DOE_CAP_OFF */
memcpy(buf + 1, udp_payload, n);            /* 8B DOEHeader + SPDM 数据,原样 */
ioctl(fd_doe, DOE_IOCTL_MBOX_CMD, buf);     /* doe.ko 完成 mailbox 时序 */
sendto(sock_doe, buf + 1, rsp_len, peer);   /* 响应(DOEHeader+payload)原样回 peer */
```

- doe.ko:probe CXL.mem 设备(PCI class `0x050210`)→ 遍历扩展配置空间找 DOE capability → MSI/MSI-X → `/dev/doe0`
- ioctl `DOE_IOCTL_MBOX_CMD`(`_IO('N', 0xb7)`)内核 `pcie_doe_exchange()` 完成"写 mbox → GO → 轮询 ready → 读 mbox"
- Security DOE 通告数据对象类型:0x01 cma spdm、0x02 secured cma spdm —— 与 libspdm `pcidoe.h` 常量一致
- cap_offset 选择:传参 `--doe-cap normal|security`(SPDM 走 Security DOE)
- host 侧 libspdm `pci_doe` transport 的 device I/O 回调 = UDP client:`sendto`(transport 输出的数据对象)+ `recvfrom`(响应)

### 8.4 MCTP 通道:保持现状

bridge → UDP A 原样 MCTP 帧(4B mctp_hdr + 1B msg_type + payload)→ 接收端 → VU vuid=0x11 → 设备固件。零改动。

### 8.5 实现改动清单

| # | 位置 | 改动 |
|---|---|---|
| 1 | 接收端 | 双 socket 监听 + `--mctp-port/--doe-port` 参数 + 按 socket 分流 |
| 2 | 接收端 DOE 分支 | open `/dev/doe0` + ioctl 封装(参考 doe_test_app `doe_ioctl`) |
| 3 | 接收端 peer 管理 | 每 socket 独立 `last_peer` 缓存(现 AER 单 peer 逻辑按 socket 拆分) |
| 4 | host SPDM app | `pci_doe` binding 的 device I/O 回调改为 UDP client |
| 5 | 前置条件 | 加载 `doe.ko`(与 cxl 驱动互斥);确认 Security DOE 通告 0x01/0x02 类型 |

### 8.6 参考代码

标 ★ 的在本仓库内；其余在 [cxl_sideband](https://github.com/whou-sfx/cxl_sideband) 里。

| 内容 | 位置 |
|---|---|
| DOE 驱动(内核) ★ | `driver/doe/doe_main.c` |
| DOE 驱动 ioctl API ★ | `driver/doe/doe_api.h` |
| host 侧 DOE 收发/Discovery ★ | `spdm_tool/transport_doe.c` |
| 驱动装载/卸载 ★ | `scripts/install_doe_driver.sh` / `scripts/remove_cxl_driver.sh`(与 cxl 驱动互斥) |
| MCTP bridge / 收端进程 | cxl_sideband 的 `sfx/driver/`（外部仓库） |

## 9. SPDM Test Tool 使用手册

> **构建与参数不在这里**，看 [README](../README.md)（构建）和
> [doe_transport.md](doe_transport.md)（DOE 三种跑法 + 报错对照表，
> 含纯软件回环）。`spdm_tool --help` 永远是最新参数表。
> 本节只保留历史验证记录。

### 验证记录(2026-08-10, WSL2 环境)

| 项 | 结果 |
|---|---|
| M1 TCP 冒烟 | ✅ 全通: GET_VERSION→CAPABILITIES→ALGORITHMS→DIGESTS→CERTIFICATE(1655B)→CHALLENGE→MEASUREMENTS(8 blocks),签名验证全 PASS |
| DOE discovery(mock) | ✅ 两轮交换,正确解析 SPDM 0x01/Secured 0x02 |
| **DOE 端到端完整 7 步** | ✅ `spdm_tool --trans doe` → UDP 数据对象 → 自研 `spdm_responder_udp`(libspdm responder 库,discovery 自处理):discovery 3 轮 + GET_VERSION/CAPABILITIES/ALGORITHMS/DIGESTS/CERTIFICATE/CHALLENGE/MEASUREMENTS 全通过,签名验证全 PASS |
| cxl_test_tool 改造 | ✅ 编译通过(`udp_doe_forward_loop` 符号确认),含 `-U/-C/-k` 参数与 skip-scan |
| switch_mode.sh | ✅ 语法检查通过 |

> 自研 UDP responder 验证中修复的 libspdm 集成要点(真实 doe.ko 集成同样适用):
> DOE Discovery(type=0x00)非 SPDM 消息,需应用层自处理;`recvfrom` 前必须初始化 `fromlen`;transport 注册 max 需 = buffer - header - tail;`PSK_CAP`/`MEAS_CAP` 宏为组合位(psk_cap/meas_cap 字段=3 非法,用单一位宏);证书链须 SPDM 格式(length+root_hash,用 `libspdm_read_responder_public_certificate_chain`);SPDM 1.3+ 需设置 `LOCAL_SUPPORTED_SLOT_MASK` 覆盖 provisioned mask。

### 待硬件环境执行

一次性冒烟脚本是 `scripts/hw_smoke.sh`（Gate 0 设备发现 → M2 MCTP → M3/M4 doe.ko + 直连 DOE）：

```bash
sudo scripts/hw_smoke.sh --cert <root.der> [--bdf bb:dd.f] [--eid 8]
```

其中 M4 走的是**直连驱动**路径（`--trans doe --doe-dev /dev/doeN`），不再需要上面的收端进程；本仓库的 DOE 三条路径见 [doe_transport.md](doe_transport.md)。

