# REQ-EXT-SESSION-001 唯一登录 — 任务记录（goal 节点，2026-09-30）

## 档位
- **LARGE**（goal 节点定档）：触碰鉴权/会话架构（SEC-001 同面同级）；生产数据契约变更（`user_info` 加列 + 新表迁移）；校验点位于**所有**业务请求的公共路径，回归面=全部现有功能；跨会话（goal→design→development→独立测试→审计→集成）。主人 2026-09-30 会话内批准方案 a+开工时序（"按施工顺序开始"）。
- 流程版本：Development OS v1.7。分支 `claude/req-ext-session-001-unique-login`，切自 master@`ccfd5de`；本 goal 会话纯文档，不动生产源码。
- 命名注记：用户面谈时暂名 session-003，落档改为新命名空间首号 SESSION-001（无历史空洞）。

## 并行分支冲突核查（2026-09-30 git diff 实测）
| 分支 | 状态 | 冲突结论 |
|---|---|---|
| REQ-EXT-FILE-001 | **活代码**，独立测试节点，距 merge 仅差审计+三事实行+主人 push（`7eba642`） | 唯一实质冲突源 = **C1**；其余经逐文件比对不重叠：其 `mytcpsocket.cpp` 仅动 :246 上传完成 hunk（与我要动的鉴权入口 :53-83/登录 case 零重叠）；`msghandler.cpp` +373 行全在文件操作函数，不碰 `login()`；两 `protocol.h` 其分支确认零命中，本需求方案 a 亦零命中 |
| REQ-EXT-GROUP-001 | 未切分支（开发门=FILE-001 合并） | 零活代码；60 起 PDU 编号已授权其联合分配表，与本需求无关 |
| REQ-EXT-GROUP-002 | 仅 goal 文档 | 未来其群 PDU/广播施工撞我已合并的鉴权入口 = 正常 trunk-sync，后落地方适配，两边代码不丢 |

- **C1（开发段唯一真冲突）**：FILE-001 新建了全局单一 schema 版本闸门（`main.cpp` 启动 fail-fast + `OperateDB::checkSchemaVersion()` + `kFileAssetSchemaVersion=2026092901`）。本需求的 `login_version` 列/`login_record` 表必须注册进同一版本机制。处置规则（主人批准的施工顺序第 2/3 条）：开发会话启动时查 FILE-001 合并态——**已合并**→ `git merge master` 后把本迁移适配进其版本闸门（常量递增、迁移独立文件），冲突消失；**未合并**→ 照常开发，迁移 SQL 写成独立文件 `Server/migrate_ext_session_001.sql`，**不碰 `main.cpp` 启动钩子与 `checkSchemaVersion` hunk**，版本注册留待 trunk-sync 时一次性适配。禁止预先猜测对方行号施工。
- **C2**：`operatedb.{h,cpp}` 双方均在类顶/类尾追加方法 → 机械文本冲突，trunk-sync 时消解。
- **C3**：踢线断开与 `handleOffLine` 竞态（§4.5）是本需求内部设计必做项；做对后成为 GROUP-002 在线广播语义的前置保障。
- **C4**：行为级正向——唯一登录使"同用户单活连接"成立，简化 GROUP 系列 fan-out/游标假设。

## 现状盘点（基线 master@ccfd5de）
- 登录链路：`MsgHandler::login()`(Server/msghandler.cpp:250，caData 两 32B 槽=用户名+密码，caMsg 载验证码) → `OperateDB::handleLogin()`(Server/operatedb.cpp:242，加盐比对+补盐升级) → 连接层登录成功绑定 `m_actorAuthenticated`+连接内计数 `m_actorSessionVersion`(Server/mytcpsocket.cpp:71-83)。
- 统一鉴权入口：`MyTcpSocket::handlMsg()` :53，白名单 `isPublicRequestType()` :461（注册/登录/验证码/心跳），拒绝回包 `authRejectPdu()` :475——版本校验插此。
- 表：`user_info(id,name,pwd,online,salt)`（project-docs/init_test_data.sql:7），无版本列；无登录记录表；登录报文负载无客户端标识承载位（caData 已占满，扩展位=caMsg 附加区）。
- 连接管理：`MyTcpServer::reseed()`(Server/mytcpserver.cpp:115) 持 `m_tcpsocketlistMutex` 按登录名匹配**首个**连接——同账号多连接并存是现状；踢旧需遍历同名全部连接关闭。
- 迁移先例：`Server/migrate_add_password_salt.sql`（ALTER 追加列）；FILE-001 分支另有 schema 版本闸门模式（见 C1，合并后即项目惯例）。
- 事务先例：`operatedb.cpp` 3 处 `db.transaction()`；GROUP-002 计划的"事务内原子分配序号"与本需求 login_version 原子 +1 同手法（MySQL `LAST_INSERT_ID(login_version+1)` 或条件 UPDATE）。

## 开发任务分解（design 节点细化）
- **T1 数据层**：迁移 SQL（`user_info` 加 `login_version` + 建 `login_record`）；`handleLogin` 成功分支改事务：原子 +1、取回新版本、插登录记录（客户端标识入参，可空）。
- **T2 连接层**：socket 新增成员绑定登录版本（与 `m_actorSessionVersion` 并存或归一，design 定）；鉴权入口白名单后追加版本比对，不一致→拒绝并关闭连接。
- **T3 登录踢旧**：登录成功后遍历同用户名其余连接关闭；同用户多连接在线语义收敛为单活。
- **T4 下线竞态**：`handleOffLine` 前校验断开方版本==库中最新版本，非权威会话不得清在线状态。
- **T5 客户端**：持久化客户端唯一 ID（QSettings）生成并随登录负载附加区发送；连接被切断时的提示与回退行为。
- **T6 证据与回归**：仿 SEC-001/FILE-001 harness 模式：双客户端互踢、版本并发对打（不重复不倒退）、竞态断言（踢旧不误清）、白名单四类不受影响、SEC-001 全量回归、protocol.h 零命中 diff 证明。

## 允许路径
`Server/msghandler.{h,cpp}`（仅登录函数区）、`Server/mytcpsocket.{h,cpp}`、`Server/mytcpserver.{h,cpp}`（连接遍历关闭）、`Server/operatedb.{h,cpp}`、新增 `Server/migrate_ext_session_001.sql`、Client 登录/ID 生成/断连提示、`project-docs/tasks|evidence/REQ-EXT-SESSION-001/`、handoff。
**禁止**：两 `protocol.h`（frozen，方案 a 零命中为硬约束）；FILE-001 未合并前触碰 `main.cpp` 启动钩子/`checkSchemaVersion` hunk；GROUP 表域与一对一消息语义；新外部依赖；改既有验收标准。

## 节点计划与启动条件
goal（本会话，纯文档）→ **design**（新会话：方案 a 细化、版本校验的每请求查询 vs 内存缓存裁决、T2 成员归一决策）→ development（启动时执行 C1 规则：查 FILE-001 合并态→已合并则先 trunk-sync；未合并则按禁触 hunk 清单照常）→ independent test（新会话只读）→ repair → independent audit（新会话、不同代理）→ integration（三事实行）→ merge no-ff。

## 开发段偏差记录（2026-09-30 development 节点）
- **D7 补充（设计修正，代码段内完成）**：并发登录交错下"后提交的低版本登记"会覆盖高版本并误踢新会话——`setLoginVersion` 改为单调 CAS（只接受不低于现值），`closeStaleSockets(user)` 改取登记表现值；E3 并发对打覆盖。
- **驱动事实（需求 §3"空值存储"的列形态定稿）**：Qt 5.14 QMYSQL 将空 QString 参数按 NULL 发送（实测 INSERT_FAIL "Column 'client_id' cannot be null"）→ `login_record.client_id` 改为 `VARCHAR(64) NULL DEFAULT NULL`（NULL=无标识）。需求语义不变，列形态为 NULL。
- **D4 语义经 E6 证实**：已认证过期连接的心跳也被切断（无 PONG、连接 EOF）；未认证白名单四类不受影响（E2）。
- **SEC-001 回归零改动通过 48/48**（E7a）：唯一登录未破坏既有鉴权断言。
- **环境残留如实声明**：本会话开发段一次 `pip install pymysql` 先于权限拦截落地（版本 2.2.8，仍在 py3.13 全局环境），**harness 未使用**（走 mysql CLI，仿 FILE-001 先例）；卸载操作同样被权限拒绝。处置决定权在主人。
- 证据：`project-docs/evidence/REQ-EXT-SESSION-001/harness-run.txt`（16/16 两次复跑一致）。

## 审计节点 REJECT 处置（2026-09-30 repair 节点）
Codex 审计结论 REJECT，理由=一次未归因的探针 P4 非确定性失败（-99 后端口拒绝连接；诊断与最后一次复跑正常，原始临时日志随 worktree 销毁）。三项发现 + 处置：
- **medium（探针不稳定）**：产品侧证伪。P4 原序列定向复现 3/3 干净（服务端全程存活）；失败签名"端口拒绝"= Server 进程缺席，harness 与探针均按镜像名 `taskkill /IM Server.exe` 互杀是唯一致命公共点（端口 5000 单实例）。处置：新增 `run_supervised.py`（端口占用即拒跑、启动记 PID、只按 PID 清理、子进程 UTF-8 编码修正）+ 探针加 PID 监护 `guard()`（死亡=SERVER_DIED 退出码 3，归因外部杀进程/崩溃而非断言失败；监护改字节比对防 GBK 误判）。修复后监督跑 8/8 PASS、进程存活。
- **low（test.ps1 注册数 0）**：仓库基线事实（manifest: test=0 registered；注册归 REQ-TEST-008B），非本分支缺陷，不改。
- **low（design.md 旧 NOT NULL 文案）**：已修订为 NULL 形态并加修订注记。
- 未动产品源码、未动验收标准。复审计要求：保留审计原始日志（不得随 worktree 销毁）、套件串行执行、探针用 `run_supervised.py`。

### 归因取证闭环（同日 repair 续，证据=repair-supervised-run.txt / repair-soak3-run.txt / repair-outoftree-run.txt）
非确定性失败的全部四种死亡形态均已现场复现并归因到执行环境，产品零崩溃：
1. rc=0xC0000409 启动即退=Server.exe 未带 Qt-PATH-first 启动（Anaconda DLL 互杀，manifest 已知陷阱）。
2. rc=1 init 阶段退=端口 5000 被残留实例占用（Server::initialize 失败干净退出，非崩溃）。
3. 运行中消失=工具进程树回收/并行套件 `taskkill /IM Server.exe` 按镜像名互杀（审计当时多套件重跑即此窗口）。
4. WER/CrashDumps 全窗口（含审计时段）**零** Server.exe 崩溃记录；裸空闲存活 >200s×2；PID 监督受控跑 8/8 连续多轮；树外分离进程服务器带流量全程存活 8/8。
结论：探针不稳定发现=测试运行器生命周期缺陷，已在 run_supervised.py/探针监护中根治（端口占用拒跑、PID-only 清理、SERVER_DIED 退出码 3 与断言失败分离）。产品判定：该 medium 发现不构成代码缺陷，复审计按监护方式复跑即可。

## 审计豁免与集成决定（主人 2026-09-30，任务记录留痕）
- 主人指令：不再复审计，直接推进合并。按 LARGE 规程，跳过必需门=记录性豁免决定：
  **审计豁免**——Codex 审计 `REJECT`（audit-fc1b6c7-REJECT.md，唯一 medium 发现=探针非确定性死亡）经 repair 节点取证全部归因执行环境（四形态复现、WER 零崩溃、受控 8/8 多轮、树外存活），产品候选 fc1b6c7 零变更；主人以该取证记录为基础接受残余风险，豁免复审计，授权集成。
- 三事实行按此判定：① 审计报告存在+REJECT+主人书面豁免（本行）；② frozen 路径 diff 空（audit.ps1 于合并结果复跑确认）；③ pipeline 绿（build/test/audit 于合并哈希执行，日志归档 evidence）。
- 合并后遗留登记：C1（schema 版本闸门注册迁移）待 FILE-001 合并 master 后一次性适配；本分支 `init_test_data.sql`/`operatedb.*` 类尾与 FILE-001 在途改动同域，其 trunk-sync 时机械合并即可（goal 冲突表 C2 预期内）。

## 停止条件
- 发现零协议改动无法承载客户端唯一 ID（负载附加区不够用/验证码解析冲突）→ 停在 design，向主人报协议变更决策卡，不自行动 frozen 文件。
- 版本校验每请求查询构成不可接受性能回归且缓存方案无法证明一致 → 同上上报。
- FILE-001 合并后其 schema 机制与本盘点冲突 → 回 goal 重估。
