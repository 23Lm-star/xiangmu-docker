T3 收口证据：small/normalize-logic-keys-predicate（FINDING-DEV-3）

分支提交 6175c54（fix），合并 master 为 fcceff8（no-ff merge）。
运行形态见 project-docs/evidence/E2E-RUNFORM.md；Python 用 py -3.13。

build-6175c54.log / test-6175c54.log / audit-branch.log   —— 分支提交上的三件套（audit PASS @6175c54）
build-fcceff8.log / test-fcceff8-merge.log                 —— 合并提交落 master 后的补证
cross_tree_e2e-BEFORE.log                                  —— 夹具对新代码绿、对旧代码红（37/41，4 FAIL）
cross_tree_e2e.log                                         —— 修复后 cross_tree 41/41
entry_ops_e2e.log 43/43 · dedup_e2e.log 13/13 · sec001_regression.log 48/48
fastupload_e2e.log 46/46 · groupfile_e2e.log 45/45
probe-count-BEFORE.log / probe-count-AFTER.log             —— 同一夹具（user_file + upload_session 各一行
  parent_path='/filesys/<owner>/sub/'）在旧码/新码下各启动两次的 LIKE '/filesys/%' 计数与重放稳定性
probe_count.py                                             —— 上述计数探针（cwd=dist，自管理 Server 生命周期）

注意：BEFORE 系列日志由"临时 stash 掉 Server/operatedb.cpp 改动后重建"的旧二进制产出，
仅用于证明夹具确实能红；当时的 dist/Server.exe 已在恢复改动后重建。
