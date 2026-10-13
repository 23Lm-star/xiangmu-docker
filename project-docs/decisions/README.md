# 决策记录

> 本目录记录所有重要架构、技术选型与项目治理决策。
> 每个决策使用 ADR（Architecture Decision Record）模板记录。

---

## 批次注册表

记录每个批次号对应的 Agent、日期和用途概要，用于跨批次文件关联查询。

| 批次号 | 日期 | Agent | 用途概要 |
|--------|------|-------|---------|
| R01 | 2026-07-24 | REV | 首次静态审查（BUG-POOL-001~008），含旧命名审查报告 + 裁决 |
| R02 | 2026-07-24 | ARC | 旧命名：fix-specs 批量审阅与排期裁决（含 DB-001~003、FILE-001~002、NET-001、SEC-001、BUILD-001~002、ARCH-001-R1a、ARCH-003） |
| R03 | 2026-07-24 | ARC | 静态审查 BUG-POOL-001~008 架构裁决（static-review-ruling-2026-07-24.md） |
| R04 | 2026-07-25 | TST/ARC | POOL-001~008 动态回归测试与架构裁决（`test-reports/R01-20260725-TST.md`, `R01-20260725-ARC-POOL动态回归裁决.md`） |
| R05 | 2026-07-26 | TST/ARC | BUG-TST-CLIENT-001 两次动态回归失败与裁决（`test-reports/R05-20260726-TST-BUG-TST-CLIENT-001-回归失败.md`, `R05-20260726-ARC-BUG-TST-CLIENT-001-第二次回归裁决.md`） |
| R06 | 2026-07-26 | REV/TST | BUG-TST-CLIENT-001 第三轮静态审查与动态回归失败（`bug-reports/R06-20260726-REV-BUG-TST-CLIENT-001-三次审查.md`, `test-reports/R06-20260726-TST-BUG-TST-CLIENT-001.md`） |
| R07 | 2026-07-26 | ARC | BUG-TST-CLIENT-001 第三次回归失败后的证据化根因裁决（`R07-20260726-ARC-BUG-TST-CLIENT-001-第三次回归证据化裁决.md`） |
| R17 | 2026-07-29 | ARC | 低耦合客户端心跳与应用内分享链接架构裁决与工程实施规格 |
| — | — | — | （以下批次待工程师生成新修复时继续） |

---

## 决策列表

| 编号 | 标题 | 日期 | 状态 |
|------|------|------|------|
| ADR-001 | 数据库连接池替换单一连接 | 2026-07-24 | 已实施 |
| ADR-002 | 使用自研内存池（TCMalloc 风格） | 2026-07-24 | 已实施 |
| ADR-003 | fix-specs 批量审阅与排期裁决 | 2026-07-24 | 已裁决 |
| ADR-004 | 使用 qmake 而非 CMake | 2026-07-24 | 已实施 |
| ADR-005 | 静态审查 BUG-POOL-001~008 架构裁决 | 2026-07-24 | 实施中（fix-spec 已下发，详见 `fix-specs/POOL-001-008-fix-spec.md`） |
| ADR-006 | connect.config Qt Resource 嵌入（延后外部化） | 2026-07-24 | 已裁决 |
| ADR-007 | POOL-001~008 动态回归裁决与 BUG-TST-CLIENT-001 处理方向 | 2026-07-25 | 已裁决（详见 `R01-20260725-ARC-POOL动态回归裁决.md`） |
| ADR-008 | BUG-TST-CLIENT-001 两次回归失败后的 UI 刷新裁决 | 2026-07-26 | 已实施但动态回归失败；由 ADR-009 接续 |
| ADR-009 | BUG-TST-CLIENT-001 第三次回归后的证据化根因裁决 | 2026-07-26 | 已裁决，待工程师调试取证（详见 `R07-20260726-ARC-BUG-TST-CLIENT-001-第三次回归证据化裁决.md`） |
| ADR-010 | 错误 Git 回退后的功能基线恢复 | 2026-07-28 | 已裁决：仅允许原子化定点恢复，禁止整体恢复历史提交 |
| ADR-011 | 低耦合客户端心跳与应用内分享链接 | 2026-07-29 | 已裁决，待工程师按两独立提交实施（`ADR-011-20260729-低耦合心跳与应用内分享链接.md`） |

---

## ADR-001: 数据库连接池替换单一连接

### 上下文

原有实现中 `OperateDB` 持有单一的 `QSqlDatabase` 实例。当 `QThreadPool` 多线程同时访问数据库时，`QSqlDatabase` 跨线程使用会导致 Qt 运行时警告和潜在的数据竞争。

### 决策

引入 `DatabaseConnectionPool` 组件：

- 按 `QThread::currentThreadId()` 隔离连接
- 每线程维护独立的空闲连接队列
- 默认每线程上限 8 个连接
- 通过 `PooledSqlConnection` RAII 封装自动归还
- 新建连接 lazily，空闲连接优先复用

### 状态

- ❌ 代码在分支 `add_数据库连接池功能` 上，尚未合并到 `master`
- ❌ `OperateDB::connect()` 中调用 `DatabaseConnectionPool::instance().init()` 初始化，但数据库配置（host/port/user/pwd/db）仍硬编码

### 后果

正面：
- 多线程数据库访问安全
- 连接可复用，减少连接建立开销
- RAII 风格降低连接泄漏风险

负面：
- 增加代码复杂度
- 初始化参数仍需外部化（见 BUG-DB-002）
- 无断线重连机制（见 BUG-DB-003）

### 关联

- BUG-DB-002：数据库密码硬编码
- BUG-DB-003：无断线重连

---

## ADR-002: 使用自研内存池（TCMalloc 风格）

### 上下文

服务端高频 PDU 分配/释放（每个请求+响应均需分配 PDU），通用 `malloc` 可能产生较多碎片和锁竞争。

### 决策

实现三级内存分配器：

1. **ThreadCache**：TLS 无锁，208 个 FreeList 桶
2. **CentralCache**：单例，桶级互斥锁，向 ThreadCache 批量供应的中介
3. **PageCache**：单例，全局锁，以 8KB 页管理系统内存

超过 256KB 的大对象直接走 `PageCache::NewSpan`。

### 状态

已实施于 `Server/` 目录（`ThreadCache`、`CentralCache`、`PageCache`、`ConCurrentAlloc`、`memalloc`）。

### 后果

正面：
- 高频小对象分配/释放无锁
- 减少碎片
- 异常时自动降级到 `malloc/free`

负面：
- 代码量大（16+ 文件）
- 仅服务端使用，客户端仍用 `malloc/free`（合理，客户端分配频率低）
- PDU 分配/释放函数两端不同，**禁止混用**

---

## ADR-003: `connect.config` 嵌入 Qt 资源文件

### 上下文

客户端和服务端都需要读取运行配置（IP、端口、文件路径）。

### 决策

使用三行文本格式，通过 `config.qrc` 嵌入 `connect.config`，运行时通过 `:/connect.config` 前缀读取。未采用外部配置文件或命令行参数。

### 状态

已实施。

### 后果

正面：
- 部署简单，无需额外配置文件
- 读取一致

负面：
- 修改配置需重新编译
- 数据库凭据等敏感信息也需放入 qrc（当前硬编码在源码中，更差）

### 关联

- BUG-DB-002：数据库密码硬编码

---

## ADR-004: 使用 qmake 而非 CMake

### 上下文

项目使用 Qt 5.14.2，构建工具需选择。

### 决策

使用 qmake（`.pro` 文件），这是 Qt 5 时代的官方推荐构建工具。

### 状态

已实施。`Client/Client.pro` 和 `Server/Server.pro` 分别构建两端。

### 后果

正面：
- Qt 5 原生支持
- 构建简单

负面：
- 跨平台可移植性不如 CMake
- 缺少现代构建特性（如自动依赖管理、测试集成）
- 如迁移 Qt 6，qmake 已被 deprecate
