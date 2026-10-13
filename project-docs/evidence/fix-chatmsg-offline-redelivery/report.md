# FINDING-PRE1 — 1:1 offline chat redelivery / issuccess — fix + evidence

Branch: `small/fix-chatmsg-offline-redelivery`
Code commit: `8f655c5` — `fix(chatmsg): harden getchatMsg offline delivery (tx + exact-row mark + error surfacing)`
Scope: `Server/operatedb.cpp` only (`OperateDB::getchatMsg`). No header change. `protocol.h` untouched. No wire change.

## TL;DR — the headline fact

**The reported defect did NOT reproduce on a faithful rebuild against the current local MySQL.**

The task brief expected: BEFORE = both GETCHATMSG calls return the payload and `issuccess` stays 0.
Observed BEFORE (pre-fix binary, pre-fix source, 4 isolated runs + the exact candidate suite twice):

```
after send issuccess = (0,)
call 1 payload_len=48   -> issuccess = (1,)
call 2 payload_len=0    -> issuccess = (1,)
```

Both root-cause hypotheses (a) "re-preparing the same QSqlQuery after iterating a SELECT silently fails"
and (b) "the failure is invisible because q.exec()'s result is discarded" are **refuted by direct
observation**: the reused query's UPDATE returned `true` and took effect. The pre-fix server console
log (`server-console-before.log`, captured with `QT_LOGGING_RULES="xmu.*.debug=true"`) shows 14/14
UPDATE attempts on the old code:

```
xmu.db: update msg issuccess=1 ret: true     (x14)
```

So on this environment neither (a) nor (b) is the cause; the old code simply worked. What the
independent test observed on 2026-10-01 is **not reproducible today** (see "Why the earlier FAIL
likely occurred" below).

## Evidence (raw)

Environment: local MySQL `5.7.3-m13-log`; server built by `pipeline/build.ps1` (Qt 5.14.2 mingw73_64,
release); server launched `QT_ASSUME_STDERR_HAS_CONSOLE=1 QT_FORCE_STDERR_LOGGING=1
QT_LOGGING_RULES="xmu.*.debug=true"` on `127.0.0.1:5000`.

BEFORE (source at `0da0cc6`, before the fix):
- `before-probe.log` — isolated probe (`probe_chatmsg_fix.py`): `issuccess (0,) -> (1,)`, second call
  `payload_len=0`. 4/4 runs identical.
- `before-part2-run.log` — the **exact** candidate script `itest_part2.py` run from `build/itest/`:
  `[PASS] FINDING-PRE1_... re-delivered=False db_issuccess=(1,)`, `TOTAL: 35/37`. 2/2 runs identical.
  The only 2 failures are `BC_slow_conn_drop_logged` / `BC_slow_is_the_dropped_conn` — an artifact of
  where the server console was redirected for the BEFORE run (written to the evidence log, not
  `dist/server-console.log`), unrelated to chat.
- `server-console-before.log` — pre-fix console with db debug: `update msg issuccess=1 ret: true` x14.

AFTER (fix applied, commit `8f655c5`):
- `after-probe.log` — `issuccess (0,) -> (1,)`, second call `payload_len=0`. 2/2 runs.
- `after-part2-run.log` — `[PASS] FINDING-PRE1_...`, `TOTAL: 37/37` (all checks, no failures).
- `server-console-after.log` — new debug line `getchatMsg update issuccess=1 rows: 1` x3, proving the
  UPDATE now reports exactly which returned rows it marked.

Pipeline (commit `8f655c5`):
- `pipeline/build-8f655c5-20261001-114448.log` — ends `build GREEN: hash=8f655c5 config=20261001-114448`.
- `pipeline/test-8f655c5-20261001-114500.log` — `test GREEN-WARN: 0 automated tests registered`.
  **Honest-0**: there is no registered automated suite; this gate proves nothing on its own. The named
  probes above are the real evidence.
- `pipeline/manifest-*` — hash/time/env-stamped evidence manifests from `record-evidence.py`.

## What the code change does (hardening, not a proven bug fix)

Even though the happy path worked, the old `getchatMsg` had three genuine latent weaknesses; the
fix closes them and is minimal/revertible (one function, `Server/operatedb.cpp`):

1. **Swallowed errors** — `qCDebug(...) << q.exec()` logged the result and discarded it; a real
   UPDATE failure would be invisible and silently re-deliver forever. Now UPDATE and COMMIT failures
   are surfaced with `qCWarning(... q.lastError().text())`.
2. **No transaction / no read lock** — SELECT and UPDATE were not atomic. Wrapped in one transaction
   with `SELECT ... FOR UPDATE` so two concurrent pulls cannot read the same rows. (The repo already
   uses `FOR UPDATE` + `db.transaction()` extensively — see `fileobjectstore.cpp`, `operatedb.cpp:1313`.)
3. **Over-broad UPDATE** — the old UPDATE matched `seed_name`/`rev_name` with no `issuccess=0` filter,
   so it would mark *every* row for the pair, including one inserted concurrently that was never
   returned. Now the UPDATE targets only the primary keys actually selected, and keeps
   `issuccess = 0`. On failure it returns no rows (messages stay pending and are re-offered next pull),
   preserving the invariant "returned ⇒ marked".

`seedchatMsg` (`operatedb.cpp:891`, `issuccess = recipient-online ? 1 : 0`) was **not** touched.

## Why the earlier FAIL likely occurred (not verified)

- Source is byte-identical to the candidate: `git diff 03dc01d HEAD -- Server/operatedb.cpp` is empty,
  and the UPDATE statement is unchanged since the repo's initial commit (`git log -S` → only `579fe76`).
  So the tested code and mine are the same source.
- The archived `REQ-EXT-GROUP-002/independent-test/server-console.log` was captured **without**
  `QT_LOGGING_RULES`, so it contains no `getchatMsg` db lines and cannot show whether that run's
  UPDATE exec returned true or false.
- The single most likely explanation is that the binary that produced the 2026-10-01 FAIL was not the
  binary those sources build today (stale/leftover `Server.exe`, or a different MySQL client plugin
  loaded from PATH). This could not be confirmed because that exact binary was not archived.
- This is stated as unresolved, **not** as a conclusion.

## What could NOT be verified

- **Concurrency**: the transaction/`FOR UPDATE` path was not exercised under genuinely concurrent
  pulls. The server handles sockets on the event loop and a user may hold only one login
  (REQ-EXT-SESSION-001), so two simultaneous pulls for the same pair are hard to stage; the transaction
  is defensive and its concurrency benefit is reasoned, not measured. No double-delivery was observed
  either before or after, in any run.
- The exact binary used by the 2026-10-01 independent test (not archived) — so the cause of the
  original FAIL remains an open question for that session's owner.
- `pipeline/test.ps1` runs 0 registered tests; it is cited honestly as such, not as a passing suite.

## Files committed on this branch

```
Server/operatedb.cpp                                              (fix)
project-docs/evidence/fix-chatmsg-offline-redelivery/            (this dir)
  report.md, probe_chatmsg_fix.py, wire.py
  before-probe.log, after-probe.log
  before-part2-run.log, after-part2-run.log
  server-console-before.log, server-console-after.log
  pipeline/{build,test}-8f655c5-*.log, pipeline/manifest-*
```
