# BUG-REV-AUTH-001：MD5 密码哈希在 PDU 中缺少 null 终止符，导致调试输出粘连

| 字段 | 值 |
|------|-----|
| **缺陷编号** | BUG-REV-AUTH-001 |
| **缺陷标题** | MD5 密码哈希在 PDU caData[32-63] 中无 null 终止符，调试输出与 caMsg 粘连 |
| **严重等级** | 一般 |
| **发现者** | Bug 查找师 |
| **发现日期** | 2026-07-28 |
| **当前状态** | ✅ 已关闭 |

## 涉及模块和代码位置

| 位置 | 说明 |
|------|------|
| `Client/client.cpp:301` | `copyExactField(pdu->caData+32, 32, passwordHash)` — 将 32 字节 MD5 哈希填入 32 字节槽位，**不留 null 终止符** |
| `Client/client.cpp:23-27` | `copyExactField()` 实现：`memset(dst, 0, dstSize)` 后 `memcpy(dst, src, qMin(src.size(), dstSize))` — 填满全部 dstSize 字节 |
| `Server/mytcpsocket.cpp:316-317` | `qDebug() << "pdu->caData+32" << pdu->caData+32` — 作为 C 字符串打印，越过 caData[63] 边界读到 caMsg 区域 |
| `Server/msghandler.cpp:98-99,130` | `fixedString(pdu->caData+32, 32)` — 当前逻辑正确限界读取，但缺乏防御性 |
| `Server/protocol.h:69-75` | PDU 结构定义：`caData[64]` → `caMsg[]` 紧邻，无间隔 |

## 触发条件

1. 用户注册或登录，客户端对密码做 MD5 哈希（32 字节 hex 字符串）
2. 客户端使用 `copyExactField` 将 32 字节哈希填入 `caData[32-63]`，填满不留 `\0`
3. 服务端接收 PDU 后：
   - **调试输出**：`qDebug() << pdu->caData+32` 作为 `char*` 打印时，因 `caData[63]` 非 `\0`，继续向后读取直到 `caMsg` 末尾的 `\0`，输出 = MD5哈希 + 验证码
   - **实际逻辑**：当前 `fixedString()` 限界 32 字节保护了功能正确性

## 问题说明

### 当前保护机制

服务端 `rigist()` 和 `login()` 均使用 `fixedString(pdu->caData+32, 32)` 提取密码，该函数通过 `QByteArray(data, maxLen)` 构造器精确限界 32 字节，不依赖 null 终止符，所以 **当前功能不会因缺少 null 终止符而读越界**。

### 潜在风险

如果未来任何代码路径直接以 C 字符串方式读取 `caData+32`（如 `QString::fromUtf8(pdu->caData+32)` 或 `strlen(pdu->caData+32)`），将越界读入 `caMsg`（验证码），导致密码 = 32字节MD5 + 验证码，造成认证失败。

### 对比

`copyFixedField`（用于用户名）使用 `dstSize - 1` 保留一个字节给 `\0`，保证 `caData[31]` 始终为 `\0`。但密码字段使用 `copyExactField`，没有这个保证。

## 影响范围

- **调试阶段**：日志输出产生误导（密码与验证码粘连），增加排错成本
- **未来风险**：任何绕过 `fixedString` 直接读取 `caData+32` 的代码都会触发越界读
- **客户端侧**：相同问题存在于客户端接收 PDU 时的 `qDebug` 中（`Client/client.cpp:238-239`）

## 修正建议

**方案 A（推荐）**：在客户端侧修复根源：

```cpp
// client.cpp — copyExactField 改为也为末尾留 \0
void copyExactField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize - 1));  // 留一个字节给 \0
}
```

或者直接统一使用 `copyFixedField` 代替 `copyExactField`（当前二者仅有 `-1` 的差异）。

**方案 B（防御）**：在服务端 `mytcpsocket.cpp:316-317` 修复调试输出：

```cpp
qDebug() << "pdu->caData+32" << QByteArray(pdu->caData+32, 32);
```

## 建议验证方式

1. 注册/登录后检查服务端日志，确认 `caData+32` 不再与 `caMsg` 粘连
2. 确认登录/注册功能正常工作（密码哈希正确匹配数据库记录）
3. 使用 Wireshark 抓包确认 PDU 数据完整

---

> **记录创建日期**：2026-07-28
> **记录创建者**：Bug 查找师
