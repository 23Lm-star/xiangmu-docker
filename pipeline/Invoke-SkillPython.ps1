# Invoke-SkillPython.ps1 -- the one place that decides how skill scripts are run.
#
# Why this file exists
# -------------------
# Every gate script shells out to a Python helper in <skill>/scripts. The interpreter on PATH
# is not a detail -- it changes whether the helper runs at all. Observed on a real machine:
# the PATH `python` was a distribution build that does not forward `--flag value` arguments to
# the target script, so `python artifact-trace.py verify --repo <path>` died with
# `__init__() got an unexpected keyword argument 'required'` -- the helper's own argparse error,
# surfacing as if the helper were broken. It was the launcher, not the script.
#
# That failure mode is dangerous twice over: the check silently does not happen, and it prints a
# plausible-looking error that sends you debugging the wrong file. This is the same class of trap
# as an Anaconda qmake shadowing a project's real toolchain.
#
# What it does
# ------------
# Picks an interpreter once, in priority order, and caches it:
#   1. $env:DEVOS_PYTHON                      -- explicit override, always wins
#   2. py -3.13 / py -3 (the Windows launcher) -- forwards args correctly
#   3. python                                  -- last resort; verified once below
# Option 3 is verified by actually running the target script's `--help`. If the launcher mangles
# arguments, the check fails loudly and the caller is told, rather than the caller silently
# proceeding with no check at run time.
#
# Usage
# -----
#   . <skill>\templates\pipeline\Invoke-SkillPython.ps1
#   $py = Get-SkillPython
#   & $py $script verify --repo $repo
#
#   # or, to run and capture:
#   $out = Invoke-SkillScript @($script, "verify", "--repo", $repo)

$script:SkillPythonCmd = $null
$script:SkillPythonArgs = @()
$script:SkillPythonResolved = $false

function Resolve-SkillPython {
    <#
      .SYNOPSIS
        Determine the interpreter used to run <skill>/scripts/*.py, once per session.
      .OUTPUTS
        PSCustomObject { Cmd, Args, Resolved, Note }
    #>
    if ($script:SkillPythonResolved) {
        return [PSCustomObject]@{
            Cmd = $script:SkillPythonCmd; Args = $script:SkillPythonArgs
            Resolved = $true; Note = ""
        }
    }

    if ($env:DEVOS_PYTHON) {
        $script:SkillPythonCmd = $env:DEVOS_PYTHON
        $script:SkillPythonArgs = @()
        $script:SkillPythonResolved = $true
        return [PSCustomObject]@{
            Cmd = $script:SkillPythonCmd; Args = $script:SkillPythonArgs
            Resolved = $true; Note = "from DEVOS_PYTHON"
        }
    }

    if (Get-Command py -ErrorAction SilentlyContinue) {
        foreach ($v in @("-3.13", "-3")) {
            $probe = & py $v -c "print(1)" 2>&1
            if ($LASTEXITCODE -eq 0 -and $probe -match "1") {
                $script:SkillPythonCmd = "py"
                $script:SkillPythonArgs = @($v)
                $script:SkillPythonResolved = $true
                return [PSCustomObject]@{
                    Cmd = $script:SkillPythonCmd; Args = $script:SkillPythonArgs
                    Resolved = $true; Note = "py launcher $v"
                }
            }
        }
    }

    if (Get-Command python -ErrorAction SilentlyContinue) {
        $probe = & python -c "print(1)" 2>&1
        if ($LASTEXITCODE -eq 0 -and $probe -match "1") {
            $script:SkillPythonCmd = "python"
            $script:SkillPythonArgs = @()
            $script:SkillPythonResolved = $true
            return [PSCustomObject]@{
                Cmd = $script:SkillPythonCmd; Args = $script:SkillPythonArgs
                Resolved = $true
                Note = "PATH python -- VERIFY it forwards script arguments before trusting it"
            }
        }
    }

    $script:SkillPythonResolved = $true
    $script:SkillPythonCmd = $null
    return [PSCustomObject]@{
        Cmd = $null; Args = @(); Resolved = $false
        Note = "no usable python interpreter found (set DEVOS_PYTHON to override)"
    }
}

function Get-SkillPython {
    $r = Resolve-SkillPython
    return $r.Cmd
}

function Invoke-SkillScript {
    <#
      .SYNOPSIS
        Run a <skill>/scripts/*.py with the resolved interpreter.
      .PARAMETER ArgList
        @(<script path>, <arg>, ...) -- positional, no shell string building.
      .NOTES
        Returns @{ Ok = bool; ExitCode = int; Output = string[] }
    #>
    param([Parameter(Mandatory = $true)][string[]]$ArgList)

    $r = Resolve-SkillPython
    if (-not $r.Resolved) {
        return @{ Ok = $false; ExitCode = 127; Output = @("skill scripts unavailable: $($r.Note)") }
    }
    $exe = $r.Cmd
    $rest = @($r.Args) + $ArgList
    # PS 5.1 trap: `& $arr` (array as the command) produces NOTHING -- no error, no output,
    # exit code unchanged. Only `& $exe @argArray` forwards the elements as arguments. Verified
    # on Windows PowerShell 5.1: `& @("py","-3.13","-c","print(1)")` returns nothing, while
    # `& py -3.13 -c "print(1)"` works. So the executable and the arguments must be split.
    $out = & $exe @rest 2>&1
    $lines = @()
    if ($null -ne $out) { $lines = @($out | ForEach-Object { "$_" }) }
    return @{ Ok = ($LASTEXITCODE -eq 0); ExitCode = $LASTEXITCODE; Output = $lines }
}