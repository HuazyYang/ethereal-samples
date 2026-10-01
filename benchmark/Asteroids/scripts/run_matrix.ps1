<#
.SYNOPSIS
Runs the benchmark matrix of docs/PLAN.md, one process at a time, into results/<session>/<config>/<rep>/.

.DESCRIPTION
Matrix: {native, nvrhi} x {d3d11, d3d12, vk} x applicable bindings x threads {1,2,4,8,15}.
Combinations that PLAN.md marks N/A are not scheduled: native+vk, nvrhi+dyn, bindless+d3d11,
and more than one thread for the two single-threaded D3D11 renderers (native d3d11, nvrhi d3d11).
One protocol for every run: 5 s warmup, 20 s measured. Headline configurations (d3d12 and vk, bindings tex_mut
and bindless, plus native d3d12; threads 1 and 8; every renderer): 5 repetitions. All others: 3 repetitions.
-Quick: every selected configuration once, 2 s warmup, 3 s measured (smoke pass).

-Set sensitivity: the nvrhi sensitivity switches instead of the matrix: nvrhi d3d12 and vk, tex_mut, threads 1 and
8, once without a switch (the baseline of the same session) and once with each of -always_set_state,
-track_liveness, -no_auto_barriers, and on d3d12 -cb_page_split_avoid and -cb_page_split_force (the two levels of
the page-split store, docs/LIMITATIONS.md); 3 repetitions. A variant runs in <config>__<switch>/ so that
scripts/analyze.py reports it in its own table and never mixes it into the main matrix.

Desktop state (docs/LIMITATIONS.md, frame pacing): whether the compositor paces a swap chain depends on whether the
window is really displayed. The runner therefore keeps the display and the system awake for the whole session
(SetThreadExecutionState, restored at exit), refuses to start - and stops before the next run - when the display
is off or the session is locked (override: -AllowHiddenDesktop), and records the desktop state of every run in
manifest.json. -CoverWindow does the opposite on purpose: every benchmark window is covered by a topmost
full-screen window, so no renderer is paced (the presents are dropped); use it only for a session of its own, e.g.
for the frame_ms-based CPU-bound check of the Vulkan renderer.

Order: repetition rounds; inside every round the configurations are shuffled with -Seed (randomized, interleaved).
Resumable: start again with the same -Session; runs whose run.json has status ok are skipped. Runs that a
renderer rejected as unsupported are not repeated either (use -RetryUnsupported). Everything else is run again.
Failed runs stay in <session>/manifest.json with status failed | timeout | crashed | unsupported and a reason.
The environment is captured once per session into <session>/environment.json (a resume writes
environment.resume-<time>.json and warns when something relevant changed).
A resume is REFUSED when the session would mix incomparable runs: an executable whose SHA-256 changed since the
session started (override: -AllowExeChange), or a different -GpuTiming, -Quick or -Width/-Height than the session
was started with (no override: use a new session). GPU-timed runs are only for the CPU-bound check and must never
share a session with the measurement runs.

Exit code: 0 when every scheduled run is ok, 1 when at least one run failed, timed out or was left pending.

.EXAMPLE
scripts\run_matrix.ps1 -Quick                                    # smoke pass over the whole matrix
.EXAMPLE
scripts\run_matrix.ps1 -Session full-01                          # full matrix (run again to resume)
.EXAMPLE
scripts\run_matrix.ps1 -Session full-01 -Set headline            # only the headline configurations
.EXAMPLE
scripts\run_matrix.ps1 -Quick -Renderer nvrhi -Api d3d12,vk -Binding tex_mut -Threads 1,8
.EXAMPLE
scripts\run_matrix.ps1 -Session sens-01 -Set sensitivity                        # nvrhi sensitivity switches
.EXAMPLE
scripts\run_matrix.ps1 -Session half-res -Set headline -Width 540 -Height 360   # CPU-bound check: half resolution
.EXAMPLE
scripts\run_matrix.ps1 -Session gpu-01 -Set headline -GpuTiming                 # CPU-bound check: gpu_ms
#>
[CmdletBinding()]
param(
    [string]$Session,                       # default: <yyyyMMdd-HHmmss>, or quick-<...> with -Quick
    [string]$ResultsRoot,                   # default: <repo>\results
    [string]$BinDir,                        # default: <ethereal>\build\bin (also the working directory)
    [string]$ReferenceExe,                  # default: <BinDir>\asteroids_native.exe (native)
    [string]$NvrhiExe,                      # default: <BinDir>\asteroids_nvrhi.exe
    [string]$SelftestExe,                   # default: <BinDir>\benchmark_selftest.exe (adapter LUID lookup)
    [string[]]$Renderer,                    # filter: native, nvrhi
    [string[]]$Api,                         # filter: d3d11, d3d12, vk
    [string[]]$Binding,                     # filter: original (= native), dyn, mut, tex_mut, tex_mut_pc, bindless
    [string[]]$Threads,                     # filter: subset of 1, 2, 4, 8, 15
    [ValidateSet('all', 'headline', 'other', 'sensitivity')][string]$Set = 'all',
    [switch]$Quick,                         # 1 repetition, 2 s warmup, 3 s measured
    [int]$Repetitions = 0,                  # override for every configuration (0 = protocol value)
    [double]$Warmup = -1,                   # override in seconds (-1 = protocol value)
    [double]$Duration = -1,                 # override in seconds (-1 = protocol value)
    [int]$Seed = 20260930,                  # shuffle seed
    [double]$CooldownSeconds = -1,          # pause after every run (-1 = 10 s, or 1 s with -Quick)
    [int]$StartupTimeoutSeconds = 180,      # a run is killed after warmup + duration + this
    [int]$Width = 1080,
    [int]$Height = 720,
    [uint64]$AdapterLuid = 0,               # 0 = look up -AdapterName with benchmark_selftest.exe -list_adapters
    [string]$AdapterName = 'RTX 4050',
    [switch]$GpuTiming,                     # pass -gpu_timing (CPU-bound check only; its own session)
    [switch]$AllowExeChange,                # resume although an executable changed since the session started
    [string[]]$ExtraArgs,                   # appended to every command line
    [switch]$RetryUnsupported,
    [switch]$AllowBattery,                  # without it a non-quick session refuses to start on battery
    [switch]$AllowHiddenDesktop,            # run although the display is off or the session is locked
    [switch]$CoverWindow,                   # cover every benchmark window (nothing is paced; its own session)
    [int]$MaxRuns = 0,                      # stop after this many executed runs (0 = no limit)
    [switch]$DryRun                         # print the schedule, run nothing, write nothing
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent           # benchmark/Asteroids
$ethereal = (Resolve-Path (Join-Path $root '..\..\..')).Path   # the ethereal repo root (build\bin lives there)

# ---------------------------------------------------------------------------------------------- helpers

function Split-List([string[]]$Values) {
    # "-Api d3d12,vk" arrives as one string when the script is started with powershell -File.
    @($Values | Where-Object { $_ } | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

function Write-Utf8([string]$Path, [string]$Text) {
    # Atomic, UTF-8 without BOM.
    $tmp = "$Path.tmp"
    [System.IO.File]::WriteAllText($tmp, $Text, (New-Object System.Text.UTF8Encoding $false))
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

function Get-UtcStamp { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ') }

function Read-JsonFile([string]$Path) {
    try { return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { return $null }
}

function Invoke-Git([string]$Dir, [string[]]$GitArgs) {
    # stderr of a native command becomes a terminating error under ErrorActionPreference Stop in PowerShell 5.1.
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & git -C $Dir @GitArgs 2>$null
        if ($LASTEXITCODE -ne 0) { return $null }
        return (($out | Out-String).TrimEnd())
    } catch { return $null } finally { $ErrorActionPreference = $old }
}

function Get-RepoState([string]$Name, [string]$Dir) {
    $state = [ordered]@{ name = $Name; path = $Dir; commit = $null; branch = $null; dirty = $null; dirty_files = @() }
    if (-not (Test-Path -LiteralPath $Dir)) { return $state }
    $state.commit = Invoke-Git $Dir @('rev-parse', 'HEAD')
    $state.branch = Invoke-Git $Dir @('rev-parse', '--abbrev-ref', 'HEAD')
    $status = Invoke-Git $Dir @('status', '--porcelain', '--ignore-submodules=dirty')
    if ($null -ne $status) {
        $lines = @($status -split "`n" | ForEach-Object { $_.TrimEnd() } | Where-Object { $_ })
        $state.dirty = ($lines.Count -gt 0)
        $state.dirty_files = @($lines | Select-Object -First 50)
    }
    return $state
}

function Get-ExeInfo([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return [ordered]@{ path = $Path; exists = $false } }
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $item.FullName; exists = $true; size = $item.Length
        modified_utc = $item.LastWriteTimeUtc.ToString('yyyy-MM-ddTHH:mm:ssZ')
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

function Get-PowerLine {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        return [string][System.Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus   # Online | Offline | Unknown
    } catch { return 'Unknown' }
}

function Get-Adapters([string]$Exe) {
    # benchmark_selftest.exe -list_adapters prints: luid TAB dxgi-index TAB vendor:device TAB name
    $list = @()
    if (-not (Test-Path -LiteralPath $Exe)) { return $list }
    foreach ($line in @(& $Exe -list_adapters)) {
        $f = "$line".Split("`t")
        if ($f.Count -ge 4 -and $f[0] -match '^\d+$') {
            $list += [ordered]@{ luid = $f[0]; dxgi_index = $f[1]; id = $f[2]; name = $f[3].Trim() }
        }
    }
    return $list
}

function Get-Environment {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $plan = (& powercfg /getactivescheme | Out-String).Trim()
    $planGuid = $null; $planName = $null
    if ($plan -match '([0-9a-fA-F]{8}-[0-9a-fA-F-]{27})') { $planGuid = $Matches[1] }
    if ($plan -match '\((.+)\)\s*$') { $planName = $Matches[1] }
    $gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object {
        $date = $null
        if ($_.DriverDate) { $date = ([DateTime]$_.DriverDate).ToString('yyyy-MM-dd') }
        [ordered]@{ name = $_.Name; driver_version = $_.DriverVersion; driver_date = $date; pnp_device_id = $_.PNPDeviceID }
    })
    return [ordered]@{
        captured_utc = Get-UtcStamp
        computer = $env:COMPUTERNAME
        cpu = [ordered]@{ name = "$($cpu.Name)".Trim(); cores = $cpu.NumberOfCores; logical_processors = $cpu.NumberOfLogicalProcessors
                          max_clock_mhz = $cpu.MaxClockSpeed }
        memory_gb = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
        gpus = $gpus
        dxgi_adapters = @(Get-Adapters $SelftestExe)
        benchmark_adapter = [ordered]@{ luid = "$AdapterLuid"; name = $script:AdapterFullName }
        os = [ordered]@{ caption = $os.Caption; version = $os.Version; build = $os.BuildNumber; ubr = $cv.UBR
                         display_version = $cv.DisplayVersion }
        power_plan = [ordered]@{ guid = $planGuid; name = $planName; raw = $plan }
        ac_line = Get-PowerLine
        desktop = Get-DesktopState
        git = @(
            (Get-RepoState 'root' $root),
            (Get-RepoState 'donut' (Join-Path $ethereal 'donut')),
            (Get-RepoState 'nvrhi' (Join-Path $ethereal 'donut\nvrhi')),
            (Get-RepoState 'ethereal' $ethereal)
        )
        executables = [ordered]@{ reference = Get-ExeInfo $ReferenceExe; nvrhi = Get-ExeInfo $NvrhiExe }
        runner = [ordered]@{
            session = $Session; seed = $Seed; quick = [bool]$Quick; set = $Set; width = $Width; height = $Height
            warmup_seconds = $protocolWarmup; duration_seconds = $protocolDuration
            repetitions = [ordered]@{ headline = $headlineReps; other = $otherReps; sensitivity = $sensitivityReps }
            overrides = [ordered]@{ repetitions = $Repetitions; warmup = $Warmup; duration = $Duration }
            gpu_timing = [bool]$GpuTiming; cooldown_seconds = $CooldownSeconds; startup_timeout_seconds = $StartupTimeoutSeconds
            cover_window = [bool]$CoverWindow; keep_display_awake = $true; allow_hidden_desktop = [bool]$AllowHiddenDesktop
            extra_args = @($ExtraArgs); powershell = "$($PSVersionTable.PSVersion)"
        }
    }
}

function Test-Headline($c) {
    # docs/PLAN.md: d3d12 and vk, bindings tex_mut and bindless (plus native d3d12), threads 1 and 8, all renderers.
    if ($c.variant) { return $false }
    if ($c.threads -ne 1 -and $c.threads -ne 8) { return $false }
    if ($c.renderer -eq 'native') { return ($c.api -eq 'd3d12') }
    return (($c.binding -eq 'tex_mut' -or $c.binding -eq 'bindless') -and ($c.api -eq 'd3d12' -or $c.api -eq 'vk'))
}

function Format-Arg([string]$a) { if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a } }

function Get-Tail([string]$Path, [int]$Lines = 3) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $text = @(Get-Content -LiteralPath $Path -Tail $Lines -ErrorAction SilentlyContinue) -join ' | '
    if ($text.Length -gt 400) { $text = $text.Substring(0, 400) }
    return $text.Trim()
}

# Desktop state: display power (GUID_CONSOLE_DISPLAY_STATE), lock screen, idle time; keeps the display awake.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class BenchDesktop {
    [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint flags);
    [StructLayout(LayoutKind.Sequential)] struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
    [DllImport("kernel32.dll")] static extern uint GetTickCount();
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr h, int index, StringBuilder buf, int len, out int needed);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr h);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASS { public uint style; public IntPtr lpfnWndProc; public int cbClsExtra; public int cbWndExtra; public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground; public string lpszMenuName; public string lpszClassName; }
    delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern ushort RegisterClass(ref WNDCLASS c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateWindowEx(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr RegisterPowerSettingNotification(IntPtr h, ref Guid g, uint flags);
    [DllImport("user32.dll")] static extern bool UnregisterPowerSettingNotification(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public int x; public int y; }
    [DllImport("user32.dll")] static extern bool PeekMessage(out MSG m, IntPtr h, uint min, uint max, uint remove);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG m);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string n);

    // ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED: no idle display-off or sleep while this thread lives.
    public static bool KeepAwake() { return SetThreadExecutionState(0x80000000u | 0x00000001u | 0x00000002u) != 0; }
    public static void Restore() { SetThreadExecutionState(0x80000000u); }

    static int displayState = -1;
    static WndProc proc = Proc;
    static string className;
    static IntPtr Proc(IntPtr h, uint m, IntPtr w, IntPtr l) {
        if (m == 0x0218 && w.ToInt64() == 0x8013) displayState = Marshal.ReadInt32(l, 20);   // WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE
        return DefWindowProc(h, m, w, l);
    }
    // GUID_CONSOLE_DISPLAY_STATE: 0 = off, 1 = on, 2 = dimmed, -1 = unknown. The current value is delivered to a
    // window right after it registers for the notification.
    public static int DisplayState() {
        displayState = -1;
        if (className == null) {
            WNDCLASS c = new WNDCLASS(); c.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(proc); c.hInstance = GetModuleHandle(null);
            c.lpszClassName = "BenchDesktopProbe" + Guid.NewGuid().ToString("N");
            if (RegisterClass(ref c) == 0) return -1;
            className = c.lpszClassName;
        }
        IntPtr hwnd = CreateWindowEx(0, className, "", 0, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        if (hwnd == IntPtr.Zero) return -1;
        Guid g = new Guid("6FE69556-704A-47A0-8F24-C28D936FDA47");
        IntPtr reg = RegisterPowerSettingNotification(hwnd, ref g, 0);
        int start = Environment.TickCount;
        MSG msg;
        while (displayState < 0 && Environment.TickCount - start < 1000) {
            while (PeekMessage(out msg, hwnd, 0, 0, 1)) DispatchMessage(ref msg);
            if (displayState < 0) System.Threading.Thread.Sleep(10);
        }
        if (reg != IntPtr.Zero) UnregisterPowerSettingNotification(reg);
        DestroyWindow(hwnd);
        return displayState;
    }
    public static uint IdleSeconds() { LASTINPUTINFO i = new LASTINPUTINFO(); i.cbSize = 8; GetLastInputInfo(ref i); return (GetTickCount() - i.dwTime) / 1000; }
    // "Default" on the user's desktop; "Winlogon" or not openable on the secure desktop.
    public static string InputDesktop() {
        IntPtr h = OpenInputDesktop(0, false, 0x0001);
        if (h == IntPtr.Zero) return "";
        StringBuilder s = new StringBuilder(256); int needed;
        bool ok = GetUserObjectInformation(h, 2, s, 512, out needed);
        CloseDesktop(h);
        return ok ? s.ToString() : "";
    }
    public static bool RemoteSession() { return GetSystemMetrics(0x1000) != 0; }
    public static uint ForegroundProcessId() { uint pid = 0; IntPtr h = GetForegroundWindow(); if (h != IntPtr.Zero) GetWindowThreadProcessId(h, out pid); return pid; }
}
'@

function Get-DesktopState {
    # locked: the lock screen is up (LogonUI.exe in this session, or the lock screen app owns the foreground window)
    # or the input desktop is not the user's.
    $display = [BenchDesktop]::DisplayState()
    $desktop = [BenchDesktop]::InputDesktop()
    $sessionId = (Get-Process -Id $PID).SessionId
    $logonUi = @(Get-Process -Name LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sessionId }).Count -gt 0
    $foreground = ''
    $fgPid = [BenchDesktop]::ForegroundProcessId()
    if ($fgPid) { try { $foreground = (Get-Process -Id $fgPid -ErrorAction Stop).Name } catch {} }
    if ($foreground -eq 'LockApp') { $logonUi = $true }
    $name = 'unknown'
    if ($display -eq 0) { $name = 'off' } elseif ($display -eq 1) { $name = 'on' } elseif ($display -eq 2) { $name = 'dimmed' }
    return [ordered]@{
        display = $name; locked = ($logonUi -or $desktop -ne 'Default'); input_desktop = $desktop
        idle_seconds = [int][BenchDesktop]::IdleSeconds(); remote_session = [BenchDesktop]::RemoteSession()
        foreground_process = $foreground
    }
}

function Test-DesktopHidden($State) {
    # Reasons why a benchmark window would not be displayed (then nothing is paced and present_ms means nothing).
    $why = @()
    if ($State.display -eq 'off') { $why += 'the display is off' }
    if ($State.locked) { $why += 'the session is locked (lock screen or secure desktop)' }
    return $why
}

function Stop-Tree($Process) {
    if ($Process -and -not $Process.HasExited) {
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try { & taskkill.exe /PID $Process.Id /T /F 2>$null | Out-Null } catch {} finally { $ErrorActionPreference = $old }
        try { [void]$Process.WaitForExit(10000) } catch {}
    }
}

# ---------------------------------------------------------------------------------------------- setup

if (-not $ResultsRoot) { $ResultsRoot = Join-Path $root 'results' }
if (-not $BinDir) { $BinDir = Join-Path $ethereal 'build\bin' }
if (-not $ReferenceExe) { $ReferenceExe = Join-Path $BinDir 'asteroids_native.exe' }
if (-not $NvrhiExe) { $NvrhiExe = Join-Path $BinDir 'asteroids_nvrhi.exe' }
if (-not $SelftestExe) { $SelftestExe = Join-Path $BinDir 'benchmark_selftest.exe' }
if (-not $Session) {
    $Session = (Get-Date).ToString('yyyyMMdd-HHmmss')
    if ($Quick) { $Session = "quick-$Session" }
}
if ($Session -match '[\\/:*?"<>|]') { throw "-Session '$Session' is not a valid directory name." }
if ($CooldownSeconds -lt 0) { if ($Quick) { $CooldownSeconds = 1 } else { $CooldownSeconds = 10 } }

$allRenderers = 'native', 'nvrhi'
$allApis = 'd3d11', 'd3d12', 'vk'
$allThreads = 1, 2, 4, 8, 15
$bindingsOf = @{
    native   = @('original')                                    # one fixed strategy; -binding is not passed
    nvrhi    = @('mut', 'tex_mut', 'tex_mut_pc', 'bindless')    # dyn is N/A
}
# One protocol for every run that enters a ratio (docs/PLAN.md "Measurement protocol").
$protocolWarmup = 5.0; $protocolDuration = 20.0; $headlineReps = 5; $otherReps = 3
# -Set sensitivity: asteroids_nvrhi switches, each singly; '' is the baseline of the same session.
$sensitivityVariants = @('', 'always_set_state', 'track_liveness', 'no_auto_barriers', 'cb_page_split_avoid', 'cb_page_split_force')
$d3d12OnlyVariants = @('cb_page_split_avoid', 'cb_page_split_force')   # placement of the nvrhi D3D12 command list objects
$sensitivityReps = 3

$fRenderer = Split-List $Renderer; $fApi = Split-List $Api; $fBinding = Split-List $Binding
$fThreads = @(Split-List $Threads | ForEach-Object { [int]$_ })
foreach ($v in $fRenderer) { if ($allRenderers -notcontains $v) { throw "Unknown -Renderer '$v' (native, nvrhi)." } }
foreach ($v in $fApi) { if ($allApis -notcontains $v) { throw "Unknown -Api '$v' (d3d11, d3d12, vk)." } }
foreach ($v in $fBinding) {
    if (@('original', 'dyn', 'mut', 'tex_mut', 'tex_mut_pc', 'bindless') -notcontains $v) {
        throw "Unknown -Binding '$v' (original, dyn, mut, tex_mut, tex_mut_pc, bindless)."
    }
}
foreach ($v in $fThreads) { if ($v -lt 1) { throw "Invalid -Threads value '$v'." } }
$threadList = $allThreads
if ($fThreads.Count) { $threadList = $fThreads }   # any positive count may be requested explicitly

# ---------------------------------------------------------------------------------------------- matrix

function Add-Config([string]$r, [string]$a, [string]$b, [int]$t, [string]$variant) {
    $c = [ordered]@{ renderer = $r; api = $a; binding = $b; threads = $t; variant = $variant }
    $c.name = '{0}_{1}_{2}_t{3:d2}' -f $r, $a, $b, $t
    if ($variant) { $c.name += "__$variant" }                  # scripts/analyze.py: VARIANT_SEPARATOR
    $c.headline = Test-Headline $c
    if ($Set -eq 'headline' -and -not $c.headline) { return }
    if ($Set -eq 'other' -and $c.headline) { return }
    $c.warmup = $protocolWarmup; $c.duration = $protocolDuration
    if ($Set -eq 'sensitivity') { $c.reps = $sensitivityReps }
    elseif ($c.headline) { $c.reps = $headlineReps }
    else { $c.reps = $otherReps }
    if ($Quick) { $c.reps = 1; $c.warmup = 2.0; $c.duration = 3.0 }
    if ($Repetitions -gt 0) { $c.reps = $Repetitions }
    if ($Warmup -ge 0) { $c.warmup = $Warmup }
    if ($Duration -ge 0) { $c.duration = $Duration }
    $script:configs += $c
}

$script:configs = @()
if ($Set -eq 'sensitivity') {
    $sensThreads = @(1, 8); if ($fThreads.Count) { $sensThreads = $fThreads }
    foreach ($a in @('d3d12', 'vk')) {
        if ($fApi.Count -and $fApi -notcontains $a) { continue }
        if ($fRenderer.Count -and $fRenderer -notcontains 'nvrhi') { continue }
        if ($fBinding.Count -and $fBinding -notcontains 'tex_mut') { continue }
        foreach ($t in $sensThreads) {
            foreach ($v in $sensitivityVariants) {
                if ($a -ne 'd3d12' -and $d3d12OnlyVariants -contains $v) { continue }
                Add-Config 'nvrhi' $a 'tex_mut' ([int]$t) $v
            }
        }
    }
} else {
    foreach ($r in $allRenderers) {
        if ($fRenderer.Count -and $fRenderer -notcontains $r) { continue }
        foreach ($a in $allApis) {
            if ($fApi.Count -and $fApi -notcontains $a) { continue }
            if ($r -eq 'native' -and $a -eq 'vk') { continue }                  # N/A: no native Vulkan renderer
            foreach ($b in $bindingsOf[$r]) {
                if ($fBinding.Count -and $fBinding -notcontains $b) { continue }
                if ($b -eq 'bindless' -and $a -eq 'd3d11') { continue }         # N/A: bindless needs d3d12 or vk
                foreach ($t in $threadList) {
                    # N/A: the native D3D11 renderer and the nvrhi D3D11 backend record on one thread only
                    if ($a -eq 'd3d11' -and [int]$t -ne 1) { continue }
                    Add-Config $r $a $b ([int]$t) ''
                }
            }
        }
    }
}
$configs = $script:configs
if (-not $configs.Count) { throw 'The filters select no configuration.' }

# Repetition rounds, each shuffled (Fisher-Yates, System.Random($Seed)): randomized and interleaved, so slow
# drift (temperature, background load) hits every configuration alike instead of one block of the matrix.
$rng = New-Object System.Random $Seed
$schedule = @()
$maxReps = ($configs | ForEach-Object { $_.reps } | Measure-Object -Maximum).Maximum
for ($rep = 1; $rep -le $maxReps; $rep++) {
    $round = @($configs | Where-Object { $_.reps -ge $rep })
    for ($i = $round.Count - 1; $i -gt 0; $i--) {
        $j = $rng.Next($i + 1)
        $tmp = $round[$i]; $round[$i] = $round[$j]; $round[$j] = $tmp
    }
    foreach ($c in $round) { $schedule += [ordered]@{ config = $c; rep = "rep$rep"; id = "$($c.name)/rep$rep" } }
}

$sessionDir = Join-Path $ResultsRoot $Session
$estimate = 0.0
foreach ($s in $schedule) { $estimate += $s.config.warmup + $s.config.duration + $CooldownSeconds + 8 }
Write-Host ("Session {0}: {1} configurations, {2} runs, seed {3}, about {4:n0} min if nothing is skipped" -f `
    $Session, $configs.Count, $schedule.Count, $Seed, ($estimate / 60))

if ($DryRun) {
    $n = 0
    foreach ($s in $schedule) {
        $n++
        $c = $s.config
        $tag = ''; if ($c.headline) { $tag = ' headline' }
        Write-Host ("{0,4}  {1,-46} {2}  warmup {3}s duration {4}s{5}" -f $n, $c.name, $s.rep, $c.warmup, $c.duration, $tag)
    }
    Write-Host "Dry run: nothing was executed. Output would go to $sessionDir"
    exit 0
}

# ---------------------------------------------------------------------------------------------- preconditions

$needed = @{}
foreach ($c in $configs) { if ($c.renderer -eq 'nvrhi') { $needed[$NvrhiExe] = $true } else { $needed[$ReferenceExe] = $true } }
foreach ($exe in $needed.Keys) { if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe (build with scripts\build.ps1, or pass -ReferenceExe / -NvrhiExe)." } }
$ReferenceExe = [System.IO.Path]::GetFullPath($ReferenceExe); $NvrhiExe = [System.IO.Path]::GetFullPath($NvrhiExe)

# Never two GPU benchmark processes at once.
$exeNames = @($ReferenceExe, $NvrhiExe | ForEach-Object { [System.IO.Path]::GetFileNameWithoutExtension($_) } | Select-Object -Unique)
$busy = @(Get-Process -Name $exeNames -ErrorAction SilentlyContinue)
if ($busy.Count) { throw "A benchmark process is already running ($(($busy | ForEach-Object { "$($_.Name) pid $($_.Id)" }) -join ', ')). Only one GPU benchmark may run at a time." }

# The adapter LUID changes on reboot, so it is looked up on every start unless given.
$script:AdapterFullName = $null
$adapters = @(Get-Adapters $SelftestExe)
if ($AdapterLuid -eq 0) {
    if (-not $adapters.Count) { throw "Cannot list adapters: $SelftestExe is missing or printed nothing. Build benchmark_selftest or pass -AdapterLuid." }
    $hit = @($adapters | Where-Object { $_.name -like "*$AdapterName*" })
    if ($hit.Count -ne 1) { throw "-AdapterName '$AdapterName' matches $($hit.Count) adapters ($(($adapters | ForEach-Object { $_.name }) -join '; ')). Pass -AdapterLuid." }
    $AdapterLuid = [uint64]$hit[0].luid
    $script:AdapterFullName = $hit[0].name
} else {
    $hit = @($adapters | Where-Object { $_.luid -eq "$AdapterLuid" })
    if ($adapters.Count -and -not $hit.Count) { throw "-AdapterLuid $AdapterLuid is not a current DXGI adapter ($(($adapters | ForEach-Object { "$($_.luid) = $($_.name)" }) -join '; '))." }
    if ($hit.Count) { $script:AdapterFullName = $hit[0].name }
}
Write-Host "Adapter: $($script:AdapterFullName) (LUID $AdapterLuid)"

$powerLine = Get-PowerLine
if ($powerLine -ne 'Online') {
    $msg = "AC line status is '$powerLine': the protocol requires AC power."
    if ($Quick -or $AllowBattery) { Write-Warning $msg } else { throw "$msg Plug in the charger, or pass -AllowBattery to run anyway." }
}

# The benchmark window must really be displayed, otherwise the compositor drops the presents and paces nothing.
$desktopState = Get-DesktopState
$hidden = @(Test-DesktopHidden $desktopState)
if ($hidden.Count) {
    $msg = "The desktop is not displayed: $($hidden -join '; ')."
    if ($AllowHiddenDesktop) { Write-Warning "$msg Continuing because of -AllowHiddenDesktop: frame_ms and present_ms of this session are not comparable with a displayed session." }
    else { throw "$msg Turn the display on and unlock the session, or pass -AllowHiddenDesktop to run anyway." }
}
if ($desktopState.display -eq 'unknown') { Write-Warning 'The display power state could not be determined.' }
if ($CoverWindow) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
}

New-Item -ItemType Directory -Force -Path $sessionDir | Out-Null
$logPath = Join-Path $sessionDir 'runner.log'
function Write-Log([string]$Text) {
    Write-Host $Text
    Add-Content -LiteralPath $logPath -Value ("{0} {1}" -f (Get-UtcStamp), $Text) -Encoding UTF8
}

# ---------------------------------------------------------------------------------------------- environment

$environment = Get-Environment
$envPath = Join-Path $sessionDir 'environment.json'
if (Test-Path -LiteralPath $envPath) {
    $first = Read-JsonFile $envPath
    $resumePath = Join-Path $sessionDir ("environment.resume-{0}.json" -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
    Write-Utf8 $resumePath ($environment | ConvertTo-Json -Depth 8)
    if ($first) {
        $before = @{}; foreach ($g in @($first.git)) { $before[$g.name] = $g.commit }
        foreach ($g in $environment.git) { if ($before[$g.name] -ne $g.commit) { Write-Warning "Resume: commit of $($g.name) changed ($($before[$g.name]) -> $($g.commit))." } }
        if ($first.power_plan.guid -ne $environment.power_plan.guid) { Write-Warning "Resume: power plan changed ($($first.power_plan.name) -> $($environment.power_plan.name))." }
        if ($first.ac_line -ne $environment.ac_line) { Write-Warning "Resume: AC line status changed ($($first.ac_line) -> $($environment.ac_line))." }
        $d0 = (@($first.gpus) | ForEach-Object { "$($_.name) $($_.driver_version)" }) -join ';'
        $d1 = (@($environment.gpus) | ForEach-Object { "$($_.name) $($_.driver_version)" }) -join ';'
        if ($d0 -ne $d1) { Write-Warning "Resume: GPU drivers changed ($d0 -> $d1)." }
        # Runs that cannot be compared must not share a session.
        $refuse = @()
        if ([bool]$first.runner.gpu_timing -ne [bool]$GpuTiming) { $refuse += "the session was started with -GpuTiming = $([bool]$first.runner.gpu_timing); GPU-timed and untimed runs must not be mixed" }
        if ([bool]$first.runner.quick -ne [bool]$Quick) { $refuse += "the session was started with -Quick = $([bool]$first.runner.quick)" }
        if ([int]$first.runner.width -ne $Width -or [int]$first.runner.height -ne $Height) { $refuse += "the session was started at $($first.runner.width)x$($first.runner.height), not ${Width}x${Height}" }
        if ([bool]$first.runner.cover_window -ne [bool]$CoverWindow) { $refuse += "the session was started with -CoverWindow = $([bool]$first.runner.cover_window); covered and displayed windows are paced differently" }
        if ($refuse.Count) { throw "Refusing to resume session '$Session': $($refuse -join '; '). Use a new -Session." }
        $changed = @()
        $exeOf = @{ reference = $ReferenceExe; nvrhi = $NvrhiExe }
        foreach ($k in 'reference', 'nvrhi') {
            if (-not $needed.ContainsKey($exeOf[$k])) { continue }                   # not used by this invocation
            if (-not $first.executables.$k.sha256) { continue }                      # did not exist when the session started
            if ($first.executables.$k.sha256 -ne $environment.executables.$k.sha256) { $changed += $k }
        }
        if ($changed.Count) {
            $msg = "the $($changed -join ' and ') executable(s) changed since session '$Session' started (SHA-256 differs from environment.json)"
            if ($AllowExeChange) { Write-Warning "Resume: $msg; continuing because of -AllowExeChange. The runs of this session are no longer from one build." }
            else { throw "Refusing to resume: $msg. Runs of different builds must not be mixed: use a new -Session, or pass -AllowExeChange to resume anyway." }
        }
    }
} else {
    Write-Utf8 $envPath ($environment | ConvertTo-Json -Depth 8)
}

# ---------------------------------------------------------------------------------------------- manifest

$manifestPath = Join-Path $sessionDir 'manifest.json'
$entries = [ordered]@{}     # id -> ordered hashtable; entries of earlier invocations are kept
$created = Get-UtcStamp
if (Test-Path -LiteralPath $manifestPath) {
    $old = Read-JsonFile $manifestPath
    if (-not $old) { throw "Existing $manifestPath is unreadable; fix or remove it before resuming." }
    if ($old.created_utc) { $created = $old.created_utc }
    foreach ($e in @($old.runs)) {
        $h = [ordered]@{}
        foreach ($p in $e.PSObject.Properties) { $h[$p.Name] = $p.Value }
        $entries[$h.id] = $h
    }
}

function Save-Manifest {
    $doc = [ordered]@{
        session = $Session; created_utc = $created; updated_utc = Get-UtcStamp; seed = $Seed; quick = [bool]$Quick
        adapter_luid = "$AdapterLuid"; adapter = $script:AdapterFullName
        runs = @($entries.Values)
    }
    Write-Utf8 $manifestPath ($doc | ConvertTo-Json -Depth 6)
}

function Test-RunOk([string]$Dir) {
    $jsonPath = Join-Path $Dir 'run.json'
    if (-not (Test-Path -LiteralPath $jsonPath) -or -not (Test-Path -LiteralPath (Join-Path $Dir 'frames.csv'))) { return $false }
    $j = Read-JsonFile $jsonPath
    return ($j -and $j.status -eq 'ok')
}

$order = 0
foreach ($s in $schedule) {
    $order++
    $c = $s.config
    $dir = Join-Path (Join-Path $sessionDir $c.name) $s.rep
    $s.dir = $dir; $s.order = $order
    $prev = $entries[$s.id]
    $status = 'pending'
    if (Test-RunOk $dir) { $status = 'ok' }
    elseif ($prev -and $prev.status -eq 'unsupported' -and -not $RetryUnsupported) { $status = 'unsupported' }
    $s.skip = ($status -ne 'pending')
    if ($s.skip -and $prev) { $prev.status = $status; continue }   # keep the recorded details of the finished run
    $entry = [ordered]@{
        id = $s.id; config = $c.name; rep = $s.rep; renderer = $c.renderer; api = $c.api; binding = $c.binding
        threads = $c.threads; variant = "$($c.variant)"; gpu_timing = [bool]$GpuTiming
        headline = [bool]$c.headline; warmup = $c.warmup; duration = $c.duration
        status = $status; failure_kind = $null; reason = ''; exit_code = $null; order = $order; attempts = 0
        started_utc = $null; elapsed_seconds = $null; command_line = $null; dir = "$($c.name)/$($s.rep)"
        desktop = $null; window_covered = [bool]$CoverWindow; window_visible_fraction = $null
    }
    if ($prev) { $entry.attempts = [int]$prev.attempts; $entry.previous_status = $prev.status; $entry.previous_reason = $prev.reason }
    $entries[$s.id] = $entry
}
Save-Manifest

$todo = @($schedule | Where-Object { -not $_.skip })
Write-Log ("Session {0}: {1} runs scheduled, {2} already done, {3} to run. Output: {4}" -f $Session, $schedule.Count, ($schedule.Count - $todo.Count), $todo.Count, $sessionDir)

# ---------------------------------------------------------------------------------------------- run loop

$executed = 0
$process = $null
$cover = $null
$stoppedHidden = $false
if (-not [BenchDesktop]::KeepAwake()) { Write-Warning 'SetThreadExecutionState failed: the display may turn off during the session.' }
try {
    foreach ($s in $todo) {
        if ($MaxRuns -gt 0 -and $executed -ge $MaxRuns) { Write-Log "Stopping after -MaxRuns $MaxRuns."; break }
        $c = $s.config
        $entry = $entries[$s.id]

        # Desktop state before every run: a run behind the lock screen or with the display off is not paced like the
        # others. The session stops (it can be resumed) instead of recording such a run.
        [void][BenchDesktop]::KeepAwake()
        $desktopState = Get-DesktopState
        $hidden = @(Test-DesktopHidden $desktopState)
        if ($hidden.Count -and -not $AllowHiddenDesktop) {
            Write-Log "Stopping before $($s.id): $($hidden -join '; '). Turn the display on, unlock the session and start again with the same -Session to resume."
            $stoppedHidden = $true
            break
        }
        $entry.desktop = $desktopState
        $exe = $ReferenceExe; if ($c.renderer -eq 'nvrhi') { $exe = $NvrhiExe }

        # A fresh directory per attempt: a stale run.json or frames.csv must never be mistaken for this run.
        if (Test-Path -LiteralPath $s.dir) { Remove-Item -LiteralPath $s.dir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $s.dir | Out-Null

        $argList = @('-benchmark', '-renderer', $c.renderer, '-api', $c.api)
        if ($c.renderer -ne 'native') { $argList += @('-binding', $c.binding) }
        $argList += @('-threads', "$($c.threads)", '-warmup', "$($c.warmup)", '-duration', "$($c.duration)",
                      '-window', "$Width", "$Height", '-adapter_luid', "$AdapterLuid")
        if ($GpuTiming) { $argList += '-gpu_timing' }
        if ($c.variant) { $argList += "-$($c.variant)" }        # nvrhi sensitivity switch
        $argList += @('-output', $s.dir)
        $argList += @(Split-List $ExtraArgs)
        $argString = ($argList | ForEach-Object { Format-Arg $_ }) -join ' '

        $timeout = [int][math]::Ceiling($c.warmup + $c.duration + $StartupTimeoutSeconds)
        $entry.command_line = "$exe $argString"
        $entry.started_utc = Get-UtcStamp
        $entry.attempts = [int]$entry.attempts + 1
        $executed++
        Write-Host ("[{0}/{1}] {2}" -f $executed, $todo.Count, $s.id) -NoNewline

        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        $stdout = Join-Path $s.dir 'stdout.txt'; $stderr = Join-Path $s.dir 'stderr.txt'
        if ($CoverWindow) {
            # A topmost window over the whole primary screen: the benchmark window is created underneath it.
            $cover = New-Object System.Windows.Forms.Form
            $cover.FormBorderStyle = 'None'; $cover.StartPosition = 'Manual'; $cover.TopMost = $true; $cover.ShowInTaskbar = $false
            $cover.BackColor = [System.Drawing.Color]::FromArgb(32, 32, 32); $cover.Text = 'benchmark window cover'
            $cover.Bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
            $cover.Show(); [System.Windows.Forms.Application]::DoEvents()
        }
        $process = Start-Process -FilePath $exe -ArgumentList $argString -WorkingDirectory (Split-Path $exe -Parent) `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -NoNewWindow -PassThru
        $null = $process.Handle      # keeps the handle, otherwise ExitCode is empty after the process ended
        $timedOut = $false
        if ($cover) {
            while (-not $process.WaitForExit(200)) {
                # The benchmark window makes itself topmost: put the cover back above it, and keep it responsive.
                $cover.TopMost = $true; $cover.BringToFront()
                [System.Windows.Forms.Application]::DoEvents()
                if ($clock.Elapsed.TotalSeconds -gt $timeout) { $timedOut = $true; break }
            }
        } else {
            $timedOut = -not $process.WaitForExit($timeout * 1000)
        }
        if ($timedOut) { Stop-Tree $process } else { $process.WaitForExit() }
        $clock.Stop()
        if ($cover) { $cover.Close(); $cover.Dispose(); $cover = $null; [System.Windows.Forms.Application]::DoEvents() }
        $exitCode = $null
        try { $exitCode = $process.ExitCode } catch {}
        $process = $null

        $jsonPath = Join-Path $s.dir 'run.json'
        $json = $null
        if (Test-Path -LiteralPath $jsonPath) { $json = Read-JsonFile $jsonPath }
        $status = 'failed'; $kind = $null; $reason = ''
        if ($timedOut) {
            $status = 'timeout'
            $reason = "no exit within $timeout s; process tree killed"
            if ($json -and $json.status -eq 'ok') {
                # The files are complete but the process hung on shutdown: not a clean run, and it must not be
                # picked up as ok by a resume or by the analysis.
                Move-Item -LiteralPath $jsonPath -Destination (Join-Path $s.dir 'run.json.rejected') -Force
                $reason += ' (it had written an ok run.json, kept as run.json.rejected)'
            }
        } elseif ($json -and $json.status -eq 'ok' -and $exitCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $s.dir 'frames.csv'))) {
            $status = 'ok'
        } elseif ($json -and $json.status -eq 'failed') {
            $kind = "$($json.failure_kind)"
            if ($kind -eq 'unsupported') { $status = 'unsupported' }
            $reason = "$($json.reason)"
        } elseif ($json -and $json.status -eq 'ok') {
            $reason = "run.json says ok but the exit code is $exitCode or frames.csv is missing"
            Move-Item -LiteralPath $jsonPath -Destination (Join-Path $s.dir 'run.json.rejected') -Force
        } else {
            $status = 'crashed'
            $reason = "exit code $exitCode without a readable run.json"
            $tail = Get-Tail $stderr
            if (-not $tail) { $tail = Get-Tail $stdout }
            if ($tail) { $reason += ": $tail" }
        }
        $entry.status = $status; $entry.failure_kind = $kind; $entry.reason = $reason; $entry.exit_code = $exitCode
        $entry.elapsed_seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1)
        $visible = $null
        if ($status -eq 'ok' -and $json.extra -and $json.extra.window_visible_fraction) { $visible = [double]$json.extra.window_visible_fraction }
        $entry.window_visible_fraction = $visible
        Save-Manifest

        $line = " -> $status"
        if ($status -eq 'ok') { $line += (" (render+submit {0:n3} ms, frame {1:n3} ms, {2} frames)" -f $json.median_cpu_render_ms, $json.median_frame_ms, $json.frame_count) }
        else { $line += " (exit $exitCode): $reason" }
        # The executable reports how much of its window was the topmost one at the end of the run.
        if ($null -ne $visible -and $visible -lt 1.0 -and -not $CoverWindow) { $line += (" WARNING: only {0:p0} of the window was visible (covered windows are not paced)" -f $visible) }
        Write-Host $line
        Add-Content -LiteralPath $logPath -Value ("{0} {1}{2}" -f (Get-UtcStamp), $s.id, $line) -Encoding UTF8

        if ($CooldownSeconds -gt 0) { Start-Sleep -Milliseconds ([int]($CooldownSeconds * 1000)) }
    }
} finally {
    Stop-Tree $process     # Ctrl+C or an error: never leave a benchmark process behind
    if ($cover) { try { $cover.Close(); $cover.Dispose() } catch {} }
    [BenchDesktop]::Restore()
    Save-Manifest
}

# ---------------------------------------------------------------------------------------------- summary

$mine = @($schedule | ForEach-Object { $entries[$_.id] })
$counts = $mine | Group-Object { $_.status } | Sort-Object Name
Write-Log ("Done: " + (($counts | ForEach-Object { "$($_.Count) $($_.Name)" }) -join ', '))
$bad = @($mine | Where-Object { $_.status -ne 'ok' -and $_.status -ne 'unsupported' })
foreach ($e in @($mine | Where-Object { $_.status -ne 'ok' -and $_.status -ne 'pending' })) { Write-Log ("  {0,-11} {1}: {2}" -f $e.status, $e.id, $e.reason) }
Write-Host "Manifest: $manifestPath"
Write-Host "Analyse : python scripts\analyze.py `"$sessionDir`""
if ($bad.Count -or $stoppedHidden) { exit 1 }
exit 0
