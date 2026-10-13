# 测试报告：心跳与分享链接功能验证及常规回归 [20260729]

- **测试批次**：R06-20260729-TST
- **测试范围**：心跳功能、分享链接接口、常规回归（登录/文件传输）
- **测试环境**：Windows 10, Qt 5.14.2, MySQL
- **测试状态**：静态验证通过 / 动态验证挂起（环境受限）

## 1. 测试用例执行情况

| 编号 | 测试目标 | 预期结果 | 实际结果 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| HB-01 | 心跳周期性验证 | 客户端每 30s 发送一次请求，服务端响应 Pong | 代码逻辑确认 30s 周期，处理逻辑完备 | ✅ 静态通过 |
| HB-02 | 心跳压力测试 | 持续运行 5 分钟，连接稳定无断开 | 逻辑支持，具体稳定性需真机验证 | 🟡 待真机 |
| SL-01 | 分享链接生成 | 调用 createPublishLink 成功生成 8 位随机码 | 代码实现调用数据库成功并返回 code | ✅ 静态通过 |
| SL-02 | 分享链接数据库校验 | 数据库 `publish_links` 表记录准确 | SQL 语句 INSERT 逻辑正确 | ✅ 静态通过 |
| RG-01 | 登录回归 | 正常登录、验证码校验逻辑正确 | 登录逻辑已包含验证码校验，实现稳定 | ✅ 静态通过 |
| RG-02 | 文件传输回归 | 上传/下载功能正常，无功能退化 | 现有流式下载及分块上传代码逻辑未变动 | ✅ 静态通过 |

## 2. 代码审查结论 (静态验证)

### 2.1 心跳功能
- **客户端** (`Client/client.cpp`): 
    - 54-56行：初始化 `m_heartbeatTimer`，周期 30000ms (30s)。
    - 116行：`sendHeartbeat()` 正确构建 `ENUM_MSG_TYPE_HEARTBEAT_REQUEST`。
    - 130行：正确处理 `ENUM_MSG_TYPE_HEARTBEAT_RESPOND` (Pong)。
- **服务端** (`Server/mytcpsocket.cpp`): 
    - 169行：识别 `ENUM_MSG_TYPE_HEARTBEAT_REQUEST` 并返回 `ENUM_MSG_TYPE_HEARTBEAT_RESPOND`。

### 2.2 分享链接
- **服务端接口** (`Server/msghandler.cpp`):
    - 723行：`createPublishLink()` 实现。
    - 生成 8 位随机数字串 (`qrand() % 90000000 + 10000000`)。
    - 调用 `OperateDB::createPublishLink`。
- **数据库** (`Server/operatedb.cpp` & `migrate_publish_links.sql`):
    - 452行：`INSERT INTO publish_links` 语句正确。
    - 表结构包含 `short_code`, `file_path`, `creator_name`。

## 3. 缺陷与回归
- **已知缺陷**：无。
- **回归情况**：文件传输采用流式分块 (`DOWNLOAD_CHUNK_SIZE`)，逻辑结构保持健壮。

## 4. 结论与建议
由于无法直接启动 GUI 服务端进行端到端自动化压力测试，**本次测试结论基于静态代码审查确认逻辑实现正确**。

**建议动作**：
1. **用户操作**：在真实环境下运行 Server 和 Client，挂机 5 分钟观察控制台是否出现 `[Heartbeat] Received Pong.` 且无断连。
2. **数据库校验**：生成分享链接后，执行 `SELECT * FROM publish_links;` 确认数据落盘。

---
**签名**：项目测试师
**日期**：2026-07-29
