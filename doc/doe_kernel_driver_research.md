# DOE 内核驱动研究报告

**日期**: 2026-08-07
**范围**: 官方 Linux 内核 DOE 实现 vs 当前 `doe.ko`(Avery Design Systems out-of-tree 测试驱动)
**来源**: websearch 检索(Linux 内核源码 torvalds/linux、内核邮件列表 lkml、openSUSE/kernel 提交记录)+ 本地 `pine_vd_scripts/cxl_tools/doe_test_app/` 源码分析

---

## 1. 结论摘要

- **Linux 官方不提供用户态直接访问 DOE 的接口**:内核仅有 `drivers/pci/doe.c`(PCI core 库,无 /dev 节点)+ `drivers/cxl/doe.c`(内核自用读 CDAT)。
- **历史上明确的用户态 DOE ioctl 方案被内核拒绝**(`DONOTMERGE`,从未合入)。
- **当前 `doe.ko` 是非官方 out-of-tree 测试驱动**(Avery Design Systems 验证工具),它实现了用户态 ioctl 访问 DOE——**仅在测试场景下可用**,前提是不与 cxl 驱动栈共存。

## 2. 官方内核 DOE 实现

### 2.1 `drivers/pci/doe.c` — PCI core 基础设施(库,非驱动)

- 作者:Jonathan Cameron (Huawei, 2021) + Ira Weiny (Intel, 2022),2022 年合入主线
- 依据:PCIe r6.0 sec 6.30(DOE)
- 位置:`drivers/pci/doe.c` + `include/linux/pci-doe.h`

核心 API:

| API | 作用 |
|---|---|
| `pci_doe_init()` | 遍历 PCIe 扩展配置空间,为每个 DOE capability 创建 mailbox |
| `pci_doe_create_mb()` | 创建单个 mailbox 状态机(ordered workqueue + flags) |
| `pci_doe_submit_task()` | 异步提交一个 request/response 任务 |
| `pci_doe()` | 同步交换 API(等待 completion) |
| `pci_find_doe_mailbox()` | 按 (vendor_id, type) 查找支持的 mailbox |
| `pci_doe_supports_feat()` | 查询 mailbox 是否支持某协议 |

mailbox 时序实现(`pci_doe_send_req` / `pci_doe_recv_resp`):
- 写 DOE Header(VID + type)→ 写 payload DW → Control 置 GO → 轮询 Status(Data Object Ready)→ 读 Read Data Mailbox
- 内置超时 `PCI_DOE_TIMEOUT`,Busy bit 轮询,Error 检查

**关键性质**:
- `EXPORT_SYMBOL_GPL`,只给内核驱动调用
- **无字符设备、无 ioctl、无 sysfs 用户态接口**
- payload 按字节原样传递(DW 对齐由 API 处理)

### 2.2 `drivers/cxl/doe.c` — CXL 侧 auxiliary driver(内核自用)

- cxl_pci probe 时调用 `pci_doe_create_doe_devices()` 为每个 DOE cap 创建 auxiliary device
- cxl_doe auxiliary driver 驱动之,`pci_doe_create_mb()` 创建 mailbox
- **用途:内核自己读 CDAT**(`CXL_DOE_PROTOCOL_TABLE_ACCESS`)
- 同样是内核态,不暴露用户态

### 2.3 用户态 DOE 接口的历史:被明确拒绝

内核邮件列表存在提交 "PCI/DOE: Add per DOE chrdev for ioctl based access"(commit `22a6e8b`):

- 状态:**`DONOTMERGE`(禁止合入)**
- 被拒原因(commit 说明原文):
  > It is not safe to access DOE mailboxes directly from userspace at the same time as the kernel may be accessing them. ... There is no sanity checking of the messages sent so this is not an appropriate interface to expose to userspace.
  > (用户态与内核同时访问 DOE mailbox 不安全;消息无校验,不适合暴露给用户态)

- 演进结果:auxiliary DOE 驱动最终落在 CXL 层(`cxl_doe`),通用用户态接口搁置;官方建议"需要时实现协议特定接口"(CDAT 为第一个例子)

## 3. 当前 `doe.ko` 分析

### 3.1 来源与性质

- 出自 **Avery Design Systems**(QEMU 的 DOE 设备模拟 `hw/pci/pcie_doe.c` 也是该公司实现)
- 是 `doe_test_app` 验证工具的配套 **out-of-tree 内核驱动**(GPL v2)
- 设计思路与内核被拒的 "chrdev for ioctl" patch 几乎一致——即"非官方路线"的实现
- 代码注释自认测试用途(如 `doe_main.c` 中被 `#if 0` 的 `do_doe_discovery`)

### 3.2 架构

```
doe.ko (doe_main.c)
  ├─ pci_driver: 匹配 CXL.mem 设备 (class 0x0502)
  ├─ probe: pcim_enable_device + 遍历扩展配置空间找 DOE cap
  │    → 每个 DOE cap 一个 doe_node (pcie_doe_register_irq 注册 MSI/MSI-X)
  ├─ 每 PCI 设备一个 /dev/doe%d 字符设备
  └─ ioctl:
      DOE_IOCTL_MBOX_CMD (_IOWR('N', 0xb7, struct doe_buf)):
        buf[0]   = cap_offset (NORMAL_DOE_CAP_OFF / SECURITY_DOE_CAP_OFF)
        buf[1..] = DOE 数据对象 (8B DOEHeader + payload)
        内核 pcie_doe_exchange() 完成一次完整 mailbox 交换
        响应写回用户 buf
      DOE_IOCTL_INIT_MSIX / DOE_IOCTL_INIT_MSI: 中断初始化
```

### 3.3 与官方实现的差异

| 维度 | 官方 (`drivers/pci/doe.c`) | 当前 `doe.ko` |
|---|---|---|
| 形态 | PCI core 库,内核 API | pci_driver + 字符设备 |
| 用户态访问 | ❌ 无 | ✅ ioctl (DOE_IOCTL_MBOX_CMD) |
| 并发仲裁 | mailbox 由内核统一管理 | 无仲裁,用户态直访 |
| 消息校验 | 有(vid/type 匹配检查) | 基本无(透传) |
| 身份 | 主线内核 | out-of-tree,测试用 |
| 中断 | workqueue + completion | MSI/MSI-X + 轮询 |

### 3.4 与 cxl 驱动栈的互斥(双重约束)

1. **驱动绑定槽位**:`doe.ko` 与 cxl 栈(cxl_pci/cxl_mem)都是标准 pci_driver,同一 PCI 设备同一时刻只能绑定一个——脚本(`install_doe_driver.sh` / `remove_cxl_driver.sh`)即互斥切换工具。
2. **DOE mailbox 仲裁**:即便绑定绕过,cxl 驱动运行时也操作同一 DOE cap(读 CDAT),用户态 ioctl 与内核无仲裁并发——这正是官方拒绝用户态接口的核心理由。

### 3.5 使用约束

```
DOE 用户态直访 (doe.ko) 有效的前提:
  ✅ 卸载 cxl 驱动栈: remove_cxl_driver.sh → install_doe_driver.sh
  ✅ 仅测试/验证场景
  ❌ 不得与 cxl 栈同时加载 (mailbox 无仲裁 + 绑定冲突)
  ❌ 生产环境 (官方无此接口,安全不达标)
```

## 4. 对本项目方案的影响

| 方案点 | 结论 |
|---|---|
| DOE 通道(双 port 方案) | 可用 `doe.ko` + `/dev/doe0` ioctl,测试环境成立 |
| 与 MCTP(VU)通道共存 | 同一时刻二选一(部署期切换),不能同时激活 |
| 生产/正式路线 | 自写内核驱动用 `pci_doe()` API(与内核 DOE 用户仲裁);或等官方协议特定接口 |
| 已知风险 | 用户态无校验、无仲裁;ioctl 超时固定(990ms/10s);`doe_buf` 大小 2048B 需确认长消息限制 |

## 5. 参考链接

- Linux 内核: https://github.com/torvalds/linux/blob/master/drivers/pci/doe.c
- 头文件: https://github.com/torvalds/linux/blob/master/include/linux/pci-doe.h
- 被拒 chrdev patch: https://github.com/0day-ci/linux/commit/22a6e8bee6206990d309c9c61941f67868c3a12c
- aux driver 讨论: https://lists.openwall.net/linux-kernel/2022/03/15/1340
- cxl/doe aux 驱动: https://lkml.indiana.edu/2204.1/10191.html
- QEMU DOE 实现(Avery): https://github.com/tillitis/qemu/blob/tk1/hw/pci/pcie_doe.c
- 本地驱动源码: `pine_vd_scripts/cxl_tools/doe_test_app/driver/doe_main.c`、`driver/doe_api.h`
