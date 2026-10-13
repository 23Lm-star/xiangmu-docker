# FINDING-IND-7（候选缺陷，人工面 ⑤ 唯一未过项）：断线后重新登录，在途任务从客户端列表消失，无法重试续传

来源：主人 GUI 第二轮（候选已部署）——①②③④ 全过；⑤"任务不在列表里面，没法测试第五条"。
节点：本文件属 independent test 节点的**运行反馈层**记录；修复属 repair 节点（owner 新会话）。本节点未改任何产品源码。

## 1. 已证明的事实（FACT）

| # | 事实 | 证据 |
|---|---|---|
| F1 | 服务端半边成立：被杀断的那次上传留下了可续传的会话与暂存 | `upload_session` 3961 `wcy`/`PC端安装程序下载（.zip`：`expired`，confirmed 1,351,614,464 / expected 2,122,598,351（被服务端空闲回收置 expired） |
| F2 | 客户端半边失败：重新登录后传输面板里**没有**该任务，主人无法执行"点重试→断点续传" | 主人实测；面板实现无任何状态过滤（`TransferTaskPanel::refresh` 遍历 `taskKeys()` 全量渲染，含 Failed 与"重试"按钮，transfertask.cpp:1075-1114） |
| F3 | 该进程**从未**把任何任务写入磁盘快照 | `C:/Users/王晨扬/AppData/Local/Client/transfers_wcy.json` = `[]`，mtime **22:19:38**（= 上一轮最后一次成功上传的时刻）；22:43-22:54 这一轮（含两次成功上传 22:44:22 / 22:46:53）**没有任何写**。而 enqueue/finish/pause/resume/cancel 都会调 `saveSnapshot()`（transfertask.cpp:205/227/245/264/555/882/913/954/999） |
| F4 | 两次登录（22:43:26 / 22:54:36）是**同一个客户端进程** | `login_record.client_id` 两次都是 `ff880f8d…`；client_id 由首启 UUID 生成且**未持久化**（`QSettings st;` 无 org/app → 注册表与 AppData 均无任何条目，已实查）→ 每进程一个新 id → 同 id = 同进程 |
| F5 | 代码里清空任务表的**唯一**位置是 `setCurrentUser()` | `m_tasks.clear()` 仅 transfertask.cpp:136（`cancel()` 只删单条）；`setCurrentUser` 的守卫是 `if (name.isEmpty() \|\| name == m_currentUser) return;` |

## 2. 根因（FACT：源码逐行 + 两轮运行证据，2026-10-09 定案）

**一行时序竞态**：登录名在**登录响应到达 1.5 秒后**才被写入，而"切换当前用户"在响应到达的**同一瞬间**就执行了。

```cpp
// Client/client.cpp:194-199（LOGIN_RESPOND 分支，同步执行）
case ENUM_MSG_TYPE_LOGIN_RESPOND:{
    m_mhptr->handllogin();                                   // ① 内部用 QTimer::singleShot(1500, ...) 才写 m_LoginName
    TransferManager::instance().setCurrentUser(m_LoginName); // ② 此刻 m_LoginName 还是旧值（首次登录=空串！）
    TransferManager::instance().onConnectionRestored();      //    （G7 新增行）
    ...
}
// Client/msghandler.cpp:83-87（handllogin 的 ret==1 分支内）
QTimer::singleShot(1500, [loginName](){ Client::getinstance().m_LoginName=loginName; ... });
```

**首次登录**（每个进程的第一登）：`setCurrentUser("")` → 空名守卫直接 return（transfertask.cpp:134）→ `m_currentUser` 全程为空 ⇒
`saveSnapshot()` 的空用户守卫（transfertask.cpp:64-66）**整轮静默 no-op**（F3 的 22:19:38/09:19 两轮都一样），
且 `restoreSavedTasks()` 一并被跳过（"重开客户端恢复未完成传输"同样失效）。

**同一进程内再次登录**（断线后回登录界面再登）：此时 `m_LoginName` 已被上次的 1.5 s 定时器填好 = `wcy` ⇒
`setCurrentUser("wcy")` 守卫放行 → **`m_tasks.clear()`（transfertask.cpp:136）把内存里还在的"失败"任务整表清掉** →
`restoreSavedTasks()` 读的是一份"整轮从未写过"的空快照 ⇒ **面板空**（F2）。

运行证据（两轮独立复现）：22:43–22:54 与 2026-10-09 09:16–09:19 两次会话，`login_record.client_id` 各自同进程，
`transfers_wcy.json` 的 mtime 都停在**上一轮**（22:19:38），期间成功完成的多次上传一次都没落盘。

## 2b. 机制链（F1-F5 ⇒ 一个自洽解释；其中 M2 的一环为 claim，见 §3 的判定实验）

- **M1（推论，强）**：该进程里 `m_currentUser` 一直是**空串** → `saveSnapshot()` 的空用户守卫（transfertask.cpp:64-66）静默 no-op → F3 成立（整轮零写盘）。
- **M2（claim）**：22:43 首次登录时 `setCurrentUser(m_LoginName)` 收到的名字是空（于是 no-op、`m_currentUser` 留空）；22:54 重连登录时名字是 `wcy` → 守卫放行 → `m_currentUser` 从""变为"wcy" ⇒ **走 `m_tasks.clear()`**（把还在内存里的在途/失败任务整表清掉）→ 再 `restoreSavedTasks()` 读一个 `[]` 的快照 ⇒ **面板空**（F2）。
- 无论 M2 的具体触发点为何，两个**结构性缺陷**是确定的（与触发点无关）：
  - **D-a**：`saveSnapshot()` 在 `m_currentUser` 为空时**静默跳过**，没有任何告警或兜底键 ⇒ 一整轮传输可以完全不被持久化。
  - **D-b**：`setCurrentUser()` 在"名字变了"时**先清空内存任务表**再按快照重建，且不清/不合并——只要快照是空的（或过期的），内存里的真实任务就被毁掉；"同账号重连"没有身份归一化（依赖调用方传进来的字符串一字不差）。
- 结果：G7 想要的"失败态保留全部续传前提，重试即续传"在 GUI 上**不可达**（服务端续传前提其实在，F1）。

## 3. 判定实验（2026-10-09 09:16–09:20 主人已做，结果与根因一致）

| 步 | 期望（我的预测） | 主人实测 |
|---|---|---|
| 1 基线：登录后查 json 时间 | — | 停在 22:19:38（上一轮） |
| 2 传大文件 | 面板显示"传输中" | 正常 ✓ |
| 3 再查 json 时间/内容 | 预测"不变、无该任务" | 与预测一致（未落盘） |
| 4 杀服务端后立刻看面板 | 秒级变"失败" | 正常 ✓（任务当场在列表且为失败） |
| 5 重新登录后看面板 | 应有该"失败"任务、可点重试 | **任务消失**（缺陷复现） |

现场痕迹：`login_record` 09:16:10 / 09:19:17 两次登录同 `client_id`（同进程）；其间成功上传 6 个文件
（`upload_session` 3962-3966 全 done）；被中断的 3967（`2222222222.txt`，237,726,577 / 238,424,570，`verifying`）
服务端续传前提完好。

## 4. 修复方向（给 repair 节点的 owner，不属本节点执行）

- **R-1（根因，最小改动）**：登录名同步化。`Client::handlMsg` 的 LOGIN_RESPOND 分支不要再依赖 1.5 s 后才写入的 `m_LoginName`：
  把"从响应 `caData+32` 取名字"提到同步位置（如让 `handllogin()` 返回名字，或在分支内直接解析），
  先 `m_LoginName=name`，再 `setCurrentUser(name)`；1.5 s 定时器只保留 UI 切换（`Index::show()`/`Client::hide()`）。
- **D-a（防御）**：`saveSnapshot()` 不允许静默无用户——以"最近一次有效登录名"兜底，或写入中性文件并在恢复时按登录名认领；至少要有告警日志。
- **D-b（防御）**：`setCurrentUser()` 不做"清表重建"。同账号（含空→真名）应先 `restoreSavedTasks()` 再**按键合并**（内存为准、快照补缺）；只在**确实是另一个账号**时才丢弃内存任务。
- 回归面：REQ-EXT-SESSION-001 的 unique-login（同账号重连语义）、TRANSFER-001/002 的暂停/续传不回归；`transfers_<user>.json` 的位置与命名不能变（主人已有数据）。人工面至少复跑 §3 五步。

## 5. 已并入的既有发现

- 同轮的"对象实体找不到"另属既有数据漂移（见 `pre-existing-object-drift.md`），与本缺陷无关。
- 人工面 ①②③④ 在候选上全部通过（③ 从 pre-003 的"每次加一行"变为单行；② 偏移量随进度前进；① 提速）。
