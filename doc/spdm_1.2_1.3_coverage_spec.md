# SPDM 1.2 + 1.3 全命令覆盖实现规格

**日期**: 2026-09-24
**前置**: `doc/test_flow.md`（传输通道）、wiki `protocol_wiki/platform-manage/wiki/specs/spdm-base.md`（DSP0274 命令模型）
**范围**: 自本规格起,推进 SPDM test tool 对 DSP0274 1.2/1.3 命令流的覆盖,达到 1.2/1.3 命令可发起、可验证、可在 hw_smoke 上收敛。

---

## 当前实现一览（开发者快速进度表）

> 对照 wiki `spdm-base.md` + `spdm-family.md` + `spdm-tcp.md` + `spdm-secured-messages.md` 的命令面以及 `spdm-emu` libspdm 4.0.0 暴露的 Requester API（`lib/spdm_requester_lib.h`）,当前 spdm-tool 的覆盖情况如下：

### 命令面（DSP0274）

| 命令 | wiki 标签 | libspdm API（已暴露） | 工具当前状态 | 备注 |
|---|---|---|---|---|
| GET_VERSION | 1.x | `libspdm_get_version` | ✅ 已实现，单步模式 `--cmd version` | 被动接受 responder 报告的 1.0/1.1/1.2/1.3/1.4 |
| GET_CAPABILITIES | 1.x | `libspdm_get_capabilities` | ✅ 已实现，单步 `--cmd capabilities` | **能力位仅 6 位**（CHAL/CERT/ENC/MAC/KEY_EX/HANDSHAKE_IN_CLEAR），1.2/1.3 其余 cap 全 0 |
| NEGOTIATE_ALGORITHMS | 1.x | `libspdm_negotiate_algorithms` | ✅ 已实现，单步 `--cmd algorithms` | **算法硬编码**：base hash=SHA-384, base asym=P-384, DHE=secp384r1, AEAD=AES-256-GCM, req asym=RSASSA-2048 |
| GET_DIGESTS | 1.x | `libspdm_get_digest` | ✅ 已实现，单步 `--cmd digest` | 1.3 的 SupportedSlotMask 字段未在工具层独立处理 |
| GET_CERTIFICATE | 1.x | `libspdm_get_certificate` | ✅ 已实现，单步 `--cmd cert` | 仅一个 `--cert`（DER 根证）作为对端信任锚，不支持多证书模型切换 |
| CHALLENGE | 1.x | `libspdm_challenge` | ✅ 已实现，单步 `--cmd chal` | 仅 NO_MEASUREMENT_SUMMARY_HASH 一种 measurement summary type |
| GET_MEASUREMENTS | 1.x | `libspdm_get_measurement` | ✅ 已实现，单步 `--cmd meas` | 总是走"生成签名 + ALL_MEASUREMENTS" |
| KEY_EXCHANGE / FINISH | 1.x | `libspdm_start_session` | ❌ 未实现（cap 开了但函数未调用） | **名义 KEY_EX_CAP，行为缺失** |
| PSK_EXCHANGE / PSK_FINISH | 1.x | `libspdm_start_session` (use_psk=true) | ❌ 未实现 |
| HEARTBEAT | 1.x | `libspdm_heartbeat` | ❌ 未实现 |
| KEY_UPDATE | 1.x | `libspdm_key_update` | ❌ 未实现 |
| END_SESSION | 1.x | libspdm secured_msg API | ❌ 未实现 |
| GET_CSR | 1.2 | `libspdm_get_csr` | ❌ 未实现 |
| SET_CERTIFICATE | 1.2 | `libspdm_set_certificate` | ❌ 未实现 |
| SUBSCRIBE_EVENT / SEND_EVENT / EVENT_ACK | **1.3** | `libspdm_send_event`，SUBSCRIBE 高阶 API 缺 | ❌ 未实现 |
| REQ_ASYM_SIGN / RESP_ASYM_SIGN | **1.3** | HAL `libspdm_req_asym_sign` | ❌ 未实现 |
| CHUNK_SEND / CHUNK_GET | **1.3** | libspdm 4.0.0 内部透明处理，未确认 | 状态待定 |
| VENDOR_DEFINED_REQUEST | 1.x | `libspdm_vendor_send_request_receive_response` | ❌ 未实现 |
| Encapsulated request flow (in-session DIGESTS/CERT/CHAL/KEY_UPDATE) | 1.2 | `libspdm_encapsulated_request` (internal) | ❌ 未实现 |
| GET_MEASUREMENT_EXTENSION_LOG | **1.4** | `libspdm_get_measurement_extension_log` | ❌ 未实现（范围外但已列出） |
| GET_ENCAPSULATED_REQUEST / DELIVER_ENCAPSULATED_RESPONSE | 1.2 | libspdm 1.x | ❌ 未实现（mut auth） |
| MULTI_KEY_NEG / MULTI_KEY_ONLY | 1.3 | via NEGOTIATE_ALGORITHMS cap | ❌ 未实现 |
| EP_INFO (EndpointInfo) | 1.3 | LIBSPDM_DATA_CAPABILITY_FLAG | ❌ 未实现 |

### 传输层

| 绑定 | wiki 规范 | 工具支持 |
|---|---|---|
| TCP | DSP0287 | ✅ |
| MCTP | DSP0275 | ✅ |
| PCIe DOE | PCI-SIG DOE ECN（无独立 wiki 条目） | ✅ 含 DOE Discovery 自实现 |
| Secured Message over MCTP (DSP0276) | DSP0276 | ❌（应用层 0x06） |
| Storage (DSP0286) | DSP0286 | N/A（不在工具范围） |
| Authorization (DSP0289) | DSP0289 | ⚠️ 仅在算法协商时设 `OpaqueDataFmt1`，未实际承载 OpaqueData |

### CLI 暴露面（截至当前提交）

- 单步命令 6 个：`version / capabilities / algorithms / digest / cert / chal / meas`
- 一次性全流（默认）：`do_connection → do_digest → do_certificate → do_challenge → do_measurement`
- 可选项：`--skip digest|cert|chal|meas`、`--slot <n>`、`--cert <peer root.der>`、`--trans tcp|mctp|doe`
- **未暴露**：`--min-version`、`--algo-*`、`--cap-*`、`--session-id`、`--cmd session`、`--cmd key-update`、`--cmd heartbeat`、`--cmd end-session`、`--cmd get-csr`、`--cmd set-cert`、`--cmd subscribe-event`、`--cmd send-event`、`--cmd vendor-req`、`--in-session`

### 与 wiki 命令面的一致度

- **SPDM 1.2 命令总数（DSP0274 1.2.x normative 约 23 条 Requester 命令 + 14 条 Responder 命令）**: 工具覆盖 **7 条**（仅连接+身份+测量），覆盖率约 **30%**
- **SPDM 1.3 命令总数**：在 1.2 基础上**新增 6 条**（EVENT 三件套、REQ_ASYM_SIGN、CHUNK_SEND、CHUNK_GET、MULTI_KEY_NEG/ONLY、CAP_MAX_SLOT_COUNT 字段），工具覆盖 **0 条**
- **共识面**：算法表完全硬编码，无法达到与 SPEC 1.3 的 RMDA/SecSPDM/ML-DSA 兼容性

---

## Problem Statement

在使用 SPDM test tool 对真实硬件（PCIe DOE + MCTP over TCP/UDP bridge）联调时，开发者只能完成"SPDM 1.2 子集（GET_VERSION/CAP/ALGO/DIGESTS/CERTIFICATE/CHALLENGE/MEASUREMENTS）"的冒烟链路。一旦遇到下列任一真实场景，工具就**毫无输出或直接失败**：

1. **会话（Session）**：设备启用安全会话后，CHALLENGE/MEASUREMENTS 等命令要在 secure channel 里发起，工具无 `KEY_EXCHANGE→FINISH` 实现，`KEY_EX_CAP` 亮了等于空头支票；
2. **生命周期管理**：会话建立后必须配套 HEARTBEAT / KEY_UPDATE / END_SESSION 维持与终结，工具一条都不发；
3. **证书自管**：设备固件升级或自托管 CA 流程要求 GET_CSR / SET_CERTIFICATE；工具不走；
4. **SPDM 1.3 事件订阅与主动通知**：`SUBSCRIBE_EVENT` / `SEND_EVENT` 是 1.3 的核心可观测面，工具完全空白；
5. **算法分歧**：responder 仅支持 SHA-256 基哈希或 EdDSA 时，工具因算法表硬编码 P-384 单套而拒绝协商；
6. **能力协商残缺**：`CHUNK_CAP`/`EVENT_CAP`/`MULTI_KEY_*`/`MAX_SLOT_COUNT` 等 cap 全部为 0，1.3 设备的有效能力位被工具"主动否决"，responder 端按 cap 选择行为后会跳过协议可选命令；
7. **Vendor / Encapsulated / Multi-key 模型**：multi-cert model（Device/Alias/Generic）、vendor 私有命令、encapsulated 流（mutual auth 的 in-session 链路）均无支撑。

以上造成了"工具能完成冒烟，但联调真实 1.2/1.3 responder 时只能用 spdm-emu 替代"的双轨局面。本规格要解决**让 spdm-tool 在不修改 libspdm 4.0.0 的前提下达到 SPDM 1.2 / 1.3 命令流 100% 覆盖**，并能在 hw_smoke 真实硬件上完成回归。

---

## Solution

围绕 `libspdm 4.0.0` 已暴露的 Requester API（`spdm_requester_lib.h`）做 **配置化扩展 + 命令编排**，在 `spdm-tool` 宿主层提供：

1. **算法 + 能力面配置化**：暴露一组 CLI 参数（`--algo-profile <name>` / `--algo-bits <hex>` / `--cap-<name>`），让算法表与 cap 位表从硬编码常量变成可运行期注入；新增 `LIBSPDM_DATA_CAPABILITY_MAX_SLOT_COUNT` 与 1.2/1.3 全部 cap 位，包括 `CHUNK_CAP`/`EVENT_CAP`/`MULTI_KEY_*`/`EP_INFO_CAP`/`ALIAS_CERT_CAP`/`GET_CSR_CAP`/`SET_CERT_CAP`；
2. **会话（Session）子系统**：实现 `KEY_EXCHANGE → FINISH → HEARTBEAT → KEY_UPDATE → END_SESSION` 完整可执行路径，配套 `--session-id <hex>` 与 `--session-state-file <path>` 让外部可持有 session_id；
3. **PSK 会话**：复用 `libspdm_start_session(use_psk=true)`，配套 `--psk <hex|file>` 与 PSK_CAP 点亮；与 KEY_EX 通路**平行**，独立编排；
4. **证书配置流**：`GET_CSR` 与 `SET_CERTIFICATE` 形成互逆配对，输出 PEM/DER 可写盘；
5. **SPDM 1.3 专属命令**：`SUBSCRIBE_EVENT`/`SEND_EVENT`/`EVENT_ACK`（1.3）走 `libspdm_send_event` + `libspdm_vendor_send_request_receive_response`（用于 SUBSCRIBE 高阶 API 缺失场景的兜底）；`REQ_ASYM_SIGN` 走 HAL `reqasymsignlib.h`；Measurement Extension Log 走 `libspdm_get_measurement_extension_log`；
6. **Vendor / Encapsulated / Multi-key**：multi-cert model 由 `--cert-model device|alias|generic` 切换 LIBSPDM_DATA 注入路径；vendor 走 `libspdm_vendor_send_request_receive_response`；in-session 命令走 `*_ex()` 系列（`libspdm_challenge_ex`、`libspdm_get_measurement_ex` 等）；
7. **统一编排**：CLI 单步模式扩展到覆盖全部新命令；现有"全流程"流（默认无 `--cmd`）补齐为"完整端到端"（连接→证书→身份→SESSION→in-session 测量→HEARTBEAT→END_SESSION）。

按 wiki 命令面的归集，将实现分成 P0–P6 七个阶段（详见 **Implementation Decisions**），每个阶段产出 runnable check，并在 `hw_smoke.sh` 上做回归。

---

## User Stories

### 阶段 P0：算法 + 能力配置化

1. As a SPDM 工具开发者，我想通过 `--algo-profile`/`--algo-bits` 选择算法集，以便在 SHA-256 基哈希或非 P-384 算法的 responder 上完成协商。
2. As a SPDM 工具开发者，我想通过 `--min-version 0x12`/`--min-version 0x13` 强制协商底版，以便在版本不兼容的 responder 上快速报错而不是 silently 选 1.0/1.1。
3. As a 1.3 设备调试者，我想打开 `--cap-chunk`、`--cap-event`、`--cap-multi-key-only` 等 cap 位，以便 1.3 responder 同意沿 CHUNK/EVENT/MULTI_KEY 路径执行。
4. As a 多证书场景调试者，我想通过 `--max-slot-count <n>` 声明 1.3 的 MaxSlotCount 字段，以便 8+ 槽位 responder 正确返回 SupportedSlotMask。
5. As a maintainer，我希望"--algo-bits"和"--cap-*"的命名严格对应 SPEC 字段名（P-384 → 0x0004, CHUNK_CAP → 0x08），以便文档无歧义。
6. As a maintainer，我希望所有 cap 默认值都是 0（opt-in），以便工具不会莫名点错 cap 让 responder 误判。

### 阶段 P1：Session（KEY_EXCHANGE/FINISH）

7. As a SPDM 工具开发者，我想用 `--cmd session` 走完 KEY_EX/FINISH 并打印会话 ID、派生密钥摘要，以便我能在 secure channel 里跑后续命令。
8. As a 嵌入式硬件联调者，我想用 `--cmd heartbeat --session-id <id>` 维护会话活跃，以便发现 30s idle 后 responder 是否主动关闭。
9. As a 关键路径测试者，我想用 `--cmd key-update --session-id <id>` 滚动 master key，以便验证 KEY_UPDATE(OPERATE) + KEY_UPDATE(ACKNOWLEDGE) 两侧行为。
10. As a 自动化测试者，我想 `--cmd end-session --session-id <id>` 干净关闭会话，以便 responder 立刻释放槽位而非等到超时。
11. As a 长时间测试脚本作者，我想 `--session-state-file <path>` 把 session_id 序列化到磁盘，以便 `--session-id $(cat file)` 跨进程传递（不需要把工具做成 daemon）。
12. As a 调试者，我希望 `KEY_EX_CAP` 与真实 `do_session` 函数状态一致：要么不点 cap，要么点了 cap 时工具提供完整 session 实现，以免 responder 因 cap 期待落空而拒绝。
13. As a maintainer，我希望 session_id 打印格式固定为 8-byte hex，无混淆字符串，以便 grep/log 检索。

### 阶段 P2：PSK 会话

14. As a SPDM 工具开发者，我想用 `--cmd psk-session --psk <hex|file>` 走 PSK_EXCHANGE/PSK_FINISH，以便在预置对称密钥的 responder 上完成 session handshake。
15. As a 安全审计者，我希望 `--psk-source` 区分"测试用 sample bin"与"生产用 KDF 派生"，以便日志审计。
16. As a 1.3 联调者，我希望 PSK 模式与 KEY_EX 模式复用一个 session_id 表示规范，以便上层编排统一。

### 阶段 P3：证书配置

17. As a CA 测试者，我想用 `--cmd get-csr --csr-out file.pem` 让 responder 生成 CSR（PKCS#10），以便我能给自有 CA 签发证书。
18. As a 固件升级脚本作者，我想用 `--cmd set-cert --cert file.der --slot <n>` 把签好的证书链注入 responder slot，以便启用 PKI 切换场景。
19. As a 1.3 调试者，我希望 GET_CSR 输出包含 SPDM Nonce 扩展字段，以便离线验签通过。
20. As a 测试者，我想 `--cmd csr-rotate`（可选）串行跑 GET_CSR → external CA → SET_CERTIFICATE → GET_DIGESTS 验证新摘要落盘，以便证书链切换可闭环。

### 阶段 P4：SPDM 1.3 独占命令

21. As a 平台可观测性开发者，我想用 `--cmd subscribe-event --event-types <bits>` 注册订阅，以便 responder 主动推送通知。
22. As a 设备遥测作者，我想用 `--cmd send-event --event-type <id> --payload <hex>` 模拟平台上行事件发送，以便测试 responder 的 EVENT 队列行为。
23. As a 联调者，我希望 SUBSCRIBE_EVENT 在 libspdm 高阶 API 缺失时能 fallback 到 `libspdm_vendor_send_request_receive_response`，以保证可以构造 0xF8 op code。
24. As a 双向认证 (Mutual Auth) 测试者，我想用 `--cmd req-asym-sign --slot <req_slot>` 配合 P3 注入的请求方证书走 `REQ_ASYM_SIGN`，以便走完双向身份建立。
25. As a 客户端实现者，我希望 1.3 大消息分片（CHUNK_SEND/GET）对工具透明（在 libspdm 内部处理时无须暴露单独 CLI）。

### 阶段 P5：Auth / Multi-key / Encapsulated / Vendor

26. As a 多证书场景调试者，我想用 `--cert-model device|alias|generic` 切换证书模型，以便 LIBSPDM_DATA 注入时切换 `--cert` 的语义（slot vs cert_model）。
27. As a multi-key 联调者，我想用 `--multi-key-only` 或 `--multi-key-neg` 跟 responder 协商多 key 切换协议。
28. As a 链上别名场景调试者，我想用 `--ep-info <json>` 注入 EndpointInfo（仅 SPDM 1.3 cap 打开时），以满足 ALIAS_CERT 前置条件。
29. As a 嵌入式开发者，我想用 `--in-session <session_id>` 复用现有命令在 secure channel 里发起（例如 `in_session challenge`、`in_session get_digest`），以便节省一次握手。
30. As a 厂商调试者，我想用 `--cmd vendor-req --vreq-id <hex> --vreq-data-file <path>` 走 vendor-defined-request 通路，以便在自有 vendor 协议上调试。
31. As a 设备侧挑战测试者（mut auth），我想加 `--cap-mut-auth` 让 responder 反向挑战 Requester，并在 CHALLENGE 命令里携带请求方证书。

### 阶段 P6：Mutual Auth

32. As a 双向认证开发者，我想加 `--mut-auth --req-slot <n>` 即可让 CHALLENGE 命令在 session 内由 responder 反向挑战 requester 证书。
33. As a 1.3 联调者，我希望 1.3 mut-auth-in-session 路径与 REQ_ASYM_SIGN 自动串联，可在一次握手里完成双向签。
34. As a 自动化测试者，我希望 `--cmd full-flow --mut-auth --mut-auth-fail` 在工具级别把"双向成功"和"响应端拒绝"两种状态都跑一遍。

### 横切：阶段边界与验收

35. As a maintainer，我希望每个 Phase 至少有一个独立 runnable 验证（不依赖其他阶段产物），以便分阶段回滚。
36. As a 设备联调者，我希望新命令在 `hw_smoke.sh` 上能累计进回归用例，期望覆盖率 → 100%。
37. As a 新入职开发者，我希望本规格同时附"当前实现一览"表 + "libspdm 4.0.0 已暴露 API 速查"，以便 5 分钟内开始改代码。

---

## Implementation Decisions

### 决策 1：算法配置通过 `--algo-profile <name>` 或 `--algo-bits <hex>` 二选一

- 提供 `default-sha384`（现状）、`low-sha256`、`high-sha512+eddsa` 三档 profile 作为 preset；
- 同时暴露 `--algo-bits <hex>` 作为 hex raw override，方便 vendor 自定义；
- 算法写入沿用现有 `set_data(LIBSPDM_DATA_*_ALGO)` 路径；
- 命名严格按 SPEC 字段（base_asym、base_hash、dhe、aead、req_asym、meas_hash）映射。

### 决策 2：cap 位作为 opt-in，默认全 0

- 增加 `--cap-flag <bits-hex>` 一键全位设置（debug 用）；
- 提供细粒度开关：`--cap-encrypt`/`--cap-mac`/`--cap-key-ex`/`--cap-psk`/`--cap-handshake-in-clear`/`--cap-cert`/`--cap-chal`/`--cap-meas-sig`/`--cap-meas-fresh`/`--cap-chunk`/`--cap-event`/`--cap-multi-key-only`/`--cap-multi-key-neg`/`--cap-get-csr`/`--cap-set-cert`/`--cap-alias-cert`/`--cap-ep-info`/`--cap-large-msg`/`--cap-mut-auth` 等；
- 默认状态（无 `--cap-*` 时）= 现工具状态（仅 6 位 cap），向后兼容；
- **强约束**：cap 位为 1 时必须能在工具里跑出对应命令路径——即 P1 之前 KEY_EX_CAP/PSK_CAP default 0，P1 完成后需在 capability flag 与 `KEY_EX_RSP` 存在性之间保留一致性检查（避免空头 cap）。

### 决策 3：Session 子系统以 `--session-id <hex>` 为外部接口

- session 建立（`--cmd session` 或 `--cmd psk-session`）时打印 `session_id`（8 byte hex）与 `session_state_file`（可选）；
- 后续命令接收 `--session-id <id>`（或 `--session-state-file <path>`），内部 cache 为 `uint32_t session_id`；
- 不为 session 单开 daemon；session_id 文件化即可让外部编排串联；
- session 关闭：默认接收 `--cmd end-session --session-id <id>`；不动 session file 即关闭会话后退出，可由外部脚本决定如何 retry。

### 决策 4：命令层在 libspdm API 与宿主层间做薄 wrapper

- 每个 do_xxx 直接调 `libspdm_xxx`，不做协议重写；
- 输出格式稳定为 `[SPDM] <cmd> OK, key=value, ...`（沿用现有 printf 风格）；
- 错误处理：libspdm 错误返回 → 打印 0x%x status + 简短 hint，进程退出码 1；
- 不引入新线程/异步机制，沿用 spdm-emu 单线程同步模型。

### 决策 5：测量/Challenge/Cert 的 session-aware 路径

- 在 session 上下文中跑这些命令时统一调用 `*_ex` 系列（`libspdm_challenge_ex`、`libspdm_get_measurement_ex`）；
- 工具层根据 `opts.session_id` 是否为空自动选用 `_ex` 还是不带 session 版本；
- 不为 `--in-session` 单独新增 do_xxx 副本，保留单实现、内部分支。

### 决策 6：SPDM 1.3 命令（SUBSCRIBE/SEND/REQ_ASYM_SIGN）通过 vendor 兜底

- `libspdm_send_event` 用于 SEND_EVENT 高阶路径；
- SUBSCRIBE_EVENT 若 libspdm 4.0.0 没有专门 high-level wrapper，则 fallback 到 `libspdm_vendor_send_request_receive_response`（0xF8/0xFF op code）；
- REQ_ASYM_SIGN 由 vendor 通路走 0xE0/0xE1 op code，签名侧用 HAL `libspdm_req_asym_sign`；
- 这一组新协议层不要求 libspdm 升级到更新版本，因为 libspdm 4.0.0 已含 vendor 请求 API。

### 决策 7：multi-cert model 与 multi-key 在 cap 协商上耦合

- `--cert-model` 影响 `LIBSPDM_DATA_LOCAL_PUBLIC_CERT_CHAIN` 注入路径（slot vs model），与 `--cap-alias-cert`、`--cap-ep-info` 一起打开；
- `--multi-key-only/--multi-key-neg` 是 NEGOTIATE_ALGORITHMS 的 cap 位与协议 bit 字段，不影响证书文件本身。

### 决策 8：Outbound 命令集（CLI）的编排

CLI 单步命令从 6 个扩展为 **≥18 个**，命名严格对应 wiki 命令名小写：

```
version | capabilities | algorithms | digest | cert | chal | meas |
session | heartbeat | key-update | end-session |
psk-session |
get-csr | set-cert |
subscribe-event | send-event | req-asym-sign |
vendor-req |
```

外加 `--in-session <id>` 修饰，作用于 `digest|cert|chal|meas|vendor-req` 命令。

### 决策 9：Mutual Auth 流程作为可选 flag

- `--mut-auth` 加在 `--cmd chal` 后，使 CHALLENGE 流程末尾由 responder 触发 in-session GET_DIGESTS/CHALLENGE 到 requester；
- `--mut-auth-no-resp-cert` 不带 requester 证书的 quick 模式（1.2 老路径）；
- `--cmd req-asym-sign` 可独立走，亦可与 `--cmd full-flow --mut-auth` 自动串成完整 mutual auth 链路。

### 决策 10：依赖 libspdm 暴露清单

实现以 libspdm 4.0.0 的 `libspdm_requester_lib.h` 现有 API 为基线，**不要求升级 libspdm**：

- 已确认暴露：`libspdm_start_session`、`libspdm_start_session_exchange`、`libspdm_start_session_finish`、`libspdm_heartbeat`、`libspdm_key_update`、`libspdm_get_csr`、`libspdm_set_certificate`、`libspdm_send_event`、`libspdm_vendor_send_request_receive_response`、`libspdm_get_measurement_extension_log`、`libspdm_challenge_ex`、`libspdm_get_measurement_ex`、`libspdm_get_measurement_ex2`、`libspdm_get_digest (session_id overload)`；
- HAL 暴露：`libspdm_req_asym_sign`（`reqasymsignlib.h`）、`libspdm_psk_exchange`/`libspdm_psk_finish`（`psklib.h`）；
- Internal：`libspdm_encapsulated_request`（`internal/libspdm_requester_lib.h`）。

仅当以上不够（如 1.3 SUBSCRIBE_EVENT 真没 high-level wrapper）时，依赖 vendor 请求 API 或升级 libspdm。

### 决策 11：版本隔离逻辑

- `--min-version <hex>` 与 `--max-version <hex>` 控制 libspdm 协商期可接受的 responder version；
- 默认 `(0x10, 0x14)`，与当前默认兼容；
- 内部职责：写入 `LOCAL_VERSIONS` 数据前先按 min/max 过滤。

### 决策 12：响应端（UDP Responder）跟随 Requester 升级

- 当前 spdm_responder_udp.c 仅支持 6 命令；
- 跟随 Requester 升级同步支持 KEY_EX/PSK/HEARTBEAT/KEY_UPDATE/CSR/SET_CERT 等；
- libspdm 库本身已支持，响应端主要工作是 cap 位与 cert chain 配置沿用 sample 库。

---

## Testing Decisions

### 原则

- **只测外部行为**：CLI 调用 + 退出码 + stdout 中关键事实；不测 libspdm 内部状态；
- **冒烟 = 真硬件或 libspdm 自有 responder**：每个 Phase 在 `hw_smoke.sh` 末尾累加 step，或用当前自实现的 `spdm_responder_udp.c` 做对拍；
- **bug 优先**：优先测历史上出 bug 的路径（如 1.3 `MEAS_CAP` 宏陷阱、`SupportedSlotMask` 关系、PSK_CAP 字段一致）。

### 各 Phase 的 runnable check（外部行为）

| Phase | Test |
|---|---|
| P0 | `--algo-bits` 与现状硬编码在固定 responder 上的 `NEGOTIATE_ALGORITHMS OK` 输出完全相同；`--min-version 0x13` 与版本过低 responder 报 ERROR；`--cap-mut-auth` 单独打开能被 responder 接受 |
| P0 | 工具 `--cmd version` 在 1.2/1.3/1.4 responder 上分别打印对应版本列表 |
| P1 | `--cmd session` 打印 session_id（固定 8 byte hex）；`--cmd measurement --session-id <id>` 至少跑通一次 in-session GET_MEASUREMENTS；`--cmd heartbeat --session-id <id>` 不报错 |
| P1 | `--cmd key-update --session-id <id>` 后立刻再次 `--cmd heartbeat --session-id <id>` 应通过（证明密钥已滚动） |
| P1 | `--cmd end-session --session-id <id>` 后 `--cmd heartbeat --session-id <id>` 应 ERROR（libspdm 内部已 cleanup） |
| P2 | `--cmd psk-session --psk hex` 配 libspdm sample responder 走通 |
| P3 | 自循环：先 GET_CSR 写 PEM + 用 sample CA 签 + SET_CERTIFICATE 后 GET_DIGESTS 的 slot digest 应变更 |
| P4 | `--cmd send-event` 至少拿到 RESP_NOT_READY（libspdm 4.0.0 返回常见值）并保留 event_type 字段打印 |
| P4 | `--cmd req-asym-sign --slot 0` 在 1.3 responder 上收到 RESP_ASYM_SIGN 或 NOT_READY |
| P5 | `--cert-model alias` 注入方式在 NEGOTIATE_ALGORITHMS 协商后与 `--cert-model device` 输出证书摘要不同 |
| P5 | `--cmd vendor-req --vreq-id 0xF0F0 --vreq-data-file hex` 拿到 RESPONSE 不为 NULL |
| P6 | `--cmd chal --mut-auth --req-slot 0` 配 mut-auth responder 跑通 |

### 单元 / 红绿测试

- 不强行引入 unit-test framework（Catch2 / gtest），跟现有风格一致：每个新 do_xxx 用一次 smoke 验证即可；
- 测试文件放在 `spdm_tool/` 不单独建 `tests/`，保持 spdm-tool 单目录；
- 添加 `scripts/spdm_smoke_1.3.sh` —— 基于 `hw_smoke.sh` 复用，专跑 P0–P4 的命令面，不依赖硬件（用 spdm_responder_udp）。

### Test Prior art（项目历史测试参考）

- `scripts/hw_smoke.sh`：M1/M2/M3-M4 真实硬件冒烟脚本，已做

直接套用 `hw_smoke.sh` 的 step 累加模式：`prepare → setup_transport → req_connect → req_session → req_in_session_meas → heartbeat → end_session`，每步带 "expected result" 字符串断言。

---

## Out of Scope

### 不在本规格范围内的事项

1. **libspdm 升级**：libspdm 4.0.0 已暴露足够 API，本规格不要求升级。但若 1.3 SUBSCRIBE_EVENT 真未暴露高阶 wrapper，可考虑 vendor 兜底（决策 6），或在 Phase P4 阶段评估升级 libspdm；
2. **PCIe 设备驱动**：doe.ko 与 cxl 驱动互斥、Discovery 二轮交换逻辑已稳定，本规格不动；
3. **响应端模拟器（spdm-emu）升级**：本规格以 `spdm_responder_udp.c` 跟版，跟 spdm-emu 主线解耦；
4. **SPDM 1.4 全量覆盖**：仅完成 GET_MEASUREMENT_EXTENSION_LOG 的暴露（libspdm 已有 API），不做 1.4 新增命令（如新 Finished/Measurement 内容）逐条覆盖；
5. **CXL TSP / TDISP / CXL IDE 等绑定层**：本规格聚焦 SPDM Base + TCP/MCTP/DOE 三传输，不扩到 CXL CID 协议；
6. **Vendor 注册表（DSP0293）**：仅实现 vendor 命令通路，不实现 vendor ID/cmd id 注册库；
7. **授权策略（DSP0289 Authori* / opaque data 完整语义）**：仅保持现有 `OpaqueDataFmt1` 占位，不解析 opaque content；
8. **PSK 的 PSDMTEST_KDF / HKDF**：仅做 sample path `libspdm_set_data(LIBSPDM_DATA_PSK_HINT)` 注入，不做 KDF 派生 UI；
9. **Discovery 改动**：现有 `transport_doe.c` 的 DOE Discovery 逻辑不动；
10. **C/C++ 双语言扩展、CMake 替换 Makefile**：不涉及构建系统重构；
11. **Persistent session 跨重启**：session state 仅以文件 / 命令行参数传递，不做工具内 daemon。

---

## Further Notes

### 学习模式下的开发者协作点（来自 CLI 设计会议结论）

工具在配置化与 session-id 表示上**留出用户业务决策点**，对应以下问题需在 P0–P1 阶段定：

1. **算法命名 vs raw hex**：是暴露 `--algo-profile` 字符串名（`low-sha256` 等友好 preset），还是直接暴露 `--algo-bits 0x...`？默认建议走 raw hex，preset 作为 sugar；
2. **session-id 持久化形态**：`--session-state-file <path>` 序列化为 JSON / plain hex / binary？建议 plain hex + newline；
3. **GET_CSR 输出格式**：DER 还是 PEM？PEM（base64 头）更通用，建议 PEM；
4. **SUBSCRIBE_EVENT fallback**：当 libspdm 4.0.0 不提供高阶 API 时，工具是否允许通过 vendor 命令拼装 0xF8？允许；
5. **cap 位默认行为**：当 `--cap-multi-key-only` 打开但 responder cap 没有时，是 warning 还是 hard fail？建议 warning + 跳过对应命令；
6. **mut-auth 与 session-id 关系**：`--mut-auth` 是否必须配合 `--session-id`（1.2 mut-auth 通常 in-session）？建议默认强制配对，单独 open 时 warning 提示。

### 与现有文档的关系

- 不替代 `test_flow.md`（这是传输通道细节与历史验证）；
- 本规格是"在总体架构上**对 SPDM 命令面的纵向补齐**"，与现有文档正交。

### 时间表建议（不含外部协作）

| Phase | 工作量估算（按 pr-feedback 类型的 30/60/90 标准估）|
|---|---|
| P0 算法配置 + cap flags | 1 个 pr，2-3 工作日 |
| P1 KEY_EX/FINISH/HEARTBEAT/KEY_UPDATE/END_SESSION | 2 个 pr，4-5 工作日 |
| P2 PSK | 1 个 pr，2 工作日 |
| P3 CSR/SET_CERT | 1 个 pr，2 工作日 |
| P4 1.3 EVENT/REQ_ASYM_SIGN/EXT_LOG | 2 个 pr，4 工作日 |
| P5 Vendor/Encaps/Multi-key/Alias/Multi-cert | 2 个 pr，4-5 工作日 |
| P6 Mutual Auth 串场 | 1 个 pr，3 工作日 |

合计 ~25 个工作日，全部可串行实现。最后一阶段打包进 `hw_smoke.sh`。

### 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| SPDM 1.3 responder 真机对该工具的 libspdm 版本敏感 | 命令发出去但 RESP 是 ERROR | 升级 libspdm 不在本规格范围，但 Phase P4 时若必须升级需单开 ADR |
| 多证书模型切换在不同 responder 实现里行为不一致 | cert_model 文件读不出来 | `--cert-model` 提供 `device/alias/generic` 三选一，工具默认 `device`，其他模式需用户显式 open |
| in-session 命令加密路径在不同 transport 上的 wire format 差异 | 调试时难定位是哪一层 | 沿用 libspdm transport 抽象，与现有 transport_mctp/transport_doe/transport_tcp 共用 |
| HW smoke 现场没有 1.3 responder | 验证不回 `subscribed` | `spdm_smoke_1.3.sh` 用 spdm_responder_udp 做对拍作为 fallback |
| 工具单进程执行导致 session 必须"重启"另一进程 | 自动化复杂 | `--session-state-file` 序列化 + 外部脚本编排 |
