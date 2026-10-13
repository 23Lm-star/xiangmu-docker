# 手动测试说明书：BUG-TST-CLIENT-001 第三轮动态回归

| 字段 | 值 |
|------|-----|
| **测试编号** | R06-20260726-TST-BUG-TST-CLIENT-001 |
| **测试时间** | 2026-07-26 |
| **测试人** | 项目测试师（需用户协助执行 GUI 操作） |
| **缺陷** | BUG-TST-CLIENT-001（第三轮修复） |
| **前置状态** | Bug 查找师三次静态审查通过 ✅ |

---

## 前置条件

| 项目 | 状态 | 说明 |
|------|------|------|
| Server | ✅ 运行中 | PID 264764，127.0.0.1:5000 |
| MySQL | ✅ 监听中 | localhost:3306, DB=mydb |
| Client Release | ✅ 已构建 | `Client/release/Client.exe`（17:14 第三轮修复） |
| Client Debug | ⚠️ 需构建 | **请在 Qt Creator 中以 Debug 模式构建并运行** |
| 测试账号 | wcy / lilei | 已有账号 |
| 源码确认 | ✅ | friend.cpp + file.cpp 第三轮修复已确认 |

---

## 🔴 重要：请使用 Debug 模式运行

由于 Release Client 是 GUI 子系统，qDebug 输出不会显示在控制台。**强烈建议在 Qt Creator 中以 Debug 模式构建并运行（F5）**，这样才能在 Qt Creator 的"应用程序输出"窗口看到完整的诊断日志。

如果无法使用 Qt Creator，请按以下步骤操作：
1. 打开命令提示符
2. 设置 PATH: `set PATH=E:\qt\5.14.2\mingw73_64\bin;%PATH%`
3. 切换到: `cd E:\qt\xiangmu\Client\release`
4. 运行: `Client.exe`

---

## 测试用例 1：登录好友页测试

**目标**：确认登录后好友列表正常显示，不崩溃，不触发 FLUSHFILE_REQUEST

### 步骤
1. 启动 Client
2. 输入账号 `wcy`、密码 `123456`，点击登录
3. 验证码输入后等待登录

### 验收标准
- [ ] 登录成功，进入主界面（好友页）
- [ ] 好友列表正常显示（如 "lilei"）
- [ ] **未出现 "pure virtual method called" 或 exit code 3 崩溃**
- [ ] 日志中应看到（Qt Creator 输出窗口）：
  - `登录账号成功，用户名为: wcy`
  - `[Friend] flushFriend_LW: 缓存 N 个好友条目`
  - `[Friend] scheduleUiRefresh: 排入一次 UI 刷新`
  - `[Friend] UI 刷新：开始重建`
  - `[Friend] UI 刷新：完成`
- [ ] 日志中**不应出现** `FLUSHFILE_REQUEST`（type=23）已发送的日志

### 结果
- [ ] 通过
- [ ] 失败 — 描述：

---

## 测试用例 2：首次进入文件页测试

**目标**：确认文件列表按需加载，而非登录时预加载

### 步骤
1. **承接上一测试**，登录成功后停留在好友页至少 2 秒
2. 点击顶部 "文件" 按钮

### 验收标准
- [ ] 文件列表正常显示
- [ ] 日志中应看到：
  - `已发送成功: pdu->uiType 23`（FLUSHFILE_REQUEST — 此时才首次发送）
  - `[File] updateCurrfile: 缓存 N 个文件条目`
  - `[File] scheduleUiRefresh: 排入一次 UI 刷新`
  - `[File] UI 刷新：开始重建`
  - `[File] UI 刷新：完成`
- [ ] **无崩溃**

### 结果
- [ ] 通过
- [ ] 失败 — 描述：

---

## 测试用例 3：网络响应期间切页测试

**目标**：确认在网络响应到达前快速切页不会崩溃

### 步骤
1. 重新登录（或刷新页面状态）
2. 登录成功后，**立即**（2 秒内）在"好友"和"文件"之间快速切换 3-5 次
3. 观察是否崩溃

### 验收标准
- [ ] 无崩溃
- [ ] 每次切回某一页时，列表状态一致（显示最新数据而非空白）

### 结果
- [ ] 通过
- [ ] 失败 — 描述：

---

## 测试用例 4：10 次好友/文件往返切换

**目标**：压力测试，确认连续切换不会累积内存问题

### 步骤
1. 登录成功后，依次点击"文件"→"好友"→"文件"→...，连续切换 10 次
2. 每次切换间隔约 1 秒

### 验收标准
- [ ] 每次切换列表正确显示
- [ ] 无崩溃、无内存异常
- [ ] 对于切换时的日志，期望看到：
  - 切回好友页时：`[Friend] showEvent: 页面变为可见` → `scheduleUiRefresh` → 如果 `pending=false` 则不会触发重建（已有数据）
  - 切到文件页时：`[File] showEvent: 页面变为可见` → 如果有 pending 数据则重建

### 结果
- [ ] 通过
- [ ] 失败 — 描述：

---

## 测试用例 5：诊断日志验证

**目标**：确认日志中包含正确的诊断模式

### 最小通过模式（必须全部出现）

以下日志**至少各出现一次**在整个测试过程中：

| # | 期望日志模式 | 出现？ |
|---|-------------|--------|
| 1 | `[Friend] flushFriend_LW: 缓存` | [ ] |
| 2 | `[Friend] scheduleUiRefresh` | [ ] |
| 3 | `[Friend] UI 刷新：开始重建` / `完成` | [ ] |
| 4 | `[File] updateCurrfile: 缓存` | [ ] |
| 5 | `[File] scheduleUiRefresh` | [ ] |
| 6 | `[File] UI 刷新：开始重建` / `完成` | [ ] |

### 🔴 如果出现以下任何日志，立即停止并报告：

- `pure virtual method called`
- `terminate called without an active exception`
- `Client.exe exited with code 3`
- 任何 SIGABRT / SIGSEGV 信号

---

## 🚨 崩溃处理流程

如果测试过程中出现崩溃：

1. **立即停止测试**
2. **保存完整日志**（Qt Creator 输出窗口 → 右键 → 全选 → 复制）
3. 将日志粘贴到此处或截图
4. **不要进行第四次猜测性修改**
5. 测试师将更新缺陷状态为"重新打开"并移交架构师

---

## 测试环境信息

| 项目 | 信息 |
|------|------|
| OS | Windows 10 |
| Qt | 5.14.2 MinGW 64-bit |
| g++ | 7.3.0 (MinGW 7.3.0 64-bit) |
| MySQL | 5.7.25, localhost:3306 |
| Server PID | 264764 |
| Client 构建 | `Client/release/Client.exe` (Jul 26 17:14) |

---

## ⚠️ 给用户的提示

1. **请务必使用 Debug 模式**运行 Client，这样才能看到 qDebug 诊断日志。
2. 如果在 Qt Creator 中运行，请在"应用程序输出"窗口观察日志。
3. 如果崩溃，请先复制全部日志再关闭崩溃对话框。
4. 完成测试后，请回复每个测试用例的结果（通过/失败 + 关键日志截图或粘贴）。
