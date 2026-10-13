# R05-20260726-REV-BUG-TST-CLIENT-001-二次审查

> 审查批次：R05 | 审查日期：2026-07-26 | 审查人：Bug 查找师（REV） | 审查类型：二次静态审查（严重缺陷修复后强制复查）

## 审查结论

✅ **通过** — 方案 B 增强版全部 6 项强制检查项通过，无方案 A/C 痕迹，无禁止区域被修改。

---

## 强制检查项逐项结果

### 1. 登录路径 — MsgHandler::handllogin() 不再调用 getFile()->flushfile()

| 项目 | 详情 |
|------|------|
| 位置 | `Client/msghandler.cpp:61-77` |
| 结果 | ✅ 通过 |
| 证据 | L72-75 内部 `QTimer::singleShot(0, ...)` 仅调用 `flushfriend()`，L74 注释明确"文件列表刷新延迟到用户首次点击'文件'页时" |
| 交叉验证 | `grep "getFile.*flushfile" Client/msghandler.cpp` — 6 处匹配均在 `mkdir()`(L207)、`changedir()`(L237)、`sharefilefinsh()`(L289)、`uploadfileinit()`(L310)、`uploadfiledata()`(L332)、`handlMoveFile()`(L526)，均为合法响应处理函数，无一位于 `handllogin()` |

### 2. 文件页入口 — Index::on_file_PB_clicked() 正确触发延迟刷新

| 项目 | 详情 |
|------|------|
| 位置 | `Client/index.cpp:38-46` |
| 结果 | ✅ 通过 |
| 证据 | L40 `setCurrentIndex(1)` → L43-45 `QTimer::singleShot(0, [this](){ ui->filePage->flushfile(); })` |
| 注释 | L41-42："切换到文件页后，通过事件队列下一轮触发文件列表刷新；确保页面已成为当前可见页后再发送网络请求" |

### 3. 数据/UI 分离 — File::updateCurrfile() 正确实现

| 项目 | 详情 |
|------|------|
| 位置 | `Client/file.cpp:97-111` |
| 结果 | ✅ 通过 |
| 步骤拆解 | ① L98-100：释放旧 `m_fileInfoList` 内存（`foreach delete`）→ ② L103：接管新数据（值传递，所有权转移）→ ③ L105-108：`!isVisible()` → `m_pendingUiRefresh=true; return;` → ④ L110：仅可见页调用 `rebuildFileList()` |
| 验证 | 隐藏页路径不调用 `ui->file_LW->clear()` 或 `addItem()`，仅设置标志位并 return |

### 4. rebuildFileList 隔离 — clear()/addItem() 仅一处

| 项目 | 详情 |
|------|------|
| 位置 | `Client/file.cpp:113-128` |
| 结果 | ✅ 通过 |
| 证据 | `grep -n "clear\|addItem" Client/file.cpp` → 仅 L114 `ui->file_LW->clear()` 和 L125 `ui->file_LW->addItem(pItem)`，均在 `rebuildFileList()` 内 |
| 验证 | `updateCurrfile()` 隐藏页路径不直接调用 `clear()`/`addItem()` |

### 5. showEvent — 页面可见时正确触发重建

| 项目 | 详情 |
|------|------|
| 位置 | `Client/file.cpp:130-135` |
| 结果 | ✅ 通过 |
| 证据 | L131 `QWidget::showEvent(event)` 先调基类 → L132-134 `if(m_pendingUiRefresh){ rebuildFileList(); }` |
| 重置 | L127 `m_pendingUiRefresh = false` 在 `rebuildFileList()` 末尾，防止重复重建 |

### 6. file.h 声明完整性

| 项目 | 详情 |
|------|------|
| 位置 | `Client/file.h` |
| 结果 | ✅ 通过 |
| 逐项 | L5 `#include <QShowEvent>` ✅ / L30 `void showEvent(QShowEvent*) override` ✅ / L56 `bool m_pendingUiRefresh = false`（C++11 类内初始化）✅ / L57 `void rebuildFileList()`（private）✅ |

---

## 禁止项检查

| 禁止项 | 结果 | 证据 |
|--------|------|------|
| 方案 A（登录切页再切回） | ✅ 无 | `handllogin()` 无 `setCurrentIndex()` 操作 |
| 方案 C（updateCurrfile 包 QTimer） | ✅ 无 | `updateCurrfile()` 中无 `QTimer::singleShot` |
| 修改 PDU 枚举 | ✅ 未修改 | `protocol.h` 未出现在变更文件列表中 |
| 修改 FileInfo 布局 | ✅ 未修改 | — |
| 修改 Server 端代码 | ✅ 未修改 | 变更仅限 Client 端 |
| 新增外部依赖 | ✅ 无 | 仅使用 Qt 标准库 |

---

## 边界条件覆盖矩阵

| 场景 | 路径 | 结论 |
|------|------|------|
| 登录→停留好友页→FLUSHFILE_RESPOND 到达 | `isVisible()=false` → `m_pendingUiRefresh=true` → return（延迟） | ✅ |
| 登录→点击"文件"页→showEvent→数据已就绪 | `showEvent` → `m_pendingUiRefresh=true` → `rebuildFileList()` | ✅ |
| 登录→点击"文件"页→showEvent(空)→QTimer→flushfile→响应到达 | `isVisible()=true` → `rebuildFileList()` 直接重建 | ✅ |
| 用户在文件页→mkdir/del/rename 完成→flushfile→响应到达 | `isVisible()=true` → `rebuildFileList()` | ✅ |
| 用户在好友页→mkdir/del/rename 完成→flushfile→响应到达 | `isVisible()=false` → `m_pendingUiRefresh=true`（覆盖旧数据） | ✅ |
| 文件页可见时两次 FLUSHFILE_RESPOND 先后到达 | 两次均 `isVisible()=true` → 直接调用 `rebuildFileList()` | ✅ |
| 用户从未点击"文件"页 | `m_pendingUiRefresh` 保持 `false`，FLUSHFILE_REQUEST 从未发送 | ✅ 预期行为（按需刷新） |
| 进入子目录（双击目录项）→ FLUSHFILE_REQUEST 直发 → 响应到达 | `isVisible()=true`（在当前页）→ `rebuildFileList()` | ✅ |
| 返回上级目录 → FLUSHFILE_REQUEST 直发 → 响应到达 | `isVisible()=true` → `rebuildFileList()` | ✅ |

---

## 调用链验证

```
登录成功
  └→ QTimer::singleShot(0) → flushfriend() only   ← 登录路径：不触发 FLUSHFILE_REQUEST

用户点击"文件"按钮
  └→ on_file_PB_clicked()
       ├→ setCurrentIndex(1)                       ← 切换页面
       │    └→ showEvent → 检查 m_pendingUiRefresh  ← 若已有数据则立即重建
       └→ QTimer::singleShot(0) → flushfile()      ← 事件循环下一轮发送 FLUSHFILE_REQUEST
            └→ Server 响应 FLUSHFILE_RESPOND
                 └→ MsgHandler::flushfile()
                      └→ updateCurrfile(qFileinfolist)
                           ├→ 释放旧 m_fileInfoList
                           ├→ 接管新数据
                           └→ isVisible()? rebuildFileList() : m_pendingUiRefresh=true
```

---

## 剩余风险

| 风险 | 等级 | 说明 |
|------|------|------|
| `m_pendingUiRefresh` 依赖 C++11 类内初始化 | 低 | 项目已使用 C++11 特性，Qt 5 兼容 |
| 用户快速切换页面可能导致多余的网络请求 | 低 | 每次点击"文件"页都发送 FLUSHFILE_REQUEST，但无崩溃风险，仅为性能优化项 |
| File 页切换到后台后响应到达的数据延迟 | 无 | 通过 `m_pendingUiRefresh` 机制覆盖，切回时自动重建，不丢数据 |

---

## 审查元数据

| 字段 | 值 |
|------|-----|
| 审查人 | Bug 查找师（REV） |
| 审查日期 | 2026-07-26 |
| 审查批次 | R05 |
| 缺陷编号 | BUG-TST-CLIENT-001 |
| 缺陷等级 | 严重 |
| 审查类型 | 二次静态审查 |
| 审查结论 | ✅ 通过 |
| 下游 Agent | 项目测试师（动态回归） |
