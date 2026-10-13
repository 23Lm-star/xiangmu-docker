fix-pipeline-audit-frozen-scope -- raw verification logs (2026-10-01)
pipeline/audit.ps1 v1.7 skeleton -> branch-scoped frozen check + docs-only evidence + advisory impact face.

scenario                                      before (skeleton, 0da0cc6)                 after (fixed)                                        files
1 docs-only HEAD / current master 0da0cc6     RED: "NO test evidence for 0da0cc6"        PASS + "docs-only commit: evidence from ffaaacf"     00-*, 01-*
2 code commit bbff4e4 WITH test evidence      PASS (bbff4e4)                             PASS (bbff4e4)                                       02-*
3 frozen path changed mid-branch, docs tip    PASS (8f03da3) -- silently green           FROZEN-PATH HIT + RED frozen:Client/protocol.h       03-*
4 hygiene check (item 2)                      PASS (no output)                           PASS (no output)                                    05-*
5 build gate on branch                        -                                          build GREEN (bbff4e4) / exit 0                       06-*

notes
- "before" = pipeline/audit.ps1 as of master 0da0cc6 (skeleton); saved to build/old-audit.ps1 for those runs.
- test logs are produced by real pipeline/test.ps1 runs (honest-0, 0 registered tests).
- docs-only evidence was transported from
  project-docs/evidence/REQ-EXT-GROUP-002/integration/test-ffaaacf-20261001-112821.log
  into build/evidence/current/ (gitignored working evidence), per the v1.7 transport contract.
- scenario 3 used throwaway branch tmp/verify-frozen-not-tip (frozen commit 27ea030, docs tip 8f03da3),
  deleted after capture; master untouched.
- impact-check.py is wired advisory-only; it never changes the verdict. On this machine it reports
  NOT_CHECKED (codegraph CLI/index unavailable here) or is skipped when no paths changed.
