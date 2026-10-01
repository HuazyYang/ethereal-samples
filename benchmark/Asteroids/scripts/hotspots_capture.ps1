<#
.SYNOPSIS
Captures and symbolises the CPU sampling profiles of the hot-spot attribution set (docs/PROFILING.md).

.DESCRIPTION
For every configuration of the list: scripts/profile.ps1 (VSDiagnostics, CpuUsageHigh = 4 kHz kernel sampling with
stacks), then the .diagsession is expanded, its ETL is dumped with symbols by xperf (Windows Performance Toolkit) and
the dump is collapsed to per-thread stack counts of the profiled process by scripts/hotspots_collapse.py.
Output per configuration under results/<Session>/profiles/:
    <name>.diagsession          raw capture (open with Visual Studio)
    <name>.profile.json         what profile.ps1 captured
    <name>_run/                 run.json, frames.csv of the profiled process (NOT a measurement)
    <name>.stacks.txt           collapsed symbolised stacks (input of scripts/hotspots.py)
    <name>.images.txt           image load addresses of the process (xperf -a process -image)
<name> is <renderer>_<api>_<binding>_t<NN> plus "__<switch>" for nvrhi placement switches.

Symbols: _NT_SYMBOL_PATH is set to the build's bin directory plus a local symbol store (results/<Session>/symbols,
"srv*<store>" without a server, so that the images of unrelated processes fail fast). Fill the store once with
-FetchSymbols (adds the Microsoft symbol server to the path for that run; slow, downloads the PDBs of every image of
every process in the trace that it can find).

.EXAMPLE
scripts\hotspots_capture.ps1 -Session profiles-01 -Only native_d3d12_original_t01
#>
[CmdletBinding()]
param(
    [string]$Session = 'profiles-01',
    [int]$Seconds = 20,
    [string[]]$Only,            # names of configurations to run (default: all)
    [switch]$FetchSymbols,      # allow the Microsoft symbol server while dumping
    [switch]$SkipCapture,       # only (re)process existing captures
    [switch]$KeepDump           # keep the multi-hundred-MB xperf text dump
)
$ErrorActionPreference = 'Stop'
if ($Only) { $Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) }   # "-File" passes one string
$root = Split-Path $PSScriptRoot -Parent           # benchmark/Asteroids
$ethereal = (Resolve-Path (Join-Path $root '..\..\..')).Path   # the ethereal repo root (build\bin lives there)
$resultsRoot = Join-Path $root 'results'
$profileDir = Join-Path (Join-Path $resultsRoot $Session) 'profiles'
$store = Join-Path (Join-Path $resultsRoot $Session) 'symbols'
$symcache = Join-Path (Join-Path $resultsRoot $Session) 'symcache'
$xperf = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
$vsdiag = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
foreach ($p in @(& $vswhere -all -products * -version '[17.0,18.0)' -property installationPath)) {
    $c = Join-Path $p 'Team Tools\DiagnosticsHub\Collector\VSDiagnostics.exe'
    if (Test-Path -LiteralPath $c) { $vsdiag = $c; break }
}
if (-not $vsdiag) { throw 'VSDiagnostics.exe not found' }
if (-not (Test-Path -LiteralPath $xperf)) { throw "xperf.exe not found at $xperf (install the Windows Performance Toolkit)" }
New-Item -ItemType Directory -Force $profileDir, $store, $symcache | Out-Null

# name, renderer, api, binding, threads, extra args, suffix
$list = @(
    @('native_d3d12_original_t01',                    'native',   'd3d12', '',           1, '',                      ''),
    @('nvrhi_d3d12_tex_mut_t01__cb_page_split_avoid', 'nvrhi',    'd3d12', 'tex_mut',    1, '-cb_page_split_avoid',  '__cb_page_split_avoid'),
    @('nvrhi_d3d12_tex_mut_t01__cb_page_split_force', 'nvrhi',    'd3d12', 'tex_mut',    1, '-cb_page_split_force',  '__cb_page_split_force'),
    @('nvrhi_d3d12_tex_mut_pc_t01__cb_page_split_avoid', 'nvrhi', 'd3d12', 'tex_mut_pc', 1, '-cb_page_split_avoid',  '__cb_page_split_avoid'),
    @('nvrhi_d3d12_bindless_t01',                     'nvrhi',    'd3d12', 'bindless',   1, '',                      ''),
    @('native_d3d12_original_t08',                    'native',   'd3d12', '',           8, '',                      ''),
    @('nvrhi_d3d12_tex_mut_t08__cb_page_split_avoid', 'nvrhi',    'd3d12', 'tex_mut',    8, '-cb_page_split_avoid',  '__cb_page_split_avoid'),
    @('nvrhi_vk_tex_mut_t01',                         'nvrhi',    'vk',    'tex_mut',    1, '',                      ''),
    @('nvrhi_vk_tex_mut_pc_t01',                      'nvrhi',    'vk',    'tex_mut_pc', 1, '',                      ''),
    @('nvrhi_vk_bindless_t01',                        'nvrhi',    'vk',    'bindless',   1, '',                      '')
)

$symPath = "$(Join-Path $ethereal 'build\bin');srv*$store"
if ($FetchSymbols) { $symPath = "$(Join-Path $ethereal 'build\bin');srv*$store*https://msdl.microsoft.com/download/symbols" }

foreach ($e in $list) {
    $name, $renderer, $api, $binding, $threads, $extra, $suffix = $e
    if ($Only -and ($Only -notcontains $name)) { continue }
    $base = '{0}_{1}_{2}_t{3:d2}' -f $renderer, $api, $(if ($renderer -eq 'native') { 'original' } else { $binding }), $threads
    $diag = Join-Path $profileDir "$name.diagsession"
    $process = $(if ($renderer -eq 'nvrhi') { 'asteroids_nvrhi.exe' } else { 'asteroids_native.exe' })

    if (-not $SkipCapture) {
        Write-Host "=== capture $name"
        $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'profile.ps1'),
                  '-Renderer', $renderer, '-Api', $api, '-Threads', "$threads", '-Seconds', "$Seconds", '-Session', $Session, '-Tool', 'vsdiag')
        if ($binding) { $args += @('-Binding', $binding) }
        if ($extra) { $args += @('-ExtraArgs', $extra) }
        & powershell @args
        if ($LASTEXITCODE -ne 0) { Write-Warning "profile.ps1 failed for $name (exit $LASTEXITCODE)"; continue }
        if ($suffix) {
            foreach ($ext in @('.diagsession', '.profile.json', '_run')) {
                $src = Join-Path $profileDir "$base$ext"; $dst = Join-Path $profileDir "$name$ext"
                if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
                Move-Item -LiteralPath $src -Destination $dst
            }
        }
    }
    if (-not (Test-Path -LiteralPath $diag)) { Write-Warning "missing $diag"; continue }

    Write-Host "=== symbolise $name"
    $expanded = Join-Path $profileDir $name
    if (Test-Path -LiteralPath $expanded) { Remove-Item -LiteralPath $expanded -Recurse -Force }
    & $vsdiag expandDiagSession $diag | Out-Null
    $etl = Get-ChildItem -LiteralPath $expanded -Recurse -Filter '*.etl' | Select-Object -First 1
    if (-not $etl) { Write-Warning "no ETL inside $diag"; continue }
    $env:_NT_SYMBOL_PATH = $symPath
    $env:_NT_SYMCACHE_PATH = $symcache
    $dump = Join-Path $profileDir "$name.dump.txt"
    $images = Join-Path $profileDir "$name.images.txt"
    & $xperf -i $etl.FullName -tle -tti -o $images -a process -image 2>&1 | Out-Null
    & $xperf -i $etl.FullName -tle -tti -symbols -o $dump -a dumper 2>&1 | Out-Null
    if (-not (Test-Path -LiteralPath $dump)) { Write-Warning "xperf produced no dump for $name"; continue }
    & python (Join-Path $PSScriptRoot 'hotspots_collapse.py') $dump $process (Join-Path $profileDir "$name.stacks.txt")
    if (-not $KeepDump) { Remove-Item -LiteralPath $dump -Force }
    Remove-Item -LiteralPath $expanded -Recurse -Force
}
