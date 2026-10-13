# 集成节点自检：冻结面 diff (§9 批准块) 与 design §9 (a) 代码块逐字节比对。
# project-docs/tasks/ 未入库 → design.md 从主工作树解析（git worktree list 首条）。
import subprocess, hashlib, sys, os
BASE, TIP = 'd85e39f', '4965738'
main = subprocess.run(['git', 'worktree', 'list', '--porcelain'],
                      capture_output=True).stdout.decode('utf-8').splitlines()[0].replace('worktree ', '').strip()
design = os.path.join(main, 'project-docs', 'tasks', 'REQ-EXT-TRANSFER-003', 'design.md')
print('design source             :', design)
d = open(design, encoding='utf-8').read().split('\n')
i9 = next(i for i, l in enumerate(d) if l.startswith('## 9.'))
s = next(i for i in range(i9, len(d)) if d[i].strip() == '```cpp')
e = next(i for i in range(s + 1, len(d)) if d[i].strip() == '```')
block = d[s + 1:e]
out = subprocess.run(['git', 'diff', f'{BASE}..{TIP}', '--', 'Server/protocol.h', 'Client/protocol.h'],
                     capture_output=True).stdout.decode('utf-8')
added = [l[1:] for l in out.split('\n') if l.startswith('+') and not l.startswith('+++')]
removed = [l[1:] for l in out.split('\n') if l.startswith('-') and not l.startswith('---')]
orig = ['static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;',
        'static constexpr int MAX_PENDING_INBOUND_BYTES = 256 * 1024;']
names = subprocess.run(['git', 'diff', '--name-only', f'{BASE}..{TIP}'],
                       capture_output=True).stdout.decode().split()
print('design §9 (a) block lines :', len(block))
print('diff added / removed      :', len(added), '/', len(removed))
print('added == §9 block bytewise:', added == block)
print('removed == original 2 lines:', removed == orig)
print('Client/protocol.h in diff :', 'Client/protocol.h' in names)
print('sha256(§9 block)          :', hashlib.sha256(('\n'.join(block) + '\n').encode()).hexdigest())
print('sha256(diff added lines)  :', hashlib.sha256(('\n'.join(added) + '\n').encode()).hexdigest())
ok = added == block and removed == orig and 'Client/protocol.h' not in names
print('FROZEN FACE VERDICT       :', 'APPROVED-ONLY' if ok else 'OUT-OF-SCOPE')
sys.exit(0 if ok else 1)
