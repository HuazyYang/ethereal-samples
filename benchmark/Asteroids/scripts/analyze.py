#!/usr/bin/env python3
"""Aggregates one benchmark session written by scripts/run_matrix.ps1.

    python scripts/analyze.py results/<session> [--out DIR] [--gpu-session results/<gpu-timed session>]
                              [--half-res-session results/<half-resolution session>] [--draws N]
                              [--refresh-hz HZ] [--no-charts] [--strict]

Input layout (docs/PLAN.md):  <session>/<config>/<rep>/{run.json,frames.csv}, optional <session>/manifest.json.
A configuration directory is <renderer>_<api>_<binding>_tNN, optionally followed by __<variant> for the nvrhi
sensitivity switches (always_set_state, track_liveness, no_auto_barriers, cb_page_split_avoid, cb_page_split_force).
Variants never enter the main matrix.

Output:  <out>/data/summary.csv                       one row per configuration of the main matrix
         <out>/data/runs.csv                          one row per run (median/p1/p99 of every column)
         <out>/data/layer_overhead_vs_native.csv      bindless versus native D3D12: layer overhead with equivalent
                                                      binding strategy (plus the D3D11 rows, see below)
         <out>/data/binding_model_cost_vs_native.csv  dyn/mut/tex_mut/tex_mut_pc versus native D3D12: binding-model cost
         <out>/data/sensitivity.csv                   nvrhi sensitivity variants (only when the session has any)
         <out>/data/cb_page_split.csv                 the two populations of the nvrhi D3D12 page-split store (only when
                                                      a run recorded cb_page_split = true)
         <out>/data/cpu_bound_check.csv               gpu_ms and half-resolution frame time per configuration
                                                      (only with --gpu-session / --half-res-session or GPU-timed runs)
         <out>/data/validation.json                   every validation issue and every failed / N/A / missing run
         <out>/charts/*.png                           charts (needs matplotlib; everything else is standard library only)

Exit code: 0 ok, 1 validation errors (scene-hash mismatch, invalid runs, no valid run; with --strict also
failed or missing runs), 2 usage error.

Definitions
  cpu_render_ms          = render_ms + submit_ms, computed per frame (the headline metric).
  cpu_render_present_ms  = render_ms + submit_ms + present_ms per frame. Sensitivity metric: native D3D11
                           replays its deferred work inside Present.
  cpu_render_gc_ms       = render_ms + submit_ms + gc_ms per frame. Sensitivity metric: gc_ms is nvrhi's per-frame
                           garbage collection (part of wait_ms; 0 for the other renderers).
  per run                : median, p1 and p99 of every column over the measured frames (linear interpolation).
  per configuration      : median of the run medians; min and max of the run medians; spread = (max - min) / median.
  status                 : ok when more than half of the scheduled repetitions are valid, partial when fewer are,
                           failed / n/a / missing when none is.
  GPU-timed runs         : runs with gpu_timing = true only feed gpu_ms and gpu_over_frame (the CPU-bound check).
                           They are excluded from every other metric: the queries perturb the timed columns.
  paced runs             : a run is `paced` when its median frame_ms is within 2 % of a multiple (or of half) of the
                           display refresh period (run.json extra.display_refresh_hz, or --refresh-hz), update + render
                           + submit fill at most 95 % of the frame, and the rest is slack that the presentation absorbs:
                           per frame, wait + present falls when update + render + submit rise (correlation <= -0.4; a
                           renderer that merely happens to need one refresh period per frame shows no such relation).
                           The compositor, not the renderer, then sets the frame time (docs/LIMITATIONS.md: a displayed
                           window can be paced to one frame per refresh). Paced runs are
                           excluded from frame_ms, present_ms, wait_ms,
                           render + submit + present and from the CPU-bound check; they stay in render + submit and the
                           other columns (render + submit of a paced run differed by -7 % to +4 % from the unpaced run
                           of the same configuration in the paired runs of docs/LIMITATIONS.md). The same exclusion
                           applies to a run whose window was not displayed (extra.window_visible_fraction < 0.5) in a
                           session whose other windows were, or the other way round.
  cb_page_split          : nvrhi D3D12 runs record whether a 4 KB page boundary lies inside the command list's volatile
                           constant buffer state (extra.cb_page_split, docs/LIMITATIONS.md). Affected processes pay
                           about 80 ns more per setGraphicsState that changes a binding set. The two populations are
                           never pooled: a configuration's metrics come from the unaffected runs when there are any
                           (column `population`), the affected runs are reported in cb_page_split.csv and in the
                           cb_page_split_* columns of summary.csv.
  ns per draw            : divided by the draw_count the runs recorded (50,001: the asteroids plus the skybox).
  overhead vs native     : (cfg - native) / native in percent and (cfg - native) / draws in ns per draw, against the
                           native renderer of the SAME API and the same thread count: native D3D12 for D3D12 rows,
                           native D3D11 for D3D11 rows. Vulkan rows have no native renderer; native D3D12 is given
                           in separate *_cross_api columns as a cross-API reference.
  native_comparison      : what the overhead means. Native D3D12 binds all 10 textures once per command list, so only
                           `bindless` uses an equivalent binding strategy ("layer overhead with equivalent binding
                           strategy"); dyn / mut / tex_mut / tex_mut_pc switch a binding per draw, and their difference
                           to native is the "binding-model cost", not abstraction overhead. Native D3D11 sets one
                           texture per draw, as the D3D11 layers do ("layer overhead, per-draw texture binding on
                           both sides (D3D11)").
  speedup                : value at 1 thread / value at N threads, same renderer, api and binding.
  vk / d3d12             : Vulkan value / D3D12 value, same renderer, binding and thread count.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
from array import array
from collections import Counter, defaultdict

CSV_HEADER = ["frame", "update_ms", "render_ms", "submit_ms", "wait_ms", "present_ms", "frame_ms", "gpu_ms",
              "draw_count", "index_count", "gc_ms"]
# Order of the metric columns in the outputs. The cpu_render_* metrics are derived per frame.
METRICS = ["cpu_render_ms", "cpu_render_present_ms", "cpu_render_gc_ms", "render_ms", "submit_ms", "update_ms",
           "wait_ms", "gc_ms", "present_ms", "frame_ms", "gpu_ms"]
# The headline metric and its two sensitivity variants (see the module docstring).
COST_METRICS = ["cpu_render_ms", "cpu_render_present_ms", "cpu_render_gc_ms"]

RENDERERS = ["native", "nvrhi"]
APIS = ["d3d11", "d3d12", "vk"]
BINDINGS = ["original", "dyn", "mut", "tex_mut", "tex_mut_pc", "bindless"]
API_LABEL = {"d3d11": "D3D11", "d3d12": "D3D12", "vk": "Vulkan"}

# nvrhi sensitivity switches (asteroids_nvrhi flags, recorded in run.json "extra"). A configuration directory
# "<config>__<variant>" holds runs with exactly the flags of <variant> ("+"-joined when more than one).
VARIANT_SEPARATOR = "__"
NVRHI_VARIANT_FLAGS = ["always_set_state", "track_liveness", "no_auto_barriers", "cb_page_split_avoid",
                       "cb_page_split_force"]

# Presentation pacing (module docstring). A multiple k means frame_ms = k refresh periods; 0.5 is two frames per refresh.
PACING_MULTIPLES = (0.5, 1, 2, 3, 4, 5, 6, 7, 8)
PACING_TOLERANCE = 0.02     # |frame_ms - k * period| <= 2 % of k * period
PACING_BUSY_LIMIT = 0.95    # ... and update + render + submit <= 95 % of frame_ms
PACING_CORRELATION = -0.4   # ... and corr(update + render + submit, wait + present) over the frames at most this
PACING_SLACK_SHARE = 0.2    # used instead of the correlation when it is undefined (constant columns): the median
                            # wait + present must then be at least this share of frame_ms
PACING_SENSITIVE = ("frame_ms", "present_ms", "wait_ms", "cpu_render_present_ms")
WINDOW_HIDDEN_BELOW = 0.5   # extra.window_visible_fraction below this: the window was covered, not displayed

LAYER_OVERHEAD = "layer overhead with equivalent binding strategy"
BINDING_MODEL_COST = "binding-model cost"
D3D11_LAYER_OVERHEAD = "layer overhead, per-draw texture binding on both sides (D3D11)"

MIN_FRAMES = 30            # fewer measured frames than this is not a usable run
DURATION_TOLERANCE = 0.10  # sum(frame_ms) may deviate from duration_seconds by 10 % ...
DURATION_SLACK_S = 0.5     # ... or by this many seconds, whichever is larger

# Statuses of a run after loading and validation.
OK, FAILED, UNSUPPORTED, MISSING, PENDING, INVALID = "ok", "failed", "unsupported", "missing", "pending", "invalid"
PARTIAL = "partial"  # configuration status only: valid runs exist, but not a majority of the scheduled ones


# ----------------------------------------------------------------------------------------------- maths

def percentile(values, q):
    """q in [0, 100]; linear interpolation between closest ranks (numpy's default method)."""
    data = sorted(values)
    if not data:
        return None
    if len(data) == 1:
        return float(data[0])
    pos = (len(data) - 1) * (q / 100.0)
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(data) - 1)
    return float(data[lo] + (data[hi] - data[lo]) * (pos - lo))


def median(values):
    return percentile(values, 50.0)


def column_stats(values):
    """median / p1 / p99 of one column of one run; None when the column has no samples."""
    data = sorted(values)
    if not data:
        return None
    return {"median": percentile(data, 50.0), "p1": percentile(data, 1.0), "p99": percentile(data, 99.0),
            "count": len(data)}


def overhead_percent(value, baseline):
    if value is None or baseline is None or baseline <= 0:
        return None
    return (value - baseline) / baseline * 100.0


def overhead_ns_per_draw(value_ms, baseline_ms, draws):
    if value_ms is None or baseline_ms is None or not draws:
        return None
    return (value_ms - baseline_ms) * 1.0e6 / draws


def ratio(numerator, denominator):
    if numerator is None or denominator is None or denominator <= 0:
        return None
    return numerator / denominator


def correlation(xs, ys):
    """Pearson correlation of two equally long sequences; None when either has no variance."""
    n = min(len(xs), len(ys))
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0.0 or syy <= 0.0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / math.sqrt(sxx * syy)


def pacing_multiple(frame_ms, busy_ms, refresh_hz, slack_ms=None, slack_correlation=None):
    """The multiple of the refresh period a run is paced to, or None (module docstring, "paced runs").
    frame_ms, busy_ms (update + render + submit) and slack_ms (wait + present) are medians over the frames;
    slack_correlation is the per-frame correlation of the last two (None: undefined)."""
    if not frame_ms or not refresh_hz or refresh_hz <= 1 or frame_ms <= 0:
        return None
    if busy_ms is not None and busy_ms > PACING_BUSY_LIMIT * frame_ms:
        return None
    if slack_correlation is not None:
        if slack_correlation > PACING_CORRELATION:
            return None
    elif slack_ms is not None and slack_ms < PACING_SLACK_SHARE * frame_ms:
        return None
    period = 1000.0 / refresh_hz
    for k in PACING_MULTIPLES:
        if abs(frame_ms - k * period) <= PACING_TOLERANCE * k * period:
            return k
    return None


# ----------------------------------------------------------------------------------------------- loading

def config_name(renderer, api, binding, threads, variant=""):
    name = "%s_%s_%s_t%02d" % (renderer, api, binding, int(threads))
    return name + VARIANT_SEPARATOR + variant if variant else name


def config_variant(name):
    """'nvrhi_d3d12_tex_mut_t08__always_set_state' -> 'always_set_state'; '' for a configuration of the main matrix."""
    return name.split(VARIANT_SEPARATOR, 1)[1] if VARIANT_SEPARATOR in name else ""


def parse_config_name(name):
    """'nvrhi_d3d12_tex_mut_pc_t08' -> ('nvrhi', 'd3d12', 'tex_mut_pc', 8); None if it is not a config name.
    A '__<variant>' suffix is ignored here (see config_variant)."""
    parts = name.split(VARIANT_SEPARATOR, 1)[0].split("_")
    if len(parts) < 4 or not parts[-1].startswith("t") or not parts[-1][1:].isdigit():
        return None
    return parts[0], parts[1], "_".join(parts[2:-1]), int(parts[-1][1:])


def is_headline(key):
    """docs/PLAN.md: D3D12 and Vulkan, bindings tex_mut and bindless, threads 1 and 8, every renderer
    (native: D3D12, its one strategy)."""
    renderer, api, binding, threads = key
    if threads not in (1, 8):
        return False
    if renderer == "native":
        return api == "d3d12"
    return binding in ("tex_mut", "bindless") and api in ("d3d12", "vk")


def native_comparison(key):
    """What an overhead-versus-native number of this configuration means (module docstring)."""
    renderer, api, binding, _ = key
    if renderer == "native":
        return ""
    if api == "d3d11":
        return D3D11_LAYER_OVERHEAD
    return LAYER_OVERHEAD if binding == "bindless" else BINDING_MODEL_COST


class Run:
    def __init__(self, path, config_dir, rep):
        self.path = path
        self.config_dir = config_dir
        self.rep = rep
        self.key = parse_config_name(config_dir)  # (renderer, api, binding, threads), refined from run.json
        self.variant = config_variant(config_dir)  # nvrhi sensitivity variant, '' for the main matrix
        self.gpu_timed = False                     # run.json gpu_timing: only used for the CPU-bound check
        self.status = MISSING
        self.reason = ""
        self.meta = {}
        self.stats = {}        # metric -> {median, p1, p99, count} or None
        self.frame_ms = None   # array('d'), kept for the distribution chart
        self.frame_count = 0
        self.draw_count = None   # median draws per frame
        self.index_count = None
        self.issues = []       # (severity, code, message)
        self.refresh_hz = None         # display refresh rate the run recorded (or --refresh-hz)
        self.paced = False             # frame time set by the compositor (module docstring)
        self.pacing_multiple = None    # frame_ms / refresh period when paced
        self.visible_fraction = None   # extra.window_visible_fraction
        self.desktop_differs = False   # window displayed / hidden unlike the rest of the session
        self.cb_page_split = ""        # extra.cb_page_split: "true" | "false" | "unknown" | "n/a" | "" (not recorded)
        self.cb_split_lists = None     # extra.cb_page_split_command_lists

    @property
    def pacing_excluded(self):
        """True when frame_ms / present_ms / wait_ms of this run say nothing about the renderer."""
        return self.paced or self.desktop_differs

    @property
    def label(self):
        return "%s/%s" % (self.config_dir, self.rep)

    def add_issue(self, severity, code, message):
        self.issues.append((severity, code, message))


def read_json(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)


def load_frames(path):
    """Reads frames.csv into {column: array('d')}. gpu_ms holds only the frames that have a value.
    Raises ValueError for a wrong header or a malformed row."""
    cols = {name: array("d") for name in CSV_HEADER}
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None or [h.strip() for h in header] != CSV_HEADER:
            raise ValueError("unexpected frames.csv header: %r" % (header,))
        for line_no, row in enumerate(reader, start=2):
            if not row:
                continue
            if len(row) != len(CSV_HEADER):
                raise ValueError("line %d: expected %d fields, got %d" % (line_no, len(CSV_HEADER), len(row)))
            for name, text in zip(CSV_HEADER, row):
                if name == "gpu_ms" and text.strip() == "":
                    continue
                try:
                    value = float(text)
                except ValueError:
                    raise ValueError("line %d: %s is not a number: %r" % (line_no, name, text))
                if not math.isfinite(value):
                    raise ValueError("line %d: %s is not finite" % (line_no, name))
                cols[name].append(value)
    return cols


def _to_float(value):
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def _load_ok_run(run, refresh_hz=None):
    """Fills stats from frames.csv for a run whose run.json says ok. Marks the run INVALID on any defect.
    `refresh_hz` overrides the display refresh rate the run recorded (pacing detection)."""
    frames_path = os.path.join(run.path, "frames.csv")
    if not os.path.isfile(frames_path):
        run.status = INVALID
        run.reason = "run.json says ok but frames.csv is missing"
        run.add_issue("error", "frames_missing", run.reason)
        return
    try:
        cols = load_frames(frames_path)
    except (ValueError, OSError) as e:
        run.status = INVALID
        run.reason = "frames.csv unreadable: %s" % e
        run.add_issue("error", "frames_unreadable", run.reason)
        return

    n = len(cols["frame"])
    run.frame_count = n
    cols["cpu_render_ms"] = array("d", (r + s for r, s in zip(cols["render_ms"], cols["submit_ms"])))
    cols["cpu_render_present_ms"] = array("d", (c + p for c, p in zip(cols["cpu_render_ms"], cols["present_ms"])))
    cols["cpu_render_gc_ms"] = array("d", (c + g for c, g in zip(cols["cpu_render_ms"], cols["gc_ms"])))
    for metric in METRICS:
        run.stats[metric] = column_stats(cols[metric])
    run.frame_ms = cols["frame_ms"]
    run.draw_count = median(cols["draw_count"])
    run.index_count = median(cols["index_count"])
    meta = run.meta
    run.gpu_timed = meta.get("gpu_timing") is True

    def invalid(code, message):
        run.status = INVALID
        run.add_issue("error", code, message)
        if not run.reason:
            run.reason = message

    # Frame count plausible.
    declared = meta.get("frame_count")
    if declared is not None and int(declared) != n:
        invalid("frame_count_mismatch", "frames.csv has %d rows, run.json frame_count is %s" % (n, declared))
    if n < MIN_FRAMES:
        invalid("too_few_frames", "only %d measured frames (minimum %d)" % (n, MIN_FRAMES))
    duration = meta.get("duration_seconds")
    if n and isinstance(duration, (int, float)) and duration > 0:
        wall = sum(cols["frame_ms"]) / 1000.0
        if abs(wall - duration) > max(DURATION_TOLERANCE * duration, DURATION_SLACK_S):
            invalid("duration_implausible",
                    "frame_ms sums to %.2f s but the measured duration is %.2f s" % (wall, duration))
    frames = cols["frame"]
    if any(frames[i + 1] != frames[i] + 1 for i in range(n - 1)):
        run.add_issue("warning", "frame_index_gap", "frame indices are not consecutive")

    # Contract invariants recorded in run.json.
    if meta.get("threads") != meta.get("requested_threads"):
        invalid("thread_mismatch", "used %s threads, requested %s" % (meta.get("threads"), meta.get("requested_threads")))
    if meta.get("build", "release") != "release":
        invalid("not_release", "build is %r, not release" % meta.get("build"))
    if meta.get("validation") is True:
        invalid("validation_on", "validation layers were enabled")
    if meta.get("vsync") is True:
        invalid("vsync_on", "vsync was enabled")
    if not meta.get("static_scene_hash"):
        invalid("hash_missing", "run.json has no static_scene_hash")
    if run.draw_count is not None and run.draw_count <= 0:
        run.add_issue("warning", "no_draws", "draw_count is 0")

    extra = meta.get("extra") if isinstance(meta.get("extra"), dict) else {}
    # The sensitivity switches the run really used must be the ones its directory stands for, so that a run with
    # a switch can never enter the main matrix (and the other way round).
    flags = "+".join(f for f in NVRHI_VARIANT_FLAGS if str(extra.get(f, "false")).lower() == "true")
    if flags != run.variant:
        invalid("variant_mismatch", "run used the sensitivity switches %r but its directory stands for %r"
                % (flags or "none", run.variant or "none"))

    # Presentation pacing and desktop state (module docstring).
    run.refresh_hz = refresh_hz or _to_float(extra.get("display_refresh_hz"))
    run.visible_fraction = _to_float(extra.get("window_visible_fraction"))
    if n:
        busy_frames = [u + c for u, c in zip(cols["update_ms"], cols["cpu_render_ms"])]
        slack_frames = [w + p for w, p in zip(cols["wait_ms"], cols["present_ms"])]
        busy, slack = median(busy_frames), median(slack_frames)
        run.pacing_multiple = pacing_multiple(run.stats["frame_ms"]["median"], busy, run.refresh_hz, slack,
                                              correlation(busy_frames, slack_frames))
        run.paced = run.pacing_multiple is not None
        if run.paced:
            run.add_issue("warning", "paced",
                          "frame_ms %.3f ms is %g x the refresh period of %g Hz while update + render + submit take %.3f ms "
                          "and wait + present %.3f ms: paced by the compositor; excluded from frame_ms, present_ms, wait_ms, "
                          "render + submit + present and the CPU-bound check"
                          % (run.stats["frame_ms"]["median"], run.pacing_multiple, run.refresh_hz, busy, slack))
    # nvrhi D3D12 page-split store (module docstring).
    run.cb_page_split = str(extra.get("cb_page_split", "")).lower()
    lists = _to_float(extra.get("cb_page_split_command_lists"))
    run.cb_split_lists = int(lists) if lists is not None else None


def load_run(path, config_dir, rep, manifest_entry=None, refresh_hz=None):
    run = Run(path, config_dir, rep)
    entry = manifest_entry or {}
    if run.key is None and entry.get("renderer"):
        run.key = (entry["renderer"], entry["api"], entry["binding"], int(entry["threads"]))

    json_path = os.path.join(path, "run.json")
    if not os.path.isfile(json_path):
        status = entry.get("status", "")
        if status == PENDING:
            run.status = PENDING
            run.reason = "scheduled but not executed yet"
        elif status in ("", OK):
            run.status = MISSING
            run.reason = "run.json is missing"
        else:  # timeout / crashed / failed recorded by the runner
            run.status = FAILED
            run.reason = "%s: %s" % (status, entry.get("reason", "no run.json written"))
        return run

    try:
        meta = read_json(json_path)
        if not isinstance(meta, dict):
            raise ValueError("top level is not an object")
    except (ValueError, OSError) as e:
        run.status = INVALID
        run.reason = "run.json unreadable: %s" % e
        run.add_issue("error", "json_unreadable", run.reason)
        return run
    run.meta = meta

    if meta.get("renderer") and meta.get("api") and meta.get("binding") and meta.get("threads") is not None:
        key = (meta["renderer"], meta["api"], meta["binding"], int(meta.get("requested_threads", meta["threads"])))
        if meta.get("status") != OK and meta.get("requested_binding") and meta["renderer"] != "native":
            key = (key[0], key[1], meta["requested_binding"], key[3])
        if run.key is not None and key != run.key:
            run.add_issue("error", "config_mismatch",
                          "run.json describes %s but the directory is %s" % (config_name(*key), config_dir))
            run.status = INVALID
            run.reason = run.issues[-1][2]
            return run
        run.key = key

    status = meta.get("status")
    if status == OK:
        run.status = OK
        _load_ok_run(run, refresh_hz)
    elif status == "failed":
        run.status = UNSUPPORTED if meta.get("failure_kind") == "unsupported" else FAILED
        run.reason = str(meta.get("reason", ""))
    else:
        run.status = INVALID
        run.reason = "run.json has unknown status %r" % (status,)
        run.add_issue("error", "status_unknown", run.reason)
    return run


def load_session(session_dir, refresh_hz=None):
    """Returns (runs, manifest). Every scheduled or present run is returned, whatever its status.
    `refresh_hz` overrides the display refresh rate the runs recorded (pacing detection)."""
    if not os.path.isdir(session_dir):
        raise FileNotFoundError("session directory not found: %s" % session_dir)
    manifest = {}
    manifest_path = os.path.join(session_dir, "manifest.json")
    if os.path.isfile(manifest_path):
        manifest = read_json(manifest_path)
    entries = {}
    for entry in manifest.get("runs", []) or []:
        entries[(entry.get("config"), str(entry.get("rep")))] = entry

    runs = []
    seen = set()
    for config_dir in sorted(os.listdir(session_dir)):
        config_path = os.path.join(session_dir, config_dir)
        if not os.path.isdir(config_path) or parse_config_name(config_dir) is None:
            continue
        for rep in sorted(os.listdir(config_path)):
            rep_path = os.path.join(config_path, rep)
            if not os.path.isdir(rep_path):
                continue
            entry = entries.get((config_dir, rep))
            if entry is None and not os.path.isfile(os.path.join(rep_path, "run.json")):
                continue  # stray empty directory
            seen.add((config_dir, rep))
            runs.append(load_run(rep_path, config_dir, rep, entry, refresh_hz))
    for (config_dir, rep), entry in sorted(entries.items(), key=lambda kv: (str(kv[0][0]), kv[0][1])):
        if (config_dir, rep) not in seen and config_dir:
            runs.append(load_run(os.path.join(session_dir, config_dir, rep), config_dir, rep, entry, refresh_hz))
    return runs, manifest


# ----------------------------------------------------------------------------------------------- validation

def _majority(runs, getter):
    counts = Counter(getter(r) for r in runs)
    return counts.most_common(1)[0][0] if counts else None


def validate_session(runs):
    """Cross-run checks. Marks deviating runs INVALID and returns the session-level issue list:
    [{'severity', 'code', 'run', 'message'}]. Per-run issues found while loading are included."""
    issues = []
    ok_runs = [r for r in runs if r.status == OK]

    # The static scene hash must be identical for every renderer. The reference value is native D3D12's
    # if such a run exists, otherwise the most common one.
    hashes = Counter(r.meta.get("static_scene_hash") for r in ok_runs)
    reference_hash = None
    if hashes:
        native = [r for r in ok_runs if r.key and r.key[0] == "native" and r.key[1] == "d3d12"]
        reference_hash = _majority(native, lambda r: r.meta.get("static_scene_hash")) if native else hashes.most_common(1)[0][0]
    if len(hashes) > 1:
        detail = ", ".join("%s x%d" % (h, c) for h, c in hashes.most_common())
        issues.append({"severity": "error", "code": "scene_hash_mismatch", "run": "",
                       "message": "static_scene_hash differs across the session (%s); reference is %s" % (detail, reference_hash)})
        for r in ok_runs:
            if r.meta.get("static_scene_hash") != reference_hash:
                r.status = INVALID
                r.reason = "static_scene_hash %s differs from the reference %s" % (r.meta.get("static_scene_hash"), reference_hash)
                r.add_issue("error", "scene_hash_mismatch", r.reason)

    # Same adapter, resolution and workload everywhere (the adapter name, because a LUID changes on reboot).
    ok_runs = [r for r in ok_runs if r.status == OK]
    checks = [
        ("adapter_mismatch", "adapter", lambda r: r.meta.get("adapter")),
        ("resolution_mismatch", "resolution", lambda r: (r.meta.get("width"), r.meta.get("height"))),
        ("workload_mismatch", "asteroids/meshes/textures",
         lambda r: (r.meta.get("asteroids"), r.meta.get("meshes"), r.meta.get("textures"))),
    ]
    for code, what, getter in checks:
        expected = _majority(ok_runs, getter)
        for r in ok_runs:
            if r.status == OK and getter(r) != expected:
                r.status = INVALID
                r.reason = "%s %r differs from the session's %r" % (what, getter(r), expected)
                r.add_issue("error", code, r.reason)

    # Draw count: a different number of draws is legitimate only if the report says so; warn.
    ok_runs = [r for r in ok_runs if r.status == OK]
    expected_draws = _majority(ok_runs, lambda r: r.draw_count)
    for r in ok_runs:
        if r.draw_count != expected_draws:
            r.add_issue("warning", "draw_count_differs",
                        "median draw_count %s differs from the session's %s" % (r.draw_count, expected_draws))

    # Displayed and covered windows are paced differently (module docstring): the minority state is flagged.
    known = [r for r in ok_runs if r.visible_fraction is not None]
    hidden = [r for r in known if r.visible_fraction < WINDOW_HIDDEN_BELOW]
    if hidden and len(hidden) < len(known):
        mostly_hidden = 2 * len(hidden) > len(known)
        for r in known:
            if (r.visible_fraction < WINDOW_HIDDEN_BELOW) != mostly_hidden:
                r.desktop_differs = True
                r.add_issue("warning", "window_visibility_differs",
                            "%.0f %% of the window was visible, unlike most runs of the session (%d of %d hidden): "
                            "excluded from frame_ms, present_ms, wait_ms, render + submit + present and the CPU-bound check"
                            % (r.visible_fraction * 100, len(hidden), len(known)))

    timed = sum(1 for r in ok_runs if r.gpu_timed)
    if 0 < timed < len(ok_runs):
        issues.append({"severity": "warning", "code": "gpu_timing_mixed", "run": "",
                       "message": "%d of %d valid runs were taken with -gpu_timing; they only feed gpu_ms / "
                                  "gpu_over_frame and are excluded from every other metric" % (timed, len(ok_runs))})

    for r in runs:
        for severity, code, message in r.issues:
            issues.append({"severity": severity, "code": code, "run": r.label, "message": message})
    return issues, reference_hash


# ----------------------------------------------------------------------------------------------- aggregation

def _sort_key(key):
    renderer, api, binding, threads = key

    def index(seq, v):
        return seq.index(v) if v in seq else len(seq)
    return (index(APIS, api), index(RENDERERS, renderer), index(BINDINGS, binding), threads)


def _metric_stats(runs, metric):
    medians = [r.stats[metric]["median"] for r in runs if r.stats.get(metric)]
    if not medians:
        return None
    mid = median(medians)
    lo, hi = min(medians), max(medians)
    return {
        "median": mid, "min": lo, "max": hi,
        "spread_pct": (hi - lo) / mid * 100.0 if mid > 0 else None,
        "p1": median([r.stats[metric]["p1"] for r in runs if r.stats.get(metric)]),
        "p99": median([r.stats[metric]["p99"] for r in runs if r.stats.get(metric)]),
        "runs": len(medians),
    }


def aggregate(runs, draws=None):
    """Returns one dict per configuration (main matrix and sensitivity variants; see 'variant'), sorted.
    Only valid runs contribute, and GPU-timed runs only to gpu_ms and gpu_over_frame.
    `draws` overrides the recorded draw count for the ns-per-draw values (None: use the recorded one)."""
    groups = defaultdict(list)
    for r in runs:
        if r.key is not None:
            groups[(r.key, r.variant)].append(r)

    configs = {}
    for (key, variant), members in groups.items():
        valid = [r for r in members if r.status == OK]
        untimed = [r for r in valid if not r.gpu_timed]
        timed = [r for r in valid if r.gpu_timed]      # CPU-bound check only
        # The two populations of the nvrhi D3D12 page-split store are never pooled: the unaffected runs carry the
        # configuration when there are any, the affected ones are reported next to them.
        split = [r for r in untimed if r.cb_page_split == "true"]
        clean = [r for r in untimed if r.cb_page_split != "true"]
        good = clean if clean else split               # the runs behind every metric but gpu_ms
        unpaced = [r for r in good if not r.pacing_excluded]   # ... and behind the pacing-sensitive ones
        timed_unpaced = [r for r in timed if not r.pacing_excluded]
        c = {
            "key": key, "renderer": key[0], "api": key[1], "binding": key[2], "threads": key[3], "variant": variant,
            "headline": is_headline(key) and not variant,
            "runs_scheduled": len(members),
            "runs_ok": len(valid),
            "runs_gpu_timed": len(timed),
            "runs_failed": sum(1 for r in members if r.status == FAILED),
            "runs_invalid": sum(1 for r in members if r.status == INVALID),
            "runs_unsupported": sum(1 for r in members if r.status == UNSUPPORTED),
            "runs_missing": sum(1 for r in members if r.status in (MISSING, PENDING)),
            "runs_paced": sum(1 for r in valid if r.paced),
            "runs_pacing_excluded": sum(1 for r in valid if r.pacing_excluded),
            "runs_cb_page_split": len(split),
            "population": "",
            "runs": good,
            "unpaced_runs": unpaced,
            "split_runs": split,
            "clean_runs": clean,
            "timed_runs": timed,
            "metrics": {},
        }
        if split:
            c["population"] = ("cb_page_split=false (%d of %d runs)" % (len(clean), len(untimed)) if clean
                               else "cb_page_split=true (no unaffected run)")
        c["cb_page_split_cpu_render"] = _metric_stats(split, "cpu_render_ms") if split else None
        c["cb_clean_cpu_render"] = _metric_stats(clean, "cpu_render_ms") if split and clean else None
        # ok only when a majority of the scheduled repetitions are valid.
        if valid and 2 * len(valid) > len(members):
            c["status"] = OK
        elif valid:
            c["status"] = PARTIAL
        elif c["runs_unsupported"] and not (c["runs_failed"] or c["runs_invalid"]):
            c["status"] = "n/a"
        elif c["runs_failed"] or c["runs_invalid"]:
            c["status"] = FAILED
        else:
            c["status"] = MISSING
        c["reason"] = "" if c["status"] == OK else next((r.reason for r in members if r.status != OK and r.reason), "")
        for metric in METRICS:
            source = timed if metric == "gpu_ms" else (unpaced if metric in PACING_SENSITIVE else good)
            c["metrics"][metric] = _metric_stats(source, metric)
        c["gpu_timed_frame_ms"] = _metric_stats(timed_unpaced, "frame_ms")
        c["frames"] = median([r.frame_count for r in good]) if good else None
        c["draw_count"] = median([r.draw_count for r in good if r.draw_count is not None]) if good else None
        configs[(key, variant)] = c

    def value(key, metric="cpu_render_ms", variant=""):
        c = configs.get((key, variant))
        m = c["metrics"].get(metric) if c else None
        return m["median"] if m else None

    def draws_of(key, variant=""):
        c = configs.get((key, variant))
        return draws if draws else (c["draw_count"] if c else None)

    for (key, variant), c in configs.items():
        renderer, api, binding, threads = key
        cpu = value(key, variant=variant)
        frame = value(key, "frame_ms", variant)
        n = draws_of(key, variant)
        d = {}
        d["ns_per_draw"] = cpu * 1.0e6 / n if cpu is not None and n else None

        # Overhead versus native: like with like only (module docstring).
        same_api = renderer != "native" and api in ("d3d12", "d3d11")
        cross_api = renderer != "native" and api == "vk"
        base_key = ("native", api if same_api else "d3d12", "original", threads)
        has_base = (same_api or cross_api) and value(base_key) is not None
        # ns per draw needs the same number of draws on both sides.
        base_draws = n if has_base and draws_of(base_key) == n else None
        d["native_baseline"] = ""
        if has_base:
            d["native_baseline"] = "native %s" % API_LABEL[base_key[1]] + (" (cross-API reference)" if cross_api else "")
        d["native_comparison"] = native_comparison(key) if has_base else ""
        over = {m: overhead_percent(value(key, m, variant), value(base_key, m)) if has_base else None for m in COST_METRICS}
        ns = overhead_ns_per_draw(cpu, value(base_key), base_draws) if has_base else None
        d["overhead_vs_native_pct"] = over["cpu_render_ms"] if same_api else None
        d["overhead_vs_native_ns_per_draw"] = ns if same_api else None
        d["overhead_vs_native_render_present_pct"] = over["cpu_render_present_ms"] if same_api else None
        d["overhead_vs_native_render_gc_pct"] = over["cpu_render_gc_ms"] if same_api else None
        d["overhead_vs_native_d3d12_cross_api_pct"] = over["cpu_render_ms"] if cross_api else None
        d["overhead_vs_native_d3d12_cross_api_ns_per_draw"] = ns if cross_api else None
        d["overhead_vs_native_d3d12_cross_api_render_present_pct"] = over["cpu_render_present_ms"] if cross_api else None
        d["overhead_vs_native_d3d12_cross_api_render_gc_pct"] = over["cpu_render_gc_ms"] if cross_api else None

        one = (renderer, api, binding, 1)
        d["speedup_vs_1_thread"] = ratio(value(one, variant=variant), cpu)
        d["frame_speedup_vs_1_thread"] = ratio(value(one, "frame_ms", variant), frame)
        if api == "vk":
            twin = (renderer, "d3d12", binding, threads)
            d["vk_over_d3d12_cpu_render"] = ratio(cpu, value(twin, variant=variant))
            d["vk_over_d3d12_frame"] = ratio(frame, value(twin, "frame_ms", variant))
        else:
            d["vk_over_d3d12_cpu_render"] = None
            d["vk_over_d3d12_frame"] = None
        # CPU-bound check of the PLAN: gpu_ms well below frame_ms, both taken from the GPU-timed runs.
        timed_frame = c["gpu_timed_frame_ms"]["median"] if c["gpu_timed_frame_ms"] else None
        d["gpu_over_frame"] = ratio(value(key, "gpu_ms", variant), timed_frame)
        # Sensitivity variants: relative to the default configuration of the same session.
        d["variant_over_default_cpu_render"] = ratio(cpu, value(key)) if variant else None
        d["variant_over_default_render_gc"] = \
            ratio(value(key, "cpu_render_gc_ms", variant), value(key, "cpu_render_gc_ms")) if variant else None
        d["variant_over_default_frame"] = ratio(frame, value(key, "frame_ms")) if variant else None
        c["derived"] = d

    return [configs[k] for k in sorted(configs, key=lambda kv: (_sort_key(kv[0]), kv[1]))]


DERIVED_COLUMNS = ["ns_per_draw", "native_baseline", "native_comparison",
                   "overhead_vs_native_pct", "overhead_vs_native_ns_per_draw",
                   "overhead_vs_native_render_present_pct", "overhead_vs_native_render_gc_pct",
                   "overhead_vs_native_d3d12_cross_api_pct", "overhead_vs_native_d3d12_cross_api_ns_per_draw",
                   "overhead_vs_native_d3d12_cross_api_render_present_pct",
                   "overhead_vs_native_d3d12_cross_api_render_gc_pct",
                   "speedup_vs_1_thread", "frame_speedup_vs_1_thread",
                   "vk_over_d3d12_cpu_render", "vk_over_d3d12_frame", "gpu_over_frame"]
STAT_SUFFIXES = [("", "median"), ("_min", "min"), ("_max", "max"), ("_spread_pct", "spread_pct"),
                 ("_p1", "p1"), ("_p99", "p99")]


def _fmt(v):
    if v is None:
        return ""
    if isinstance(v, float):
        return "%.6g" % v
    return str(v)


def summary_rows(configs):
    header = ["renderer", "api", "binding", "threads", "headline", "status", "runs_scheduled", "runs_ok",
              "runs_gpu_timed", "runs_failed", "runs_invalid", "runs_unsupported", "runs_missing", "runs_paced",
              "runs_cb_page_split", "frames", "draw_count"]
    for metric in METRICS:
        header += [metric + suffix for suffix, _ in STAT_SUFFIXES]
    header += DERIVED_COLUMNS + ["population", "cb_page_split_cpu_render_ms", "cb_page_split_over_unaffected", "reason"]
    rows = [header]
    for c in configs:
        row = [c["renderer"], c["api"], c["binding"], c["threads"], int(c["headline"]), c["status"], c["runs_scheduled"],
               c["runs_ok"], c["runs_gpu_timed"], c["runs_failed"], c["runs_invalid"], c["runs_unsupported"],
               c["runs_missing"], c["runs_paced"], c["runs_cb_page_split"], c["frames"], c["draw_count"]]
        for metric in METRICS:
            m = c["metrics"].get(metric)
            row += [m[field] if m else None for _, field in STAT_SUFFIXES]
        row += [c["derived"][name] for name in DERIVED_COLUMNS]
        affected, unaffected = c["cb_page_split_cpu_render"], c["cb_clean_cpu_render"]
        row += [c["population"], affected["median"] if affected else None,
                ratio(affected["median"], unaffected["median"]) if affected and unaffected else None]
        row.append(c["reason"])
        rows.append([_fmt(v) for v in row])
    return rows


def _med(c, metric):
    m = c["metrics"].get(metric) if c else None
    return m["median"] if m else None


def native_comparison_rows(configs, kinds):
    """Overhead versus native for the configurations whose native_comparison label is in `kinds`.
    One table per meaning, so that binding-model cost is never read as abstraction overhead."""
    by_key = {c["key"]: c for c in configs}
    header = ["comparison", "renderer", "api", "binding", "threads", "headline", "status", "native_baseline"]
    for metric in COST_METRICS:
        header += [metric, "native_" + metric, "overhead_" + metric[:-3] + "_pct"]
    header += ["overhead_ns_per_draw", "draw_count"]
    rows = [header]
    for c in configs:
        d = c["derived"]
        if d["native_comparison"] not in kinds:
            continue
        cross = c["api"] == "vk"
        base = by_key.get(("native", "d3d12" if cross else c["api"], "original", c["threads"]))
        row = [d["native_comparison"], c["renderer"], c["api"], c["binding"], c["threads"], int(c["headline"]), c["status"],
               d["native_baseline"]]
        for metric in COST_METRICS:
            row += [_med(c, metric), _med(base, metric), overhead_percent(_med(c, metric), _med(base, metric))]
        row += [d["overhead_vs_native_d3d12_cross_api_ns_per_draw" if cross else "overhead_vs_native_ns_per_draw"],
                c["draw_count"]]
        rows.append([_fmt(v) for v in row])
    return rows


SENSITIVITY_METRICS = ["cpu_render_ms", "cpu_render_gc_ms", "cpu_render_present_ms", "render_ms", "submit_ms", "gc_ms",
                       "present_ms", "frame_ms"]


def sensitivity_rows(variants, configs):
    """The nvrhi sensitivity variants next to the default configuration of the same session (variant 'default')."""
    by_key = {c["key"]: c for c in configs}
    header = ["renderer", "api", "binding", "threads", "variant", "status", "runs_scheduled", "runs_ok"]
    for metric in SENSITIVITY_METRICS:
        header += [metric, metric + "_min", metric + "_max"]
    header += ["variant_over_default_cpu_render", "variant_over_default_render_gc", "variant_over_default_frame", "reason"]
    rows = [header]
    seen = set()
    for v in variants:
        group = []
        default = by_key.get(v["key"])
        if default is not None and v["key"] not in seen:
            seen.add(v["key"])
            group.append((default, "default"))
        group.append((v, v["variant"]))
        for c, name in group:
            row = [c["renderer"], c["api"], c["binding"], c["threads"], name, c["status"], c["runs_scheduled"], c["runs_ok"]]
            for metric in SENSITIVITY_METRICS:
                m = c["metrics"].get(metric)
                row += [m["median"], m["min"], m["max"]] if m else [None, None, None]
            d = c["derived"]
            row += [d["variant_over_default_cpu_render"], d["variant_over_default_render_gc"],
                    d["variant_over_default_frame"], c["reason"]]
            rows.append([_fmt(v2) for v2 in row])
    return rows


def cb_page_split_rows(everything):
    """The two populations of the nvrhi D3D12 page-split store, for every configuration (main matrix and variants)
    that has at least one affected run. Columns are the CPU render cost (render + submit) of each population."""
    header = ["renderer", "api", "binding", "threads", "variant", "runs_unaffected", "cpu_render_ms_unaffected",
              "cpu_render_ms_unaffected_min", "cpu_render_ms_unaffected_max", "runs_affected", "cpu_render_ms_affected",
              "cpu_render_ms_affected_min", "cpu_render_ms_affected_max", "affected_over_unaffected",
              "affected_command_lists_min", "affected_command_lists_max", "threads_recording"]
    rows = [header]
    for c in everything:
        if not c["split_runs"]:
            continue
        clean = _metric_stats(c["clean_runs"], "cpu_render_ms") if c["clean_runs"] else None
        split = _metric_stats(c["split_runs"], "cpu_render_ms")
        lists = [r.cb_split_lists for r in c["split_runs"] if r.cb_split_lists is not None]
        row = [c["renderer"], c["api"], c["binding"], c["threads"], c["variant"] or "default"]
        row += [clean["runs"], clean["median"], clean["min"], clean["max"]] if clean else [0, None, None, None]
        row += [split["runs"], split["median"], split["min"], split["max"]] if split else [0, None, None, None]
        row += [ratio(split["median"], clean["median"]) if split and clean else None,
                min(lists) if lists else None, max(lists) if lists else None, c["threads"]]
        rows.append([_fmt(v) for v in row])
    return rows


# A configuration is flagged when the GPU takes more than this share of the frame, or when halving the
# resolution shortens the frame by more than HALF_RES_TOLERANCE. Flags are hints for the report, not verdicts.
GPU_OVER_FRAME_LIMIT = 0.8
HALF_RES_TOLERANCE = 0.05


def cpu_bound_rows(configs, gpu_configs=None, half_configs=None):
    """CPU-bound check of docs/PLAN.md: gpu_ms well below frame_ms (GPU-timed runs, of this session or of
    --gpu-session) and frame_ms unchanged at half resolution (--half-res-session)."""
    gpu_by_key = {c["key"]: c for c in (gpu_configs if gpu_configs is not None else configs)}
    half_by_key = {c["key"]: c for c in (half_configs or [])}
    header = ["renderer", "api", "binding", "threads", "headline", "frame_ms", "cpu_render_ms", "wait_ms",
              "gpu_ms", "gpu_timed_frame_ms", "gpu_over_frame", "gpu_timed_runs",
              "half_res_frame_ms", "half_res_over_full_frame", "half_res_cpu_render_ms", "half_res_over_full_cpu_render",
              "half_res_runs", "suspect_gpu_bound"]
    rows = [header]
    for c in configs:
        g = gpu_by_key.get(c["key"])
        h = half_by_key.get(c["key"])
        gpu = _med(g, "gpu_ms")
        timed_frame = g["gpu_timed_frame_ms"]["median"] if g and g["gpu_timed_frame_ms"] else None
        gpu_share = ratio(gpu, timed_frame)
        frame, cpu = _med(c, "frame_ms"), _med(c, "cpu_render_ms")
        half_frame = ratio(_med(h, "frame_ms"), frame)
        if gpu is None and h is None:
            continue
        flags = []
        if gpu_share is not None and gpu_share > GPU_OVER_FRAME_LIMIT:
            flags.append("gpu_ms is %.0f %% of frame_ms" % (gpu_share * 100))
        if half_frame is not None and half_frame < 1.0 - HALF_RES_TOLERANCE:
            flags.append("frame_ms drops to %.0f %% at half resolution" % (half_frame * 100))
        # Paced runs say nothing about the GPU: the check needs an unpaced frame time on both sides.
        for label, cfg in (("", c), ("GPU-timed ", g if g is not c else None), ("half-resolution ", h)):
            if cfg and cfg["runs_pacing_excluded"]:
                flags.append("%d of %d %sruns paced or not displayed: frame_ms not usable"
                             % (cfg["runs_pacing_excluded"], cfg["runs_ok"], label))
        row = [c["renderer"], c["api"], c["binding"], c["threads"], int(c["headline"]), frame, cpu, _med(c, "wait_ms"),
               gpu, timed_frame, gpu_share, g["runs_gpu_timed"] if g else 0,
               _med(h, "frame_ms"), half_frame, _med(h, "cpu_render_ms"), ratio(_med(h, "cpu_render_ms"), cpu),
               len(h["runs"]) if h else 0, "; ".join(flags)]
        rows.append([_fmt(v) for v in row])
    return rows


def run_rows(runs):
    header = ["config", "rep", "renderer", "api", "binding", "threads", "variant", "gpu_timing", "status", "frames",
              "draw_count", "static_scene_hash", "adapter"]
    for metric in METRICS:
        header += [metric + "_median", metric + "_p1", metric + "_p99"]
    header += ["paced", "pacing_multiple", "display_refresh_hz", "window_visible_fraction", "pacing_excluded", "present_mode",
               "cb_page_split", "cb_page_split_command_lists", "reason"]
    rows = [header]
    for r in sorted(runs, key=lambda r: (_sort_key(r.key) if r.key else (99,), r.rep)):
        key = r.key or ("", "", "", "")
        row = [r.config_dir, r.rep, key[0], key[1], key[2], key[3], r.variant, int(r.gpu_timed), r.status,
               r.frame_count or None, r.draw_count, r.meta.get("static_scene_hash"), r.meta.get("adapter")]
        for metric in METRICS:
            s = r.stats.get(metric)
            row += [s["median"], s["p1"], s["p99"]] if s else [None, None, None]
        extra = r.meta.get("extra") if isinstance(r.meta.get("extra"), dict) else {}
        row += [int(r.paced), r.pacing_multiple, r.refresh_hz, r.visible_fraction, int(r.pacing_excluded),
                extra.get("present_mode"), r.cb_page_split, r.cb_split_lists]
        row.append(r.reason)
        rows.append([_fmt(v) for v in row])
    return rows


def write_csv(path, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        csv.writer(f).writerows(rows)


# ----------------------------------------------------------------------------------------------- charts

# One fixed colour per renderer in every chart (colour follows the entity, never the rank).
RENDERER_COLOR = {"native": "#2a78d6", "nvrhi": "#1baf7a"}
SURFACE, INK, INK_2, MUTED, GRID, AXIS = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"
Y_LABEL = "CPU render cost, render + submit (ms per frame)"


def _plt():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({
        "font.family": ["Segoe UI", "DejaVu Sans", "sans-serif"], "font.size": 9,
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "text.color": INK, "axes.labelcolor": INK_2, "axes.edgecolor": AXIS,
        "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelcolor": INK_2, "ytick.labelcolor": INK_2,
        "axes.spines.top": False, "axes.spines.right": False,
        "axes.grid": False, "grid.color": GRID, "grid.linewidth": 0.8,
        "legend.frameon": False, "axes.titlesize": 10, "axes.titleweight": "bold",
    })
    return plt


def _titles(fig, title, subtitle):
    fig.suptitle(title, x=0.01, y=0.995, ha="left", va="top", fontsize=12, fontweight="bold", color=INK)
    fig.text(0.01, 0.995, "\n" + subtitle, ha="left", va="top", fontsize=8.5, color=INK_2, linespacing=1.9)


def _save(plt, fig, path, top):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fig.tight_layout(rect=(0, 0, 1, top))
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path


def _legend(fig, renderers, extra=()):
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch
    handles = [Patch(facecolor=RENDERER_COLOR[r], label=r) for r in RENDERERS if r in renderers]
    handles += [Line2D([0], [0], color=c, linestyle=ls, linewidth=1.5, label=label) for label, c, ls in extra]
    fig.legend(handles=handles, loc="upper right", bbox_to_anchor=(0.995, 0.995), ncol=len(handles), fontsize=8.5)


def _m(c, metric="cpu_render_ms"):
    return c["metrics"].get(metric)


def _config_label(c, with_threads=False):
    text = "%s  %s  %s" % (c["renderer"], API_LABEL.get(c["api"], c["api"]), c["binding"])
    return text + "  %dT" % c["threads"] if with_threads else text


def chart_render_cost(configs, threads, path):
    rows = [c for c in configs if c["threads"] == threads and _m(c)]
    if not rows:
        return None
    plt = _plt()
    fig, ax = plt.subplots(figsize=(9, 1.5 + 0.3 * len(rows)))
    ys, y, last_api = [], 0.0, None
    for c in rows:
        if last_api is not None and c["api"] != last_api:
            y += 0.6
        ys.append(y)
        y += 1.0
        last_api = c["api"]
    values = [_m(c)["median"] for c in rows]
    err = [[_m(c)["median"] - _m(c)["min"] for c in rows], [_m(c)["max"] - _m(c)["median"] for c in rows]]
    ax.margins(y=0.02)
    ax.barh(ys, values, height=0.68, color=[RENDERER_COLOR.get(c["renderer"], MUTED) for c in rows],
            xerr=err, error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 2})
    limit = max(_m(c)["max"] for c in rows)
    for yy, c in zip(ys, rows):
        ax.text(_m(c)["max"] + limit * 0.012, yy, "%.2f" % _m(c)["median"], va="center", ha="left", fontsize=8, color=INK)
    ax.set_yticks(ys)
    ax.set_yticklabels([_config_label(c) for c in rows])
    ax.invert_yaxis()
    ax.set_xlim(0, limit * 1.1)
    ax.set_xlabel(Y_LABEL)
    ax.xaxis.grid(True)
    ax.set_axisbelow(True)
    ax.tick_params(axis="y", length=0)
    _titles(fig, "CPU render cost by configuration, %d render thread%s" % (threads, "" if threads == 1 else "s"),
            "Median of run medians; whiskers show the min-max of the run medians. Lower is better.")
    _legend(fig, {c["renderer"] for c in rows})
    return _save(plt, fig, path, 1 - 0.4 / fig.get_figheight())


def _scaling_series(configs, metric_getter):
    """{api: {renderer: [(threads, value, lo, hi)]}} for tex_mut (native: original)."""
    out = defaultdict(lambda: defaultdict(list))
    for c in configs:
        wanted = "original" if c["renderer"] == "native" else "tex_mut"
        if c["binding"] != wanted:
            continue
        point = metric_getter(c)
        if point is not None:
            out[c["api"]][c["renderer"]].append((c["threads"],) + point)
    return out


def _scaling_chart(configs, path, getter, ylabel, title, subtitle, ideal=False):
    series = _scaling_series(configs, getter)
    apis = [a for a in APIS if a in series]
    if not apis:
        return None
    threads = sorted({t for a in apis for pts in series[a].values() for t, _, _, _ in pts})
    if len(threads) < 2:
        return None
    pos = {t: i for i, t in enumerate(threads)}
    plt = _plt()
    fig, axes = plt.subplots(1, len(apis), figsize=(3.6 * len(apis) + 0.6, 4.3), sharey=True, squeeze=False)
    used = set()
    for ax, api in zip(axes[0], apis):
        if ideal:
            ax.plot([pos[t] for t in threads], threads, color=MUTED, linestyle="--", linewidth=1)
        for renderer in RENDERERS:
            pts = sorted(series[api].get(renderer, []))
            if not pts:
                continue
            used.add(renderer)
            xs = [pos[t] for t, _, _, _ in pts]
            vals = [v for _, v, _, _ in pts]
            color = RENDERER_COLOR[renderer]
            ax.errorbar(xs, vals, yerr=[[v - lo for _, v, lo, _ in pts], [hi - v for _, v, _, hi in pts]],
                        color=color, linewidth=2, marker="o", markersize=5, markeredgecolor=SURFACE,
                        markeredgewidth=1, ecolor=color, elinewidth=1, capsize=2)
        ax.set_title(API_LABEL.get(api, api), loc="left")
        ax.set_xticks(range(len(threads)))
        ax.set_xticklabels([str(t) for t in threads])
        ax.set_xlim(-0.3, len(threads) - 1 + 0.3)
        ax.set_xlabel("Render threads")
        ax.yaxis.grid(True)
        ax.set_axisbelow(True)
    # The y axis is shared: fix the bottom once, after every panel has been drawn. Setting it inside the loop
    # froze the shared top at the first panel's autoscaled extent and clipped the taller series of the others.
    axes[0][0].set_ylim(bottom=0)
    axes[0][0].set_ylabel(ylabel)
    _titles(fig, title, subtitle)
    _legend(fig, used, [("linear scaling", MUTED, "--")] if ideal else [])
    return _save(plt, fig, path, 1 - 0.4 / fig.get_figheight())


def chart_thread_scaling(configs, path):
    def getter(c):
        m = _m(c)
        return (m["median"], m["min"], m["max"]) if m else None
    return _scaling_chart(configs, path, getter, Y_LABEL, "Thread scaling of the CPU render cost",
                          "Binding tex_mut (native: its original strategy). Median of run medians, min-max whiskers. Lower is better.")


def chart_thread_speedup(configs, path):
    def getter(c):
        s = c["derived"].get("speedup_vs_1_thread")
        return (s, s, s) if s is not None else None
    return _scaling_chart(configs, path, getter, "Speed-up over 1 thread (x)", "Thread-scaling speed-up of the CPU render cost",
                          "Binding tex_mut (native: its original strategy). Cost at 1 thread divided by cost at N threads. Higher is better.",
                          ideal=True)


def chart_binding_modes(configs, threads, path):
    bindings = [b for b in BINDINGS if b != "original"]
    layers = ["nvrhi"]
    by_key = {c["key"]: c for c in configs}
    apis = [a for a in APIS if any(_m(c) for c in configs if c["api"] == a and c["threads"] == threads and c["renderer"] in layers)]
    if not apis:
        return None
    plt = _plt()
    fig, axes = plt.subplots(1, len(apis), figsize=(4.2 * len(apis) + 0.6, 4.4), sharey=True, squeeze=False)
    width, used, has_native = 0.38, set(), False
    for ax, api in zip(axes[0], apis):
        top = 0.0
        for j, renderer in enumerate(layers):
            for i, binding in enumerate(bindings):
                c = by_key.get((renderer, api, binding, threads))
                x = i + (j - (len(layers) - 1) / 2) * (width + 0.04)
                m = _m(c) if c else None
                if m is None:
                    # N/A by design or no valid run: say so instead of leaving an unexplained gap.
                    label = "n/a" if (c is None or c["status"] == "n/a") else "failed"
                    ax.text(x, 0, label, ha="center", va="bottom", fontsize=7, color=MUTED, rotation=90)
                    continue
                used.add(renderer)
                ax.bar(x, m["median"], width=width, color=RENDERER_COLOR[renderer],
                       yerr=[[m["median"] - m["min"]], [m["max"] - m["median"]]],
                       error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 2})
                ax.text(x, m["max"], "%.2f" % m["median"], ha="center", va="bottom", fontsize=7.5, color=INK)
                top = max(top, m["max"])
        native = by_key.get(("native", api, "original", threads))
        panel_title = API_LABEL.get(api, api)
        if native and _m(native):
            has_native = True
            value = _m(native)["median"]
            ax.axhline(value, color=RENDERER_COLOR["native"], linewidth=1.5, linestyle="--")
            panel_title = "%s   (native %.2f ms)" % (API_LABEL.get(api, api), value)
        ax.set_title(panel_title, loc="left")
        ax.set_xlim(-0.6, len(bindings) - 0.4)
        ax.set_xticks(range(len(bindings)))
        ax.set_xticklabels(bindings)
        ax.set_xlabel("Binding mode")
        ax.yaxis.grid(True)
        ax.set_axisbelow(True)
        ax.tick_params(axis="x", length=0)
    axes[0][0].set_ylabel(Y_LABEL)
    axes[0][0].set_ylim(bottom=0)
    _titles(fig, "Binding modes, %d render thread%s" % (threads, "" if threads == 1 else "s"),
            "Median of run medians, min-max whiskers. n/a: the layer cannot express the mode. Lower is better.")
    _legend(fig, used, [("native " + "(same API)", RENDERER_COLOR["native"], "--")] if has_native else [])
    return _save(plt, fig, path, 1 - 0.4 / fig.get_figheight())


def _ratio_chart(rows, field, path, labeler, xlabel, title, subtitle, color_of):
    """Lollipop chart of a ratio around 1.0, one row per configuration."""
    rows = [c for c in rows if c["derived"].get(field) is not None]
    if not rows:
        return None
    plt = _plt()
    fig, ax = plt.subplots(figsize=(8, 1.5 + 0.26 * len(rows)))
    ys, y, last = [], 0.0, None
    for c in rows:
        group = (c["renderer"], c["api"], c["binding"])
        if last is not None and group != last:
            y += 0.5
        ys.append(y)
        y += 1.0
        last = group
    values = [c["derived"][field] for c in rows]
    colors = [color_of(c) for c in rows]
    ax.margins(y=0.02)
    ax.axvline(1.0, color=AXIS, linewidth=1.2)
    ax.hlines(ys, 1.0, values, colors=colors, linewidth=2)
    ax.scatter(values, ys, s=42, c=colors, edgecolors=SURFACE, linewidths=1, zorder=3)
    lo, hi = min(values + [1.0]), max(values + [1.0])
    pad = max((hi - lo) * 0.12, 0.02)
    for yy, v in zip(ys, values):
        ax.annotate("%.2fx" % v, (v, yy), xytext=(7 if v >= 1 else -7, 0), textcoords="offset points",
                    va="center", ha="left" if v >= 1 else "right", fontsize=8, color=INK)
    ax.set_xlim(lo - pad * 1.8, hi + pad * 1.8)
    ax.set_yticks(ys)
    ax.set_yticklabels([labeler(c) for c in rows])
    ax.invert_yaxis()
    ax.set_xlabel(xlabel)
    ax.xaxis.grid(True)
    ax.set_axisbelow(True)
    ax.tick_params(axis="y", length=0)
    _titles(fig, title, subtitle)
    return plt, fig, rows


def chart_vulkan_vs_d3d12(configs, path):
    rows = [c for c in configs if c["api"] == "vk"]
    rows.sort(key=lambda c: (RENDERERS.index(c["renderer"]) if c["renderer"] in RENDERERS else 9,
                             BINDINGS.index(c["binding"]) if c["binding"] in BINDINGS else 9, c["threads"]))
    made = _ratio_chart(rows, "vk_over_d3d12_cpu_render", path,
                        lambda c: "%s  %s  %dT" % (c["renderer"], c["binding"], c["threads"]),
                        "Vulkan CPU render cost / D3D12 CPU render cost (ratio)",
                        "Vulkan versus D3D12 per abstraction layer",
                        "Ratio of the CPU render cost (render + submit), same renderer, binding and threads. Below 1: Vulkan is cheaper.",
                        lambda c: RENDERER_COLOR.get(c["renderer"], MUTED))
    if made is None:
        return None
    plt, fig, rows = made
    _legend(fig, {c["renderer"] for c in rows})
    return _save(plt, fig, path, 1 - 0.4 / fig.get_figheight())


def chart_frame_time_distribution(configs, path):
    # Paced runs are left out: their frame time is the refresh period, not the renderer's.
    rows = [c for c in configs if c["headline"] and c["unpaced_runs"]]
    title = "Frame-time distribution, headline configurations"
    if not rows:
        low = min((c["threads"] for c in configs if c["unpaced_runs"]), default=None)
        rows = [c for c in configs if c["unpaced_runs"] and c["threads"] == low]
        title = "Frame-time distribution, %s render thread(s)" % low
    if not rows:
        return None
    plt = _plt()
    fig, ax = plt.subplots(figsize=(9, 1.5 + 0.34 * len(rows)))
    right = 0.0
    for y, c in enumerate(rows):
        pooled = sorted(v for r in c["unpaced_runs"] for v in r.frame_ms)
        p1, p25, p50, p75, p99 = (percentile(pooled, q) for q in (1, 25, 50, 75, 99))
        color = RENDERER_COLOR.get(c["renderer"], MUTED)
        ax.hlines(y, p1, p99, color=color, linewidth=1.5)
        ax.vlines([p1, p99], y - 0.16, y + 0.16, color=color, linewidth=1.5)
        ax.barh(y, p75 - p25, left=p25, height=0.56, color=color)
        ax.vlines(p50, y - 0.4, y + 0.4, color=INK, linewidth=1.5)
        ax.annotate("%.2f" % p50, (p99, y), xytext=(6, 0), textcoords="offset points", va="center", fontsize=8, color=INK)
        right = max(right, p99)
    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels([_config_label(c, True) for c in rows])
    ax.invert_yaxis()
    ax.set_xlim(0, right * 1.1)
    ax.set_xlabel("Frame time (ms)")
    ax.xaxis.grid(True)
    ax.set_axisbelow(True)
    ax.tick_params(axis="y", length=0)
    _titles(fig, title, "All measured frames of all unpaced runs pooled. Box: p25-p75, dark tick: median (value at right), whiskers: p1-p99.")
    _legend(fig, {c["renderer"] for c in rows})
    return _save(plt, fig, path, 1 - 0.4 / fig.get_figheight())


def write_charts(configs, charts_dir):
    """Writes every chart that has data. Returns the list of files written."""
    written = []
    present = sorted({c["threads"] for c in configs if _m(c)})
    picks = [t for t in (1, 8) if t in present] or present[:1]
    for t in picks:
        written.append(chart_render_cost(configs, t, os.path.join(charts_dir, "render_cost_by_configuration_t%02d.png" % t)))
        written.append(chart_binding_modes(configs, t, os.path.join(charts_dir, "binding_modes_t%02d.png" % t)))
    written.append(chart_thread_scaling(configs, os.path.join(charts_dir, "thread_scaling.png")))
    written.append(chart_thread_speedup(configs, os.path.join(charts_dir, "thread_speedup.png")))
    written.append(chart_vulkan_vs_d3d12(configs, os.path.join(charts_dir, "vulkan_vs_d3d12.png")))
    written.append(chart_frame_time_distribution(configs, os.path.join(charts_dir, "frame_time_distribution.png")))
    return [w for w in written if w]


# ----------------------------------------------------------------------------------------------- entry point

def _load_aux_session(session_dir, what, log, refresh_hz=None):
    """Loads and aggregates a session that only supplies the CPU-bound check (GPU-timed or half resolution)."""
    runs, _ = load_session(session_dir, refresh_hz)
    issues, _ = validate_session(runs)
    errors = [i for i in issues if i["severity"] == "error"]
    for i in errors:
        log("  ERROR       %s session %s [%s] %s" % (what, i["run"], i["code"], i["message"]))
    return [c for c in aggregate(runs) if not c["variant"]], errors


def analyze(session_dir, out_dir, draws=None, charts=True, strict=False, log=print, gpu_session=None, half_res_session=None,
            refresh_hz=None):
    """Runs the whole analysis. Returns (exit_code, result dict)."""
    runs, manifest = load_session(session_dir, refresh_hz)
    issues, reference_hash = validate_session(runs)
    everything = aggregate(runs, draws)
    configs = [c for c in everything if not c["variant"]]      # the main matrix
    variants = [c for c in everything if c["variant"]]         # nvrhi sensitivity switches, reported separately

    counts = Counter(r.status for r in runs)
    not_ok = [r for r in runs if r.status != OK]

    data_dir = os.path.join(out_dir, "data")
    tables = ["summary.csv", "runs.csv", "layer_overhead_vs_native.csv",
              "binding_model_cost_vs_native.csv", "validation.json"]
    write_csv(os.path.join(data_dir, "summary.csv"), summary_rows(configs))
    write_csv(os.path.join(data_dir, "runs.csv"), run_rows(runs))
    write_csv(os.path.join(data_dir, "layer_overhead_vs_native.csv"),
              native_comparison_rows(configs, (LAYER_OVERHEAD, D3D11_LAYER_OVERHEAD)))
    write_csv(os.path.join(data_dir, "binding_model_cost_vs_native.csv"), native_comparison_rows(configs, (BINDING_MODEL_COST,)))
    # Optional tables: a stale file from an earlier analysis of another session must not survive.
    for name in ("sensitivity.csv", "cpu_bound_check.csv", "cb_page_split.csv"):
        if os.path.isfile(os.path.join(data_dir, name)):
            os.remove(os.path.join(data_dir, name))
    if variants:
        write_csv(os.path.join(data_dir, "sensitivity.csv"), sensitivity_rows(variants, configs))
        tables.append("sensitivity.csv")

    populations = cb_page_split_rows(everything)
    if len(populations) > 1:
        write_csv(os.path.join(data_dir, "cb_page_split.csv"), populations)
        tables.append("cb_page_split.csv")
    # Configuration-level notes for the report author (validation.json "warnings").
    for c in everything:
        name = config_name(*c["key"], variant=c["variant"])
        if c["split_runs"] and c["clean_runs"] and not c["variant"]:
            issues.append({"severity": "warning", "code": "cb_page_split_mixed", "run": name,
                           "message": "%d of %d runs hit the nvrhi D3D12 page-split store (render + submit %.3f ms against "
                                      "%.3f ms): the configuration is reported from the unaffected runs, see cb_page_split.csv"
                                      % (len(c["split_runs"]), len(c["split_runs"]) + len(c["clean_runs"]),
                                         c["cb_page_split_cpu_render"]["median"], c["cb_clean_cpu_render"]["median"])})
        elif c["split_runs"] and not c["variant"]:
            issues.append({"severity": "warning", "code": "cb_page_split_all", "run": name,
                           "message": "every run hit the nvrhi D3D12 page-split store: there is no unaffected run to "
                                      "report, see cb_page_split.csv"})
        if c["runs_pacing_excluded"] and not c["unpaced_runs"] and c["runs"]:
            issues.append({"severity": "warning", "code": "paced_config", "run": name,
                           "message": "every run is paced by the compositor or was not displayed: the configuration has "
                                      "no frame_ms, present_ms, wait_ms or render + submit + present"})

    gpu_configs = half_configs = None
    aux_errors = []
    if gpu_session:
        gpu_configs, e = _load_aux_session(gpu_session, "gpu", log, refresh_hz)
        aux_errors += e
    if half_res_session:
        half_configs, e = _load_aux_session(half_res_session, "half-res", log, refresh_hz)
        aux_errors += e
    check = cpu_bound_rows(configs, gpu_configs, half_configs)
    if len(check) > 1:
        write_csv(os.path.join(data_dir, "cpu_bound_check.csv"), check)
        tables.append("cpu_bound_check.csv")

    errors = [i for i in issues if i["severity"] == "error"]
    warnings = [i for i in issues if i["severity"] == "warning"]
    validation = {
        "session": os.path.abspath(session_dir),
        "gpu_session": os.path.abspath(gpu_session) if gpu_session else None,
        "half_res_session": os.path.abspath(half_res_session) if half_res_session else None,
        "draws_per_frame": ("override %d" % draws) if draws else "recorded draw_count of each configuration",
        "reference_static_scene_hash": reference_hash,
        "run_counts": dict(counts),
        "config_status_counts": dict(Counter(c["status"] for c in configs)),
        "sensitivity_status_counts": dict(Counter(c["status"] for c in variants)),
        "paced_runs": sum(1 for r in runs if r.status == OK and r.paced),
        "pacing_excluded_runs": sum(1 for r in runs if r.status == OK and r.pacing_excluded),
        "cb_page_split_runs": sum(1 for r in runs if r.status == OK and r.cb_page_split == "true"),
        "errors": errors,
        "aux_session_errors": aux_errors,
        "warnings": warnings,
        "runs_not_ok": [{"run": r.label, "status": r.status, "reason": r.reason} for r in not_ok],
    }
    with open(os.path.join(data_dir, "validation.json"), "w", encoding="utf-8") as f:
        json.dump(validation, f, indent=2)

    chart_files, chart_error = [], None
    if charts:
        try:
            chart_files = write_charts(configs, os.path.join(out_dir, "charts"))
        except ImportError as e:
            chart_error = "charts were NOT written: %s (pip install -r requirements.txt)" % e

    log("session   : %s" % os.path.abspath(session_dir))
    log("runs      : %d total; %s" % (len(runs), ", ".join("%s %d" % kv for kv in sorted(counts.items())) or "none"))
    log("configs   : %d in the main matrix (%s); %d sensitivity variant(s)"
        % (len(configs), ", ".join("%s %d" % kv for kv in sorted(Counter(c["status"] for c in configs).items())) or "none",
           len(variants)))
    log("scene hash: %s" % reference_hash)
    log("pacing    : %d valid run(s) paced by the compositor, %d excluded from the frame_ms-based metrics"
        % (validation["paced_runs"], validation["pacing_excluded_runs"]))
    log("page split: %d valid run(s) hit the nvrhi D3D12 page-split store" % validation["cb_page_split_runs"])
    for r in not_ok:
        log("  %-11s %s: %s" % (r.status, r.label, r.reason))
    for i in warnings:
        log("  warning     %s [%s] %s" % (i["run"], i["code"], i["message"]))
    for i in errors:
        log("  ERROR       %s [%s] %s" % (i["run"], i["code"], i["message"]))
    log("written   : %s: %s; %d chart(s)" % (data_dir, ", ".join(tables), len(chart_files)))
    if chart_error:
        log("  ERROR       " + chart_error)

    code = 0
    if errors or aux_errors or chart_error or counts.get(OK, 0) == 0:
        code = 1
    if strict and any(r.status in (FAILED, MISSING, PENDING) for r in runs):
        code = 1
    return code, {"runs": runs, "configs": configs, "sensitivity": variants, "issues": issues, "validation": validation,
                  "charts": chart_files, "manifest": manifest, "tables": tables}


def main(argv=None):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description="Aggregate a benchmark session into report/data and report/charts.")
    parser.add_argument("session", help="session directory, e.g. results/full-01")
    parser.add_argument("--out", default=os.path.join(root, "report"), help="report directory (default: <repo>/report)")
    parser.add_argument("--gpu-session", help="session taken with run_matrix.ps1 -GpuTiming (CPU-bound check: gpu_ms)")
    parser.add_argument("--half-res-session", help="session taken at half resolution (CPU-bound check: frame_ms)")
    parser.add_argument("--draws", type=int, default=None,
                        help="override the draws per frame used for ns-per-draw (default: the recorded draw_count)")
    parser.add_argument("--refresh-hz", type=float, default=None,
                        help="display refresh rate for the pacing check (default: extra.display_refresh_hz of each run)")
    parser.add_argument("--no-charts", action="store_true", help="write only the CSV/JSON data")
    parser.add_argument("--strict", action="store_true", help="also exit 1 when any run failed or is missing")
    args = parser.parse_args(argv)
    try:
        code, _ = analyze(args.session, args.out, draws=args.draws, charts=not args.no_charts, strict=args.strict,
                          gpu_session=args.gpu_session, half_res_session=args.half_res_session, refresh_hz=args.refresh_hz)
    except FileNotFoundError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    return code


if __name__ == "__main__":
    sys.exit(main())
