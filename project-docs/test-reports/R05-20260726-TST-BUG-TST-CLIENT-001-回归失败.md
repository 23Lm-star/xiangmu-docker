# R05-20260726-TST-BUG-TST-CLIENT-001-回归失败

> 批次：R05 | 日期：2026-07-26 | 测试师：项目测试师（TST） | 类型：动态回归验证

## 回归结论

❌ **回归失败** — TC-1 崩溃，崩溃路径与预期不同。

---

## 测试环境

| 项目 | 值 |
|------|-----|
| Server | Release 编译，`build-Server-Desktop_Qt_5_14_2_MinGW_64_bit-Release\release\Server.exe` |
| Client | Debug 编译，`build-Client-Desktop_Qt_5_14_2_MinGW_64_bit-Debug\debug\Client.exe` |
| Qt | 5.14.2 MinGW 64-bit, g++ 7.3.0 |
| MySQL | localhost:3306, mydb |
| 测试账号 | wcy |
| 监听地址 | 127.0.0.1:5000 |

---

## 构建验证

✅ Client Debug 编译成功（零错误，仅 2 个预存警告）

---

## 测试用例执行结果

| # | 用例 | 结果 | 证据 |
|---|------|------|------|
| TC-1 | 登录后停留好友页不崩溃 | ❌ **失败** | FLUSHFRIEND_RESPOND→flushFriend_LW()→pure virtual method called→exit code 3 |
| TC-2 | 登录后点击文件页正常加载 | ⏭️ 未执行 | 阻塞于 TC-1 |
| TC-3 | 文件页操作后刷新正常 | ⏭️ 未执行 | 阻塞于 TC-1 |
| TC-4 | 好友页后台操作→切到文件页 | ⏭️ 未执行 | 阻塞于 TC-1 |
| TC-5 | 多次往复切换不崩溃 | ⏭️ 未执行 | 阻塞于 TC-1 |

---

## TC-1 失败详情

### 实际崩溃调用链

```
handllogin() → QTimer::singleShot(0, ...) → Friend::flushfriend()
  → FLUSHFRIEND_REQUEST(type=11) 发送
  → FLUSHFRIEND_RESPOND(type=12) 到达，"lilei"
    → MsgHandler::handlFlushFriend() (msghandler.cpp:138-148)
      → Friend::flushFriend_LW(userNamelist) (friend.cpp:35-39)
        → ui->listWidget->clear()
        → ui->listWidget->addItems(q)
          → pure virtual method called 💥
          → terminate called without an active exception
          → exit code 3
```

### 关键日志

```
登录账号成功，用户名为: "wcy"
已发送成功: pdu->uiType 11  (FLUSHFRIEND_REQUEST)
服务器响应 已接收成功: pdu->uiType 12  (FLUSHFRIEND_RESPOND)
pdu->caMsg lilei
pure virtual method called
terminate called without an active exception
Client.exe exited with code 3
```

### 注意

- ❌ **无** FLUSHFILE_REQUEST 发出（方案 B 增强版 File 按需刷新生效）
- ❌ **无** FLUSHFILE_RESPOND 到达
- 崩溃直接发生在 FLUSHFRIEND_RESPOND 处理路径上

---

## 根因分析（更新）

方案 B 增强版的 File 组件数据/UI 分离设计方向正确，但修复范围**不完整**：

| 组件 | 数据/UI 分离 | 当前状态 |
|------|-------------|---------|
| **File** | ✅ 已实施 | `updateCurrfile()` → 先缓存 → `isVisible()` 检查 → 隐藏页仅标记 `m_pendingUiRefresh` |
| **Friend** | ❌ 未实施 | `flushFriend_LW()` 直接 `clear()` + `addItems()`，无任何可见性检查或延迟机制 |

`Friend` 组件需要与 `File` 组件相同的处理：
1. 内部缓存好友列表数据（`QStringList m_friendList`）
2. `flushFriend_LW()` 先接管数据，`!isVisible()` 时仅标记待刷新
3. 新增 `showEvent()` 在页面可见时重建列表

**更新后的根因表述**：`QStackedWidget` 主窗口初始化阶段，**任何**直接在 QListWidget 上执行 `clear()` + `addItems()` 的操作都可能触发 vtable 异常——无论该页面是否是当前页。根本原因是主窗口 `setupUi()` 完成后控件树的延迟布局事件与 QListWidget 内容更新产生竞争。

---

## 影响范围

- 完全阻塞 Client 端全部动态测试（与之前相同，但崩溃点从 File 转移到 Friend）
- 不影响 Server 端独立测试
- 不影响已通过静态审查的 File 组件修复（修复本身正确，只是不完整）

---

## 建议

1. 对 `Friend` 组件实施与 `File` 相同的数据/UI 分离模式
2. 保持 File 组件已有修复不变
3. 由架构师裁决修复方案（是否将数据/UI 分离抽象为公共基类或模板）

---

## 回归元数据

| 字段 | 值 |
|------|-----|
| 测试师 | 项目测试师（TST） |
| 测试日期 | 2026-07-26 |
| 测试批次 | R05 |
| 缺陷编号 | BUG-TST-CLIENT-001 |
| 缺陷等级 | 严重 |
| 回归结论 | ❌ 失败 |
| 下游 Agent | 项目架构师（回归结论 + 根因分析） |
