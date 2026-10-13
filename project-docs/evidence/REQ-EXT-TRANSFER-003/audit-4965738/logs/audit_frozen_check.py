# audit-only helper: byte-exact comparison of the frozen face against design §9
import re, subprocess, sys, hashlib, os

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = WT  # this is the audit worktree at 4965738

def sh(*a, cwd=REPO):
    return subprocess.run(a, cwd=cwd, capture_output=True).stdout.decode('utf-8', 'replace')

DESIGN = r'E:\qt\xiangmu\build\wt-claude\project-docs\tasks\REQ-EXT-TRANSFER-003\design.md'
design_lines = open(DESIGN, encoding='utf-8', newline='').read().split('\n')

# §9 block (a): lines 234..238 (1-based) inside the ```cpp fence after "(a) 在既有"
i = next(k for k, l in enumerate(design_lines) if l.startswith('**(a) 在既有'))
# find opening fence
j = i
while not design_lines[j].startswith('```'):
    j += 1
j += 1
approved = []
while not design_lines[j].startswith('```'):
    approved.append(design_lines[j])
    j += 1
print(f'approved block lines = {len(approved)}')
for k, l in enumerate(approved):
    print(f'  A{k}: {l!r}')

# actual file content at 4965738
actual = sh('git', 'show', '4965738:Server/protocol.h').split('\n')
# locate MAX_INBOUND_PDU_SIZE
idx = next(k for k, l in enumerate(actual) if l.startswith('static constexpr uint MAX_INBOUND_PDU_SIZE'))
got = actual[idx:idx + len(approved)]
print(f'\nactual file lines {idx+1}..{idx+len(approved)} of Server/protocol.h@4965738:')
for k, l in enumerate(got):
    print(f'  G{k}: {l!r}')

ok = True
for k, (a, g) in enumerate(zip(approved, got)):
    if a != g:
        ok = False
        print(f'\nMISMATCH at offset {k}:\n  design={a!r}\n  file  ={g!r}')
print(f'\nBYTE-EXACT (block a): {ok}')

# also confirm no other Server/protocol.h deviation vs approved: whole-file diff vs design intent
# Client/protocol.h must be untouched d85e39f..4965738
d = sh('git', 'diff', 'd85e39f..4965738', '--stat', '--', 'Client/protocol.h')
print(f'\nClient/protocol.h diff d85e39f..4965738: {d.strip()!r}  -> zero-change: {d.strip() == ""}')

# is there any other hunk in Server/protocol.h beyond the approved block?
full = sh('git', 'diff', '-U0', 'd85e39f..4965738', '--', 'Server/protocol.h')
adds = [l for l in full.split('\n') if l.startswith('+') and not l.startswith('+++')]
dels = [l for l in full.split('\n') if l.startswith('-') and not l.startswith('---')]
print(f'Server/protocol.h: +{len(adds)} -{len(dels)}; hunks={full.count("@@")}')
print('added set == approved set:', set(a[1:] for a in adds) == set(approved))
print('removed:', [d[1:] for d in dels])

# hash the frozen regions for the report
def h(s):
    return hashlib.sha256(s.encode('utf-8')).hexdigest()
print('\nsha256(approved block joined) =', h('\n'.join(approved)))
print('sha256(actual  block joined) =', h('\n'.join(got)))
