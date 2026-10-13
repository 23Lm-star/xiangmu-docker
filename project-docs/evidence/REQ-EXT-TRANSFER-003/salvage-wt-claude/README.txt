清理 build/wt-claude 前抢救的、别处没有的文本产物（集成节点 2026-10-09）。

来源: E:/qt/xiangmu/build/wt-claude/build/group004/{evidence.json,verify-output.log}
性质: REQ-EXT-GROUP-004（已合并的早期任务）在 wt-claude 里的运行留痕，时间戳 2026-10-09 09:36
      —— 即 repair 节点 13 套复跑时 group004 的那一次。
处置: project-docs/evidence/REQ-EXT-GROUP-004/ 只归档了 cursor_probe.cpp 与 verify.py，无运行留痕；
      本目录仅为避免工作树删除造成不可逆丢失，不构成本任务的验收证据。
未抢救: build/wt-claude/build/probe/group004_cursor_probe.exe（176 KB，可由 cursor_probe.cpp 重建）
       与 dist/（318 MB filesys + 二进制，属运行树，按清理指令删除）。
