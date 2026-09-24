# DOE 传输：三种跑法

**日期**: 2026-09-24
**范围**: `spdm_tool --trans doe` 的三条路径——纯软件回环（无硬件）、直连 DOE 驱动（真机）、经 UDP 中继（真机 + 外部收端）。判据是**要不要硬件**，不是协议差异。

---

## 0. 选哪条

| 路径 | 需要 | 验证到什么 | 用在哪 |
|---|---|---|---|
| **A. 纯软件回环** | 无（只要本仓库） | SPDM 协议本身 + DOE 数据对象封装 + 全部密码学 | 改工具、改算法、跑回归；CI |
| **B. 直连驱动** `--doe-dev` | `doe.ko` + 设备 + 内核头文件 | 上面全部 **+** DOE mailbox/ioctl 时序 + 设备固件实现 | 真机联调（主路径） |
| **C. UDP 中继** `--doe-udp` | 外部 [cxl_sideband](https://github.com/whou-sfx/cxl_sideband) 的收端进程 + 设备 | B 的内容，但 mailbox 侧由收端进程代做 | 兼容历史流程；MCTP 同栈复用 |

**本仓库自包含 A 和 B**（驱动源码在 `driver/doe/`，装载脚本在 `scripts/`）。C 需要外部仓库。

---

## A. 纯软件回环（无需硬件）

不需要 `doe.ko`、不需要 PCI 设备、不需要 cxl_sideband。工具用 `--doe-udp` 把 DOE 数据对象发到一个 UDP 端点，端点换成**本仓库自带的 responder** 即可——它用 libspdm 的 responder 库真跑一套 SPDM，所以协议与密码学部分是**真的在验证**，只是 DOE mailbox 那一层被 UDP 替换掉了。

```
┌── spdm_tool (requester) ──┐        ┌── responder (本仓库自带) ──┐
│ libspdm pci_doe transport │        │ libspdm responder 库       │
│  → DOE 数据对象           │──UDP──▶│  → SPDM 应答               │
│ does_exchange() = send/recv│◀──────│ 自带 discovery 应答        │
└───────────────────────────┘  2326  └────────────────────────────┘
                  (内核/驱动/设备 均不参与)
```

### 步骤

```bash
REPO=<本仓库路径>
cd $REPO/spdm_tool && make          # 同时产出 spdm_tool 和 responder

# 1) responder 的工作目录必须能找到 ecp384/ 证书样例（相对路径，见下方"坑"）
mkdir -p /tmp/spdm-sim && cd /tmp/spdm-sim
ln -s $REPO/lib/libspdm/unit_test/sample_key/ecp384 ecp384

# 2) 起 responder（前台）
$REPO/spdm_tool/responder
#   → [RSP] UDP DOE responder ready on port 2326

# 3) 另开一个终端跑工具
$REPO/spdm_tool/spdm_tool --trans doe --doe-udp 127.0.0.1:2326 \
    --cert $REPO/lib/libspdm/unit_test/sample_key/ecp384/ca.cert.der
```

### 期望输出

```
[DOE] UDP client -> 127.0.0.1:2326
[DOE] discovery...
[DOE] discovery[0]: vid=0x0001 type=0x01 next=0x01
[DOE] discovery[1]: vid=0x0001 type=0x02 next=0x00
[DOE] discovery OK: spdm=1 secured_spdm=1
[SPDM] connection established (GET_VERSION/CAPABILITIES/ALGORITHMS OK)
[SPDM] GET_DIGESTS OK, slot_mask=0x01
[SPDM] GET_CERTIFICATE slot0 OK, chain size=1655
[SPDM] CHALLENGE slot0 OK, slot_mask=0x01
!!! verify_measurement_signature - PASS !!!
[SPDM] GET_MEASUREMENTS OK, blocks=8 record_len=528 content_changed=32
```
退出码 0，`verify_measurement_signature - PASS` 是关键判据（签名是真的验了）。

`--cert` 可省略：省略时跳过链信任锚校验，但完整性/哈希/签名校验照做。

### 这条路径验证不到什么

- 真实 DOE mailbox 的时序（GO bit / ready 轮询 / 超时）
- `doe.ko` 的 ioctl 行为与 cap offset 校验
- 设备固件自己的 SPDM 实现

→ 这些只有 B 能覆盖。反过来，B 出问题但 A 通过，基本可以断定问题在驱动/设备侧，不在 SPDM 协议层——这是 A 最实用的地方（二分定位）。

### 已知坑

`responder` 通过 libspdm 的 `device_secret_lib_sample` 以**相对路径**读 `ecp384/bundle_responder.certchain.der` 和 `ecp384/end_responder.key`，所以它的**当前工作目录**必须能找到 `ecp384/`。否则启动即报：

```
read_input_file: cannot open ecp384/bundle_responder.certchain.der
[RSP] read responder cert chain failed (run from spdm-emu/build/bin)
```
后一句提示是过时的（那是 spdm-emu 的路径）——照上面第 1 步建个带 `ecp384/` 软链的目录即可。证书样例来自 libspdm submodule，所以跑之前需要 `git submodule update --init`。

---

## B. 直连 DOE 驱动（真机主路径）

工具自己 `ioctl` 到 `/dev/doeN`，中间没有任何进程。

```
┌── spdm_tool (host userspace) ─────────────────────────────┐
│ libspdm pci_doe transport  →  DOE 数据对象                │
│ doe_exchange():                                           │
│   g_dev_buf = [DW0 = cap_off][DW1.. = 8B DOEHeader+payload]│
│   ioctl(fd, DOE_IOCTL_MBOX_CMD, g_dev_buf)                │
└───────────────────────────┬───────────────────────────────┘
                            │  /dev/doeN
                            ▼
┌── doe.ko (driver/doe/, kernel) ───────────────────────────┐
│ 校验 cap_offset（只认编译进去的 0xd00 / 0xd80）            │
│ 找匹配的 DOE capability 节点 → pcie_doe_exchange():       │
│   写 mailbox → 置 GO → 轮询 ready → 读 mailbox            │
└───────────────────────────┬───────────────────────────────┘
                            │  PCIe 扩展配置空间
                            ▼
┌── 设备 DOE mailbox ──▶ 设备固件 (SPDM responder) ─────────┐
└───────────────────────────────────────────────────────────┘

响应沿原路返回；doe.ko 把响应从 DW0 开始拷回用户缓冲区
（覆盖掉 DW0 里的 cap_off 选择子）。
```

### 步骤

```bash
# 0) 前提：内核头文件在 /lib/modules/$(uname -r)/build；设备存在

# 1) 建模块（install 脚本也会按需自动做这一步）
make -C driver/doe

# 2) 切模式：卸载 cxl 栈 → 装 doe.ko（两种模式互斥，见 scripts/switch_mode.sh）
sudo scripts/switch_mode.sh doe
#   → /dev/doe0 出现

# 3) 跑工具（--doe-dev 默认就是 /dev/doe0）
spdm_tool/spdm_tool --trans doe --cert <root-cert.der>
```

DOE Discovery（数据对象类型 `0x00`）在这一层同样先跑：它是 PCIe 层的交换，不是 SPDM；工具在起 SPDM 之前会遍历 index 并确认设备通告了 SPDM（`0x01`）/Secured SPDM（`0x02`）。

### 实例选择

设备上可能有两个 DOE 实例，capability offset 不同：

| 实例 | cap offset | 选择方式 |
|---|---|---|
| NORMAL | `0xd00` | `--doe-cap normal`（或默认） |
| SECURITY | `0xd80` | `--doe-cap security` |

SPDM 通常挂在 Security 实例上。`--doe-cap-offset 0xNNN` 可给原始 offset，但**驱动只接受它编译进去的那两个值**（`driver/doe/Makefile` 的 `-DNORMAL_DOE_CAP_OFF` / `-DSECURITY_DOE_CAP_OFF`），别的值会被拒。

### 回切

```bash
sudo scripts/switch_mode.sh mctp   # 卸 doe.ko，重新装 cxl 栈
```
卸载脚本还会把设备重新 bind 回 `cxl_pci`（否则 mem/dax 设备不会出现）。

### 报错对照

工具把 ioctl 的 errno 翻译成了具体成因：

| 输出 | 含义 | 怎么办 |
|---|---|---|
| `no DOE capability at 0xNNN on this device` | 该实例在这台设备上不存在（驱动找不到匹配的 capability 节点），或两种 ioctl 编码都不认 | 换另一个实例 `--doe-cap normal\|security` |
| `this driver build accepts only 0xd00 (normal) / 0xd80 (security)` | `--doe-cap-offset` 给的值驱动不接受 | 改用 `--doe-cap`；要别的值得改驱动宏重编 |
| `device exchange timed out` | mailbox 交换超时（驱动内 ~1s） | 查固件/中断状态 |
| `device returned an empty response` | 设备回了零长度 | 设备侧问题 |
| `udp recv timeout (5000 ms)` | 走的是 C（`--doe-udp`）而非直连 | 见下；这条是 UDP 端点没回应 |

---

## C. UDP 中继（真机 + 外部收端）

工具把 DOE 数据对象经 UDP 发给一个**收端进程**，收端再 `ioctl` 到 `/dev/doe0`。

```
spdm_tool ──UDP──▶ 收端进程 (cxl_sideband) ──ioctl──▶ /dev/doe0 ──▶ 设备
```

收端进程（`cxl_test_tool -U <port> -k`）**不在本仓库**，来自外部仓库
[cxl_sideband](https://github.com/whou-sfx/cxl_sideband)。这条路径保留用于兼容历史流程，
以及和 MCTP 路径共用同一套收端基础设施的场景——新联调直接用 B 更短。

```bash
cxl_test_tool -s <bdf> -U 2324 -k                    # 收端（外部仓库）
spdm_tool --trans doe --doe-udp 127.0.0.1:2324 --cert <root-cert.der>
```

注意：中继模式下 `--doe-cap` / `--doe-cap-offset` **是无效的**（实例选择由收端决定），
工具会直接报错而不是静默忽略。

---

## 附：一条命令做真机冒烟

`scripts/hw_smoke.sh` 把 Gate 0（设备发现）→ M2（MCTP）→ M3/M4（doe.ko + 直连 DOE）
串成一次性冒烟，M4 就是上面的 B 路径：

```bash
sudo scripts/hw_smoke.sh --cert <root.der> [--bdf bb:dd.f] [--eid 8]
```
