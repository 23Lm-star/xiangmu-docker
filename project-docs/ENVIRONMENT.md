# project-docs/ENVIRONMENT.md — facts that cost a session to rediscover

Bootstrap artifact. Filled once, referenced forever. This is NOT documentation for humans to
read; it is the block that `pipeline/state.ps1` prints at session opening so the next session
starts with the environment already known.

Prefilled 2026-10-10 from build.ps1 evidence during the pipeline v1.12 upgrade; correct or
extend in place — a fact learned mid-session belongs here immediately.

```text
toolchain:Qt 5.14.2 MinGW 7.3 pinned: C:\Qt\5.14.2\mingw73_64\bin + C:\Qt\Tools\mingw730_64\bin (abs); never use the Anaconda qmake on PATH
run-shape:dist\Server.exe with cwd=dist; e2e suites share TCP port 5000 + MySQL database mydb
data-root:Client\connect.config + Server\connect.config are git-ignored runtime configs, embedded via qrc at build time
line-endings:repo defaults; git warns LF->CRLF on pipeline/*.ps1 checkout - harmless for PowerShell
encoding:PS 5.1 ACP936 console; gate scripts derive repo root from $PSScriptRoot, never git rev-parse --show-toplevel (CJK mojibake)
clean-cmd:dist\Server.exe and Client.exe must be stopped before redeploy (Windows locks running exes)
known-traps:linker fails "-fuse-linker-plugin, but liblto_plugin-0.dll not found" unless mingw730_64\libexec\gcc\...\7.3.0 is on PATH; running exe blocks Copy-Item -Force in artifacts deploy
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
