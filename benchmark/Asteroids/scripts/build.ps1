# Builds the Asteroids benchmark executables (Release) through ethereal's root build.ps1.
#   scripts\build.ps1                      configure + build asteroids_native, asteroids_nvrhi, benchmark_selftest
#   scripts\build.ps1 -ConfigureOnly
#   scripts\build.ps1 -Target asteroids_nvrhi
# Output: <ethereal>\build\bin. The root script sets up the Visual Studio environment (fxc and the
# compiler need it) and uses the Ninja Multi-Config preset.
param([switch]$ConfigureOnly, [string[]]$Target)
$ErrorActionPreference = 'Stop'
$ethereal = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$Target = @($Target | Where-Object { $_ } | ForEach-Object { $_ -split ',' })   # "-Target a,b" arrives as one string via -File
if (-not $Target) { $Target = @('asteroids_native', 'asteroids_nvrhi', 'benchmark_selftest') }
if ($ConfigureOnly) {
    & (Join-Path $ethereal 'build.ps1') -Config Release -ConfigureOnly
} else {
    & (Join-Path $ethereal 'build.ps1') -Config Release -Target $Target
}
