#!/usr/bin/env python3
"""Capture the retained Lucy/explosion scene and report per-mode RT differences.

Requires Pillow and numpy. GPU processes run serially. The optional finite level-set
reference is diagnostic evidence from the separately documented NaN-offset repair;
the unchanged reference remains in the report and is never silently replaced.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import time

import numpy as np
from PIL import Image, ImageFilter

MODES = ("volume", "trilinear", "emptyskip", "levelset")
APIS = ("dx12", "vk", "cuda")


def capture(executable, arguments, output, label):
    start = time.perf_counter()
    with (output / f"{label}.log").open("w", encoding="utf-8") as log:
        subprocess.run([str(executable), *arguments], cwd=output, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=180)
    return (time.perf_counter() - start) * 1000


def pixels(path, sigma=0):
    with Image.open(path) as image:
        if image.size != (1024, 768):
            raise ValueError(f"Unexpected image dimensions: {path}: {image.size}")
        image = image.convert("RGB")
        if sigma:
            image = image.filter(ImageFilter.GaussianBlur(sigma))
        return np.asarray(image, dtype=np.float64)


def compare(reference, current):
    delta = np.abs(pixels(reference) - pixels(current))
    smooth = np.abs(pixels(reference, 4) - pixels(current, 4))
    # Fixed volume ROI prevents the largely empty frame from hiding regressions.
    roi = (slice(220, 590), slice(500, 880))
    return {"meanRGB": float(delta.mean()), "maxRGB": int(delta.max()),
            "pixelsOver8": int(np.any(delta > 8, axis=2).sum()),
            "filteredMeanRGB": float(smooth.mean()),
            "volumeRoiMeanRGB": float(delta[roi].mean()),
            "volumeRoiFilteredMeanRGB": float(smooth[roi].mean())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy", type=Path)
    parser.add_argument("--portable", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--legacy-api", choices=("dx12", "vk"), default="dx12")
    parser.add_argument("--finite-levelset-reference", type=Path)
    parser.add_argument("--secondary-bias", type=float, default=0,
                        help="same explicit world-space secondary-ray origin offset in both renderers")
    parser.add_argument("--reuse", action="store_true", help="measure existing captures")
    parser.add_argument("--verify", action="store_true",
                        help="check the documented 0.01-bias fixture; requires the separate finite level-set reference")
    args = parser.parse_args()
    if not math.isfinite(args.secondary_bias) or args.secondary_bias < 0:
        parser.error("--secondary-bias must be finite and nonnegative")
    if args.verify and (abs(args.secondary_bias - .01) > 1e-9 or not args.finite_levelset_reference):
        parser.error("--verify requires --secondary-bias 0.01 and --finite-levelset-reference")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"samples": 8, "width": 1024, "height": 768,
              "secondaryBias": args.secondary_bias,
              "timingScope": "process wall time including startup, asset upload and PNG I/O",
              "modes": {}}
    if not args.reuse and (not args.legacy or not args.portable):
        parser.error("capture requires --legacy and --portable")
    previous = {}
    if args.reuse and (output / "comparison.json").exists():
        previous = json.loads((output / "comparison.json").read_text(encoding="utf-8"))
        if previous.get("secondaryBias", 0) != args.secondary_bias:
            parser.error("--reuse bias does not match the recorded capture")
    for mode in MODES:
        item = report["modes"][mode] = {}
        old_item = previous.get("modes", {}).get(mode, {})
        if "legacyProcessMs" in old_item:
            item["legacyProcessMs"] = old_item["legacyProcessMs"]
        if not args.reuse:
            # Avoid the base application's frame-limit screenshot overwriting
            # the renderer's raw output with a screenshot containing the UI.
            item["legacyProcessMs"] = capture(args.legacy.resolve(),
                [f"-{args.legacy_api}", "--shading", mode, "--samples", "8",
                 "--secondary-bias", str(args.secondary_bias),
                 "--frames", "1000000", "--screenshot", f"legacy-{mode}.png"],
                output, f"legacy-{mode}")
        for api in APIS:
            if not args.reuse:
                elapsed = capture(args.portable.resolve(),
                    [f"-{api}", "--headless", "--shading", mode, "--samples", "8",
                     "--secondary-bias", str(args.secondary_bias),
                     "--width", "1024", "--height", "768", "--screenshot", f"{api}-{mode}.png"],
                    output, f"{api}-{mode}")
            metrics = item[api] = compare(output / f"legacy-{mode}.png", output / f"{api}-{mode}.png")
            if not args.reuse:
                metrics["processMs"] = elapsed
            elif "processMs" in old_item.get(api, {}):
                metrics["processMs"] = old_item[api]["processMs"]
            metrics["versusDX12"] = compare(output / f"dx12-{mode}.png", output / f"{api}-{mode}.png")
            if mode == "levelset" and args.finite_levelset_reference:
                metrics["finiteReferenceDiagnostic"] = compare(args.finite_levelset_reference,
                                                               output / f"{api}-{mode}.png")
    failures = []
    if args.verify:
        # Eight-sample stochastic images with corrected independent random seeds
        # and centered primary rays. The fixed ROI constrains volume error too.
        limits = {"meanRGB": 2.5, "filteredMeanRGB": .8, "volumeRoiFilteredMeanRGB": 2.5}
        report["acceptance"] = {"limits": limits, "crossApiMeanLimit": .02,
                                "levelsetReference": "finite-offset diagnostic; original also reported"}
        for mode in MODES:
            for api in APIS:
                metrics = report["modes"][mode][api]
                selected = metrics["finiteReferenceDiagnostic"] if mode == "levelset" else metrics
                for name, limit in limits.items():
                    if selected[name] > limit:
                        failures.append(f"{mode}/{api}: {name}={selected[name]} > {limit}")
                if metrics["versusDX12"]["meanRGB"] > .02:
                    failures.append(f"{mode}/{api}: cross-API mean exceeds .02")
        report["acceptance"]["failures"] = failures
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if failures:
        raise SystemExit("Mixed RT comparison failed: " + "; ".join(failures))


if __name__ == "__main__":
    main()
