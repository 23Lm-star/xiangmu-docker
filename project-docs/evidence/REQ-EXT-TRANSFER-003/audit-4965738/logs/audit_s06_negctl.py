# audit-only helper: run the ACTUAL S-06a..f check block from verify.py against
# OLD sources (git show <rev>:<path>) — an independent non-tautology probe.
import os, re, subprocess, sys

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERIFY = os.path.join(WT, 'project-docs', 'evidence', 'REQ-EXT-TRANSFER-003', 'verify.py')
SRC = open(VERIFY, encoding='utf-8').read()

# 1) extract the S-06 block verbatim (from the marker comment to the blank line before 环境准备)
start = SRC.index('# ---- G7 修复面')
end = SRC.index('# ================= 环境准备')
block = SRC[start:end]
print(f'--- extracted block: {len(block.splitlines())} lines, '
      f'check() call sites: {len(re.findall(r"check\(", block))}\n')

REV = sys.argv[1] if len(sys.argv) > 1 else 'b861351'

def show(path):
    return subprocess.run(['git', 'show', f'{REV}:{path}'], cwd=WT,
                          capture_output=True, text=True, encoding='utf-8').stdout

ns = {
    're': re,
    'subprocess': subprocess,
    'read': show,
    'check': None,
}
results = []
def check(sid, desc, cond, detail=''):
    results.append((sid, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} :: {desc} :: {detail}")
ns['check'] = check
ns['cli'] = show('Client/client.cpp')
ns['tt'] = show('Client/transfertask.cpp')
# same regex verify.py uses for the LOGIN_RESPOND body
ns['lr'] = re.search(r"case ENUM_MSG_TYPE_LOGIN_RESPOND:\s*\{(.*?)\n\s*break;", ns['cli'], re.S)

exec(compile(block, '<s06-block>', 'exec'), ns)

print(f'\n{REV}: {sum(1 for _, c in results if c)}/{len(results)} passed '
      f'(expect 6/6 FAIL on the pre-repair revision)')
