"""Builds synthetic benchmark sessions in the layout written by scripts/run_matrix.ps1."""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts"))

import analyze  # noqa: E402

GOOD_HASH = "0x00000000deadbeef"
HEADER = ",".join(analyze.CSV_HEADER)


class Session:
    def __init__(self, root):
        self.root = root
        self.manifest_runs = []

    def run_dir(self, renderer, api, binding, threads, rep, variant=""):
        return os.path.join(self.root, analyze.config_name(renderer, api, binding, threads, variant), "rep%d" % rep)

    def add_ok(self, renderer, api, binding, threads, rep, render=1.0, submit=0.5, frames=200, duration=None,
               scene_hash=GOOD_HASH, gpu=None, draws=50000, rows=None, used_threads=None, present=0.05, gc=0.0,
               variant="", extra=None, **overrides):
        """A successful run. Every frame has the same timings unless `rows` gives (render, submit) per frame.
        frame_ms is chosen so that the frames add up to the declared duration. `gpu` makes it a GPU-timed run.
        `variant` is the nvrhi sensitivity variant: its flags are recorded in "extra" unless `extra` is given."""
        path = self.run_dir(renderer, api, binding, threads, rep, variant)
        os.makedirs(path)
        if extra is None:
            extra = {flag: "true" for flag in variant.split("+") if flag}
        if rows is None:
            rows = [(render, submit)] * frames
        duration = duration if duration is not None else 2.0
        frame_ms = duration * 1000.0 / len(rows)
        with open(os.path.join(path, "frames.csv"), "w", encoding="utf-8", newline="") as f:
            f.write(HEADER + "\n")
            for i, (r, s) in enumerate(rows):
                f.write("%d,0.250000,%.6f,%.6f,%.6f,%.6f,%.6f,%s,%d,%d,%.6f\n"
                        % (100 + i, r, s, 0.1 + gc, present, frame_ms, "" if gpu is None else "%.6f" % gpu, draws,
                           draws * 60, gc))
        meta = {
            "status": "ok", "renderer": renderer, "api": api, "binding": binding,
            "requested_binding": "" if renderer == "native" else binding,
            "threads": threads if used_threads is None else used_threads, "requested_threads": threads, "width": 1080, "height": 720,
            "asteroids": 50000, "meshes": 1000, "textures": 10, "seed": 1337,
            "warmup_seconds": 1.0, "duration_seconds": duration, "measured_seconds": duration,
            "frame_count": len(rows), "vsync": False, "validation": False, "gpu_timing": gpu is not None,
            "adapter_luid": "96800", "adapter": "NVIDIA GeForce RTX 4050 Laptop GPU",
            "static_scene_hash": scene_hash, "final_dynamic_scene_hash": "0x0000000000000001",
            "build": "release", "extra": extra,
        }
        meta.update(overrides)
        with open(os.path.join(path, "run.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f)
        self._manifest(renderer, api, binding, threads, rep, "ok", variant=variant)
        return path

    def add_failed(self, renderer, api, binding, threads, rep, kind="error", reason="device removed"):
        """A run that wrote a failed run.json itself (exit code 2 or 3)."""
        path = self.run_dir(renderer, api, binding, threads, rep)
        os.makedirs(path)
        meta = {"status": "failed", "failure_kind": kind, "reason": reason, "renderer": renderer, "api": api,
                "binding": binding, "requested_binding": binding, "threads": threads}
        with open(os.path.join(path, "run.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f)
        self._manifest(renderer, api, binding, threads, rep, "unsupported" if kind == "unsupported" else "failed", reason)
        return path

    def add_manifest_only(self, renderer, api, binding, threads, rep, status="timeout", reason="killed after 95 s"):
        """A run that left no run.json (crash, timeout) or has not been executed yet (status pending)."""
        self._manifest(renderer, api, binding, threads, rep, status, reason)

    def _manifest(self, renderer, api, binding, threads, rep, status, reason="", variant=""):
        self.manifest_runs.append({
            "config": analyze.config_name(renderer, api, binding, threads, variant), "rep": "rep%d" % rep,
            "renderer": renderer, "api": api, "binding": binding, "threads": threads, "variant": variant,
            "status": status, "reason": reason})

    def write_manifest(self):
        with open(os.path.join(self.root, "manifest.json"), "w", encoding="utf-8") as f:
            json.dump({"session": os.path.basename(self.root), "runs": self.manifest_runs}, f)
