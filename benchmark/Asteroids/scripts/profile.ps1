<#
.SYNOPSIS
Captures a CPU sampling profile of one benchmark configuration for N seconds.

.DESCRIPTION
Starts the configuration in benchmark mode, waits for start-up and warmup, samples for -Seconds, stops the
capture and lets the process finish. Output: results/<session>/profiles/
    <config>.etl           (wpr)      open with Windows Performance Analyzer
    <config>.diagsession   (vsdiag)   open with Visual Studio 2022
    <config>.profile.json  what was captured, how and when
    <config>_run/          run.json, frames.csv, stdout/stderr of the profiled process (NOT a measurement:
                           the sampling perturbs the timings)

Tools
    wpr     Windows Performance Recorder, built-in "CPU" profile. Needs an ELEVATED shell. The script does
            not try to elevate itself: in a non-elevated shell it stops with an error.
    vsdiag  VSDiagnostics.exe of Visual Studio 2022 (CpuUsageHigh agent configuration), attached to the
            process. Works without elevation.
    auto    wpr when the shell is elevated, otherwise vsdiag when Visual Studio has it, otherwise an error.

Symbols: the Release build links with /DEBUG:FULL, the .pdb files are next to the executables.

.EXAMPLE
scripts\profile.ps1 -Renderer nvrhi -Api d3d12 -Binding tex_mut -Threads 1 -Seconds 20 -Session full-01
.EXAMPLE
scripts\profile.ps1 -Renderer native -Api d3d12 -Threads 8 -Seconds 20 -Session full-01 -Tool wpr    # elevated shell
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('native', 'nvrhi')][string]$Renderer,
    [Parameter(Mandatory = $true)][ValidateSet('d3d11', 'd3d12', 'vk')][string]$Api,
    [ValidateSet('', 'original', 'dyn', 'mut', 'tex_mut', 'tex_mut_pc', 'bindless')][string]$Binding = '',
    [int]$Threads = 1,
    [int]$Seconds = 20,                     # length of the capture
    [double]$Warmup = 5,                    # benchmark warmup before the capture starts
    [double]$StartupSeconds = 8,            # allowance for process start and scene creation before the warmup clock runs
    [ValidateSet('auto', 'wpr', 'vsdiag')][string]$Tool = 'auto',
    [string]$Session,                       # default: profile-<yyyyMMdd-HHmmss>
    [string]$ResultsRoot,                   # default: <repo>\results
    [string]$BinDir,                        # default: <ethereal>\build\bin
    [string]$Exe,                           # default: asteroids_nvrhi.exe for nvrhi, else asteroids_native.exe
    [string]$SelftestExe,
    [uint64]$AdapterLuid = 0,               # 0 = look up -AdapterName
    [string]$AdapterName = 'RTX 4050',
    [int]$Width = 1080,
    [int]$Height = 720,
    [string]$WprProfile = 'CPU',
    [string]$VsDiagnosticsExe,              # default: found through vswhere
    [string]$VsAgentConfig = 'CpuUsageHigh.json',
    [string[]]$ExtraArgs
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent           # benchmark/Asteroids
$ethereal = (Resolve-Path (Join-Path $root '..\..\..')).Path   # the ethereal repo root (build\bin lives there)

function Write-Utf8([string]$Path, [string]$Text) { [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding $false)) }
function Format-Arg([string]$a) { if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a } }
function Test-Elevated {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object System.Security.Principal.WindowsPrincipal $id).IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Invoke-Native([string]$File, [string[]]$NativeArgs) {
    # Returns @{ code; output }. stderr must not become a terminating error (PowerShell 5.1).
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        $out = & $File @NativeArgs 2>&1 | ForEach-Object { "$_" }
        return @{ code = $LASTEXITCODE; output = (($out | Out-String).Trim()) }
    } finally { $ErrorActionPreference = $old }
}
function Find-VsDiagnostics {
    $rel = 'Team Tools\DiagnosticsHub\Collector\VSDiagnostics.exe'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        foreach ($p in @(& $vswhere -all -products * -version '[17.0,18.0)' -property installationPath)) {
            if ($p -and (Test-Path -LiteralPath (Join-Path $p $rel))) { return (Join-Path $p $rel) }
        }
    }
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        $hit = Get-ChildItem (Join-Path $base "Microsoft Visual Studio\2022\*\$rel") -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}
function Stop-Tree($Process) {
    if ($Process -and -not $Process.HasExited) {
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try { & taskkill.exe /PID $Process.Id /T /F 2>$null | Out-Null } catch {} finally { $ErrorActionPreference = $old }
        try { [void]$Process.WaitForExit(10000) } catch {}
    }
}

# ---------------------------------------------------------------------------------------------- configuration

if ($Seconds -lt 1) { throw '-Seconds must be at least 1.' }
if ($Renderer -eq 'native') {
    if ($Binding -and $Binding -ne 'original') { throw "The native renderer has one fixed strategy; omit -Binding." }
    if ($Api -eq 'vk') { throw 'N/A: there is no native Vulkan renderer.' }
    $Binding = 'original'
} else {
    if (-not $Binding -or $Binding -eq 'original') { throw "-Binding is required for renderer $Renderer (dyn, mut, tex_mut, tex_mut_pc, bindless)." }
    if ($Renderer -eq 'nvrhi' -and $Binding -eq 'dyn') { throw 'N/A: dyn is not expressible in nvrhi.' }
    if ($Binding -eq 'bindless' -and $Api -eq 'd3d11') { throw 'N/A: bindless requires d3d12 or vk.' }
}

if (-not $ResultsRoot) { $ResultsRoot = Join-Path $root 'results' }
if (-not $BinDir) { $BinDir = Join-Path $ethereal 'build\bin' }
if (-not $Exe) { if ($Renderer -eq 'nvrhi') { $Exe = Join-Path $BinDir 'asteroids_nvrhi.exe' } else { $Exe = Join-Path $BinDir 'asteroids_native.exe' } }
if (-not $SelftestExe) { $SelftestExe = Join-Path $BinDir 'benchmark_selftest.exe' }
if (-not $Session) { $Session = 'profile-' + (Get-Date).ToString('yyyyMMdd-HHmmss') }
if (-not (Test-Path -LiteralPath $Exe)) { throw "Executable not found: $Exe" }
$Exe = [System.IO.Path]::GetFullPath($Exe)

# ---------------------------------------------------------------------------------------------- tool selection

$elevated = Test-Elevated
$wpr = (Get-Command wpr.exe -ErrorAction SilentlyContinue).Source
if (-not $VsDiagnosticsExe) { $VsDiagnosticsExe = Find-VsDiagnostics }
$haveVs = ($VsDiagnosticsExe -and (Test-Path -LiteralPath $VsDiagnosticsExe))

if ($Tool -eq 'wpr') {
    if (-not $wpr) { throw 'wpr.exe was not found on PATH.' }
    if (-not $elevated) {
        throw 'wpr needs an elevated shell and this one is not elevated. Start PowerShell with "Run as administrator" and run the same command again, or use -Tool vsdiag (no elevation needed).'
    }
} elseif ($Tool -eq 'vsdiag') {
    if (-not $haveVs) { throw 'VSDiagnostics.exe of Visual Studio 2022 was not found (pass -VsDiagnosticsExe), and -Tool vsdiag was requested.' }
} else {
    if ($elevated -and $wpr) { $Tool = 'wpr' }
    elseif ($haveVs) {
        $Tool = 'vsdiag'
        Write-Host 'This shell is not elevated, so wpr cannot be used: capturing with VSDiagnostics.exe instead.'
    } else {
        throw 'No usable profiler: wpr needs an elevated shell (start PowerShell with "Run as administrator"), and VSDiagnostics.exe of Visual Studio 2022 was not found.'
    }
}
$agentConfig = $null
if ($Tool -eq 'vsdiag') {
    $agentConfig = $VsAgentConfig
    if (-not (Test-Path -LiteralPath $agentConfig)) { $agentConfig = Join-Path (Join-Path (Split-Path $VsDiagnosticsExe -Parent) 'AgentConfigs') $VsAgentConfig }
    if (-not (Test-Path -LiteralPath $agentConfig)) { throw "VSDiagnostics agent configuration not found: $VsAgentConfig" }
}

# Never two GPU benchmark processes at once.
$busy = @(Get-Process -Name 'asteroids_native', 'asteroids_nvrhi', ([System.IO.Path]::GetFileNameWithoutExtension($Exe)) -ErrorAction SilentlyContinue)
if ($busy.Count) { throw "A benchmark process is already running ($(($busy | ForEach-Object { "$($_.Name) pid $($_.Id)" }) -join ', '))." }

if ($AdapterLuid -eq 0) {
    if (-not (Test-Path -LiteralPath $SelftestExe)) { throw "Cannot look up the adapter: $SelftestExe is missing. Pass -AdapterLuid." }
    $hit = @(& $SelftestExe -list_adapters | Where-Object { "$_".Split("`t").Count -ge 4 -and "$_".Split("`t")[3] -like "*$AdapterName*" })
    if ($hit.Count -ne 1) { throw "-AdapterName '$AdapterName' matches $($hit.Count) adapters. Pass -AdapterLuid." }
    $AdapterLuid = [uint64]($hit[0].Split("`t")[0])
}

# ---------------------------------------------------------------------------------------------- run

$config = '{0}_{1}_{2}_t{3:d2}' -f $Renderer, $Api, $Binding, $Threads
$profileDir = Join-Path (Join-Path $ResultsRoot $Session) 'profiles'
$runDir = Join-Path $profileDir "${config}_run"
if (Test-Path -LiteralPath $runDir) { Remove-Item -LiteralPath $runDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$ext = 'etl'; if ($Tool -eq 'vsdiag') { $ext = 'diagsession' }
$tracePath = Join-Path $profileDir "$config.$ext"
if (Test-Path -LiteralPath $tracePath) { Remove-Item -LiteralPath $tracePath -Force }

# The measured window must outlast the capture: start-up allowance + capture + time to stop the collector.
$duration = $StartupSeconds + $Seconds + 20
$argList = @('-benchmark', '-renderer', $Renderer, '-api', $Api)
if ($Renderer -ne 'native') { $argList += @('-binding', $Binding) }
$argList += @('-threads', "$Threads", '-warmup', "$Warmup", '-duration', "$duration", '-window', "$Width", "$Height",
              '-adapter_luid', "$AdapterLuid", '-output', $runDir)
$argList += @($ExtraArgs | Where-Object { $_ } | ForEach-Object { $_ -split ',' })
$argString = ($argList | ForEach-Object { Format-Arg $_ }) -join ' '

Write-Host "Profiling $config for $Seconds s with $Tool -> $tracePath"
$process = $null
$capturing = $false
$vsSession = Get-Random -Minimum 1 -Maximum 255
$captureStart = $null; $captureStop = $null; $exitedEarly = $false
try {
    $process = Start-Process -FilePath $Exe -ArgumentList $argString -WorkingDirectory (Split-Path $Exe -Parent) `
        -RedirectStandardOutput (Join-Path $runDir 'stdout.txt') -RedirectStandardError (Join-Path $runDir 'stderr.txt') -NoNewWindow -PassThru
    $null = $process.Handle

    # Start-up and warmup run unprofiled; the capture covers steady-state frames only.
    if ($process.WaitForExit([int](($StartupSeconds + $Warmup) * 1000))) {
        $why = ''
        $j = Join-Path $runDir 'run.json'
        if (Test-Path -LiteralPath $j) { try { $why = (Get-Content -LiteralPath $j -Raw | ConvertFrom-Json).reason } catch {} }
        if (-not $why) { $why = (@(Get-Content -LiteralPath (Join-Path $runDir 'stderr.txt') -Tail 3 -ErrorAction SilentlyContinue) -join ' | ') }
        throw "The benchmark process exited (code $($process.ExitCode)) before the capture started: $why"
    }

    if ($Tool -eq 'wpr') {
        $r = Invoke-Native $wpr @('-start', $WprProfile, '-filemode')
        if ($r.code -ne 0) { throw "wpr -start $WprProfile failed (exit $($r.code)): $($r.output). If another recording is active, stop it with 'wpr -cancel' first." }
    } else {
        $r = Invoke-Native $VsDiagnosticsExe @('start', "$vsSession", "/attach:$($process.Id)", "/loadConfig:$agentConfig")
        if ($r.code -ne 0) { throw "VSDiagnostics start failed (exit $($r.code)): $($r.output)" }
    }
    $capturing = $true
    $captureStart = [DateTime]::UtcNow
    $exitedEarly = $process.WaitForExit($Seconds * 1000)
    $captureStop = [DateTime]::UtcNow

    if ($Tool -eq 'wpr') { $r = Invoke-Native $wpr @('-stop', $tracePath, "asteroids benchmark $config") }
    else { $r = Invoke-Native $VsDiagnosticsExe @('stop', "$vsSession", "/output:$tracePath") }
    $capturing = $false
    if ($r.code -ne 0) { throw "Stopping the $Tool capture failed (exit $($r.code)): $($r.output)" }
    if ($exitedEarly) { throw "The benchmark process exited (code $($process.ExitCode)) during the capture; the trace at $tracePath is incomplete." }

    # Let the run finish by itself; kill it if it overruns.
    if (-not $process.WaitForExit([int](($duration + 60) * 1000))) { Write-Warning 'The benchmark process did not exit in time and was killed.' }
} finally {
    if ($capturing) {
        if ($Tool -eq 'wpr') { [void](Invoke-Native $wpr @('-cancel')) }
        else { [void](Invoke-Native $VsDiagnosticsExe @('stop', "$vsSession", "/output:$tracePath")) }
    }
    Stop-Tree $process
}

if (-not (Test-Path -LiteralPath $tracePath)) { throw "The capture reported success but $tracePath does not exist." }
$exitCode = $null; try { $exitCode = $process.ExitCode } catch {}
$meta = [ordered]@{
    config = $config; renderer = $Renderer; api = $Api; binding = $Binding; threads = $Threads
    tool = $Tool; elevated = $elevated; trace = (Split-Path $tracePath -Leaf)
    trace_bytes = (Get-Item -LiteralPath $tracePath).Length
    capture_seconds = $Seconds
    capture_start_utc = $captureStart.ToString('yyyy-MM-ddTHH:mm:ssZ'); capture_stop_utc = $captureStop.ToString('yyyy-MM-ddTHH:mm:ssZ')
    warmup_seconds = $Warmup; startup_allowance_seconds = $StartupSeconds
    wpr_profile = $(if ($Tool -eq 'wpr') { $WprProfile } else { $null })
    vs_agent_config = $agentConfig
    executable = $Exe; command_line = "$Exe $argString"; process_exit_code = $exitCode
    adapter_luid = "$AdapterLuid"
    note = 'Timings of the profiled run are perturbed by sampling and are not measurements.'
}
Write-Utf8 (Join-Path $profileDir "$config.profile.json") ($meta | ConvertTo-Json -Depth 4)
Write-Host ("Profile written: {0} ({1:n1} MB)" -f $tracePath, ($meta.trace_bytes / 1MB))
if ($exitCode -ne 0) { Write-Warning "The profiled process ended with exit code $exitCode (see $runDir)."; exit 1 }
exit 0
