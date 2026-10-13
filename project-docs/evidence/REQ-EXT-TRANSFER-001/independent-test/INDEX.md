# REQ-EXT-TRANSFER-001 independent test evidence (candidate 520c714)

- `test-report.md` -- the node report. Verdict: PASS, 0 candidate-attributable defects, findings F-1..F-5 (non-blocking), NEEDS-HUMAN = design 6.3 checklist (not executed, not counted as pass).
- `environment.txt`, `static-independent.log`, `frozen-content.log`, `readonly-confirm.log` -- environment, frozen-path diff (22 ins / 0 del, byte-identical to approved draft sec.9), worktree read-only proof.
- `pipeline-audit.log`, `build-520c714-*.log`, `test-520c714-*.log` -- the three pipeline gates rerun in the detached worktree.
- `entry_ops.log` 43/43, `dedup.log` 13/13, `cross_tree.log` 41/41, `sec001.log` 48/48, `fastupload.log` 46/46, `groupfile.log` 45/45, `parallel_e2e.log` 43/43.
- `parallel_e2e-rerun.log` -- determinism rerun, 43/43 (added by the test node; 2 identical runs).
