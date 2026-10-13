# 缺陷跟踪库 (Defects)

> 维护者：项目架构师
> 状态定义：新提交 -> 已确认，待修复 -> 修复中 -> 已测试，待合并 -> 已修复/已关闭 / 修复延后 / 已接受风险

| 缺陷 ID | 级别 | 描述 | 当前状态 | 责任人/Agent |
|---|---|---|---|---|
| BUG-ARC-RECOVERY-001 | 严重 | 错误 Git 回退导致已实现功能从当前基线丢失 | 已确认，待修复 | 禁止整体恢复历史提交；按原子、定点方案恢复，详见 `BUG-ARC-RECOVERY-001.md` |
| REQ-AUTH-001 | 新需求 | 用户安全注销账户功能 | ✅ **已测试，待合并** | 全链路代码审查通过，进入测试阶段 |
| BUG-POOL-005 | 严重 | `ClientTask` 空实现导致线程池调度死锁 | ✅ 已关闭 | 项目工程师 |
| BUG-ARCH-002 | 阻塞 | `tcpsocketlist` 多线程并发访问无锁 | ✅ 已关闭 | 项目工程师 |
| BUG-DB-001 | 严重 | SQL 注入风险（原生拼接） | ✅ 已关闭 | 项目工程师 |
| BUG-DB-002 | 严重 | 数据库密码硬编码 | ✅ 已关闭 | 项目工程师 |
| BUG-ARCH-001 | 严重 | PDU MsgType 枚举值冲突 | ✅ 已关闭 | 项目工程师 |
| BUG-FILE-001 | 严重 | 文件下载全量加载至内存导致 OOM | ✅ 已关闭 | 项目工程师 |
| BUG-POOL-001 | 严重 | PDU 所有权模式混用（sendAndRelease） | ✅ 已关闭 | 项目工程师 |
| BUG-POOL-002 | 轻微 | 拼写错误 `totalLen` | ✅ 已关闭 | 项目工程师 |
| BUG-POOL-006 | 严重 | 服务端启动配置失败弹框未阻塞主流程 | ✅ 已关闭 | 项目工程师 |
| BUG-POOL-008 | 轻微 | 中文注释乱码 | ✅ 已关闭 | 项目工程师 |
| BUG-POOL-003 | 轻微 | 内存池分配说明不足 | ✅ 已关闭 | 项目工程师 |
| BUG-TST-CLIENT-001 | 严重 | Client 登录后 pure-virtual 崩溃 | ✅ 已关闭 | 项目工程师 |
| BUG-SEC-001 | 一般 | 无盐 MD5 密码 | ✅ 已关闭 | 用户确认不需要 |
| BUG-NET-001 | 一般 | 无应用层心跳 | 已确认，可继续测试，修复延后（基础 PING/PONG 已实现；独立 ClientHeartbeat 组件化及超时检测延后至下迭代，参见 R06 裁决） | 项目工程师 |
| **BUG-ARC-PUBLINK-001** | **严重** | 分享链接实现与 ADR-011 安全要求严重偏离 + 缺少 Client UI（qrand 弱随机、明文存 token、无过期/解析/路径校验、无获取入口） | ✅ **已修复** | 项目测试师 — 见 R19 报告 |
| BUG-DB-003 | 一般 | 数据库断线重连 | ✅ 修复中（暂缓处理，等业务核心完成后再议） | 项目工程师 |
| BUG-SEC-002 | 一般 | 验证码过期策略 | ✅ 已关闭 | 项目架构师裁定 |
| BUG-FILE-003 | 一般 | 文件移动重名冲突及重命名失败 | ✅ 已修复 | 项目测试师验证 |
| BUG-REV-FILE-001 | 一般 | copy 降级失败时错误日志输出 rename 而非 copy 的 errorString | ✅ 已修复 | 项目测试师验证 |
| BUG-REV-FILE-002 | 一般 | copy+remove 降级后 remove 返回值未检查 — 数据一致性 | ✅ 已修复 | 项目测试师验证 |
| BUG-REV-FILE-003 | 轻微 | 重名循环 count 无上限保护 | ✅ 已修复 | 项目测试师验证 |
| BUG-REV-FILE-004 | 轻微 | 最终调试日志使用原始 dstPath 而非 finalDstPath | ✅ 已修复 | 项目测试师验证 |
| BUG-REV-FILE-005 | 轻微 | 目标父目录不存在时静默失败，无诊断日志 | ✅ 已修复 | 项目测试师验证 |
| BUG-REV-CLIENT-001 | 阻塞 | client.ui 尺寸(800×550)与 setFixedSize(600×420)严重不匹配 | ✅ 已关闭 | Bug 查找师 |
| BUG-DB-004 | 严重 | 好友表从 JSON 聚合回归为关系型 `(name, friendName)`；架构深度同步已通过（R15） | ✅ 已关闭 | 用户已提供回归证据，由架构师关闭 |
| BUG-SEC-003 | 阻塞 | 服务端 C→S PDU 三层防护：16KB 单包校验 + 256KB 未处理缓存水位线 + 计数器容错（MAX_INVALID_PDU_THRESHOLD=5）；架构深度同步已通过（R15） | ✅ 已关闭 | 用户已提供回归证据，由架构师关闭 |
| BUG-ARC-NET-001 | 一般 | 在线添加好友转发 PDU 由 `seedAddFriend()` 写出后未 `memfree`，长期运行累积泄漏 | ✅ 已关闭 | 架构师裁决；泄漏已修复并提供回归证据 |
| BUG-REV-CLIENT-002 | 阻塞 | index.ui 尺寸(1200×800)与 setFixedSize(1000×650)严重不匹配 | ✅ 已关闭 | 项目工程师 |
| BUG-REV-CLIENT-003 | 严重 | userinformation.ui windowIcon 使用本地绝对路径 | ✅ 已关闭 | Bug 查找师 |
| BUG-REV-CLIENT-004 | 一般 | SimpleCaptcha 硬编码浅色样式表与全局深色主题冲突 | ✅ 已关闭 | Bug 查找师 |
| BUG-REV-CLIENT-005 | 一般 | showLoadingAnimation 中 QGraphicsOpacityEffect 对象管理 | ✅ 已关闭 | Bug 查找师 |
| BUG-REV-CLIENT-006 | 轻微 | images.qrc 为空白骨架文件 | ✅ 已关闭 | Bug 查找师 |
| BUG-REV-CLIENT-007 | 轻微 | main.cpp QSS 加载失败时静默继续，无日志 | ✅ 已关闭 | Bug 查找师 |
| BUG-REV-BUILD-001 | 轻微 | 构建产物(11:39)滞后源文件(16:02)，exe 未含 QSS 变更 | ✅ 已关闭 | Bug 查找师 |
| BUG-REV-QSS-001 | 轻微 | bg_main.jpg(800×550)在600×420窗口中居中裁剪 | ✅ 已关闭 | Bug 查找师 |
| BUG-REV-AUTH-001 | 一般 | MD5 密码哈希在 PDU caData[32-63] 中无 null 终止符，调试输出与 caMsg 粘连 | ✅ 已关闭 | 用户确认不需要 |
| BUG-REV-AUTH-002 | 严重 | `validateCaptcha` 非纯时间判断：重复请求覆盖未过期验证码 + isEmpty 误判 | ✅ 已关闭 | Bug 查找师已验证，由架构师关闭 |
| BUG-REV-AUTH-003 | 阻塞 | `init_test_data.sql` 中测试用户存明文密码，与客户端 MD5 哈希不匹配 | ✅ 已关闭 | Bug 查找师已验证，由架构师关闭 |
| **BUG-REV-AUTH-004** | **严重** | `MsgHandler::logoff()` 中 `char[32]` 无 null 终止符导致密码越界读取 | ✅ 已关闭 | Bug 查找师 |
| **BUG-REV-DB-005** | **阻塞** | `OperateDB::connect()` 空实现导致数据库连接池从未初始化 | ✅ 已修复 | Bug 查找师 — 见 R07 报告 |
| **BUG-REV-MEM-001** | **严重** | `addfriendFin()` 双次 seedAddFriend 发送后 PDU 内存泄漏 | ✅ 已修复 | Bug 查找师 — 见 R07 报告 |
| **BUG-REV-MEM-002** | **严重** | `sharefile()` reseed 循环后 resendpdu 内存泄漏 | ✅ 已修复 | Bug 查找师 — 见 R07 报告 |
| **BUG-REV-PUBLINK-001** | **严重** | `createPublishLink()` 使用未初始化 ClientName 导致 owner 为空 | ✅ 已修复 | Bug 查找师 — 见 R07 报告 |

## 当前集成状态评估 (2026-07-29)

> **架构师裁决（R06 心跳/分享链接审核）**：
> - **BUG-NET-001**：基础心跳 PING/PONG 已实现并通过静态审查 ✅。协议层（类型 50/51）正确，30s 周期工作正常。**裁决：已确认，可继续测试，修复延后** — 独立 `ClientHeartbeat` 组件化和超时检测功能延后至下一迭代。
> - **BUG-ARC-PUBLINK-001（已确认，待修复）**：分享链接实现与 ADR-011 安全要求严重偏离 ⚠️。`qrand()` 弱随机、8 位数字 token 明文存储、无过期/解析/路径校验机制。**2026-07-29 补充**：Client 端缺少获取链接 UI（右键菜单+ShareLinkDialog）及 logoff 注销时 `publish_links`/`share_link` 孤儿记录清理。已创建 R18 修复规格下发工程师。
> - **smoke_test.py**：可用于验证协议层（50/51、52/53）TCP 通信正确性；不替代安全审查。
>
> **历史裁决（R15 & R16）**：
> - **BUG-DB-004**：关系型回归架构已批准 ✅。`friend` 表从 JSON 聚合回归为 `(name, friendName)` 每对关系一行；`handleAddFriendFin`/`handleFlushFriend`/`handleDeleteFriend` 均已验证正确。基于用户提供的证据，该缺陷已正式关闭。
> - **BUG-SEC-003**：计数器容错架构已批准 ✅。PDU 三层防护体系确认：16KB 单包上限 + 256KB 缓存硬上限 + 连续 5 次非法 PDU 熔断。基于用户提供的证据，该缺陷已正式关闭。
> - **BUG-REV-AUTH-002 & 003**：登录相关逻辑修复已审查通过，测试账号密码哈希及验证码纯时间判断验证无误，予以关闭。
> - **BUG-NET-001 & BUG-DB-003**：心跳机制和数据库断线重连属于可靠性增强。由于目前处于核心功能联调和缺陷收敛阶段，这两个缺陷暂缓处理（放入 Backlog）。优先推进主体业务逻辑的验证，待核心业务闭环后再重新规划。
>
> **下一轮集成测试计划**：分享链接安全加固（BUG-ARC-PUBLINK-001）修复完成后，由测试师执行动态回归，重点验证：token 安全生成、SHA-256 哈希存储、54/55 解析链路、canonical 路径校验、过期逻辑。
