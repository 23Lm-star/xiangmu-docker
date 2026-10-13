# BUG-ARC-PUBLINK-001 — 分享链接实现与 ADR-011 安全要求严重偏离

| 字段 | 值 |
|------|-----|
| **编号** | BUG-ARC-PUBLINK-001 |
| **模块** | PUBLINK — 应用内分享链接功能 |
| **等级** | **严重** |
| **发现者** | 项目架构师（在审核 R06 测试报告时发现） |
| **发现日期** | 2026-07-29 |
| **关联决策** | `decisions/ADR-011-20260729-低耦合心跳与应用内分享链接.md` |
| **关联规格** | `fix-specs/R17-20260729-ARC-低耦合心跳与应用内分享链接-工程实施规格.md`（原始）、`fix-specs/R18-20260729-ARC-BUG-ARC-PUBLINK-001-安全加固与UI.md`（当前修复规格） |
| **当前状态** | **已确认，待修复** |

## 问题描述

R06 测试报告经代码确认，分享链接功能（`createPublishLink`）的实现与 ADR-011 / R17 工程实施规格存在以下严重偏离：

### 1. 随机源降级（安全）
- **要求**：`QRandomGenerator::system()` 生成 ≥128-bit 随机字节 → URL-safe 编码
- **实际**：`qrand() % 90000000 + 10000000` → 8 位十进制数字（仅 90M 空间）
- **风险**：`qrand()` 是确定性伪随机生成器，8 位数字可被单机在数秒内暴力枚举所有有效链接

### 2. Token 明文存储（安全）
- **要求**：数据库仅存 `SHA-256(token)` hash，不记录原始 token
- **实际**：`short_code` 明文存入 `publish_links` 表
- **风险**：数据库泄露直接暴露所有有效链接，无需暴力破解

### 3. 表结构不符合规格（功能完整性）
- **要求**：`share_link(token_hash, owner_name, relative_path, expire_time, revoked, created_at)`
- **实际**：`publish_links(short_code, file_path, creator_name)` — 无过期时间、无撤销字段
- **风险**：链接永久有效，无生命周期控制

### 4. 协议缺失解析链路（功能完整性）
- **要求**：新增 `RESOLVE_PUBLINK_REQUEST=54 / RESPOND=55` 用于 token 解析
- **实际**：仅有 `CREATE_PUBLINK_REQUEST=52 / RESPOND=53`，解析协议未实现
- **风险**：客户端无法通过 token 解析和下载分享的文件

### 5. 路径校验缺失（安全）
- **要求**：`canonicalFilePath()` + owner root canonical 前缀复核，拒绝路径逃逸
- **实际**：未见路径校验逻辑
- **风险**：存在路径穿越/越权读取可能

### 6. Client 端缺少分享链接获取 UI（功能完整性 — 2026-07-29 新增）
- **要求**：File 界面中右击文件 → 弹出右键菜单 → "获取分享链接" → 弹窗显示已生成的链接（可复制）；若未曾生成则为空白
- **实际**：Client 端无任何分享链接入口，用户无法获取已创建的链接
- **风险**：分享链接虽可在服务端创建，但用户无途径获取和使用

## 影响范围

- `Server/msghandler.cpp:723` — createPublishLink()
- `Server/operatedb.cpp:447-464` — createPublishLink() + getFilePathByCode()
- `Server/migrate_publish_links.sql` — 表结构
- 两端 `protocol.h` — 缺少 54/55 及链接查询协议
- **`Client/file.h/.cpp/.ui`** — 需增加右键菜单和获取分享链接入口
- **`Client/sharelinkdialog.h/.cpp/.ui`**（新增）— 分享链接展示弹窗
- **`Client/msghandler.h/.cpp`** — 需增加链接查询响应处理

## 修复方案（遵循 ADR-011 + 2026-07-29 UI 补充需求）

### A. 安全加固（服务端）
1. 将 `publish_links` 表迁移为 `share_link` 表，包含所有必需字段
2. 替换 `qrand()` 为 `QRandomGenerator::system()` 生成 128-bit token
3. 数据库存储 `SHA-256(token)` hash，不存原文
4. 追加 `protocol.h` 中的 54/55 枚举值（链接解析）
5. 实现链接解析服务端逻辑
6. 添加 canonical 路径校验

### B. 客户端 UI（新增）
7. File 界面文件列表添加右键上下文菜单
8. 菜单项"获取分享链接"：查询服务端 → 弹窗展示已生成链接
9. 新增协议类型用于"按文件路径查询已有链接"
10. 新增 `ShareLinkDialog` 弹窗，含只读输入框展示链接（可复制），未生成时显示空白

## 裁决记录

| 日期 | 裁决 | 裁决人 |
|------|------|--------|
| 2026-07-29 | 创建缺陷，等级严重；当前实现仅作为开发原型，不得作为功能交付 | 项目架构师 |
| 2026-07-29 | 补充 Client UI 需求（右键菜单+ShareLinkDialog），创建 R18 修复规格 | 项目架构师 |
