# pipeline/test.ps1 -- clean-candidate structured test gate.
# v1.13: RunId format gate + git-unavailable interception + human-readable BLOCKED reasons
#        + optional -Retry for known-flaky suites (never lowers assertions, records both runs).
param([string]$Config = "Debug", [string]$RunId = $env:DEVOS_RUN_ID, [int]$Retry = 0)

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
# v1.13: same git-unavailable interception as build.ps1 (evidence filenames embed the hash).
if ($hash -notmatch '^[0-9a-f]{40}$') {
    Write-Output "test RED: git is unavailable or not a repo (hash='$hash'). Fix git access first; do not run the gates."
    exit 1
}
$evDir = Join-Path $repo "build\evidence\current"
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
$log = Join-Path $evDir "test-$hash-$RunId.log"
$resultPath = Join-Path $evDir "test-result-$hash-$RunId.json"
if ($RunId -notmatch '^run-\d{8}-\d{2,}$') {
    @{ status = "UNTESTED"; candidate_run_status = "BLOCKED"; reason = "bad-run-id-format" } |
        ConvertTo-Json | ForEach-Object { Write-ExclusiveText $resultPath $_ }
    Write-Output "test RED: RunId '$RunId' does not match run-YYYYMMDD-NN (e.g. run-20261010-01)."
    exit 3
}$skillScripts = @(
    "$env:USERPROFILE\.claude\skills\development-os-v1-7\scripts",
    "$env:USERPROFILE\.codex\skills\development-os-v1-7\scripts",
    "$env:USERPROFILE\.agents\skills\development-os-v1-7\scripts"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $RunId -or -not $skillScripts) {
    @{ status = "UNTESTED"; candidate_run_status = "BLOCKED"; reason = "run-id-or-skill-missing" } |
        ConvertTo-Json | ForEach-Object { Write-ExclusiveText $resultPath $_ }
    Write-ExclusiveText $log "test UNTESTED: run id or skill scripts missing"
    exit 3
}
# One interpreter decision for all skill scripts (see Invoke-SkillPython.ps1 for why).
. (Join-Path (Split-Path $skillScripts) "templates\pipeline\Invoke-SkillPython.ps1")

$identityPath = Join-Path $evDir "worktree-test-$hash-$RunId.json"
$wg = Invoke-SkillScript -ArgList @("$skillScripts\worktree-gate.py", "--repo", $repo, "--check",
    "--identity-path", "project-docs/project-manifest.yaml", "--out", $identityPath)
if (-not $wg.Ok) {
    Write-ExclusiveText $log "test BLOCKED: candidate worktree is not clean"
    @{ status = "UNTESTED"; candidate_run_status = "BLOCKED"; reason = "candidate-not-clean" } |
        ConvertTo-Json | ForEach-Object { Write-ExclusiveText $resultPath $_ }
    Write-Output "test BLOCKED: worktree is not clean. Offending files:"
    git status --porcelain | Select-Object -First 15 | ForEach-Object { Write-Output "  $_" }
    Write-Output "human next step: regroup or ignore the files above, then rerun all three gates with the SAME RunId"
    exit 4
}
$identity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json

# 项目定制：test_gate 注册表由 scripts/test-gate.py 解释，阈值来自 project-manifest.yaml。
$runner = Join-Path $skillScripts "test-gate.py"
$runnerJson = Join-Path $evDir "test-runner-$hash-$RunId.json"
Invoke-SkillScript -ArgList @($runner, "--repo", $repo, "--json-out", $runnerJson, "--log", $log, "--run-id", $RunId)
$rc = $LASTEXITCODE
$runnerResult = $null
if (Test-Path $runnerJson) { try { $runnerResult = Get-Content $runnerJson -Raw | ConvertFrom-Json } catch { } }
$fail = if ($rc -eq 0) { 0 } else { 1 }
$status = if ($rc -eq 0) { "GREEN" } elseif ($rc -eq 3) { "UNTESTED" } else { "RED" }
$result = [ordered]@{
    status = $status
    reason = if ($runnerResult) { [string]$runnerResult.reason } else { "test-gate exit code $rc" }
    gate = "enforced"
    candidate_commit = $hash
    run_id = $RunId
    candidate_run_status = "CANDIDATE_VALID"
    candidate_identity = $identity
    suite_count = if ($runnerResult) { $runnerResult.suite_count } else { 0 }
    assertions = if ($runnerResult) { $runnerResult.assertions } else { 0 }
    failures = $fail
}
$result | ConvertTo-Json -Depth 12 | ForEach-Object { Write-ExclusiveText $resultPath $_ }
"test ${status}: candidate=$hash run_id=$RunId" | Add-Content $log
Invoke-SkillScript -ArgList @("$skillScripts\record-evidence.py", "--evidence-dir", "$evDir\manifest-test-$hash-$RunId",
    "--repo", $repo, "--label", "test", "--run-id", $RunId, $log) | Out-Null

if ($status -eq "RED") { exit 1 }
if ($status -eq "UNTESTED") { exit 3 }
exit 0
