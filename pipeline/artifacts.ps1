# pipeline/artifacts.ps1 -- record where the deployed binaries came from.
#
# A binary is compiled from ONE checkout. The directory a user actually runs from is
# gitignored and owned by nobody: build.ps1 stops in build/, and dist/ (or deploy/, or an
# install prefix) is written by hand. So nothing on screen says which commit the exe in front
# of you was compiled from -- and every conclusion drawn from running it is a guess.
#
# Measured, on one project: the main worktree sat on a feature branch while a fix had merged
# to master in another worktree. A rebuild in the main worktree overwrote dist/ with binaries
# that did not contain the fix, and the owner concluded three times in a row that the fix had
# not been made, or had not been merged. It had been both.
#
# This script closes that loop. Two halves:
#   artifacts.ps1 -Deploy <dir>   after a build: copy into the deploy dir AND record provenance
#   artifacts.ps1 -Verify         at session opening: is what I run still what HEAD builds?
#
# PS 5.1 traps (same as the other gate scripts):
#   $ErrorActionPreference = "Continue"; judge by exit codes only. Repo root from
#   $PSScriptRoot, NOT `git rev-parse --show-toplevel` (console-codepath mojibake on non-ASCII
#   paths).
param(
  [string]$Deploy = "",   # e.g. "dist" -- deploy target, repo-root relative
  [switch]$Verify,        # check-only mode, called by state.ps1
  [string[]]$Binary = @() # explicit binaries; default = every exe under build/build-*/
)
$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$skillScripts = @(
    "$env:USERPROFILE\.claude\skills\development-os-v1-7\scripts",
    "$env:USERPROFILE\.codex\skills\development-os-v1-7\scripts",
    "$env:USERPROFILE\.agents\skills\development-os-v1-7\scripts"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $skillScripts) {
    Write-Output "artifacts: development-os-v1-7 scripts not found - provenance NOT recorded"
    exit 0
}
# One interpreter decision for all skill scripts (see Invoke-SkillPython.ps1 for why).
. (Join-Path (Split-Path $skillScripts) "templates\pipeline\Invoke-SkillPython.ps1")
$tracer = Join-Path $skillScripts "artifact-trace.py"

# --- Verify mode: read-only, safe to call from state.ps1 on every session opening ---
if ($Verify) {
    $v = Invoke-SkillScript -ArgList @($tracer, "verify", "--repo", $repo)
    # Never fails a session -- it is information. STALE lines are printed so the
    # "the fix didn't work" misdiagnosis cannot come back quietly.
    if ($v.Output.Count -gt 0) { $v.Output | ForEach-Object { Write-Output "artifacts: $_" } }
    exit 0
}

if (-not $Deploy) {
    Write-Output "artifacts: nothing to do. Use -Deploy <dir> after a build, or -Verify to check."
    exit 0
}

# --- Deploy mode: copy the fresh build into the run directory, then record provenance ---
$deployDir = Join-Path $repo $Deploy
if (-not (Test-Path $deployDir)) {
    New-Item -ItemType Directory -Force -Path $deployDir | Out-Null
}

# Find the freshly built binaries. Shadow builds put them under build/build-<Target>/...
# Generic discovery: every exe under build/build-*. A project with helper tools it does
# not deploy passes -Binary with the explicit list instead.
$built = @()
if ($Binary.Count -gt 0) {
    $built = $Binary
} else {
    foreach ($d in (Get-ChildItem (Join-Path $repo "build") -Directory -Filter "build-*" -ErrorAction SilentlyContinue)) {
        foreach ($exe in (Get-ChildItem $d.FullName -Recurse -Filter "*.exe" -File -ErrorAction SilentlyContinue)) {
            $built += $exe.FullName
        }
    }
}
if ($built.Count -eq 0) {
    Write-Output "artifacts: no built binaries found under build/build-*; run build first"
    exit 0
}

$branch = git rev-parse --abbrev-ref HEAD
$commit = git rev-parse --short HEAD
$argsList = @($tracer, "record", "--repo", $repo, "--deploy-dir", $Deploy,
              "--branch", $branch, "--commit", $commit)

$copied = @()
foreach ($src in $built) {
    $name = Split-Path -Leaf $src
    $dst = Join-Path $deployDir $name
    try {
        # A running binary cannot be overwritten on Windows -- that is a real failure mode,
        # not a hypothetical one, so it is reported rather than swallowed.
        Copy-Item $src $dst -Force
        $copied += $name
    } catch {
        Write-Output "artifacts: could not deploy $name -- is it still running?"
    }
}
if ($copied.Count -eq 0) {
    Write-Output "artifacts: nothing deployed; provenance not recorded"
    exit 0
}

# Side-by-side DLLs the deployed app needs at runtime, when they sit next to the built exe.
# 项目定制：从固定 Qt 5.14.2 工具链补齐运行时 DLL，避免 shadow build 携带旧 Qt 版本。
$qtBin = "C:\Qt\5.14.2\mingw73_64\bin"
$qtRuntime = @("Qt5Core.dll", "Qt5Gui.dll", "Qt5Network.dll", "Qt5Sql.dll",
              "Qt5Svg.dll", "Qt5Widgets.dll", "Qt5Concurrent.dll")
foreach ($dllName in $qtRuntime) {
    $qtDll = Join-Path $qtBin $dllName
    if (Test-Path $qtDll) {
        try { Copy-Item $qtDll (Join-Path $deployDir $dllName) -Force } catch { }
    }
}
foreach ($name in $copied) {
    $srcExe = $built | Where-Object { (Split-Path -Leaf $_) -eq $name } | Select-Object -First 1
    $srcDir = Split-Path -Parent $srcExe
    # 只在部署目录缺失该 DLL 时补，绝不覆盖。为什么：源 exe 目录可能是遗留的别的 flavor
    # （build/build-Server-diag/debug、build/build-Client/release 等），它旁边的 DLL 可能是
    # 完全不同版本的 Qt。实测 2026-10-11：这里把 2018-04-25 版 Qt5Core/Gui/Network/Widgets/
    # Svg + libEGL/libGLESv2/d3dcompiler_47/libmysql 共 9 个 DLL 覆盖进 dist，14 套全部
    # 「Server.exe 在监听 5000 之前退出」，只能按 canonical 目录手工还原。
    # 部署目录的 Qt 版本由上面「固定 Qt 5.14.2 工具链补齐」那一段决定，本段只做缺件补齐。
    foreach ($dll in (Get-ChildItem $srcDir -Filter "*.dll" -File -ErrorAction SilentlyContinue)) {
        $dllDst = Join-Path $deployDir $dll.Name
        if (Test-Path -LiteralPath $dllDst) { continue }
        try { Copy-Item $dll.FullName $dllDst -Force } catch { }
    }
}

foreach ($name in $copied) {
    $rel = "$($Deploy.Replace('\','/'))/$name"
    $argsList += @("--binary", $rel)
}
$r = Invoke-SkillScript -ArgList $argsList
if ($r.Output.Count -gt 0) { $r.Output | ForEach-Object { Write-Output "artifacts: $_" } }
Write-Output "artifacts: deployed $($copied.Count) binary/binaries to $Deploy @ $commit"
exit 0
