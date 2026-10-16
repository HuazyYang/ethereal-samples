<#
.SYNOPSIS
Runs the MultithreadedRendering benchmark matrix and prints a summary table.

.DESCRIPTION
Matrix: {native d3d12, nvrhi d3d12, nvrhi vk} x {st t1, mt_scene t6, mt_chunk t2 t4 t8}, plus the nvrhi
-explicit_barriers variant of st t1 and mt_chunk t4. Every run: -warmup / -duration seconds (defaults 3 / 10),
one process at a time, a cooldown between runs (the laptop throttles), results in <ResultsRoot>\<Session>\<config>\.
Writes <session>\summary.md (median render / submit / wait / frame ms per configuration) and prints it.

Launch from an interactive desktop session: a window that is never displayed is never rendered to.

.EXAMPLE
scripts\run_matrix.ps1                           # full matrix, 3 s warmup, 10 s measured
scripts\run_matrix.ps1 -Quick                    # 1 s warmup, 2 s measured (smoke pass)
scripts\run_matrix.ps1 -Renderer nvrhi -Api vk -Mode mt_chunk -Threads 4
#>
[CmdletBinding()]
param(
    [string]$Session = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [string]$ResultsRoot = (Join-Path $PSScriptRoot '..\results'),
    [string]$BinDir = (Join-Path $PSScriptRoot '..\..\..\..\build\bin'),
    [string[]]$Renderer,          # filter: native, nvrhi
    [string[]]$Api,               # filter: d3d12, vk
    [string[]]$Mode,              # filter: st, mt_scene, mt_chunk
    [int[]]$Threads,              # filter: 1, 2, 4, 6, 8
    [switch]$Quick,
    [double]$Warmup = 3,
    [double]$Duration = 10,
    [double]$CooldownSeconds = 5,
    [int]$Width = 1280,
    [int]$Height = 720,
    [switch]$NoExplicitBarriers
)
$ErrorActionPreference = 'Stop'
if ($Quick) { $Warmup = 1; $Duration = 2; $CooldownSeconds = 1 }
$BinDir = (Resolve-Path $BinDir).Path
$sessionDir = Join-Path (Join-Path $ResultsRoot $Session) ''
New-Item -ItemType Directory -Force $sessionDir | Out-Null

$configs = @()
foreach ($r in @(@{ renderer = 'native'; api = 'd3d12' }, @{ renderer = 'nvrhi'; api = 'd3d12' }, @{ renderer = 'nvrhi'; api = 'vk' })) {
    foreach ($m in @(@{ mode = 'st'; threads = 1 }, @{ mode = 'mt_scene'; threads = 6 }, @{ mode = 'mt_chunk'; threads = 2 },
                     @{ mode = 'mt_chunk'; threads = 4 }, @{ mode = 'mt_chunk'; threads = 8 })) {
        $configs += @{ renderer = $r.renderer; api = $r.api; mode = $m.mode; threads = $m.threads; variant = '' }
        if ($r.renderer -eq 'nvrhi' -and -not $NoExplicitBarriers -and (($m.mode -eq 'st') -or ($m.mode -eq 'mt_chunk' -and $m.threads -eq 4))) {
            $configs += @{ renderer = $r.renderer; api = $r.api; mode = $m.mode; threads = $m.threads; variant = 'explicit_barriers' }
        }
    }
}
if ($Renderer) { $configs = $configs | Where-Object { $Renderer -contains $_.renderer } }
if ($Api) { $configs = $configs | Where-Object { $Api -contains $_.api } }
if ($Mode) { $configs = $configs | Where-Object { $Mode -contains $_.mode } }
if ($Threads) { $configs = $configs | Where-Object { $Threads -contains $_.threads } }

function Invoke-Run($cfg) {
    $exe = if ($cfg.renderer -eq 'native') { Join-Path $BinDir 'mt_rendering_native.exe' } else { Join-Path $BinDir 'mt_rendering_nvrhi.exe' }
    $name = '{0}_{1}_{2}_t{3:D2}' -f $cfg.renderer, $cfg.api, $cfg.mode, $cfg.threads
    if ($cfg.variant) { $name += "__$($cfg.variant)" }
    $outDir = Join-Path $sessionDir $name
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $args = "-benchmark -renderer $($cfg.renderer) -api $($cfg.api) -mode $($cfg.mode) -threads $($cfg.threads) -warmup $Warmup -duration $Duration -window $Width $Height -output `"$outDir`""
    if ($cfg.variant -eq 'explicit_barriers') { $args += ' -explicit_barriers' }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe; $psi.Arguments = $args; $psi.WorkingDirectory = $BinDir
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
    $timeout = [int](($Warmup + $Duration + 90) * 1000)
    if (-not $p.WaitForExit($timeout)) { $p.Kill(); $p.WaitForExit(); $code = 'timeout' } else { $p.WaitForExit(); $code = $p.ExitCode }
    Set-Content (Join-Path $outDir 'stdout.txt') $so.Result -Encoding UTF8
    Set-Content (Join-Path $outDir 'stderr.txt') $se.Result -Encoding UTF8
    $runJson = Join-Path $outDir 'run.json'
    $row = [ordered]@{ config = $name; status = "exit $code"; render = ''; submit = ''; wait = ''; present = ''; frame = ''; frames = ''; draws = '' }
    if (Test-Path $runJson) {
        $j = Get-Content $runJson -Raw | ConvertFrom-Json
        $row.status = $j.status
        if ($j.status -eq 'ok') {
            $row.render = '{0:F3}' -f $j.median_render_ms
            $row.submit = '{0:F3}' -f $j.median_submit_ms
            $row.wait = '{0:F3}' -f $j.median_wait_ms
            $row.frame = '{0:F3}' -f $j.median_frame_ms
            $row.frames = $j.frame_count
            $csv = Import-Csv (Join-Path $outDir 'frames.csv')
            if ($csv.Count -gt 0) {
                $row.draws = '{0:F0}' -f (($csv | ForEach-Object { [double]$_.draw_count } | Measure-Object -Average).Average)
                $present = ($csv | ForEach-Object { [double]$_.present_ms } | Sort-Object)
                $row.present = '{0:F3}' -f $present[[int]($present.Count / 2)]
            }
        } else { $row.status = "$($j.status): $($j.reason)" }
    }
    Write-Host ("{0,-44} {1,-10} render {2,8} submit {3,8} wait {4,8} present {5,8} frame {6,8}  frames {7,5}  draws {8}" -f $row.config, $row.status, $row.render, $row.submit, $row.wait, $row.present, $row.frame, $row.frames, $row.draws)
    return $row
}

$rows = @()
$i = 0
foreach ($cfg in $configs) {
    $i++
    Write-Host ("[{0}/{1}] " -f $i, $configs.Count) -NoNewline
    $rows += Invoke-Run $cfg
    if ($i -lt $configs.Count) { Start-Sleep -Seconds $CooldownSeconds }
}

$md = @("# MultithreadedRendering benchmark: session $Session", "",
        "Protocol: $Warmup s warmup, $Duration s measured, $Width x $Height, one run per configuration, medians over the measured frames (ms, main-thread wall clock).", "",
        "| configuration | status | render | submit | wait | present | frame | frames | draws/frame |", "|---|---|---:|---:|---:|---:|---:|---:|---:|")
foreach ($r in $rows) { $md += "| $($r.config) | $($r.status) | $($r.render) | $($r.submit) | $($r.wait) | $($r.present) | $($r.frame) | $($r.frames) | $($r.draws) |" }
$summaryPath = Join-Path $sessionDir 'summary.md'
Set-Content $summaryPath ($md -join "`n") -Encoding UTF8
Write-Host "`nsummary -> $summaryPath"
