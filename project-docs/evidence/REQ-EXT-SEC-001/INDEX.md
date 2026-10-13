# REQ-EXT-SEC-001 证据索引（审计链按时间排列）

| 文件 | 节点 | 结论/内容 | 锚定哈希 |
|---|---|---|---|
| audit-c7c4eed-PASS-M1open.md | independent audit | PASS（附 M-1 Medium） | c7c4eed |
| reaudit-eb0ecc1-REJECT-B1.md | incremental re-audit | REJECT（B-1 High：自我登记绕过） | eb0ecc1 |
| reaudit-0efccb9-ACCEPT.md | incremental re-audit | ACCEPT（B-1 关闭）+ 残余观察分级 | 0efccb9 |
| verify-44-c7c4eed.log / verify-47-eb0ecc1.log / verify-48-0efccb9.log | e2e | 各候选端到端 | 各自哈希 |
| verify-48-final-7fc47c1.log + pipeline-run-final-7fc47c1.log | integration | 合并结果三事实行+48/48 | 7fc47c1 |
| reject-lines-0efccb9.log | 审计面 | 拒绝日志（仅 conn/type/原因） | 0efccb9 |
| verify.py | — | 端到端 harness（48 用例含 K2d/B-1） | — |
| manifest-*.txt | — | record-evidence.py 原生盖章（含 sha256 清单） | 各自哈希 |
| f1-verify-output.log | 关联 SMALL | F-1 发布链接去重修复 e2e 5/5 | 4ae532e→master fad502b |

原始逐字节工作传输保留在 `build/evidence/codex-req-ext-sec-001-auth-context/<full-hash>/`（gitignored）。
