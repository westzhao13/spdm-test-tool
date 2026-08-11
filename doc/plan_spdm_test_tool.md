# Plan: SPDM Test Tool + cxl_test_tool 双通道改造

**日期**: 2026-08-07
**执行状态(2026-08-10 更新)**: M1 ✅ 完成并验证;M2/M3/M4 ⏸️ 待硬件环境(WSL2 无 PCI/DOE/MCTP);M5 部分完成

**执行摘要**:
- `cxl_sideband/sfx/spdm_tool/`(7 文件)已实现,链 spdm-emu 子模块 libspdm(同版本,避免 TCP binding wire 格式不匹配)
- M1 TCP 冒烟全 PASS:GET_VERSION→CAPABILITIES→ALGORITHMS→DIGESTS→CERTIFICATE→CHALLENGE→MEASUREMENTS(对 spdm_responder_emu)
- **DOE 端到端完整 7 步 PASS**:spdm_tool --trans doe → UDP 数据对象 → 自研 libspdm UDP responder(`spdm_responder_udp.c`,discovery 自处理 + SPDM dispatch):
  - DOE Discovery 3 轮 ✅(0x00→0x01 SPDM→0x02 Secured)
  - GET_VERSION/CAPABILITIES/ALGORITHMS/DIGESTS/CERTIFICATE(1655B)/CHALLENGE/MEASUREMENTS(8 blocks)全 ✅,签名验证全 PASS
  - 期间修复 responder 侧 6 个问题:discovery 需自处理(非 SPDM)、fromlen 初始化、dts/max 对齐、PSK_CAP 宏陷阱(psk_cap=3)、MEAS_CAP 宏陷阱(meas_cap=3)、证书链需 SPDM 格式(length+root_hash,用 sample 库构造)、SupportedSlotMask 需覆盖 ProvisionedSlotMask
- cxl_test_tool 改造完成:`-U <doe_port>` `-C normal|security` `-k`(skip-scan)+ `udp_doe_forward_loop()`
- `switch_mode.sh mctp|doe` 驱动互斥切换脚本就绪
- 冒烟中发现并修复的 wire 协议 bug:platform framing(3×BE u32)、Role-ACK 不应回、TCP `payload_length=size-2`、`dts==max`(无 CHUNK)、doe_cap 需 uint16_t、discovery 晚于 UDP init、recv 长度 vs 缓冲、entry 偏移 8B

**范围**:
1. 实现主机端 SPDM test tool(基于 libspdm,参考 spdm-emu)
2. 完善 cxl_test_tool UDP 流程:双 port,MCTP/DOE 二选一(驱动互斥),DOE 走 `/dev/doe0` ioctl
**前置文档**: `test_flow.md`(方案汇总)、`doe_kernel_driver_research.md`(DOE 驱动研究)

---

## 0. 背景与约束

| 约束 | 说明 |
|---|---|
| 驱动互斥 | cxl 驱动栈(VU/mailbox 路径)与 doe.ko(`/dev/doe0`)不能同时加载 → **一次运行二选一** |
| 传输路径 | MCTP: AF_MCTP → mctp_bridge → UDP → 接收端 → VU vuid=0x11 → 设备固件 |
| | DOE: SPDM tool → UDP → 接收端 → `/dev/doe0` ioctl → 设备 DOE 硬件 |
| 已知事实 | DOE cap: NORMAL=0xd00, SECURITY=0xd80;数据对象类型 SPDM=0x01, Secured=0x02 |
| ⚠️ 待确认 | 用户提到 `/cxl/doe`——现有 doe.ko 创建的是 `/dev/doe0`,本 plan 按 `/dev/doe0` 实现,若实际路径不同仅改宏 |

## 1. 架构总览

```
Host SPDM test tool (新, libspdm requester)          接收端 (cxl_test_tool 改造)
┌───────────────────────────────────┐                ┌──────────────────────────────────────┐
│ main.c (CLI: --trans mctp|doe)    │                │ udp_forward_loop 改造                │
│  ├─ mctp 模式: AF_MCTP socket     │──UDP A────────▶│  socket A (--mctp-port)              │
│  │   (EID 8, type 0x05/0x06)      │  (经 mctp_bridge│    MCTP 帧 → VU vuid=0x11 → 设备固件 │
│  │                                 │   提取转发)    │    ← 响应 → sendto 回 peer           │
│  └─ doe 模式: UDP client          │──UDP B────────▶│  socket B (--doe-port)               │
│      (DOE 数据对象原样收发)        │                │    DOE 数据对象 → /dev/doe0 ioctl    │
│                                    │                │      (DOE_IOCTL_MBOX_CMD) → 设备 DOE │
└───────────────────────────────────┘                │    ← 响应 → sendto 回 peer           │
                                                     └──────────────────────────────────────┘
```

**二选一**:`--trans mctp` 或 `--trans doe` 决定 host 工具用哪个 transport;接收端 `--mctp-port` / `--doe-port` 决定激活哪个 socket;部署脚本按模式切换驱动(见 Phase 3)。

**UDP payload 契约**(跨组件接口定义,详见 test_flow.md §8):
```
MCTP 分支: payload = 原样 MCTP message [4B mctp_hdr(ver/dst/src/flags)][1B msg_type][payload]
           UDP 对端 = host 侧 bridge(经 mctp_bridge 提取转发);响应沿原路返回
DOE 分支:  payload = 原样 DOE 数据对象 [8B DOEHeader(vendor_id/type/len)][payload]
           UDP 对端 = host SPDM tool 直发;响应原样返回
无封装、无长度前缀(UDP datagram 长度自带);接收端按 socket 分流,不做内容解析
```

## 2. 文件规划

```
新工程: cxl_sideband/sfx/spdm_tool/            (host 侧 SPDM test tool)
  ├─ Makefile              # 链接本地 libspdm(/home/xiangzhao/pine/sfx/spdm/libspdm)
  ├─ main.c                # CLI + 握手/度量流程编排
  ├─ spdm_client.c         # libspdm init + transport/device-io 注册(参考 spdm-emu spdm_requester_spdm.c:171-238)
  ├─ spdm_client.h
  ├─ transport_mctp.c      # AF_MCTP socket device I/O (send/recv, EID 8, type 0x05/0x06)
  ├─ transport_doe.c       # UDP device I/O: 发 DOE 数据对象 → UDP B;收响应
  └─ transport_doe.h

改造: pine_vd_scripts/CXL_SCAN_TOOL/cxl_test_tool/cxl_test_tool.c
  ├─ udp_vu_forward_loop() → 支持双 socket + 模式参数
  └─ 新增 doe_forward 分支 (ioctl /dev/doe0 封装)
```

## 3. Phase 分解

### Phase 0: Gate 0 前置验证(必过,不过不进入 P2/P3)

| Gate | 验证内容 | 方法 | 通过标准 |
|---|---|---|---|
| G1 | 设备 Security DOE 通告 SPDM 类型 | `doe -s <bdf> -t 1 -S`(doe_test_app discovery) | 输出含 `[PCI-SIG] cma spdm`(0x01)与 `secured cma spdm`(0x02) |
| G2 | 设备固件 SPDM-over-MCTP 支持 | 查固件代码 vuid=0x11 分派逻辑 | 有 msg_type=0x05/0x06 的 SPDM 处理分支(或确认固件将实现) |
| G3 | DOE 设备节点路径 | `ls /dev/doe0`(doe.ko 加载后) | 存在;确认 `/cxl/doe` 是否有软链计划 |
| G4 | 设备证书 provision 状态 | 固件侧确认 slot0 证书链已安装 + CHALLENGE 试跑一次 | slot 0 已有证书链(CHALLENGE 用例前提,无证书则冻结 CHAL 用例) |

注意: G1/G4 需先切到 doe 驱动(`remove_cxl_driver.sh` → `install_doe_driver.sh`),测完切回 cxl 栈才能做 M2——**Gate 0 阶段就有一次驱动切换,`switch_mode.sh` 需在 Gate 0 前就绪**。

输出:Gate 通过记录写入 test_flow.md;G1/G2 任一 FAIL → 冻结对应模式开发,先与固件/设备侧对齐。

### Phase 1: Host SPDM test tool 骨架

**1.1 libspdm 初始化**(复用现有模板,零发明)
```c
spdm_context = malloc(libspdm_get_context_size());
libspdm_init_context(spdm_context);
libspdm_register_device_io_func(spdm_context, device_send, device_receive);
/* transport 二选一注册 */
libspdm_register_transport_layer_func(spdm_context, max_msg_size,
    LIBSPDM_MCTP_TRANSPORT_HEADER_SIZE, LIBSPDM_MCTP_TRANSPORT_TAIL_SIZE,
    libspdm_transport_mctp_encode_message, libspdm_transport_mctp_decode_message);
/* 或 pci_doe 对应参数 */
libspdm_register_device_buffer_func(...);
libspdm_set_scratch_buffer(...);
```
参考: `libspdm/doc/user_guide.md:74-100`、`spdm-emu/spdm_emu/spdm_requester_emu/spdm_requester_spdm.c:171-238`

**1.2 device I/O 两个实现**
- `transport_mctp.c`: `socket(AF_MCTP, SOCK_DGRAM, 0)` + `sockaddr_mctp`(network=1, EID=8, tag=MCTP_TAG_OWNER);send = libspdm transport 输出帧直接 sendto,recv = recvfrom(参考 `sfx/driver/send.c`)
  - ⚠️ **secured 消息收包问题**: AF_MCTP socket bind 具体 `smctp_type` 后只收该类型——bind 0x05 则会话内 secured(0x06)响应收不到。
    - 先验证内核语义: `smctp_type = 0xff`(MCTP_TYPE_ANY)是否匹配所有类型;若支持则 bind ANY,发送时用 0x05/0x06(由 libspdm transport 输出决定)
    - 若不支持: 双 socket(0x05 + 0x06),recv 时按 smctp_type 分发
    - 验证方法: 用例 6 会话内 GET_MEASUREMENTS 必须回包成功
- `transport_doe.c`: UDP client socket;send = transport 输出的 DOE 数据对象原样 sendto(接收端 --doe-port),recv = recvfrom 等响应(超时映射)

**1.3 流程编排**(参考 spdm-emu `spdm_requester_emu.c` / `--exe_conn` 逻辑)
```
DOE 模式初始化: DOE Discovery(type=0x00, index 遍历) → 校验 Security DOE 通告 0x01/0x02 → 才开始 SPDM
SPDM 流程: GET_VERSION → GET_CAPABILITIES → NEGOTIATE_ALGORITHMS → GET_DIGESTS
→ GET_CERTIFICATE → CHALLENGE → GET_MEASUREMENTS [→ KEY_EXCHANGE 会话]
```
CLI 最小集: `--trans mctp|doe --eid <n> --doe-udp <host:port> --cap security|normal --ver 1.2 --meas` 等,可逐步扩充

**1.4 构建**: Makefile 链接本地 libspdm 静态库 + openssl 后端(先 `git submodule update --init` 拉取 mbedtls/openssl 子模块,或直接用系统 openssl;复用 spdm-emu 构建参数 CRYPTO=openssl)

**验收(M1,二进制判定)**: 对 `spdm_responder_emu --trans TCP` 执行 GET_VERSION,工具 stdout 打印版本列表含 0x10/0x11/0x12 且退出码 0;再做一次 CHALLENGE(证书链校验成功)。transport 层先插 TCP 实现做冒烟,再替换 MCTP/DOE 传输。

### Phase 2: cxl_test_tool UDP 流程改造

**2.1 参数**: 新增 `--mctp-port <n>` / `--doe-port <n>`,二选一(或扩展 `-u`);`--doe-cap normal|security`(默认 security);DOE 模式新增 `--skip-scan`(跳过 BAR scan/mmap,仅起 UDP 循环)

**2.2 双 socket 循环**(改造 `udp_vu_forward_loop`)
```
poll({fd_vu_sock, fd_doe_sock})  # 按激活模式只建对应 socket
MCTP 分支(现有逻辑): recvfrom → MCTP 分片重组 → send_vu_cmd(vuid=0x11) → sendto 回 peer
DOE 分支(新增):      recvfrom → doe_ioctl_exchange() → sendto 回 peer
peer 缓存: 每 socket 独立 last_peer(现 aer_ctx.last_peer 单份,按 socket 拆分)
DOE 模式: AER 线程禁用(secondary mailbox 无意义,doe.ko 独占设备)
DOE 模式: 启动路径支持 --skip-scan,不 mmap BAR(/dev/mem strict 内核下映射可能失败)
```

**2.3 DOE ioctl 封装**(参考 doe_test_app `doe_ioctl` / `doe_discovery.c`)
```c
/* buffer 大小: DOE 数据对象最大 2^18 DW = 1MB;doe_api.h 的 doe_buf[2048]
 * 只是 ioctl 命令编码尺寸(compat 检查),不限制拷贝量;
 * 实际限制是用户态缓冲区大小 → 必须堆分配(1MB 栈数组会溢出) */
uint32_t *buf = malloc((PCI_DOE_MAX_DW_SIZE + 1) * sizeof(uint32_t));
int doe_exchange(int fd, uint32_t cap_offset, const uint8_t *obj, int obj_len,
                 uint8_t *rsp, size_t *rsp_len)
{
    buf[0] = cap_offset;                 /* 0xd00 normal / 0xd80 security */
    memcpy(buf + 1, obj, obj_len);       /* 8B DOEHeader + payload 原样 */
    ioctl(fd, DOE_IOCTL_MBOX_CMD, buf);  /* 一次完整 mailbox 交换 */
    /* ioctl 后响应从 buf[0] 开始(内核 copy_to_user(arg, rsp_buf, len),
     * cap_offset 被响应 DOEHeader 覆盖)——与 doe_discovery.c 语义一致 */
    *rsp_len = ((DOEHeader *)buf)->length * 4;
    memcpy(rsp, buf, *rsp_len);
}
```
- open `/dev/doe0` O_RDWR
- 前置检查: 若 `/dev/doe0` 不存在,提示先 `install_doe_driver.sh`
- **DOE Discovery 透传**: host 工具初始化阶段发 Discovery 数据对象(type=0x00, index 遍历)→ 接收端原样 ioctl → 返回协议表;host 工具校验 Security DOE 含 0x01/0x02 后才开始 SPDM(参考 doe_discovery.c 的 next_index 循环)

**2.4 MCTP 分片重组(新增,blocking)**
- 现象: `mctp_bridge0` MTU=1024,SPDM 大消息(GET_CERTIFICATE 响应等)超 MTU 时内核拆成多个 skb → 多个 UDP 数据报
- 方案(二选一,推荐 A):
  - A. 接收端重组: 按 flags 中 SOM/EOM + tag(msg_tag/tag_owner)缓存分片,凑齐完整 MCTP message 再送 VU;超时(如 1s)清残留
  - B. bridge 侧重组后再转发 UDP(bridge.c 增加重组逻辑,接收端无感)
- 验证: 构造 >1024B 的 MCTP 消息(bridge 侧 send 长 PLDM payload)确认重组正确

**2.5 超时与可靠性映射表**
| 层 | 超时 | 处理 |
|---|---|---|
| libspdm device_receive | 调用方 timeout 参数 | 透传给 UDP recv timeout |
| UDP recvfrom | SO_RCVTIMEO(与 libspdm timeout 取 min) | 超时返回,libspdm 侧 RESPOND_IF_READY 流程 |
| DOE ioctl | 驱动内固定 990ms(`__ZEBU__=0`)/ 10s(`__ZEBU__=1`) | ⚠️ **大响应风险**: CHALLENGE/KEY_EXCHANGE 证书链(几 KB)在 FPGA/仿真下可能 >990ms 导致 ioctl 失败——选项: 重编 doe.ko 加 `-D__ZEBU__=1`(Makefile `VU13P_CFLAGS`),联调前确认编译配置 |
| MCTP 重组缓存 | 1s | 超时丢弃残留分片 |
| UDP 丢包 | 无重传 | 同步一发一收,丢包 → libspdm 超时报错(测试环境接受,联调时勿当 bug 查;必要时接收端加 ACK/重传为后续项) |

**验收**: DOE 分支可用 doe_test_app 自身对拍验证:`doe -s <bdf> -t 1 -S` 的 discovery 输出与 `doe_exchange()` 透传结果**逐字节 diff 一致**;再走 UDP 注入验证回环(含 Discovery 透传)。

### Phase 3: 部署与驱动切换(二选一)

| 模式 | 驱动状态 | 脚本 |
|---|---|---|
| mctp (VU) | cxl 栈加载 | `install_cxl_driver.sh` |
| doe | cxl 栈卸载 + doe.ko 加载 | `remove_cxl_driver.sh` → `install_doe_driver.sh` |

封装 `switch_mode.sh mctp|doe` 一键切换,并打印当前 `/dev/cxl/<mem>` 与 `/dev/doe0` 存在性校验。

**Teardown/清理**: 切换脚本同时负责进程清理(bridge / cxl_test_tool 接收端 / demo 均需 kill,可用 pidfile);模式退出后 `uninstall_doe_driver.sh` 或 cxl 栈卸载不留残余模块。

**验收**: 切换后 `ls /dev/cxl/* /dev/doe0` 符合预期;两种模式各自跑通 Phase 4 用例。

### Phase 4: 联调验证

| # | 用例 | 模式 | 预期 |
|---|---|---|---|
| 1 | GET_VERSION | mctp | tool 打印响应版本 1.0/1.1/1.2 |
| 2 | CHALLENGE | mctp | 证书链校验成功(SLOT0) |
| 3 | GET_MEASUREMENTS | mctp | 度量值 + 签名验证 |
| 4 | GET_VERSION | doe | 同 1,经 /dev/doe0 |
| 5 | CHALLENGE | doe | 同 2,经 /dev/doe0 |
| 6 | KEY_EXCHANGE 会话 | 两模式 | 会话内 GET_MEASUREMENTS(secured 0x02/0x06 类型验证) |

调试: 接收端打印帧(现有 print_mctp_meta/print_hex 风格);可选 --pcap 抓包(spdm-dump 解析)。

## 4. 待确认项(分级)

**Blocking(实现前必须敲定,先过 Gate 0)**:
1. **DOE 设备节点路径**: `/dev/doe0`(doe.ko 实际创建)vs 用户提到的 `/cxl/doe`——确认是否存在别名/软链计划(G3)
2. **设备固件 MCTP 支持**: 固件 VU vuid=0x11 目前只处理 PLDM/CCI,是否已实现 SPDM-over-MCTP 解析?没有则 MCTP 用例需要固件配合(G2)
3. **设备 DOE 支持**: Security DOE(0xd80)是否已通告 SPDM(0x01)/Secured SPDM(0x02)类型?discovery 实测确认(G1)

**可 defer(不阻塞,用默认值起步)**:
4. **libspdm 版本**: 使用本地 `/home/xiangzhao/pine/sfx/spdm/libspdm`,构建参数默认 `CRYPTO=openssl`
5. **端口分配**: 默认 MCTP=2323、DOE=2324,冲突再改
6. **host 工具位置**: 默认 `cxl_sideband/sfx/spdm_tool/`

## 5. 里程碑

| M0 | Gate 0 全过(G1/G2/G3 记录入 test_flow.md) |
| M1 | host tool 骨架 + TCP transport 冒烟:对 spdm_responder_emu(TCP)GET_VERSION 打印 0x10/0x11/0x12 + CHALLENGE 证书链校验成功 |
| M2 | MCTP 全链路:GET_VERSION/CHALLENGE 经 bridge→VU(含分片重组验证) |
| M3 | cxl_test_tool DOE 分支:ioctl 封装与 doe_test_app 对拍 + Discovery 透传 + UDP 回环 |
| M4 | DOE 全链路(Phase 4 用例 4/5/6)+ 模式切换脚本 + teardown |
| M5 | 文档收尾(test_flow.md 更新运行手册) |
