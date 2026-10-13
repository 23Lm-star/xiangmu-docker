# Merge summary — FINDING-GATE-2 lifecycle 收口 + test_gate 转 enforced（LARGE）

| 项 | 值 |
|---|---|
| 需求分支 | `codex/fix/finding-gate-2-lifecycle` |
| 受审候选 | `d691891f82774f53165fa22dd469e9b41858a503` |
| 合并提交（主干） | `6af69196603c0878542a3428a6c1ee3d3456acf1`（parents `0195384` + `d691891`，`--no-ff`） |
| 合并后树 | 与候选逐字节相同（`git diff d691891 HEAD` 为空） |
| 候选三闸 | RunId `run-20261011-07`：build GREEN / test GREEN 14 套 514 断言 / audit PASS |
| 主干三闸 | RunId `run-20261011-08`：build GREEN / test GREEN 14 套 514 断言 / audit PASS |
| 审计包 | `AUD-20261011-FINDING-GATE-2`，payload sha256 `7c3d7bff07fa18d559e41921048cd2030eb5da118e2a2f2bfee9c673c06591b5` |
| AI 结论 | **PASS**，阻塞 0，`independence: limited`（同模型家族/同一操作者，已披露） |
| 人工审批 | `APPROVED`，SID-backed `local-personal`；`scope ["merge","candidate"]` |

## 本次上线的内容

1. **FINDING-GATE-2 lifecycle 收口**（`6d597e6`）：原先两个套件要求外部先起服务端；enforced 门禁下前面的套件会按 PID 清掉自己的进程，于是必然 `ConnectionRefused`。现全部自管 `dist/Server.exe` 生命周期（`_harness/server_ctl.py`，按 PID，端口被占只报错不盲杀）。
2. **enforced 门禁基线首次全绿**（`4e15cb1`）：清掉 3 个非产品红 ——
   - `parallel-transfer`：假红。`test-gate.py` 取输出中**最后一个** `\d+/\d+` 当断言行数，套件尾部 NOTE 里的 `31/32` 盖掉了真实的 `54/54`；改为把汇总行打印在最后。
   - `transfer003 S-03a/S-03b`：空转红。基准是移动的 `master`，master 一旦含批准块 diff 即恒空；改为钉在改动引入前的 `f820e61`。审计判定为**合法纠正而非削弱检查**：固定基准对后续任何改动仍 fail-closed。
   - `group-002`：从未进入 setup。修 ROOT `parents[2]→parents[3]`、filesys 改 `dist/filesys`、纳入自管生命周期；manifest 声明数 `15→38`（15 是它从未跑过 setup 的残留值）。
3. **产品改动**（`a50190b` + `4e76269`）：`Server/Server.pro` 加 `CONFIG += console`，使服务端运行日志可被重定向捕获（`p7_slow_conn_dropped` 要读控制台 drop 日志）。两处缺口一并补：qmake 对 console 会跳过 `QMAKE_LIBS_QT_ENTRY`，须按 mkspec 成对补 `-lmingw32 -L$$[QT_INSTALL_LIBS] -lqtmain`（顺序：`-lmingw32` 必须在前）；仅子系统为 console 仍不够，重定向到文件时 Qt 走 `OutputDebugString`，harness 须置 `QT_ASSUME_STDERR_HAS_CONSOLE=1`。
4. **manifest 散文纠偏**（`d691891`）：独立审计条件 NB-1 —— 4 处与候选自身代码/证据矛盾的说明（491→514、残余红描述、group-002 "无 CONFIG += console"、sec001 BASE 来历）。`test_gate.status: enforced` 与各套 `asserts:` 本来就正确。

## 主干复测的意义

本仓库的已知风险是「分支绿、主干翻」：`transfer003` 的 `git diff master` 族与 `unique-login` E7b 在分支↔主干互换。主干实测 `transfer003` **59/59**、`unique-login` 16/16，14 套全绿 —— 钉基准的修法在主干同样成立，旧写法在主干必翻成 57/59。

## 审计明确未确立的事（残余风险）

- `dist/` 的 Qt 运行时是**手工还原**（`artifacts.ps1` 把 2018 版 Qt DLL 覆盖进 dist，致 `run-20261011-05` 14 套全红；已按 `C:\Qt\5.14.2\mingw73_64\bin` 与 `mingw730_64\bin` 还原 9 个 DLL），非端到端重建；dist 不在候选 diff 内。
- `test-gate.py` 的「最后一个 `x/y`」计数启发式未修（NB-2）——本次是在套件侧绕开。
- `unique_login_e2e.py` E7b 用三点 `master...HEAD`，合并后变空转真（NB-4）。
- `server_ctl.ServerCtl.start()` 在 `Popen` 抛错路径不关日志句柄（NB-5）。
- 三处 pipeline 工具缺口未修（NB-3）：`build.ps1` 只改 `.pro` 不重链；`artifacts.ps1` 递归 `build/build-*` 扫 exe 会部署遗留二进制；`artifacts.ps1` 用源 exe 同目录 DLL 覆盖 dist。
- `transfer003` 固定基准硬编码 `f820e61`，依赖 merge-commit 历史。
- 审计独立度为 `limited`；最强独立信号是非空转断言（group-002 p7 要求 >0 条真实 drop 日志；transfer003 S-03a/b 要求与批准字面逐字节相符）。
- 未做 GUI / 旧服务端人工复验。

### dist 运行时 DLL 可验证化（R1）

`artifacts.ps1` 曾把 2018-04-25 版 Qt DLL 覆盖进 `dist/`。手工还原后，2026-10-11 对
`dist/*.dll` 与 canonical 逐字节 `cmp`：**14 个有对应源的全部相同、0 个不同**；`icudt58.dll` /
`icuin58.dll` / `icuuc58.dll` 在 canonical 目录无对应源，未参与比对（不在被污染集内）。
比对源：`C:\Qt\5.14.2\mingw73_64\bin`、`C:\Qt\Tools\mingw730_64\bin`。

被还原的 9 个文件 sha256：

| DLL | sha256 |
|---|---|
| `Qt5Core.dll` | `019ab6aa811087b36d0a91c35a70b8eb7756b7f0f072afa0449b3805ccac7efc` |
| `Qt5Gui.dll` | `43deed28133c0e530ed44fff3649a87022a80bdb4ef493048518c72ac4656b99` |
| `Qt5Network.dll` | `5f013568cbdf748337821460af19fa7283ac6e3befb9685543b0517eda2f3e90` |
| `Qt5Widgets.dll` | `90fe759952447724da52351e7bcee6f51248b5d9577ab39f22bfd548c8931de9` |
| `Qt5Svg.dll` | `e8543ca27629869a6287ac20e55e542ac0f094d336b988c72db570fe913d408e` |
| `libEGL.dll` | `6f2af191db10019374e79e2afe2febfaa53c21257efcd4d41b50d947c700e2d3` |
| `libGLESv2.dll` | `46998b7ca48999b07005d4bcaa4256abce3150cc5f2c527fff26779cd76f1bd8` |
| `d3dcompiler_47.dll` | `e994847e01a6f1e4cbdc5a864616ac262f67ee4f14db194984661a8d927ab7f4` |
| `libmysql.dll` | `425ab60c449b939cae492f0285275ae09b164e0d88e27beb37099a2dd03918ae` |

复核命令（应无输出）：`for f in dist/*.dll; do n=$(basename $f); cmp -s "$f" "/c/Qt/5.14.2/mingw73_64/bin/$n" 2>/dev/null || cmp -s "$f" "/c/Qt/Tools/mingw730_64/bin/$n" 2>/dev/null || echo "DIFF $n"; done`

## 归档索引（sha256）

| 文件 | sha256 |
|---|---|
| `independent-ai-audit-package-d691891.json` | `7c3d7bff07fa18d559e41921048cd2030eb5da118e2a2f2bfee9c673c06591b5` |
| `audit-package-wrapper.json` | `e76e927ca2e352d03ee2836bdb6254fb87031036b14ce511faf3f30fd17afb5c` |
| `approval-6948e81e605f4bb280d2b90581df5f5a.json` | `9386223f242a7bf45e6f1ec9d4e8f73845f68b5c735dba5c2dc569edefa3e107` |
| `events.ndjson` | `9386223f242a7bf45e6f1ec9d4e8f73845f68b5c735dba5c2dc569edefa3e107` |
| `candidate-test-result-d691891-run-20261011-07.json` | `354d3d5782aa65a0c57d3af08a64dabea1728c07920c4372498274b0d7918d30` |
| `trunk-build-result-6af6919-run-20261011-08.json` | `807c3005ec857a7b278f78863452b5109436402a2c342e97665d539b9892ac0c` |
| `trunk-test-result-6af6919-run-20261011-08.json` | `01b3475ad1f051b1fb9a5682f3e3abfa4870b7dfee6e6727705087e4ec40e324` |
| `trunk-test-runner-6af6919-run-20261011-08.json` | `8cd6ffce15d552f2148711dd2896e4468a9f6b53d0d96a4e158c0b533cdefaa6` |
| `trunk-audit-result-6af6919-run-20261011-08.json` | `3bf935ba31fe27c6d821543211457cdf5948cc4dab7a64fb94a2e007ab6c8932` |

审计包与审批的原始位置（per-worktree，gitignored）：`build/evidence/merge-d691891/`。
