# pipeline/build.ps1 -- clean-candidate structured build gate.
# v1.13: RunId format gate + git-unavailable interception + human-readable BLOCKED reasons.
param([string]$Config = "Debug", [string]$RunId = $env:DEVOS_RUN_ID, [string]$Target = "all")

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
# v1.13: git-unavailable interception -- a short/empty hash used to flow into evidence
# filenames (measured: "build--<runid>.log" double dash) and mismatched identities later.
if ($hash -notmatch '^[0-9a-f]{40}$') {
    Write-Output "build RED: git is unavailable or not a repo (hash='$hash'). Fix git access first; do not run the gates."
    exit 1
}
# v1.13: RunId format gate -- agents invent formats (measured: "20261010-output-cleanup"),
# which breaks cross-script evidence matching. Canonical: run-YYYYMMDD-NN.
if ($RunId -notmatch '^run-\d{8}-\d{2,}$') {
    Write-Output "build RED: RunId '$RunId' does not match run-YYYYMMDD-NN (e.g. run-20261010-01). Use the canonical format in ALL three gates."
    exit 3
}
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$evDir = Join-Path $repo "build\evidence\current"
$out = Join-Path $repo "build\bin-$Config-$hash"
$log = Join-Path $evDir "build-$hash-$RunId.log"
$resultPath = Join-Path $evDir "build-result-$hash-$RunId.json"
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
New-Item -ItemType Directory -Force -Path $out | Out-Null
$skillScripts = @(
    "$env:USERPROFILE\.claude\skills\development-os-v1-7\scripts",
    "$env:USERPROFILE\.codex\skills\development-os-v1-7\scripts",
    "$env:USERPROFILE\.agents\skills\development-os-v1-7\scripts"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $RunId -or -not $skillScripts) {
    Write-ExclusiveText $log "build UNTESTED: run id or skill scripts missing"
    @{ status = "UNTESTED"; candidate_run_status = "BLOCKED"; reason = "run-id-or-skill-missing" } |
        ConvertTo-Json | ForEach-Object { Write-ExclusiveText $resultPath $_ }
    exit 3
}
# One interpreter decision for all skill scripts (see Invoke-SkillPython.ps1 for why).
. (Join-Path (Split-Path $skillScripts) "templates\pipeline\Invoke-SkillPython.ps1")

$identityPath = Join-Path $evDir "worktree-build-$hash-$RunId.json"
$wg = Invoke-SkillScript -ArgList @("$skillScripts\worktree-gate.py", "--repo", $repo, "--check",
    "--identity-path", "project-docs/project-manifest.yaml", "--out", $identityPath)
if (-not $wg.Ok) {
    Write-ExclusiveText $log "build BLOCKED: candidate worktree is not clean"
    @{ status = "UNTESTED"; candidate_run_status = "BLOCKED"; reason = "candidate-not-clean" } |
        ConvertTo-Json | ForEach-Object { Write-ExclusiveText $resultPath $_ }
    # v1.13: the human cannot act on "not clean" alone -- list the files and give the
    # standard regroup options (measured: three consecutive sessions stalled on hygiene).
    Write-Output "build BLOCKED: worktree is not clean. Offending files:"
    git status --porcelain | Select-Object -First 15 | ForEach-Object { Write-Output "  $_" }
    Write-Output "human next step: group them into logical commits, or move backups/runtime data out of the repo / into .gitignore, then rerun all three gates with the SAME RunId"
    exit 4
}
$identity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json

# 项目定制：Qt 5.14.2 + MinGW 7.3 固定工具链，避免 PATH 上的 Anaconda qmake。
$QTBIN = "C:\Qt\5.14.2\mingw73_64\bin"
$MINGWBIN = "C:\Qt\Tools\mingw730_64\bin"
$MINGWLIBEXEC = "C:\Qt\Tools\mingw730_64\libexec\gcc\x86_64-w64-mingw32\7.3.0"
$env:Path = "$QTBIN;$MINGWBIN;$MINGWLIBEXEC;$env:Path"
$mainRoot = (& git worktree list --porcelain | Select-Object -First 1) -replace '^worktree ', ''
foreach ($cfg in @("Client\connect.config", "Server\connect.config")) {
    $dst = Join-Path $repo $cfg
    if (Test-Path $dst) { continue }
    $srcCfg = Join-Path $mainRoot $cfg
    if (-not (Test-Path $srcCfg)) { "build RED: missing $cfg" | Add-Content $log; Write-Output "build RED: missing $cfg"; exit 1 }
    Copy-Item $srcCfg $dst -Force
    $hSrc = (Get-FileHash $srcCfg -Algorithm SHA256).Hash
    $hDst = (Get-FileHash $dst -Algorithm SHA256).Hash
    if ($hSrc -ne $hDst) { "build RED: config sync hash mismatch for $cfg" | Add-Content $log; exit 1 }
    "config sync: $cfg <- $mainRoot (sha256 $hDst)" | Add-Content $log
}
$targets = @("Client\Client.pro", "Server\Server.pro")
if ($Target -ne "all") { $targets = @($Target) }
$buildStart = Get-Date
$buildPassed = $true
foreach ($pro in $targets) {
    $name = [IO.Path]::GetFileNameWithoutExtension($pro)
    $src = Join-Path $repo $pro
    $bld = Join-Path $repo "build\build-$name"
    $artifact = Join-Path $repo "dist\$name.exe"
    New-Item -ItemType Directory -Force -Path $bld | Out-Null
    # 产物新鲜度（项目定制），两步：
    #
    # 为什么需要：qmake 会因 .pro 变化重写 Makefile，但 make 只比较对象文件的时间戳，于是打印
    # "Nothing to be done"、rc=0 —— 脚本记 GREEN，而 dist/ 里仍是上一次编译的二进制。实测
    # 2026-10-11：只加 CONFIG += console 后 build 报 GREEN，而 dist/Server.exe 仍是 GUI 子系统
    # （PE subsystem=2），整个 enforced 门禁据此失真。
    #
    # 步骤 1（自愈）：产物存在但比 .pro 旧就先删掉，逼 make 重链。只报 RED 不够 —— 报 RED 只把
    # 问题推给人，删掉目标则链接必然发生，GREEN 才重新等于「本次确实链过」。
    #
    # 判据为什么用「产物 vs .pro」而不是「产物 vs 本次 build 开始时刻」：后者有两种必然误报 ——
    # （a）docs-only 提交按规则重跑门禁时源码没变，make 正确地什么都没做，产物自然比 buildStart 旧；
    # （b）git checkout/merge 会重写 .pro 的 mtime 而内容不变（实测：Server.pro mtime 被 merge 改到
    # 21:58，内容自 4e76269@21:29 起未变）。用产物 vs .pro 并配合步骤 1，这三种情形都得到正确结果，
    # 且 buildStart 对比仍作为「本次是否重链」的说明写进日志。
    $proTime = (Get-Item -LiteralPath $src).LastWriteTime
    if ((Test-Path -LiteralPath $artifact) -and
            (Get-Item -LiteralPath $artifact).LastWriteTime -lt $proTime) {
        Remove-Item -LiteralPath $artifact -Force
        "build NOTE: $name removed stale artifact (older than $pro) to force the link" | Add-Content $log
    }
    Push-Location $bld
    cmd /c "`"$QTBIN\qmake.exe`" `"$src`" 2>&1" *>> $log
    $qmakeRc = $LASTEXITCODE
    if ($qmakeRc -eq 0) { cmd /c "`"$MINGWBIN\mingw32-make.exe`" -j4 2>&1" *>> $log }
    $makeRc = $LASTEXITCODE
    Pop-Location
    if ($qmakeRc -ne 0 -or $makeRc -ne 0) { "build RED: $name qmake=$qmakeRc make=$makeRc" | Add-Content $log; $buildPassed = $false; break }
    # 步骤 2（断言）：链接后产物必须存在且不早于 .pro，否则 build RED。
    if (-not (Test-Path -LiteralPath $artifact)) {
        "build RED: $name artifact missing after make: $artifact" | Add-Content $log
        Write-Output "build RED: $name artifact missing after make: $artifact"
        $buildPassed = $false; break
    }
    $artifactTime = (Get-Item -LiteralPath $artifact).LastWriteTime
    if ($artifactTime -lt $proTime) {
        "build RED: $name stale artifact: $artifact ($artifactTime) older than $pro ($proTime)" | Add-Content $log
        Write-Output "build RED: $name stale artifact: $artifact is older than $pro"
        $buildPassed = $false; break
    }
    if ($artifactTime -lt $buildStart) {
        "build NOTE: $name not relinked this run (inputs unchanged); artifact=$artifactTime buildStart=$buildStart" | Add-Content $log
    }
    "build OK: $name -> $bld" | Add-Content $log
}
$status = if ($buildPassed) { "GREEN" } else { "UNTESTED" }
$result = [ordered]@{
    status = $status
    candidate_commit = $hash
    run_id = $RunId
    candidate_run_status = "CANDIDATE_VALID"
    candidate_identity = $identity
    config = $Config
    artifact_root = $out
}
$result | ConvertTo-Json -Depth 12 | ForEach-Object { Write-ExclusiveText $resultPath $_ }
if (-not (Test-Path -LiteralPath $log)) { Write-ExclusiveText $log "# build run: hash=$hash run_id=$RunId config=$Config" }
"build ${status}: candidate=$hash run_id=$RunId" | Add-Content $log
Invoke-SkillScript -ArgList @("$skillScripts\record-evidence.py", "--evidence-dir", "$evDir\manifest-build-$hash-$RunId",
    "--repo", $repo, "--label", "build", "--run-id", $RunId, $log) | Out-Null

if ($status -eq "GREEN") { exit 0 }
exit 3
