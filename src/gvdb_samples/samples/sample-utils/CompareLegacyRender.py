"""Optional retained-CUDA image and end-to-end timing comparison.

Requires NumPy and Pillow. Runs each GPU process serially, at the retained
render-to-file resolution of 1024x768. Runtime timings include startup, asset
loading, rendering/readback and PNG encoding; they are not kernel benchmarks.
The portable program's extra reference-view VBX metadata parse is included.
No process-memory estimate is inferred from these timings. Logical atlas and
topology storage are reported by each executable in the captured logs.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time

import numpy as np
from PIL import Image


def verify(report):
    # Explosion fixture only: software interpolation and finite traversal repairs
    # can move discontinuous shading at a small number of threshold/voxel edges.
    # Most channels must still agree within one byte; broad image drift fails.
    failures = []
    layered = "secondLayerIndexOffset" in report
    # The two-instance fixture doubles the independent sampling-error allowance.
    # Exact integer over can differ by one byte from retained approximate CUDA
    # division, including alpha 255 becoming 254 in otherwise opaque images.
    mean_rgb, p99_rgb, mean_alpha, outliers = (.5, 2, 1.025, .02) if layered else (.25, 1, .025, .01)
    for mode, entry in report["modes"].items():
        for api, metrics in entry["backends"].items():
            if (metrics["meanRGBError"] > mean_rgb or metrics["p99RGBError"] > p99_rgb or
                    metrics["meanAlphaError"] > mean_alpha or metrics["fractionPixelsAboveEightCodes"] > outliers):
                failures.append(f"mode {mode} {api}")
    if len(report["modes"]) != 8 or failures:
        raise RuntimeError(f"Incomplete or out-of-tolerance explosion comparison: {failures}")


def compare(actual, reference):
    a = np.asarray(Image.open(actual)).astype(np.int16)
    b = np.asarray(Image.open(reference)).astype(np.int16)
    if a.shape != b.shape or a.shape != (768, 1024, 4):
        raise ValueError(f"Unexpected image extent: {a.shape}, {b.shape}")
    difference = abs(a - b)
    return {
        "changedPixels": int(np.any(difference, axis=2).sum()),
        "maximumChannelError": int(difference.max()),
        "meanRGBError": float(difference[:, :, :3].mean()),
        "p99RGBError": float(np.quantile(difference[:, :, :3], .99)),
        "meanAlphaError": float(difference[:, :, 3].mean()),
        "fractionPixelsAboveEightCodes": float(np.any(difference > 8, axis=2).mean()),
    }


def run(command, folder, log):
    start = time.perf_counter()
    result = subprocess.run(command, cwd=folder, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    elapsed = (time.perf_counter() - start) * 1000
    log.write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}")
    return elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy", type=Path, required=True)
    parser.add_argument("--portable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--apis", nargs="+", choices=["dx11", "dx12", "vk", "cuda"],
                        default=["dx11", "dx12", "vk", "cuda"])
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--verify", action="store_true", help="Check the documented explosion-fixture tolerances")
    parser.add_argument("--layer-offset", type=float, nargs=3, metavar=("X", "Y", "Z"),
                        help="Compare a second translated explosion instance in scene order")
    parser.add_argument("--layer-asset", type=Path,
                        help="Portable explosion VBX path; required with --layer-offset")
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("repetitions must be positive")
    if args.layer_offset and not args.layer_asset:
        parser.error("--layer-offset requires --layer-asset")
    legacy, portable, folder = args.legacy.resolve(), args.portable.resolve(), args.output.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    report = {"resolution": [1024, 768], "timing": "process wall milliseconds, including I/O",
              "legacyExecutable": str(legacy), "portableExecutable": str(portable), "modes": {}}
    legacy_layers, portable_layers = [], []
    if args.layer_offset:
        offset = [str(x) for x in args.layer_offset]
        legacy_layers = ["--duplicate-offset", *offset]
        portable_layers = ["--layer", str(args.layer_asset.resolve()), "--layer-offset", *offset]
        report["secondLayerIndexOffset"] = args.layer_offset
    for mode in range(8):
        reference = folder / f"reference-{mode}.png"
        timings = [run([str(legacy), "--shade", str(mode), *legacy_layers], folder, folder / f"reference-{mode}-{i}.log")
                   for i in range(args.repetitions)]
        (folder / "out_rendtofile.png").replace(reference)
        entry = {"legacyWallMilliseconds": timings, "backends": {}}
        for api in args.apis:
            output = folder / f"{api}-{mode}.png"
            timings = [run([str(portable), f"-{api}", "--reference-scene", "--shade", str(mode),
                            "--output", str(output), *portable_layers], folder, folder / f"{api}-{mode}-{i}.log")
                       for i in range(args.repetitions)]
            entry["backends"][api] = {**compare(output, reference), "wallMilliseconds": timings}
        report["modes"][str(mode)] = entry
        (folder / "comparison.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"Compared mode {mode}", flush=True)
    if args.verify:
        verify(report)
    print(f"Wrote {folder / 'comparison.json'}")


if __name__ == "__main__":
    main()
