# pipeline/audit.ps1 -- structured candidate identity audit.
# Formal audit selection is JSON identity matching, never newest-log or text matching.
# v1.13: RunId/git gates + human-readable identity-mismatch guidance (measured: a human
# saw test GREEN but audit RED with "build-identity-mismatch" and could not act on it).
param([string]$RunId = $env:DEVOS_RUN_ID)

$ErrorActionPreference = "Continue"
function Write-ExclusiveText([string]$Path, [string]$Content) {
    if (Test-Path -LiteralPath $Path) { throw "evidence file already exists: $Path" }
    $tmp = "$Path.$PID.tmp"
    [IO.File]::WriteAllText($tmp, $Content, (New-Object Text.UTF8Encoding($false)))
    try { [IO.File]::Move($tmp, $Path) } catch { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue; throw }
}
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$hash = (git rev-parse HEAD).Trim()
if ($hash -notmatch '^[0-9a-f]{40}$') {
    Write-Output "audit RED: git is unavailable or not a repo (hash='$hash'). Fix git access first."
    exit 1
}
if ($RunId -notmatch '^run-\d{8}-\d{2,}$') {
    Write-Output "audit RED: RunId '$RunId' does not match run-YYYYMMDD-NN (e.g. run-20261010-01)."
    exit 1
}
$evDir = Join-Path $repo "build\evidence\current"
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
$fail = @()
$identity = $null

$skillScripts = @(
    "$env:USERPROFILE\.claude\skills\development-os-v1-7\scripts",
    "$env:USERPROFILE\.codex\skills\development-os-v1-7\scripts",
    "$env:USERPROFILE\.agents\skills\development-os-v1-7\scripts"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $RunId) { $fail += "run-id-required" }
if (-not $skillScripts) { $fail += "skill-scripts-missing" }
if ($skillScripts) { . (Join-Path (Split-Path $skillScripts) "templates\pipeline\Invoke-SkillPython.ps1") }

# The candidate gate calculates the identity internally. Audit trusts its JSON output,
# not a filename, log phrase, or short hash.
$gateJson = Join-Path $evDir ("worktree-audit-$hash-$RunId.json")
if ($skillScripts -and $RunId) {
    $wg = Invoke-SkillScript -ArgList @("$skillScripts\worktree-gate.py", "--repo", $repo, "--check",
        "--identity-path", "project-docs/project-manifest.yaml", "--out", $gateJson)
    if (Test-Path $gateJson) {
        try { $identity = Get-Content -LiteralPath $gateJson -Raw | ConvertFrom-Json }
        catch { $fail += "candidate-identity-json-invalid" }
    } else { $fail += "candidate-identity-missing" }
    if (-not $wg.Ok -or -not $identity -or $identity.worktree_state -ne "CLEAN") {
        $fail += "candidate-not-clean"
        Write-Output "audit BLOCKED: worktree is not clean. Offending files:"
        git status --porcelain | Select-Object -First 15 | ForEach-Object { Write-Output "  $_" }
        Write-Output "human next step: commit or ignore the files above, then rerun all three gates with the SAME RunId"
    }
}

function Read-Json([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    try { return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json }
    catch { return $null }
}

function Test-CandidateIdentity($record) {
    if (-not $record -or -not $identity) { return $false }
    if ($record.run_id -ne $RunId) { return $false }
    if ($record.candidate_commit -ne $hash) { return $false }
    if ($record.candidate_run_status -ne "CANDIDATE_VALID") { return $false }
    $ri = $record.candidate_identity
    if (-not $ri -or $ri.repo_root -ne $identity.repo_root) { return $false }
    if ($ri.branch -ne $identity.branch) { return $false }
    if ($ri.candidate_commit -ne $identity.candidate_commit) { return $false }
    if ($ri.worktree_state -ne "CLEAN") { return $false }
    if ($ri.worktree_diff_hash -ne $identity.worktree_diff_hash) { return $false }
    $manifestKey = "project-docs/project-manifest.yaml"
    if ($ri.manifest_hashes.$manifestKey -ne $identity.manifest_hashes.$manifestKey) { return $false }
    return $true
}

# Structured results are selected by full commit and the shared run id.
$testPath = Join-Path $evDir ("test-result-$hash-$RunId.json")
$buildPath = Join-Path $evDir ("build-result-$hash-$RunId.json")
$testResult = Read-Json $testPath
$buildResult = Read-Json $buildPath
if (-not (Test-CandidateIdentity $testResult)) { $fail += "test-identity-mismatch" }
elseif ($testResult.status -ne "GREEN") { $fail += "test-not-green" }
if (-not (Test-CandidateIdentity $buildResult)) { $fail += "build-identity-mismatch" }
elseif ($buildResult.status -ne "GREEN") { $fail += "build-not-green" }
# v1.13: identity mismatch is the single most confusing RED for a human -- translate it.
if ($fail | Where-Object { $_ -match 'identity-mismatch' }) {
    $mismatches = @($fail | Where-Object { $_ -match 'identity-mismatch' }) -join ', '
    Write-Output "audit explanation: build and test evidence must come from the SAME commit AND the SAME RunId as this audit."
    Write-Output "  mismatch reasons found: $mismatches"
    Write-Output "human next step: rerun build, test, audit in one go with a NEW RunId (run-YYYYMMDD-NN); partial reruns never satisfy the identity check"
}

# 项目定制：敏感路径来自 project-manifest.yaml，兼容迁移期 frozen_paths 别名。
$manifestPath = Join-Path $repo "project-docs\project-manifest.yaml"
$sensitivePaths = @()
foreach ($key in @("sensitive_paths", "frozen_paths")) {
    if ($sensitivePaths.Count -gt 0 -or -not (Test-Path $manifestPath)) { break }
    $inside = $false
    foreach ($line in (Get-Content -LiteralPath $manifestPath -Encoding UTF8)) {
        # 本项目 manifest 的 `sensitive_paths:` 行带行尾注释，故容错 `#...`
        if ($line -match ("^" + [regex]::Escape($key) + "\s*:\s*(?:#.*)?$")) { $inside = $true; continue }
        if ($inside -and $line -match '^\S') { $inside = $false }
        if ($inside -and $line -match '^\s*-\s*(.+?)\s*(?:#.*)?$') { $sensitivePaths += $matches[1].Trim().Trim('"').Trim("'") }
    }
}
if ($sensitivePaths.Count -eq 0) { $fail += "sensitive-paths-unfilled" }
$auditLog = Join-Path $evDir ("audit-$hash-$RunId.log")
$resultPath = Join-Path $evDir ("audit-result-$hash-$RunId.json")
function Write-AuditResult([string]$status, [string[]]$reasons) {
    $payload = [ordered]@{
        status = $status
        candidate_commit = $hash
        run_id = $RunId
        candidate_run_status = if ($status -eq "GREEN") { "CANDIDATE_VALID" } else { "BLOCKED" }
        candidate_identity = $identity
        findings = $reasons
    }
    $payload | ConvertTo-Json -Depth 12 | ForEach-Object { Write-ExclusiveText $resultPath $_ }
}

if ($fail.Count -gt 0) {
    Write-ExclusiveText $auditLog "audit RED: $($fail -join ', ')"
    Write-AuditResult "RED" $fail
    if ($skillScripts -and $RunId) {
        $mStamp = Get-Date -Format "yyyyMMdd-HHmmss"
        Invoke-SkillScript -ArgList @("$skillScripts\record-evidence.py", "--evidence-dir", "$evDir\manifest-audit-$hash-$RunId-$mStamp",
            "--repo", $repo, "--label", "audit", "--run-id", $RunId, $auditLog) | Out-Null
    }
    Write-Output "audit RED: $($fail -join ', ')"
    exit 1
}

Write-ExclusiveText $auditLog "audit GREEN: candidate=$hash run_id=$RunId"
Write-AuditResult "GREEN" @()
$mStamp = Get-Date -Format "yyyyMMdd-HHmmss"
Invoke-SkillScript -ArgList @("$skillScripts\record-evidence.py", "--evidence-dir", "$evDir\manifest-audit-$hash-$RunId-$mStamp",
    "--repo", $repo, "--label", "audit", "--run-id", $RunId, $auditLog) | Out-Null
Write-Output "audit PASS: candidate=$hash run_id=$RunId"
exit 0
