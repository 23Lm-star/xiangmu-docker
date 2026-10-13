small/pipeline-audit-frozen-parse — F1 + F2 verification evidence (2026-10-01)
branch: small/pipeline-audit-frozen-parse   base: master@81f2beb

parse-before.txt   main worktree (manifest LF), PS5.1 default ANSI/ACP936 read  -> 1 frozen item
parse-after.txt    same file, raw UTF-8 read                                  -> 2 frozen items
audit-frozen-branch.log  scratch/t1-frozen-probe (both protocol.h touched, commit d0d2dd8)
                         -> 2x FROZEN-PATH HIT (proves both entries are parsed AND matched)
                         branch deleted after capture; never pushed, never merged
audit-branch.log   this branch (no frozen change) -> audit PASS
build-360baeb.log  pipeline/build.ps1 GREEN @ 360baeb
test-360baeb.log   pipeline/test.ps1 GREEN-WARN (0 automated tests registered, honest)
