# REQ-EXT-TRANSFER-002 合并后 pipeline（master@f3cf883，主工作树，2026-10-09）

- build.ps1 → GREEN（build-f3cf883-*.log；dist/Server.exe+Client.exe 自 f3cf883 重出）
- test.ps1 → exit 3 UNTESTED（门册 waived 在册，FINDING-GATE-2）
- audit.ps1 → 骨架失真在册（F-5），边界由审计人工逐字节替代（两轮报告）

## 14 套（串行；轮一 suites-round1.log，重跑 suites-rerun*.log）
| 套件 | 终态 | 说明 |
|---|---|---|
| sec001 | 48/48 rc=0 | 文档形态=预启服务端+SEC001_SBX=dist/filesys（裸跑红=形态，非产品） |
| sec001-regression / entry-ops / dedup / cross-tree / fastupload / groupfile / group004 / sec002 | 48/48·43/43·13/13·41/41·46/46·45/45·23/23·14/14 | 轮一即绿 |
| parallel-transfer | 54/54 | 轮一 47/54 红=共享 mydb 悬空 file_object 行（主树旧 filesys 未 priming）；filesys 以 wt-test 克隆置换后复跑 54/54（机制同 repair 轮一发现，置换可逆：dist/filesys.stale-20261009 备份） |
| transfer002 | 26/26 | 同上置换后 rc=0 |
| transfer003 | 57/59 | 仅 S-03a/b diff-master 空转族（N-5 在册；主干侧结构性，两侧都空转，memory 口径不当产品信号） |
| unique-login | 16/16 | E7b 主干侧空转 PASS（分支↔主干互换在册）；E7a 裸跑 L1b/R1d 红=SBX 指旧 Server/filesys；SEC001_SBX=dist/filesys 后 48/48 内嵌全绿 |
| group-002 | rc=1 | 在册既有红当场复现：verify.py ROOT=parents[2] off-by-one → FileNotFoundError project-docs/Server/connect.config |

## 事实线
① 独立审计（R2）在册且被接受（handoff: audit: accepted by 主人, 2026-10-09）② 冻结面双端逐字节==§9 批准块 4/0（两轮审计人工重导）③ 合并结果 pipeline=build GREEN + 上表矩阵（12 绿 + transfer003 N-5 空转 + group-002 在册红）
NEEDS-HUMAN 后补：GUI 8 项 + §4.7 旧服务端复验一步。
