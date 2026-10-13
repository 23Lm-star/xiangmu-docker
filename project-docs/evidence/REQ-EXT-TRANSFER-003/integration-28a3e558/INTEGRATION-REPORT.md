# REQ-EXT-TRANSFER-003 集成节点报告（no-ff 合并 + 三事实线复核）

- 日期: 2026-10-09 10:12–10:26 +0800
- 节点: integration（Development OS v1.7 LARGE 收尾）
- 工作树: `E:/qt/xiangmu/build/wt-integration`（从 `master@d85e39f` 新建）
- 合并提交: **`28a3e5582b87a205cdc3c05e5f42442cfeeee858`**（no-ff，父 `d85e39f` + `4965738`）
- 合并树: `ba790e861d8c69a5afc755d051a7424576f4e58b` == `4965738` 树（**TREE-EQUAL: YES**，合并未引入任何新内容）
- 独立性披露: development / test / repair / audit / integration 同为 `claude-code`，跨代理独立性未满足（本仓先例：报告首部标注）

---

## 1. 三事实线复核（本节点自行执行，不采信任何预检文字）

### 事实线 1 — 独立审计报告在册且被接受 ✅ PASS
- `project-docs/handoffs/req-ext-transfer-003-audit-2026-10-09.txt` 的 `audit:` 行 = `accepted by 主人(wcy), 2026-10-09`
- 主报告归档 `project-docs/evidence/REQ-EXT-TRANSFER-003/audit-4965738/audit-report.md`（23992 B）
- 第二意见同目录 `second-opinion/audit-report.md`

### 事实线 2 — 冻结面 diff 仅 §9 批准块 ✅ PASS（逐字节）
- 范围 `git diff d85e39f..4965738 -- Server/protocol.h Client/protocol.h`
- 结果 `Server/protocol.h` **+5/−2（1 hunk）**；`Client/protocol.h` **零改动**
- 删除行 == 原两行常量；新增 5 行 == design §9 (a) 代码块**逐字节相等**
- sha256（§9 代码块）= sha256（diff 新增行）= `844cb4c0da0eb914965e89069a05b58b126392199dcbc1fe57b4f7c452888889`
- **预检文字不采信**：design §9 散文写「原两行删除，新增四行」，而代码块为 **5 行** —— 以代码块为准比对，结论 APPROVED-ONLY
- 可复跑: `check-frozen-face.py`（本目录），输出 `frozen-face.txt`

### 事实线 3 — 合并结果流水线 ✅（build GREEN / audit PASS / 套件面见 §2）
| 项 | 结果 |
|---|---|
| `pipeline/build.ps1` | **GREEN** (`28a3e55`, 20261009-101237) |
| `pipeline/test.ps1` | **UNTESTED** exit=3 —— `test_gate.status: waived`（主人既有决定，非本次引入） |
| `pipeline/audit.ps1` | **PASS** (`28a3e55`) |
| 13 套件手动矩阵 | **11 绿 + 1 既有红（group-002）+ 1 结构红（transfer003 57/59）**，见 §2 |

**关于 `FROZEN-PATH HIT` 的实测更正**：`pipeline/audit.ps1`（本仓 adapted 版）的冻结检查按
`git merge-base master HEAD .. HEAD` 定范围。**合并之后 `master == HEAD`，该范围为空**，脚本按
设计静默（脚本第 12–14 行自述："an already-merged --no-ff merge reports no frozen hit"）。因此
合并结果上 **不会** 出现 `FROZEN-PATH HIT`。用预合并基复现同一判定谓词（`base=d85e39f`）：

```
repro base=d85e39f  HEAD=28a3e55  changed=23 paths
frozen_paths = Client/protocol.h, Server/protocol.h
FROZEN-PATH HIT: Server/protocol.h -> escalate to LARGE + user approval
REPRO VERDICT: 1 frozen hit(s)
```

唯一命中即 **§9 已批准变更**（design §10 有批准记录），属预期。`Client/protocol.h` 无命中。

---

## 2. 合并结果 13 套件矩阵（`build/run_declared_suites.py`，串行，端口 5000 空闲后实跑）

| 套件 | rc | 结果 | 分类 |
|---|---|---|---|
| sec001 | 0 | 48/48 PASS | 绿 |
| sec001-regression | 0 | 48/48 PASS | 绿 |
| entry-ops | 0 | 43/43 passed | 绿 |
| dedup | 0 | 13/13 passed | 绿 |
| cross-tree | 0 | 41/41 passed | 绿 |
| fastupload | 0 | 46/46 passed | 绿 |
| **group-002** | **1** | `FileNotFoundError: .../project-docs/Server/connect.config` | **既有红（harness ROOT off-by-one）**，与 2026-10-08 记录同症状，见 `group-002-repro.txt` |
| groupfile | 0 | 45/45 passed | 绿 |
| group004 | 0 | 23/23 PASS | 绿 |
| sec002 | 0 | 14/14 passed | 绿 |
| unique-login | 0 | **16/16 PASS, 0 FAIL** | 绿（E7b 在 trunk 上**空转通过**，见下） |
| **transfer003** | **1** | **57/59 passed**（S-03a/S-03b 红） | **结构红**：diff 断言在 trunk 上空转，见下 |
| parallel-transfer | 0 | rc=0（NOTE 提及 §6.3 人工清单，无断言计数） | 绿 |

### 2.1 transfer003 的 2 条红不是产品缺陷 —— 与 unique-login 的 E7b 是同一枚硬币的两面

`verify.py:304/316` 断言 `git diff master -- Server/protocol.h` 与 `Client/protocol.h`
（`cwd=REPO`）。合并后 `master == HEAD` ⇒ **diff 为空** ⇒ `rem=[]`/`n=0` ⇒ S-03a/S-03b 双双转红，
S-03c（Client 零改动）也空转通过。`unique-login` 的 E7b 断言 `git diff master...HEAD` 不含
protocol.h —— 在 trunk 上同样为空 ⇒ **空转通过**。两者互斥：

| 状态 | S-03a/b（要 diff 有 §9 块） | E7b（要 diff 无 protocol.h） |
|---|---|---|
| 分支 @4965738 | PASS | FAIL |
| 合并结果 @28a3e55 | FAIL | PASS |

**两者都只能由 diff 范围解释，不携带产品信号**；同一状态无法同时满足。S-03a/b 想验证的性质
（冻结面改动逐字等于 §9 批准稿）已由事实线 2 在**正确的范围**（`d85e39f..4965738`）上**非空转地**
验证：逐字节相等 + sha256 一致。

### 2.2 与分支基线对照（repair 节点 @4965738 的 13 套矩阵）
分支：11 绿 + group-002 红 + unique-login 15/16(E7b) + transfer003 59/59。
合并：11 绿 + group-002 红 + unique-login 16/16(空转) + transfer003 57/59(空转)。
**差额全部落在 §2.1 的 diff 空转上；产品信号无变化，未引入新失败。**

---

## 3. 结论

- 三事实线齐备（事实线 3 的套件面按 §2 分类如实记录），**保留合并**，不触发回退。
- `master` 前进到 `28a3e5582b87a205cdc3c05e5f42442cfeeee858`；预合并 master `d85e39f` 为第一父。
- 分支 `feat/req-ext-transfer-003-upload-session-throughput` 提交已被 master 可达 ⇒ 可删。

## 4. 过程中的一次污染（如实记录）

首次套件扫描在端口仍被占用时执行，`run_declared_suites.py` 对**未做端口守卫**的三套
（entry-ops / cross-tree / sec002）无保护，它们继续连上了主人当时在跑的 `dist/Server.exe`
（PID 26708，`E0EC1C97…`＝repair 节点从 4965738 部署的构建），得到 24/43、21/41、9/14
—— **对本次合并无效**（非套件自启的服务端、共享库存在前次残留），也与合并无关。
主人关闭 GUI 后端口释放，本报告 §2 为**重新完整实跑**的结果（`suites.log` 为最终版）。

测试账号残留（28 个，含 `e2eop1/2`、`xtuser1/2`、`sec2a` 等，跨多次会话累积）经
`build/cleanup_test_data.py` 干跑确认；**未执行** `--execute`（清理共享库不在本节点授权范围，
且主人会话当时在线）。建议由主人在方便时执行。
