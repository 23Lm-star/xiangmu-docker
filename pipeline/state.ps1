# pipeline/state.ps1 -- v1.7 bootstrap skeleton (rev v1.7.3). The ONLY session-opening
# action. <=25 lines out.
# v1.7.3 (v1.13 bundle): HUMAN VIEW section -- every session opening ends with
#         "what the human must decide now" derived from machine state, because the
#         human is the only role without a dedicated interface (measured: 7 attendances,
#         zero signal). Also: tier-jump WARN vs previous handoff, backup-directory hint,
#         docs-only rule in plain words (the old "current wrapper" line was unreadable).
# v1.7.2: multi-worktree aware -- if the newest handoff's branch is not checked out HERE,
#         state.ps1 locates the worktree that owns it and prints that task's real
#         branch/HEAD/tier/node, so sessions stop hand-re-verifying detached state;
#         pending audit reports filtered by disposition resolved|accepted and capped at
#         newest 3 with total count (stale reports from merged tasks stop shouting).
# v1.7.1: tier/node/owner/audit read from the newest handoff (no hardcoded defaults);
#         the "rerun after each committed change" prompt replaced by the docs-only
#         exemption rule; pending audit reports filtered by the handoff's `audit:` line.
# Repo root from script location, NOT `git rev-parse --show-toplevel` (PS 5.1 codepage issue).
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo  # git commands must run against this repo, not the caller's CWD
$branch = git rev-parse --abbrev-ref HEAD
$hash = (git rev-parse HEAD).Trim()
$dirty = git status --porcelain
$handoff = Get-ChildItem (Join-Path $repo "project-docs\handoffs") -Filter "*.txt" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$auditDisp = "none"; $hBranch = ""; $hTier = ""; $hNode = ""; $hOwner = ""
if ($handoff) {
    $hl = Get-Content -LiteralPath $handoff.FullName
    $hTier  = ($hl | Where-Object { $_ -match '^tier:' }   | Select-Object -First 1) -replace '^tier:\s*', ''
    $hNode  = ($hl | Where-Object { $_ -match '^node:' }   | Select-Object -First 1) -replace '^node:\s*', ''
    $hOwner = ($hl | Where-Object { $_ -match '^owner:' }  | Select-Object -First 1) -replace '^owner:\s*', ''
    $hBranch= ($hl | Where-Object { $_ -match '^branch:' } | Select-Object -First 1) -replace '^branch:\s*', ''
    $auditDisp = ($hl | Where-Object { $_ -match '^audit:' } | Select-Object -First 1) -replace '^audit:\s*', ''
    if (-not $auditDisp) { $auditDisp = "none" }
}
Write-Output "branch: $branch   head: $hash"
if ($dirty) {
    Write-Output "SUSPENDED git state (closeout before any node):"; $dirty | Select-Object -First 5
    if ($dirty | Where-Object { $_ -match 'pipeline\.bak-' }) {
        Write-Output "hint: pipeline.bak-* directories block the clean-candidate gate -- move them OUT of the repo (they are backups, not project files)"
    }
} else { Write-Output "suspended git state: none" }
if ($handoff) { Write-Output "last handoff: $($handoff.Name)" }
# v1.7.3: validate the newest handoff at session opening (measured: an agent-written
# handoff passed 13 lines / missing keys / wrong tier to every later session because
# nothing ever ran the validator). Prefer `py -3` (Windows launcher picks the newest
# python; measured: PATH python 3.8 crashed the validator on PEP 585 annotations).
# A validator crash is reported as such -- never as "handoff invalid". Never blocks.
$vh = Get-ChildItem (Join-Path $env:USERPROFILE ".claude\skills\development-os-v1-7\scripts\validate-handoff.py") -ErrorAction SilentlyContinue
if ($vh -and $handoff) {
    $py = Get-Command py -ErrorAction SilentlyContinue
    $pyArgs = @("-3")
    if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue; $pyArgs = @() }
    if ($py) {
        $vhOut = & $py.Source @pyArgs $vh.FullName $handoff.FullName 2>&1
        if ($LASTEXITCODE -ne 0) {
            if (($vhOut | Out-String) -match 'Traceback') {
                Write-Output "WARN: validate-handoff crashed (python too old for the validator?); handoff was NOT validated this opening"
            } else {
                Write-Output "WARN: last handoff FAILS validate-handoff -- fix it before building on it:"
                $vhOut | Select-Object -First 4 | ForEach-Object { Write-Output "  $_" }
            }
        }
    }
}
# Task-worktree section: the active task's branch often lives in build/wt-*, not here.
# Tier-jump check (v1.7.3): a handoff whose tier is HIGHER than the previous handoff's,
# without an obvious cause, is usually a copy-paste error -- measured: a SMALL cleanup
# task was written up as LARGE and every later session opening inherited the lie.
$prevHandoff = Get-ChildItem (Join-Path $repo "project-docs\handoffs") -Filter "*.txt" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 2 | Select-Object -Last 1
$prevTier = ""
if ($prevHandoff -and $handoff -and $prevHandoff.FullName -ne $handoff.FullName) {
    $prevTier = (Get-Content -LiteralPath $prevHandoff.FullName |
        Where-Object { $_ -match '^tier:' } | Select-Object -First 1) -replace '^tier:\s*', ''
}
$rank = @{ SMALL = 1; MEDIUM = 2; LARGE = 3 }
if ($prevTier -and $hTier -and $rank.ContainsKey($prevTier) -and $rank.ContainsKey($hTier) `
        -and $rank[$hTier] -gt $rank[$prevTier] -and $hNode -ne 'merged') {
    Write-Output "WARN: tier jumped $prevTier -> $hTier vs previous handoff ($($prevHandoff.Name)); if this task is not genuinely higher-risk, fix the handoff tier before proceeding"
}
if ($hBranch -and $hBranch -ne $branch -and $hBranch -ne 'master' -and $hBranch -ne 'main') {
    $wtLine = git worktree list | Where-Object { $_ -match [regex]::Escape($hBranch) } | Select-Object -First 1
    if ($wtLine) {
        $wtPath = $wtLine.Trim() -split '\s+' | Select-Object -First 1
        $wtHash = (git -C $wtPath rev-parse HEAD 2>$null).Trim()
        Write-Output "task branch: $hBranch   head: $wtHash   worktree: $wtPath"
    } else {
        Write-Output "task branch: $hBranch NOT checked out in any worktree"
    }
    Write-Output "tier: $hTier   node: $hNode   owner: $hOwner   audit: $auditDisp"
} elseif ($handoff) {
    Write-Output "tier: $hTier   node: $hNode   owner: $hOwner   audit: $auditDisp"
} else {
    Write-Output "tier: SMALL   node: bootstrap   owner: codex"
}
Write-Output "pipeline: docs-only commits (handoff/docs edits) still need all three gates rerun with a fresh RunId, because the candidate identity changed"
if ($auditDisp -notmatch '^(resolved|accepted)') {
    $allReports = Get-ChildItem (Join-Path $repo "build\evidence") -Recurse -Filter "audit-result-$hash-*.json" -ErrorAction SilentlyContinue |
        ForEach-Object {
            try {
                $r = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
                if ($r.candidate_commit -eq $hash) { $_ }
            } catch { }
        } | Sort-Object LastWriteTime -Descending
    if ($allReports) {
        Write-Output "pending audit reports ($($allReports.Count) total, newest 3):"
        $allReports | Select-Object -First 3 | ForEach-Object { Write-Output "  $($_.FullName.Replace($repo,''))" }
    }
}
$wt = git worktree list
Write-Output "worktrees:"; $wt | Select-Object -First 4
# --- FILL: dev worktree re-create if missing (idempotent; see multi-agent.md) ---
# --- v1.12: environment facts (project-docs/ENVIRONMENT.md) + deploy provenance check. ---
# Informational only: a missing file or a STALE binary never fails the session opening.
$envDoc = Join-Path $repo "project-docs\ENVIRONMENT.md"
if (Test-Path -LiteralPath $envDoc) {
    $inFacts = $false
    foreach ($ln in (Get-Content -LiteralPath $envDoc)) {
        if ($ln -match '^```text') { $inFacts = $true; continue }
        if ($inFacts -and $ln -match '^```') { break }
        if ($inFacts -and $ln.Trim()) { Write-Output "env: $($ln.Trim())" }
    }
}
$artifactsScript = Join-Path $PSScriptRoot "artifacts.ps1"
if (Test-Path -LiteralPath $artifactsScript) { & $artifactsScript -Verify }

# --- HUMAN VIEW (v1.7.3): what the human must decide NOW. Plain words, one line each.
# --- This section is for the person at the keyboard, not for the agent.
$humanSteps = @()
if ($dirty) {
    $humanSteps += "decide how to group the suspended files above into commits (or move backups out of the repo), then commit"
}
if ($auditDisp -and $auditDisp -match 'RED|pending') {
    $humanSteps += "a gate is RED or pending: have the owner session fix it or record a waiver -- do not treat the task as done"
}
if ($prevTier -and $hTier -and $rank.ContainsKey($prevTier) -and $rank.ContainsKey($hTier) `
        -and $rank[$hTier] -gt $rank[$prevTier] -and $hNode -ne 'merged') {
    $humanSteps += "confirm the tier jump above is real (it usually is a copy-paste error)"
}
if ($humanSteps.Count -eq 0) {
    Write-Output "HUMAN VIEW: nothing waiting on you -- state is clean and gates are accepted"
} else {
    Write-Output "HUMAN VIEW (decisions waiting on you):"
    $humanSteps | ForEach-Object { Write-Output "  - $_" }
}
