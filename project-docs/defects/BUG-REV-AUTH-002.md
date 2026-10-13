# BUG-REV-AUTH-002：验证码逻辑未纯基于过期时间 —— 覆盖旧码 + isEmpty 误判

| 字段 | 值 |
|------|-----|
| **缺陷编号** | BUG-REV-AUTH-002 |
| **缺陷标题** | `validateCaptcha` 非纯时间判断：重复请求覆盖未过期验证码 + isEmpty 误返回"过期" |
| **严重等级** | 严重 |
| **发现者** | Bug 查找师 |
| **发现日期** | 2026-07-28 |
| **当前状态** | 已确认，待修复 |

## 涉及模块和代码位置

| 位置 | 说明 |
|------|------|
| `Server/msghandler.cpp:45-73` | `validateCaptcha()` 验证码校验逻辑 |
| `Server/msghandler.cpp:51-54` | `isEmpty()` 检查 → 返回 -3（"过期"）—— **问题点 1** |
| `Server/msghandler.cpp:75-84` | `captcha()` 请求时无条件覆盖 `m_captcha` —— **问题点 2** |
| `Server/msghandler.h:47` | `QPair<QString, QDateTime> m_captcha` — 只存储单组验证码 |

## 触发条件（场景还原）

### 场景 A：用户快速连续点击"获取验证码"
1. 用户第一次点击 → 服务器生成验证码 **ABC123**，过期时间 T+60s
2. 用户网络延迟/手抖，2秒后再次点击 → 服务器生成验证码 **XYZ789**，**覆盖** `m_captcha`
3. 用户看到弹窗显示验证码 **ABC123**（第一个响应来得晚）
4. 用户输入 **ABC123** → `validateCaptcha` 比较 `m_captcha.first`（=XYZ789） → **不匹配**，返回 -2（"验证码错误"）
5. 用户确认没输错，但反复失败

### 场景 B：`isEmpty()` 误判
1. Socket 断开重连 → 新 `MyTcpSocket` 创建新的 `MsgHandler` → `m_captcha` 为空
2. 用户发送登录请求（带之前获取的验证码，尚未过期）
3. `validateCaptcha` 走到 `if (m_captcha.first.isEmpty())` → 返回 -3
4. 客户端显示 "验证码已过期，请重新获取" —— **实际上验证码并未过期，只是验证码上下文丢失**

## 问题说明

### 问题点 1：覆盖旧验证码

```cpp
// msghandler.cpp:77-78
PDU *MsgHandler::captcha()
{
    m_captcha.first = generateCaptchaCode(6);   // 无条件覆盖！
    m_captcha.second = QDateTime::currentDateTime().addSecs(60);
    ...
}
```

`captcha()` 在每次被调用时**无条件覆盖** `m_captcha`。这意味着：
- 旧验证码（即使还剩 58 秒才过期）被立即废弃
- 违反了用户需求："只要没有过过期时间就不会过期"

### 问题点 2：isEmpty 误判为"过期"

```cpp
// msghandler.cpp:51-54
if (m_captcha.first.isEmpty()) {
    qDebug() << "验证码校验失败: 内存中验证码为空 (可能已被清除)";
    return -3;  // ← 与"已过期"返回相同错误码
}
```

`isEmpty()` 可能由多种原因触发，不一定是过期：
- 验证码从未被请求过
- Socket 重连导致 MsgHandler 重建
- 验证码被其他逻辑意外清除

这些情况下返回 -3 是**语义错误**：客户端收到 -3 会显示"验证码已过期"，但真实原因可能是"验证码不存在"或"上下文丢失"。

### 核心问题：逻辑不够"纯"

用户要求："验证码要完全依照过期时间来做，你其他的逻辑都不要管，只要到过期时间过期就可以了"

当前 `validateCaptcha` 的判断链是：
```
isEmpty? → return -3
expired? → return -3, clear
match?   → return 0, clear
else     → return -2
```

纯时间逻辑应该是：
```
expired? → return -3
match?   → return 0
else     → return -2
```

## 影响范围

- **用户体验**：验证码在有效期内却无法使用，需要反复获取
- **登录/注册流程**：两个入口都依赖 `validateCaptcha`（`rigist()` 和 `login()`）
- **调试**：误判导致日志信息不准确，增加排错难度

## 修正建议

### 立即修复：移除 `isEmpty()` 检查

```cpp
int MsgHandler::validateCaptcha(const char* caMsg, uint uiMsgLen)
{
    if (uiMsgLen < 4) return -2;
    QString userInput = QString::fromUtf8(caMsg).trimmed();
    
    // 【删除】isEmpty 检查 — 不是"过期"的原因
    
    // 检查是否过期（保留）
    qint64 remaining = QDateTime::currentDateTime().secsTo(m_captcha.second);
    if (remaining < 0) {
        qDebug() << "验证码校验失败: 已过期" << abs(remaining) << "秒";
        m_captcha.first.clear();
        return -3;
    }
    
    // 【新增】如果验证码为空但未过期 → 说明上下文丢失，单独处理
    if (m_captcha.first.isEmpty()) {
        qDebug() << "验证码校验失败: 服务端无有效验证码上下文（可能连接已重建）";
        return -4;  // 新错误码，客户端显示"请重新获取验证码"
    }
    
    bool match = (userInput == m_captcha.first);
    ...
}
```

### 中期修复：支持多验证码共存（解决覆盖问题）

将 `QPair<QString, QDateTime> m_captcha` 改为 `QList<QPair<QString, QDateTime>>` 或 `QMap<QString, QDateTime>`，允许同时存在多个未过期的验证码。

```cpp
// 方案：使用 QMap<验证码, 过期时间> 存储
QMap<QString, QDateTime> m_captchas;

PDU *MsgHandler::captcha()
{
    QString code = generateCaptchaCode(6);
    QDateTime expiry = QDateTime::currentDateTime().addSecs(60);
    m_captchas.insert(code, expiry);  // 追加，不覆盖旧的
    // 可选：清理已过期的旧验证码
    ...
}

int MsgHandler::validateCaptcha(const char* caMsg, uint uiMsgLen)
{
    ...
    QString userInput = QString::fromUtf8(caMsg).trimmed();
    
    auto it = m_captchas.find(userInput);
    if (it == m_captchas.end()) {
        return -2;  // 不匹配
    }
    
    if (QDateTime::currentDateTime() > it.value()) {
        m_captchas.erase(it);
        return -3;  // 已过期
    }
    
    m_captchas.erase(it);  // 成功后移除
    return 0;
}
```

### 客户端适配

在 `Client/msghandler.cpp:29-31` 新增对 -4 的处理：
```cpp
if(ret == -4){
    QMessageBox::information(&Client::getinstance(), "提示", "验证码上下文已丢失，请重新获取验证码");
    return;
}
```

## 建议验证方式

1. **场景 A 验证**：连续点击"获取验证码"两次，使用第一次获取的验证码进行登录 → 应成功
2. **场景 B 验证**：获取验证码后断开并重连 socket，使用原验证码登录 → 当前预期失败，修复后应给出明确提示
3. **过期验证**：等待超过 60 秒后使用验证码 → 应返回"已过期"
4. **正常流程**：单次获取 → 输入正确 → 验证通过
5. **错误输入**：输入错误验证码 → 应返回 -2，允许重试

---

> **记录创建日期**：2026-07-28
> **记录创建者**：Bug 查找师
