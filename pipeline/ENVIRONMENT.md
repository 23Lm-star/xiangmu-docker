# project-docs/ENVIRONMENT.md — facts that cost a session to rediscover

Bootstrap artifact. Filled once, referenced forever. This is NOT documentation for humans to
read; it is the block that `pipeline/state.ps1` prints at session opening so the next session
starts with the environment already known.

## Why this file exists

Measured on one project: the owner asked for a "handover prompt" for the next session **10 times**,
the longest **16,244 characters** — while session-lifecycle.md explicitly forbids that ritual and
says the next session opens by running `state.ps1` and reading the handoff. The ritual persisted
because `state.ps1` could not carry the load: the most expensive knowledge in that project was
environmental, and none of it appeared in the opening readout. So it migrated into prose that the
owner had to copy by hand, and it decayed differently in every session.

The facts below are the ones that repeatedly cost time:

```text
toolchain:Qt 5.14.2 MinGW 7.3 pinned: C:\Qt\5.14.2\mingw73_64\bin + C:\Qt\Tools\mingw730_64\bin (abs); never use Anaconda qmake on PATH
run-shape:dist\Server.exe with cwd=dist; end-to-end suites share TCP port 5000 and MySQL database mydb
data-root:Client\connect.config + Server\connect.config are git-ignored runtime configs embedded via qrc at build time
line-endings:repo defaults; pipeline/*.ps1 may warn LF->CRLF on checkout without changing PowerShell semantics
encoding:PowerShell 5.1 ACP936 console; gate scripts derive repo root from $PSScriptRoot, never git rev-parse --show-toplevel
clean-cmd:stop dist\Server.exe and Client.exe before redeploy because Windows locks running executables
known-traps:linker needs C:\Qt\Tools\mingw730_64\libexec\gcc\x86_64-w64-mingw32\7.3.0 on PATH; running exes block Copy-Item -Force
```

Rules:

1. **Facts, not narrative.** One line each, `key: value`. If it needs a paragraph, it belongs in
   a design doc instead and the key here points at it.
2. **Absolute paths only where the machine forces one**, and mark them `(abs)`. Anything the repo
   can express relative must be relative — see `scripts/path-check.py` and invariant 20.
3. **`state.ps1` prints this file's facts block at session opening.** Keep it under ~10 lines;
   it is on the opening screen, not a document to browse.
4. **A fact discovered mid-session belongs here immediately**, in the session that learned it.
   The next session must not pay for it again.