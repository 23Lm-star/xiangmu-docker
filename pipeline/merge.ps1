# pipeline/merge.ps1 -- mandatory structured merge entry gate.
param(
    [Parameter(Mandatory = $true)][string]$Handoff,
    [Parameter(Mandatory = $true)][string]$AuditPackage,
    [Parameter(Mandatory = $true)][string]$ApprovalRecord,
    [string]$Verifier = $env:DEVOS_APPROVAL_VERIFIER
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$candidateRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$skillRoots = @(
    $candidateRoot,
    "$env:USERPROFILE\.claude\skills\development-os-v1-7",
    "$env:USERPROFILE\.codex\skills\development-os-v1-7",
    "$env:USERPROFILE\.agents\skills\development-os-v1-7"
)
$skillRoot = $skillRoots | Where-Object { Test-Path (Join-Path $_ "scripts\merge-gate.py") } | Select-Object -First 1
$skillScripts = if ($skillRoot) { Join-Path $skillRoot "scripts" } else { $null }
if (-not $skillScripts) { throw "development-os-v1-7 scripts not found" }
if (-not $Verifier) {
    $bundledVerifier = $skillRoots |
        ForEach-Object { Join-Path $_ "scripts\local-approval-verifier.py" } |
        Where-Object { Test-Path $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not (Test-Path $bundledVerifier -PathType Leaf)) {
        throw "local-personal approval verifier is missing; set -Verifier or DEVOS_APPROVAL_VERIFIER"
    }
    $Verifier = $bundledVerifier
}
# One interpreter decision for all skill scripts (see Invoke-SkillPython.ps1 for why).
. (Join-Path $skillRoot "templates\pipeline\Invoke-SkillPython.ps1")
$candidate = (git -C $repo rev-parse HEAD).Trim()
$mg = Invoke-SkillScript -ArgList @("$skillScripts\merge-gate.py", "--handoff", $Handoff, `
    "--candidate-commit", $candidate, "--audit", $AuditPackage, "--approval", $ApprovalRecord, "--verifier", $Verifier)
$mg.Output | ForEach-Object { Write-Output $_ }
if (-not $mg.Ok) { throw "merge gate rejected candidate" }
Write-Output "merge gate passed; invoke the repository's actual merge command now"
