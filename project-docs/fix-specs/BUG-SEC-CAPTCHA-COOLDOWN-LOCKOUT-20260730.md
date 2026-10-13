# BUG-SEC-CAPTCHA-COOLDOWN-LOCKOUT-20260730

## 1. 漏洞根因

- 服务端 `MsgHandler::m_captchas` 是 `QMap<验证码, 过期时间>`，键为验证码字符串本身；`captcha()` 用 `insert()` **追加**，不覆盖旧码 → 同一连接内可同时存在多个有效码，校验时命中池里任意一个即通过（多码并存）。
- 客户端 60s 冷却只是 `QTimer` + 禁用按钮，**服务端零管控**，重开客户端即可绕过。
- 缺服务端强制冷却 + 连续错误锁定机制，暴力枚举验证码无成本。

## 2. 设计目标（用户需求）

1. **单码复用**：每连接只保留一个当前有效码，重取即让旧码失效。
2. **服务端强制冷却**：正常 **60s（1 分钟）**，请求过频服务端拒绝并回传剩余秒数。
3. **连续错 5 次 → 锁定 10 分钟**。
4. **锁定期间倒计时临时 10 分钟**：冷却升级为锁定时长，并作废当前码。
5. **10 分钟后恢复 1 分钟**正常冷却。
6. **短信验证码**：当前账号表无手机号字段、未接 SMS 网关，按"做不了就算了"未实现，仅在第九节留扩展点。

## 3. 状态机（per-connection）

`MsgHandler` 是每连接一个实例（在 `MyTcpSocket` 构造时创建、断开时销毁），状态天然 per-connection。

成员变量：
- `QString m_captchaCode` / `QDateTime m_captchaExpiry` —— 单码及其过期时间（覆盖式）。
- `QDateTime m_captchaNextAllowed` —— 最早可再次成功获取验证码的时间。
- `QDateTime m_captchaLockUntil` —— 锁定结束时间（`isValid()` 且 `now < 此值` ⇒ 锁定中）。
- `int m_captchaFailCount` —— 连续错误计数。

常量（`msghandler.h` 内 `static const int`）：
- `CAPTCHA_COOLDOWN_SEC = 60` 正常冷却
- `CAPTCHA_LOCKOUT_SEC = 600` 锁定（10 分钟）
- `CAPTCHA_MAX_FAILS = 5` 触发锁定的连续错误数
- `CAPTCHA_EXPIRE_SEC = 60` 单码有效期

## 4. `captcha()`（获取新码）流程

```
now = now
if (锁定中)        → status=-6, remaining=secsTo(lockUntil)
else if (冷却中)   → status=-5, remaining=secsTo(nextAllowed)
else:
    覆盖生成新码; expiry=now+60; nextAllowed=now+60
    status=0; remaining=60
respdu.caData[0..3]=status; caData[4..7]=remaining
if (status==0) respdu.caMsg = 新码
```

## 5. `validateCaptcha()`（登录/注册校验）流程

```
if (曾锁定且已过期) 清除锁定, failCount=0
if (锁定中)          → -4, remaining=secsTo(lockUntil)
if (输入过短/空)     → failCount++, -2
if (无码/过期)       → -3（不累加，避免误锁）
if (不匹配)          → failCount++
    if (failCount>=5) 设 lockUntil=now+600, nextAllowed=lockUntil, 作废码 → -4, remaining=600
    else             → -2
if (匹配)            → 作废码, failCount=0, 0
```

## 6. 错误码与 PDU 布局

服务端 → 客户端（复用 `caData`/`caMsg`，**未改 PDU 结构**）：

| 响应类型 | 字段 | 含义 |
|---|---|---|
| `CAPTCHA_RESPOND` | `caData[0..3]` | 状态码：`0` 成功 / `-5` 冷却中 / `-6` 锁定中 |
| | `caData[4..7]` | 剩余秒数（成功=60，冷却/锁定=实际剩余） |
| | `caMsg` | 仅 `status==0` 时携带新码 |
| `REGIST_RESPOND` / `LOGIN_RESPOND` | `caData[0..3]` | `validateCaptcha` 返回值：`0` / `-2` / `-3` / `-4` |
| | `caData[4..7]` | 仅 `-4` 时有效，锁定剩余秒数 |

## 7. 改动文件清单

- **Server/msghandler.h**：去掉 `QMap<QString,QDateTime> m_captchas`，新增单码 + 冷却 + 锁定成员与 4 个常量；`validateCaptcha` 签名加 `int& remainingSec` 输出参数（返回 `0/-2/-3/-4`）。
- **Server/msghandler.cpp**：构造函数初始化 `m_captchaFailCount`；重写 `validateCaptcha`、`captcha()`；`rigist()`/`login()` 在 `captchaRet==-4` 时把 `remaining` 写入 `caData+4` 回传。
- **Client/client.h**：`startCaptchaCooldown(int sec, bool locked = false)`；新增 `bool m_captchaLocked` 锁定标志。
- **Client/client.cpp**：`startCaptchaCooldown` 用服务端下发秒数驱动倒计时（格式 `MM:SS`）；`locked=true` 时**同时禁用注册/登录按钮**并把 `m_captchaLocked` 置位；倒计时归零（`onCaptchaCooldownTick`）时若处于锁定态则恢复注册/登录并复位标志。新增客户端拦截守卫：`on_pushButton_clicked` 在冷却/锁定中直接拦截获取请求；`on_register_PB_clicked` / `on_login_PB_clicked` 在 `m_captchaLocked` 时直接拦截发送并提示。
- **Client/msghandler.cpp**：`handleCaptcha()` 解析 `0/-5/-6` 与剩余秒数；`-6`（锁定）调用 `startCaptchaCooldown(rem, true)` 触发客户端 10 分钟锁定；`handlrigist()`/`handllogin()` 的 `-4` 分支读 `caData+4` 剩余秒数并调用 `startCaptchaCooldown(rem, true)` 触发客户端锁定、弹"账号已锁定，请 N 分钟后再试"。

## 8. 测试要点

1. 连续点"获取验证码"：第 1 次成功，60s 内再点 → 客户端提示"获取验证码过于频繁，请 N 分钟后再试"，按钮倒计时显示服务端下发秒数（格式 `MM:SS`，正常冷却为 `01:00`）；且冷却中按钮本就禁用，slot 内还有 `m_captchaCooldownSec>0` 兜底拦截。
2. 连续输错验证码 5 次 → 第 5 次服务端返回 `-4`，客户端弹"验证码错误次数过多，账号已锁定，请 10 分钟后再试"，获取验证码按钮显示 `10:00` 倒计时（约 600s），**且注册/登录按钮同时被禁用**；10 分钟内点击注册/登录会被客户端直接拦截并提示。
3. 锁定期间再点获取 → 服务端返回 `-6`。
4. 锁定结束后（>600s）首次错误不应立即再锁定（`failCount` 已重置），恢复正常 60s 冷却。
5. 单码复用：获取新码后，旧码立即失效（用旧码登录返回 `-2`/`-3`）。
6. 用过期码登录 → `-3`。

## 9. 短信验证码扩展点（未实现）

按用户"做不了就算了"的指示，本次**未实现短信验证码**：账号表无手机号字段、也未接 SMS 网关。

将来若接入：在 `captcha()` 的成功分支里，若该账号已绑定手机号，则改为**服务端存储验证码哈希、不下发明文**（当前是明文回传客户端显示在占位符），并通过 SMS 网关下发。需要：账号表增加 `phone` 字段；`captcha()` 增加"图形码 or 短信码"分支；`validateCaptcha()` 比对哈希。

## 10. 已知限制

- **锁定是 per-connection 的**：攻击者**断开重连**即可清零锁定状态（`MsgHandler` 重建，`m_captchaLockUntil` 归零）。若要彻底防住暴力枚举，需把冷却/锁定提升到**按 IP/账号的全局限流表**（例如 `QHash<QString, CaptchaLimitState>` 或 Redis），位于单例/连接池层，跨连接共享。本方案按"先出机制"落地，未引入该层。
- **沙箱未编译验证**：此环境只有 Anaconda 的 `qmake`（Python 绑定），没有 Qt 5.14.2 MinGW C++ 工具链，无法在此编译。改动严格沿用项目现有写法（`mkPDU` / `memcpy` / `QDateTime`），请在 Qt Creator 中编译确认。
