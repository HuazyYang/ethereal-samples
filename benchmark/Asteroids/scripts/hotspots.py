"""Hot-spot attribution from the collapsed CPU sampling stacks of scripts/hotspots_capture.ps1 (docs/PROFILING.md).

    python scripts/hotspots.py results/profiles-01 --out report/data

Reads <session>/profiles/<name>.stacks.txt, .images.txt, .profile.json and <name>_run/run.json + frames.csv, and writes
    <out>/hotspots_tables.md   per configuration: module shares, grouped shares, top functions, hot source lines
    <out>/hotspots.json        the same numbers

Method (all per thread, samples at the collector's rate, nominally 4 kHz):
- The main thread's samples are classified into the benchmark's columns by the source line of the renderer's
  ``Render`` frame (resolved with dbghelp from the PDB): render + submit, update, present, outside. For nvrhi the whole
  of ``AsteroidsRenderer::Render`` is render + submit (update is in ``Animate``, Present in Donut's DeviceManager).
- Shares are relative to the render + submit samples of the main thread; ns per draw = share x median_cpu_render_ms of
  the profiled run (run.json) x 1e6 / draw_count. The profiled run carries the sampler's overhead and is not a measurement.
- "Charged" grouping: a sample is charged to the first frame from the leaf upward that belongs to the application, the
  abstraction layer, the API runtime or the driver; OS (kernel, ntdll, kernelbase) and CRT (memcpy ...) time is thereby
  charged to the code that called into it. The raw leaf-module table is given next to it.
- Worker thread (8 threads): the worker's samples in its recording function, converted with the same ms-per-sample as
  the main thread's render + submit samples, divided by the worker's draws (ceil(50000 / threads)).
Caveats: /GL + LTCG inlines freely, so a frame name is the outermost non-inlined function; the hot-line table resolves
the inline chain of the sampled instruction. /OPT:ICF folds identical functions, so a tiny function may carry another
name (e.g. a trivial getter named after an unrelated getter). The NVIDIA driver has no public symbols: module level only.
"""
import argparse
import csv
import ctypes
import ctypes.wintypes as wt
import json
import math
import os
import re
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ETHEREAL = os.path.dirname(os.path.dirname(os.path.dirname(ROOT)))   # benchmark/Asteroids -> benchmark -> ethereal-samples -> ethereal
BIN = os.path.join(ETHEREAL, 'build', 'bin')
DBGHELP_CANDIDATES = [
    r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll',
    r'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\dbghelp.dll',
    r'C:\Windows\System32\dbghelp.dll',
]
SAMPLE_MS_NOMINAL = 0.25  # CpuUsageHigh.json: cpuSampleRate 4000

# --- renderer specifics ------------------------------------------------------------------------------------------
# Line ranges of the Render function that belong to each benchmark column (see the lap.To calls in the sources).
RENDERERS = {
    'native': dict(
        exe='asteroids_native.exe', render_fn='AsteroidsD3D12::Asteroids::Render', render_file='asteroids_d3d12.cpp',
        phases=[(967, 998, 'render'), (999, 1030, 'update'), (1031, 1177, 'render'), (1178, 1183, 'other'),
                (1184, 1189, 'present'), (1190, 1194, 'render'), (1195, 1300, 'other')],
        subset_fn='AsteroidsD3D12::Asteroids::RenderSubset', worker_fn='AsteroidsD3D12::Asteroids::WorkerMain',
        update_fn='AsteroidsSimulation::Update'),
    'nvrhi': dict(
        exe='asteroids_nvrhi.exe', render_fn='AsteroidsRenderer::Render', render_file='renderer.cpp', phases=None,
        subset_fn='AsteroidsRenderer::RecordAsteroids', worker_fn='AsteroidsRenderer::WorkerMain',
        update_fn='AsteroidsSimulation::Update'),
}
LINE_MODULES = ('asteroids_native.exe', 'asteroids_nvrhi.exe')

RUNTIME_MODULES = {'d3d12.dll', 'd3d12core.dll', 'dxgi.dll', 'vulkan-1.dll', 'dxcore.dll', 'd3d11.dll', 'd3d10warp.dll'}
OS_USER_MODULES = {'ntdll.dll', 'kernelbase.dll', 'kernel32.dll', 'win32u.dll', 'user32.dll', 'gdi32.dll', 'gdi32full.dll',
                   'combase.dll', 'msvcp_win.dll', 'msvcrt.dll', 'rpcrt4.dll', 'sechost.dll', 'advapi32.dll', 'bcryptprimitives.dll'}
CRT_MODULES = {'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'ucrtbase.dll'}
KERNEL_MODULES = {'ntoskrnl.exe', 'hal.dll', 'ntkrnlmp.exe'}
WAIT_RE = re.compile(r'JoinWorkers|SwitchToThread|NtYieldExecution|KeYieldExecution|this_thread::yield')


CRT_LIKE_RE = re.compile(r'^(std::|__security_check_cookie|memcpy|memset|memmove|operator new|operator delete|free$|malloc$)')


def module_category(mod, func):
    m = mod.lower()
    if m in ('asteroids_native.exe', 'asteroids_nvrhi.exe'):
        if CRT_LIKE_RE.match(func):
            return 'crt'  # library code linked into the image (std::unordered_map, stack cookie ...): charged to its caller
    if m in ('asteroids_native.exe', 'asteroids_nvrhi.exe'):
        if func.startswith('nvrhi::'):
            return 'layer:nvrhi'
        if func.startswith('donut::'):
            return 'layer:donut'
        return 'app'
    if m in RUNTIME_MODULES:
        return 'runtime'
    if m.startswith('nv') or m.startswith('amd') or m.startswith('ati'):
        return 'driver'
    if m in KERNEL_MODULES or m.endswith('.sys'):
        return 'os-kernel'
    if m in OS_USER_MODULES:
        return 'os-user'
    if m in CRT_MODULES:
        return 'crt'
    return 'other'


CHARGEABLE = ('app', 'layer:nvrhi', 'layer:donut', 'runtime', 'driver')


# --- dbghelp --------------------------------------------------------------------------------------------------------
class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD), ('FileName', ctypes.c_wchar_p), ('Address', ctypes.c_uint64)]


MAX_SYM_NAME = 2000


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('TypeIndex', wt.DWORD), ('Reserved', ctypes.c_uint64 * 2), ('Index', wt.DWORD), ('Size', wt.DWORD),
                ('ModBase', ctypes.c_uint64), ('Flags', wt.DWORD), ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.DWORD),
                ('Scope', wt.DWORD), ('Tag', wt.DWORD), ('NameLen', wt.DWORD), ('MaxNameLen', wt.DWORD), ('Name', ctypes.c_wchar * MAX_SYM_NAME)]


class Symbolizer:
    """Source lines and inline chains for addresses of the images loaded from the build's bin directory."""

    def __init__(self, search_path):
        self.ok = False
        for cand in DBGHELP_CANDIDATES:
            if os.path.exists(cand):
                try:
                    self.dbg = ctypes.WinDLL(cand)
                    break
                except OSError:
                    continue
        else:
            return
        d = self.dbg
        d.SymSetOptions.restype = wt.DWORD
        d.SymInitializeW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, wt.BOOL]
        d.SymInitializeW.restype = wt.BOOL
        d.SymCleanup.argtypes = [ctypes.c_void_p]
        d.SymLoadModuleExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
        d.SymLoadModuleExW.restype = ctypes.c_uint64
        d.SymUnloadModule64.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        d.SymGetLineFromAddrW64.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINEW64)]
        d.SymGetLineFromAddrW64.restype = wt.BOOL
        d.SymFromAddrW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
        d.SymFromAddrW.restype = wt.BOOL
        d.SymAddrIncludeInlineTrace.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        d.SymAddrIncludeInlineTrace.restype = wt.DWORD
        d.SymQueryInlineTrace.argtypes = [ctypes.c_void_p, ctypes.c_uint64, wt.DWORD, ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(wt.DWORD)]
        d.SymQueryInlineTrace.restype = wt.BOOL
        d.SymFromInlineContextW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, wt.DWORD, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
        d.SymFromInlineContextW.restype = wt.BOOL
        d.SymGetLineFromInlineContextW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, wt.DWORD, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINEW64)]
        d.SymGetLineFromInlineContextW.restype = wt.BOOL
        d.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES
        self.h = ctypes.c_void_p(0x4711)
        if not d.SymInitializeW(self.h, search_path, False):
            return
        self.ok = True
        self.loaded = {}
        self.cache = {}

    def load(self, name, base, size):
        if not self.ok or name.lower() in self.loaded:
            return
        path = os.path.join(BIN, name)
        if not os.path.exists(path):
            return
        r = self.dbg.SymLoadModuleExW(self.h, None, path, None, base, size, None, 0)
        self.loaded[name.lower()] = (base, size, bool(r))

    def unload_all(self):
        if not self.ok:
            return
        for base, size, okk in self.loaded.values():
            if okk:
                self.dbg.SymUnloadModule64(self.h, base)
        self.loaded = {}
        self.cache = {}

    def line(self, addr):
        """(file, line) of the outer function's line table at addr, or (None, None)."""
        if not self.ok:
            return None, None
        key = ('L', addr)
        if key in self.cache:
            return self.cache[key]
        l = IMAGEHLP_LINEW64(); l.SizeOfStruct = ctypes.sizeof(l); disp = wt.DWORD()
        r = (None, None)
        if self.dbg.SymGetLineFromAddrW64(self.h, addr, ctypes.byref(disp), ctypes.byref(l)):
            r = (l.FileName, l.LineNumber)
        self.cache[key] = r
        return r

    def inline_chain(self, addr):
        """[(function, file, line), ...] innermost inlinee first, for addr; [] when nothing is inlined there."""
        if not self.ok:
            return []
        key = ('I', addr)
        if key in self.cache:
            return self.cache[key]
        out = []
        n = self.dbg.SymAddrIncludeInlineTrace(self.h, addr)
        if n:
            ctx = wt.DWORD(); idx = wt.DWORD()
            if self.dbg.SymQueryInlineTrace(self.h, addr, 0, addr, addr, ctypes.byref(ctx), ctypes.byref(idx)):
                for i in range(n):
                    s = SYMBOL_INFOW(); s.SizeOfStruct = 88; s.MaxNameLen = MAX_SYM_NAME; d = ctypes.c_uint64()
                    l = IMAGEHLP_LINEW64(); l.SizeOfStruct = ctypes.sizeof(l); dl = wt.DWORD()
                    name = None; f = None; ln = None
                    if self.dbg.SymFromInlineContextW(self.h, addr, ctx.value + i, ctypes.byref(d), ctypes.byref(s)):
                        name = s.Name
                    if self.dbg.SymGetLineFromInlineContextW(self.h, addr, ctx.value + i, 0, ctypes.byref(dl), ctypes.byref(l)):
                        f, ln = l.FileName, l.LineNumber
                    out.append((name, f, ln))
        self.cache[key] = out
        return out


# --- parsing ----------------------------------------------------------------------------------------------------------
FRAME_RE = re.compile(r'^(.*?)@(0x[0-9a-fA-F]+)$')


class Frame:
    __slots__ = ('name', 'mod', 'func', 'addr', 'cat')

    def __init__(self, text):
        m = FRAME_RE.match(text)
        if m:
            self.name, self.addr = m.group(1), int(m.group(2), 16)
        else:
            self.name, self.addr = text, None
        if '!' in self.name:
            self.mod, self.func = self.name.split('!', 1)
        else:
            self.mod, self.func = self.name, ''
        self.cat = module_category(self.mod, self.func)


def parse_stacks(path):
    threads = {}
    stacks = []  # (tid, count, [Frame...])
    header = {}
    frame_cache = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('# thread '):
                m = re.match(r'# thread (\d+) samples=(\d+) start=(.*)$', line)
                threads[m.group(1)] = dict(samples=int(m.group(2)), start=m.group(3))
                continue
            if line.startswith('#'):
                for kv in line[1:].split():
                    if '=' in kv:
                        k, v = kv.split('=', 1)
                        header[k] = v
                continue
            tid, start, cnt, st = line.split('\t')
            frames = []
            for t in st.split(';'):
                fr = frame_cache.get(t)
                if fr is None:
                    fr = Frame(t)
                    frame_cache[t] = fr
                frames.append(fr)
            stacks.append((tid, int(cnt), frames))
    return header, threads, stacks


def parse_images(path, pid):
    images = {}
    rx = re.compile(r'^\s*\S+,\s*\S+,\s*Image,\s*\S+,\s*(.+?) \(\s*(\d+)\),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),.*?"([^"]+)",')
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if ', Image,' not in line and ',   Image,' not in line:
                continue
            m = rx.match(line)
            if not m or int(m.group(2)) != pid:
                continue
            base, end, name = int(m.group(3), 16), int(m.group(4), 16), m.group(5)
            images[name.lower()] = (base, end - base, name)
    return images


# --- analysis ---------------------------------------------------------------------------------------------------------
def classify_phase(frames, info, sym):
    """Benchmark column of a main-thread sample: render, update, present, other, outside."""
    rf = None
    for i, fr in enumerate(frames):
        if fr.func == info['render_fn']:
            rf = (i, fr)
            break
    if rf is None:
        return 'outside', None
    if info['phases'] is None:
        return 'render', None
    i, fr = rf
    if fr.addr is None:
        return 'render?', None
    addr = fr.addr if i == 0 else fr.addr - 1
    file, ln = sym.line(addr)
    if ln is None:
        return 'render?', None
    base = os.path.basename(file or '')
    if base.lower() != info['render_file'].lower():
        # a header inlined into Render (e.g. DirectXMath): charge like the surrounding code is unknown -> render?
        return 'render?', (base, ln)
    for lo, hi, ph in info['phases']:
        if lo <= ln <= hi:
            return ph, (base, ln)
    return 'render?', (base, ln)


def charged_category(frames, is_worker=False):
    """First chargeable frame from the leaf upward; OS/CRT leaves are charged to their caller."""
    if any(WAIT_RE.search(fr.func) for fr in frames[:8]) and not is_worker:
        return 'wait-for-workers'
    for fr in frames:
        if fr.cat in CHARGEABLE:
            return fr.cat
    return frames[0].cat


def analyse(session_dir, name, sym, top_n=25):
    pdir = os.path.join(session_dir, 'profiles')
    m = re.match(r'^(native|nvrhi)_(d3d11|d3d12|vk)_(\w+?)_t(\d+)(__.*)?$', name)
    renderer, api, binding, threads, suffix = m.group(1), m.group(2), m.group(3), int(m.group(4)), (m.group(5) or '')
    info = RENDERERS[renderer]
    header, thr, stacks = parse_stacks(os.path.join(pdir, name + '.stacks.txt'))
    pid = int(header['pid'])
    images = parse_images(os.path.join(pdir, name + '.images.txt'), pid)
    sym.unload_all()
    for mod in LINE_MODULES:
        if mod.lower() in images:
            base, size, _ = images[mod.lower()]
            sym.load(mod, base, size)
    with open(os.path.join(pdir, name + '_run', 'run.json'), encoding='utf-8') as f:
        run = json.load(f)
    with open(os.path.join(pdir, name + '.profile.json'), encoding='utf-8') as f:
        prof = json.load(f)
    draws = None
    with open(os.path.join(pdir, name + '_run', 'frames.csv'), encoding='utf-8') as f:
        for row in csv.DictReader(f):
            draws = int(row['draw_count'])
            break
    per_frame_ms = run['median_cpu_render_ms']
    frame_ms = run['median_frame_ms']

    # threads
    main_tid = next((t for t, v in thr.items() if 'mainCRTStartup' in v['start']), None)
    worker_tid = None
    if threads > 1:
        wsamples = Counter()
        for tid, c, frames in stacks:
            if tid != main_tid and any(fr.func == info['worker_fn'] for fr in frames):
                wsamples[tid] += c
        if wsamples:
            worker_tid = wsamples.most_common(1)[0][0]

    # main thread: phases
    phase_counts = Counter()
    phase_unknown = Counter()
    render_stacks = []
    for tid, c, frames in stacks:
        if tid != main_tid:
            continue
        ph, where = classify_phase(frames, info, sym)
        phase_counts[ph] += c
        if ph == 'render?':
            phase_unknown[where] += c
        if ph in ('render', 'render?'):
            render_stacks.append((c, frames))
    rs = sum(c for c, _ in render_stacks)
    main_total = thr[main_tid]['samples']

    # calibration: nominal sample interval against the measured render+submit time
    frames_in_capture = prof['capture_seconds'] * 1000.0 / frame_ms
    nominal_ms_per_frame = rs * SAMPLE_MS_NOMINAL / frames_in_capture
    ms_per_sample = per_frame_ms * frames_in_capture / rs if rs else 0
    # 1 thread: share x measured render+submit per frame (the main thread is on the CPU for the whole column, the
    # calibration above agrees to 2 %). More threads: the main thread yields while it waits for the workers and is
    # then not sampled, so samples are converted at the nominal interval and the remainder is reported as off-CPU.
    if threads == 1:
        def ns(x):
            return x / rs * per_frame_ms * 1e6 / draws if rs else 0.0
        off_cpu_ms = 0.0
    else:
        def ns(x):
            return x * SAMPLE_MS_NOMINAL / frames_in_capture * 1e6 / draws
        off_cpu_ms = per_frame_ms - nominal_ms_per_frame

    def tables(stack_list, total, nsf, boundary_fn):
        leaf_mod = Counter(); leaf_cat = Counter(); charged = Counter()
        excl = Counter(); incl = Counter(); incl_cat = Counter()
        leaf_addr = Counter()
        entry = defaultdict(Counter)
        for c, frames in stack_list:
            lf = frames[0]
            leaf_mod[lf.mod] += c
            leaf_cat[lf.cat] += c
            ch = charged_category(frames, is_worker=(stack_list is not render_stacks))
            charged[ch] += c
            # entry point below the renderer's frame: the outermost layer frame, else the outermost runtime frame, else the leaf
            below = frames
            for i, fr in enumerate(frames):
                if fr.func == boundary_fn:
                    below = frames[:i]
                    break
            ep = None
            for fr in reversed(below):
                if fr.cat.startswith('layer'):
                    ep = 'layer: ' + short(fr.func); break
            if ep is None:
                for fr in reversed(below):
                    if fr.cat == 'runtime':
                        ep = 'runtime: ' + short(fr.func); break
            if ep is None:
                if ch == 'wait-for-workers':
                    ep = 'wait-for-workers (yield)'
                else:
                    owner = next((fr for fr in below if fr.cat in CHARGEABLE), lf)
                    ep = ch + ': ' + short(owner.func)
            entry[ep]['total'] += c
            entry[ep][ch] += c
            excl[lf.name] += c
            seen = set()
            cats = set()
            for fr in frames:
                if fr.name not in seen:
                    seen.add(fr.name)
                    incl[fr.name] += c
                cats.add(fr.cat)
            for cat in cats:
                incl_cat[cat] += c
            if lf.cat == 'crt':
                # memcpy / memset / std::unordered_map / stack cookie: attribute to the calling line of the owner
                owner = next((fr for fr in frames if fr.cat in CHARGEABLE), None)
                if owner is not None and owner.addr is not None and owner.mod in LINE_MODULES:
                    leaf_addr[(owner.name, owner.addr - 1, 'via ' + short(lf.func))] += c
                else:
                    leaf_addr[(lf.name, lf.addr, '')] += c
            elif lf.addr is not None and lf.mod in LINE_MODULES:
                leaf_addr[(lf.name, lf.addr, '')] += c
        # hot lines: resolve the top leaf addresses
        lines = Counter()
        for (fname, addr, via), c in leaf_addr.most_common(600):
            if addr is None:
                lines[(fname, '?', 0, via)] += c
                continue
            file, ln = sym.line(addr)
            chain = sym.inline_chain(addr)
            inl = ' <- '.join(f"{short(nm)} ({os.path.basename(f or '?')}:{l})" for nm, f, l in chain) if chain else ''
            if via:
                inl = (inl + ' ' if inl else '') + via
            key = (fname, os.path.basename(file or '?'), ln, inl)
            lines[key] += c
        eps = sorted(entry.items(), key=lambda kv: -kv[1]['total'])
        out = dict(
            total=total,
            entry_points=[(k, v['total'], v['total'] / total, nsf(v['total']),
                           {cat: (n, nsf(n)) for cat, n in v.items() if cat != 'total'}) for k, v in eps[:25]],
            leaf_module=[(k, v, v / total, nsf(v)) for k, v in leaf_mod.most_common()],
            leaf_category=[(k, v, v / total, nsf(v)) for k, v in leaf_cat.most_common()],
            charged=[(k, v, v / total, nsf(v)) for k, v in charged.most_common()],
            inclusive_category=[(k, v, v / total, nsf(v)) for k, v in incl_cat.most_common()],
            top_exclusive=[(k, v, v / total, nsf(v), incl[k], incl[k] / total, nsf(incl[k])) for k, v in excl.most_common(top_n)],
            top_inclusive=[(k, v, v / total, nsf(v), excl[k], excl[k] / total, nsf(excl[k])) for k, v in incl.most_common(60)],
            hot_lines=[(k, v, v / total, nsf(v)) for k, v in lines.most_common(top_n)],
        )
        # layer functions, inclusive and exclusive
        layer = [(k, v) for k, v in incl.most_common() if Frame(k).cat.startswith('layer')]
        out['layer_inclusive'] = [(k, v, v / total, nsf(v), excl[k], excl[k] / total, nsf(excl[k])) for k, v in layer[:30]]
        rt = [(k, v) for k, v in incl.most_common() if Frame(k).cat == 'runtime']
        out['runtime_inclusive'] = [(k, v, v / total, nsf(v), excl[k], excl[k] / total, nsf(excl[k])) for k, v in rt[:20]]
        return out

    result = dict(
        name=name, renderer=renderer, api=api, binding=binding, threads=threads, suffix=suffix,
        pid=pid, draws=draws, per_frame_ms=per_frame_ms, frame_ms=frame_ms, update_ms=None,
        extra=run.get('extra', {}), process_exit_code=prof.get('process_exit_code'),
        main_tid=main_tid, main_samples=main_total, render_samples=rs,
        phases={k: (v, v / main_total) for k, v in phase_counts.items()},
        phase_unknown=[(str(k), v) for k, v in phase_unknown.most_common(10)],
        frames_in_capture=frames_in_capture, nominal_ms_per_frame=nominal_ms_per_frame, ms_per_sample=ms_per_sample,
        off_cpu_ms=off_cpu_ms, off_cpu_ns_per_draw=off_cpu_ms * 1e6 / draws,
        symbols_loaded={k: v[2] for k, v in sym.loaded.items()} if sym.ok else {},
        main=tables(render_stacks, rs, ns, info['render_fn']),
    )
    if worker_tid:
        wst = [(c, frames) for tid, c, frames in stacks if tid == worker_tid and any(fr.func == info['subset_fn'] for fr in frames)]
        wtotal = sum(c for c, _ in wst)
        wdraws = math.ceil(50000 / threads)
        w_ms_per_frame = wtotal * SAMPLE_MS_NOMINAL / frames_in_capture
        result['worker'] = dict(tid=worker_tid, samples_total=thr[worker_tid]['samples'], samples_record=wtotal,
                                draws=wdraws, ms_per_frame=w_ms_per_frame,
                                **{'t': tables(wst, wtotal, lambda x: (x / wtotal * w_ms_per_frame * 1e6 / wdraws) if wtotal else 0.0,
                                               info['worker_fn'])})
        # main thread's own recording share versus waiting
        result['main_record_samples'] = sum(c for c, frames in render_stacks if any(fr.func == info['subset_fn'] for fr in frames))
    return result


def short(name):
    if not name:
        return '?'
    name = re.sub(r'<.*>', '<>', name)
    return name


# --- output -----------------------------------------------------------------------------------------------------------
def pct(x):
    return f'{100 * x:.1f}'


def md_tables(results, out_md):
    L = []
    L.append('# Hot-spot tables (generated by scripts/hotspots.py)\n')
    L.append('Per configuration, main thread, samples inside render + submit. Share = share of those samples; ns/draw = share x '
             'median render + submit of the profiled run / draw count. Profiled runs carry sampler overhead and are not the measured numbers. '
             'See report/data/hotspots.md for the reading and docs/PROFILING.md for the procedure.\n')
    for r in results:
        L.append(f"\n## {r['name']}\n")
        ex = r['extra']
        L.append(f"- profiled run: render + submit median {r['per_frame_ms']:.3f} ms, frame {r['frame_ms']:.3f} ms, draws {r['draws']}, "
                 f"cb_page_split={ex.get('cb_page_split', 'n/a')}, exit code {r['process_exit_code']}")
        ph = r['phases']
        L.append('- main thread samples: ' + str(r['main_samples']) + ', by column: ' + ', '.join(
            f"{k} {v[0]} ({pct(v[1])} %)" for k, v in sorted(ph.items(), key=lambda kv: -kv[1][0])))
        L.append(f"- render + submit samples {r['render_samples']}: nominal 0.25 ms x samples / frames = {r['nominal_ms_per_frame']:.3f} ms per frame "
                 f"against {r['per_frame_ms']:.3f} ms measured in the run (effective {r['ms_per_sample']:.3f} ms per sample)")
        if r['threads'] > 1:
            L.append(f"- conversion for {r['threads']} threads: samples x 0.25 ms / {r['frames_in_capture']:.0f} frames; the main thread is off the CPU "
                     f"(yielded while waiting for workers, or pre-empted) for the remaining {r['off_cpu_ms']:.3f} ms per frame = {r['off_cpu_ns_per_draw']:.1f} ns/draw")
        if r['phase_unknown']:
            L.append('- Render-frame lines not in a known range (counted as render): ' + ', '.join(f'{k} {v}' for k, v in r['phase_unknown']))
        L.append('- symbols loaded for line info: ' + ', '.join(f'{k}={v}' for k, v in r['symbols_loaded'].items()))
        emit_tables(L, r['main'], 'Main thread')
        if 'worker' in r:
            w = r['worker']
            L.append(f"\n### Worker thread {w['tid']} (one of {r['threads'] - 1}): {w['samples_record']} samples in its recording function "
                     f"of {w['samples_total']} on the thread; {w['ms_per_frame']:.3f} ms per frame at 0.25 ms per sample, {w['draws']} draws\n")
            L.append(f"Main thread samples with its own RecordAsteroids/RenderSubset: {r.get('main_record_samples')} of {r['render_samples']} render + submit samples.\n")
            emit_tables(L, w['t'], 'Worker thread', worker=True)
    with open(out_md, 'w', encoding='utf-8') as f:
        f.write('\n'.join(L) + '\n')


def emit_tables(L, t, title, worker=False):
    L.append(f'\n### {title}: grouped (charged to the first application / layer / runtime / driver frame)\n')
    L.append('| group | samples | share % | ns/draw |\n|---|---:|---:|---:|')
    for k, v, s, n in t['charged']:
        L.append(f'| {k} | {v} | {pct(s)} | {n:.1f} |')
    L.append(f'\n### {title}: entry points (outermost layer call, else outermost runtime call; a partition of the samples)\n')
    L.append('| entry point | samples | share % | ns/draw | of which layer own | runtime | driver | app | other |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    for k, v, s, n, sub in t['entry_points']:
        own = sum(x[1] for c, x in sub.items() if c.startswith('layer'))
        rt = sub.get('runtime', (0, 0))[1]; dr = sub.get('driver', (0, 0))[1]; ap = sub.get('app', (0, 0))[1]
        oth = n - own - rt - dr - ap
        L.append(f'| {esc(k)} | {v} | {pct(s)} | {n:.1f} | {own:.1f} | {rt:.1f} | {dr:.1f} | {ap:.1f} | {oth:.1f} |')
    L.append(f'\n### {title}: leaf module (exclusive)\n')
    L.append('| module | samples | share % | ns/draw |\n|---|---:|---:|---:|')
    for k, v, s, n in t['leaf_module'][:14]:
        L.append(f'| {k} | {v} | {pct(s)} | {n:.1f} |')
    L.append(f'\n### {title}: leaf category (exclusive) and inclusive category (any frame)\n')
    L.append('| category | exclusive samples | excl % | excl ns/draw | inclusive samples | incl % | incl ns/draw |\n|---|---:|---:|---:|---:|---:|---:|')
    inc = {k: (v, s, n) for k, v, s, n in t['inclusive_category']}
    for k, v, s, n in t['leaf_category']:
        iv = inc.get(k, (0, 0, 0))
        L.append(f'| {k} | {v} | {pct(s)} | {n:.1f} | {iv[0]} | {pct(iv[1])} | {iv[2]:.1f} |')
    L.append(f'\n### {title}: top functions by exclusive samples\n')
    L.append('| function | excl samples | excl % | excl ns/draw | incl samples | incl % | incl ns/draw |\n|---|---:|---:|---:|---:|---:|---:|')
    for k, v, s, n, iv, is_, in_ in t['top_exclusive']:
        L.append(f'| {esc(k)} | {v} | {pct(s)} | {n:.1f} | {iv} | {pct(is_)} | {in_:.1f} |')
    L.append(f'\n### {title}: abstraction-layer functions by inclusive samples\n')
    L.append('| function | incl samples | incl % | incl ns/draw | excl samples | excl % | excl ns/draw |\n|---|---:|---:|---:|---:|---:|---:|')
    for k, v, s, n, ev, es, en in t['layer_inclusive']:
        L.append(f'| {esc(k)} | {v} | {pct(s)} | {n:.1f} | {ev} | {pct(es)} | {en:.1f} |')
    L.append(f'\n### {title}: API runtime entry points by inclusive samples (driver and kernel below them included)\n')
    L.append('| function | incl samples | incl % | incl ns/draw | excl samples | excl % | excl ns/draw |\n|---|---:|---:|---:|---:|---:|---:|')
    for k, v, s, n, ev, es, en in t['runtime_inclusive'][:12]:
        L.append(f'| {esc(k)} | {v} | {pct(s)} | {n:.1f} | {ev} | {pct(es)} | {en:.1f} |')
    L.append(f'\n### {title}: hot source lines of the application and layer modules (sampled instruction, inline chain innermost first)\n')
    L.append('| function | file:line | inlined | samples | share % | ns/draw |\n|---|---|---|---:|---:|---:|')
    for (fn, file, ln, inl), v, s, n in t['hot_lines']:
        L.append(f'| {esc(fn)} | {file}:{ln} | {esc(inl)} | {v} | {pct(s)} | {n:.1f} |')


def esc(s):
    return str(s).replace('|', '\\|').replace('<', '&lt;').replace('>', '&gt;')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('session')
    ap.add_argument('--out', default=os.path.join(ROOT, 'report', 'data'))
    ap.add_argument('--only', nargs='*')
    a = ap.parse_args()
    pdir = os.path.join(a.session, 'profiles')
    names = sorted(f[:-len('.stacks.txt')] for f in os.listdir(pdir) if f.endswith('.stacks.txt'))
    if a.only:
        names = [n for n in names if n in a.only]
    sym = Symbolizer(BIN)
    if not sym.ok:
        print('warning: dbghelp not available, no line information', file=sys.stderr)
    results = []
    for n in names:
        try:
            r = analyse(a.session, n, sym)
        except FileNotFoundError as e:
            print(f'{n}: skipped ({e})', file=sys.stderr)
            continue
        results.append(r)
        ph = r['phases']
        print(f"{n}: main {r['main_samples']} samples, render+submit {r['render_samples']} "
              f"({pct(r['render_samples'] / r['main_samples'])} %), {r['per_frame_ms']:.3f} ms/frame, eff {r['ms_per_sample']:.3f} ms/sample; "
              + ', '.join(f"{k} {pct(v[1])}%" for k, v in sorted(ph.items(), key=lambda kv: -kv[1][0])))
        for k, v, s, nsd in r['main']['charged']:
            print(f'    {k:18s} {pct(s):>6s} %  {nsd:7.1f} ns/draw')
        if r['threads'] > 1:
            print(f"    {'off-CPU remainder':18s}         {r['off_cpu_ns_per_draw']:7.1f} ns/draw")
            w = r.get('worker')
            if w:
                print(f"    worker {w['tid']}: {w['samples_record']} samples recording, {w['ms_per_frame']:.3f} ms/frame, {w['draws']} draws")
                for k, v, s, nsd in w['t']['charged']:
                    print(f'      {k:18s} {pct(s):>6s} %  {nsd:7.1f} ns/draw')
    os.makedirs(a.out, exist_ok=True)
    md_tables(results, os.path.join(a.out, 'hotspots_tables.md'))
    with open(os.path.join(a.out, 'hotspots.json'), 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=1, default=str)


if __name__ == '__main__':
    main()
