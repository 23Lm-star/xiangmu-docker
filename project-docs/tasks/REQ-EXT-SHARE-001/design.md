# REQ-EXT-SHARE-001 设计（design 节点）

> 分支 `feat/publink-renew-regenerate`｜基线 `master@0c3c398`｜节点 design→development｜日期 2026-10-12
> 档位 MEDIUM｜owner_agent: claude-code｜design_status: CONFIRMED（主人 2026-10-12 授权"续期时长可自行设计"）

---

## 1. 设计裁决

| # | 问题 | 裁决 | 理由 |
|---|---|---|---|
| D1 | 新增报文还是复用既有 52/56？ | **复用**。零新 PDU 类型 | 新枚举要动两端 `protocol.h` 的编号面与全部 `switch` 门（actor 门白名单、authReject 映射、group 分派），爆炸半径远大于 4 字节状态字；本项目已有"零协议改动扩展现有报文"的先例（SESSION-001 D1 的登录附加区） |
| D2 | 动作区放哪 | 52 请求 **caMsg 首个 NUL 之后**：`<relativePath>\0<action>\0<seconds>` | `fixedString(caMsg, uiMsgLen)` 读到首个 NUL 即止 → 旧服务端把附加区当垃圾忽略，路径解析行为逐字节不变 |
| D3 | 状态字放哪 | 53/57 响应 **caData[8..11]**（u32） | caData 只用了 [0]（bool）与 [32..63]（名字字段）；`mkPDU` 全零初始化，旧服务端不写该区间 ⇒ 旧客户端读到 0=有效，与旧版语义一致 |
| D4 | 「重新生成」要不要作废旧链接 | **要**，先精确 DELETE 旧行再建新 token | "重新获取链接"的实用语义就是撤销已泄露的那条；不作废则等于只是刷新了有效期（与 D5 重复） |
| D5 | 「续期」是否换 token | **不换**，只 UPDATE `expire_time` | 续期面向"还要继续用同一条链接"的场景；换 token 就等于重新生成，两个选项就没有区别了 |
| D6 | 已撤销的链接能否续期 | **不能**。`UPDATE ... AND revoked = 0`；命中 0 行即拒绝，响应 state=REVOKED | 撤销是"作废"的意思，靠续期把它复活会推翻撤销语义；要恢复只能重新生成 |
| D7 | 续期时长 | 客户端给 7/30/90/365 天四选一，缺省 30 天；服务端 `seconds<=0` 落 `PUBLINK_RENEW_DEFAULT_SECONDS`(30 天) | 主人授权自定；四选一既覆盖常见需求又不引入日历/永久语义（"永久"需要哨兵值，会污染 `expire_time` 的 DATETIME 语义） |
| D8 | 旧语义（无动作区）命中过期链接怎么办 | 仍返回该链接（不改旧行为），但状态字回**真实** EXPIRED | 只把"谎报有效"改掉；返回内容与旧版一致，老客户端最多是维持现状，不会变差 |
| D9 | 无记录时收到 `renew` | 等同首次创建，按请求时长生效 | 避免"没有可续期的记录"变成硬失败；与 `new` 的区别仅在是否换 token |
| D10 | 排序稳定性 | `getShareLinkByPath` 与新增 `getShareLinkInfoByPath` 统一 `ORDER BY created_at DESC, id DESC` | `created_at` 是秒精度，同秒两行时旧排序不稳定，会让"查询拿到的 hash"和"续期改的那一行"指向不同记录 |

## 2. 冻结面（逐字，两端 `protocol.h` 同块，len=716）

```cpp
// ===== REQ-EXT-SHARE-001：外链分享生命周期（两端逐字节一致，仅追加） =====
// 53/57 响应 caData[PUBLINK_STATE_OFFSET]：u32 状态字。旧服务端不写此区间、
// mkPDU 全零初始化 → 旧客户端读到 0 = 有效，语义与旧版一致。
static const int PUBLINK_STATE_OFFSET = 8;
static const quint32 PUBLINK_STATE_VALID = 0;
static const quint32 PUBLINK_STATE_EXPIRED = 1;
static const quint32 PUBLINK_STATE_REVOKED = 2;
// 52 请求 caMsg 的附加动作区：<relativePath>\0[<action>\0[<seconds>]]。action 缺省 = 旧语义
// （有记录则复用、无则新建）。旧服务端用 fixedString 读到首个 NUL 即止 → 附加区被忽略，向后兼容。
static const char PUBLINK_ACTION_NEW[] = "new";       // 作废旧行并重新生成
static const char PUBLINK_ACTION_RENEW[] = "renew";   // 续期：延长既有行有效期
static const int PUBLINK_RENEW_DEFAULT_SECONDS = 30 * 24 * 3600;   // 续期缺省 30 天
```

## 3. 服务端裁决表（`createPublishLink`）

| 动作 | 有记录 | 记录状态 | 行为 | ok / state |
|---|---|---|---|---|
| `new` | 是 | 任意 | DELETE 旧行 → 生成 token → INSERT(defaultExpire) | true / VALID |
| `new` | 否 | — | 生成 token → INSERT | true / VALID |
| `renew` | 是 | revoked | 拒绝 | false / REVOKED |
| `renew` | 是 | 有效或过期 | UPDATE expire = now+seconds | true / VALID |
| `renew` | 否 | — | 生成 token → INSERT(now+seconds) | true / VALID |
| 缺省 | 是 | 任意 | 复用 `token_hash`（hash 形式链接） | true / **真实状态** |
| 缺省 | 否 | — | 生成 token → INSERT(defaultExpire) | true / VALID |

任一数据库写失败 → ok=false, state 保持"真实状态"（客户端据 state 决定是否给恢复入口）。

## 4. 客户端交互

```
获取分享链接(56) ── exists=false ──→ "暂无分享链接"
                 ├─ state=VALID ──→ ShareLinkDialog（原行为）
                 └─ state≠VALID ──→ promptShareLinkRecovery
生成分享链接(52,无动作) ── ok&&state=VALID ──→ "生成成功"
                       └─ state≠VALID ──────→ promptShareLinkRecovery

promptShareLinkRecovery(state):
   [重新生成链接] → 52{new}      → 回到 53 响应分支
   [续期]         → QInputDialog(7/30/90/365 天) → 52{renew,seconds}   （state=REVOKED 时按钮禁用）
   [取消]         → 什么都不发
```

## 5. 测试与验收映射

| 契约 | 断言 |
|---|---|
| 冻结面同块 | S1 / S2 |
| 旧语义 + 三态查询 | A1–A8 |
| 续期（生效/token 不变/可解析/拒撤销） | B1–B6 |
| 重新生成（换 token/旧链失效/新链可解析/清撤销位） | C1–C6 |
| 向后兼容（caData[0] 语义、无动作区请求） | D1–D2 |

## 6. 残留风险（交主人裁决）
- R1：`renew` 的 seconds 由客户端提供，服务端不设上限——恶意客户端可把链接续到很远。可接受：链接本就由主人自己签发给自己，`seconds` 只影响自己的行。
- R2：`new` 会物理删除旧行，旧链接即刻不可解析。这是特性不是缺陷，但不可撤销（无"恢复旧链接"）。
- R3：状态字只看 `expire_time`/`revoked`，不检查源文件是否仍存在——文件被删时 `deleteShareLinkByPath` 已联动清行，残留窗口极小。
