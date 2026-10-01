"""Unit tests for scripts/analyze.py over synthetic sessions.

    python -m unittest discover -s tests/analysis
"""
import contextlib
import csv
import importlib.util
import io
import json
import os
import shutil
import tempfile
import unittest

from synthetic import GOOD_HASH, Session, analyze

HAVE_MATPLOTLIB = importlib.util.find_spec("matplotlib") is not None


class SessionCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="analysis_test_")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.session = Session(os.path.join(self.tmp, "session"))
        os.makedirs(self.session.root)
        self.out = os.path.join(self.tmp, "report")

    def run_analysis(self, **kwargs):
        self.session.write_manifest()
        log = io.StringIO()
        kwargs.setdefault("charts", False)
        code, result = analyze.analyze(self.session.root, self.out, log=lambda s: log.write(s + "\n"), **kwargs)
        result["log"] = log.getvalue()
        result["by_key"] = {c["key"]: c for c in result["configs"]}
        return code, result

    def summary(self):
        with open(os.path.join(self.out, "data", "summary.csv"), newline="", encoding="utf-8") as f:
            return {(r["renderer"], r["api"], r["binding"], int(r["threads"])): r for r in csv.DictReader(f)}

    def table(self, name):
        with open(os.path.join(self.out, "data", name), newline="", encoding="utf-8") as f:
            return list(csv.DictReader(f))


class PercentileTests(unittest.TestCase):
    def test_linear_interpolation(self):
        data = list(range(1, 101))  # 1..100
        self.assertAlmostEqual(analyze.percentile(data, 50), 50.5)
        self.assertAlmostEqual(analyze.percentile(data, 1), 1.99)
        self.assertAlmostEqual(analyze.percentile(data, 99), 99.01)
        self.assertAlmostEqual(analyze.percentile(data, 0), 1.0)
        self.assertAlmostEqual(analyze.percentile(data, 100), 100.0)

    def test_unsorted_single_and_empty(self):
        self.assertAlmostEqual(analyze.median([5.0, 1.0, 3.0]), 3.0)
        self.assertAlmostEqual(analyze.median([4.0, 1.0]), 2.5)
        self.assertEqual(analyze.percentile([7.0], 99), 7.0)
        self.assertIsNone(analyze.percentile([], 50))
        self.assertIsNone(analyze.column_stats([]))

    def test_matches_numpy_when_available(self):
        try:
            import numpy
        except ImportError:
            self.skipTest("numpy is not installed")
        data = [((i * 7919) % 1013) / 7.0 for i in range(997)]
        for q in (1, 25, 50, 75, 99):
            self.assertAlmostEqual(analyze.percentile(data, q), float(numpy.percentile(data, q)), places=9)


class HelperTests(unittest.TestCase):
    def test_config_names_round_trip(self):
        for key in [("nvrhi", "d3d12", "tex_mut_pc", 8), ("native", "d3d11", "original", 1), ("nvrhi", "vk", "bindless", 15)]:
            self.assertEqual(analyze.parse_config_name(analyze.config_name(*key)), key)
        self.assertIsNone(analyze.parse_config_name("profiles"))
        self.assertIsNone(analyze.parse_config_name("report"))

    def test_variant_suffix(self):
        name = analyze.config_name("nvrhi", "vk", "tex_mut", 8, "always_set_state")
        self.assertEqual(name, "nvrhi_vk_tex_mut_t08__always_set_state")
        self.assertEqual(analyze.parse_config_name(name), ("nvrhi", "vk", "tex_mut", 8))
        self.assertEqual(analyze.config_variant(name), "always_set_state")
        self.assertEqual(analyze.config_variant("nvrhi_vk_tex_mut_t08"), "")

    def test_headline_definition(self):
        self.assertTrue(analyze.is_headline(("nvrhi", "vk", "tex_mut", 8)))
        self.assertTrue(analyze.is_headline(("nvrhi", "d3d12", "tex_mut", 1)))
        self.assertTrue(analyze.is_headline(("native", "d3d12", "original", 1)))
        self.assertTrue(analyze.is_headline(("nvrhi", "d3d12", "bindless", 8)))
        self.assertTrue(analyze.is_headline(("nvrhi", "vk", "bindless", 1)))
        self.assertFalse(analyze.is_headline(("nvrhi", "vk", "dyn", 1)))
        self.assertFalse(analyze.is_headline(("native", "d3d11", "original", 1)))
        self.assertFalse(analyze.is_headline(("nvrhi", "d3d12", "tex_mut", 4)))
        self.assertFalse(analyze.is_headline(("nvrhi", "d3d12", "mut", 1)))
        self.assertFalse(analyze.is_headline(("nvrhi", "d3d11", "tex_mut", 1)))

    def test_overhead_helpers(self):
        self.assertAlmostEqual(analyze.overhead_percent(2.2, 2.0), 10.0)
        self.assertAlmostEqual(analyze.overhead_percent(1.5, 2.0), -25.0)
        self.assertIsNone(analyze.overhead_percent(2.2, None))
        self.assertIsNone(analyze.overhead_percent(2.2, 0.0))
        # 0.2 ms spread over 50,000 draws is 4 ns per draw.
        self.assertAlmostEqual(analyze.overhead_ns_per_draw(2.2, 2.0, 50000), 4.0)
        self.assertIsNone(analyze.overhead_ns_per_draw(None, 2.0, 50000))
        self.assertIsNone(analyze.overhead_ns_per_draw(2.2, 2.0, None))
        self.assertIsNone(analyze.ratio(1.0, 0.0))


class AggregationTests(SessionCase):
    def test_per_run_statistics(self):
        # render = 1..100 ms, submit = 1 ms: cpu_render = 2..101.
        rows = [(float(i), 1.0) for i in range(1, 101)]
        self.session.add_ok("native", "d3d12", "original", 1, 1, rows=rows, gpu=0.4)
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        run = result["runs"][0]
        self.assertEqual(run.status, analyze.OK)
        self.assertEqual(run.frame_count, 100)
        self.assertAlmostEqual(run.stats["render_ms"]["median"], 50.5)
        self.assertAlmostEqual(run.stats["render_ms"]["p1"], 1.99)
        self.assertAlmostEqual(run.stats["render_ms"]["p99"], 99.01)
        self.assertAlmostEqual(run.stats["submit_ms"]["median"], 1.0)
        self.assertAlmostEqual(run.stats["cpu_render_ms"]["median"], 51.5)
        self.assertAlmostEqual(run.stats["cpu_render_ms"]["p99"], 100.01)
        self.assertAlmostEqual(run.stats["update_ms"]["median"], 0.25)
        self.assertAlmostEqual(run.stats["gpu_ms"]["median"], 0.4)
        self.assertAlmostEqual(run.stats["frame_ms"]["median"], 20.0)
        self.assertTrue(run.gpu_timed)

    def test_sensitivity_metrics_are_summed_per_frame(self):
        self.session.add_ok("nvrhi", "vk", "tex_mut", 1, 1, render=1.0, submit=0.5, present=0.25, gc=2.0)
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        stats = result["runs"][0].stats
        self.assertAlmostEqual(stats["cpu_render_ms"]["median"], 1.5)
        self.assertAlmostEqual(stats["cpu_render_present_ms"]["median"], 1.75)
        self.assertAlmostEqual(stats["cpu_render_gc_ms"]["median"], 3.5)
        self.assertAlmostEqual(stats["gc_ms"]["median"], 2.0)
        row = self.summary()[("nvrhi", "vk", "tex_mut", 1)]
        self.assertAlmostEqual(float(row["cpu_render_present_ms"]), 1.75)
        self.assertAlmostEqual(float(row["cpu_render_gc_ms"]), 3.5)
        self.assertAlmostEqual(float(row["gc_ms"]), 2.0)

    def test_cpu_render_is_summed_per_frame_not_from_medians(self):
        # median(render) + median(submit) = 1 + 1 = 2, but the per-frame sums are 11, 2, 11 -> median 11.
        rows = ([(10.0, 1.0)] * 40) + ([(1.0, 1.0)] * 20) + ([(1.0, 10.0)] * 40)
        self.session.add_ok("native", "d3d12", "original", 1, 1, rows=rows)
        _, result = self.run_analysis()
        stats = result["runs"][0].stats
        self.assertAlmostEqual(stats["render_ms"]["median"], 1.0)
        self.assertAlmostEqual(stats["submit_ms"]["median"], 1.0)
        self.assertAlmostEqual(stats["cpu_render_ms"]["median"], 11.0)

    def test_median_of_run_medians_and_spread(self):
        for rep, render in enumerate([1.5, 1.9, 1.7], start=1):  # cpu_render 2.0, 2.4, 2.2
            self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, rep, render=render, submit=0.5)
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]
        m = c["metrics"]["cpu_render_ms"]
        self.assertEqual(c["runs_ok"], 3)
        self.assertAlmostEqual(m["median"], 2.2)
        self.assertAlmostEqual(m["min"], 2.0)
        self.assertAlmostEqual(m["max"], 2.4)
        self.assertAlmostEqual(m["spread_pct"], 0.4 / 2.2 * 100.0)
        self.assertIsNone(c["metrics"]["gpu_ms"])  # no GPU timing recorded
        row = self.summary()[("nvrhi", "d3d12", "tex_mut", 1)]
        self.assertAlmostEqual(float(row["cpu_render_ms"]), 2.2)
        self.assertAlmostEqual(float(row["cpu_render_ms_min"]), 2.0)
        self.assertAlmostEqual(float(row["cpu_render_ms_max"]), 2.4)
        self.assertEqual(row["gpu_ms"], "")
        self.assertEqual(row["status"], "ok")
        self.assertEqual(row["headline"], "1")

    def test_even_number_of_runs(self):
        self.session.add_ok("nvrhi", "vk", "mut", 2, 1, render=1.0, submit=1.0)
        self.session.add_ok("nvrhi", "vk", "mut", 2, 2, render=2.0, submit=1.0)
        _, result = self.run_analysis()
        self.assertAlmostEqual(result["by_key"][("nvrhi", "vk", "mut", 2)]["metrics"]["cpu_render_ms"]["median"], 2.5)


class DerivedTests(SessionCase):
    def build(self):
        s = self.session
        s.add_ok("native", "d3d12", "original", 1, 1, render=1.5, submit=0.5)   # 2.0
        s.add_ok("native", "d3d12", "original", 8, 1, render=0.3, submit=0.2)   # 0.5
        s.add_ok("native", "d3d11", "original", 1, 1, render=3.5, submit=0.5)   # 4.0
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=1.7, submit=0.5)     # 2.2
        s.add_ok("nvrhi", "d3d12", "tex_mut", 8, 1, render=0.35, submit=0.2)    # 0.55
        s.add_ok("nvrhi", "vk", "tex_mut", 1, 1, render=1.48, submit=0.5)       # 1.98
        s.add_ok("nvrhi", "d3d12", "mut", 1, 1, render=2.5, submit=0.5)         # 3.0
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        return result["by_key"]

    def test_overhead_versus_native(self):
        by_key = self.build()
        d = by_key[("nvrhi", "d3d12", "tex_mut", 1)]["derived"]
        self.assertAlmostEqual(d["overhead_vs_native_pct"], 10.0)
        self.assertAlmostEqual(d["overhead_vs_native_ns_per_draw"], 4.0)     # 0.2 ms / 50,000
        self.assertEqual(d["native_baseline"], "native D3D12")
        self.assertIsNone(d["overhead_vs_native_d3d12_cross_api_pct"])
        self.assertAlmostEqual(d["ns_per_draw"], 44.0)
        d = by_key[("nvrhi", "d3d12", "mut", 1)]["derived"]
        self.assertAlmostEqual(d["overhead_vs_native_pct"], 50.0)
        self.assertAlmostEqual(d["overhead_vs_native_ns_per_draw"], 20.0)

    def test_overhead_compares_like_with_like(self):
        s = self.session
        s.add_ok("nvrhi", "d3d11", "tex_mut", 1, 1, render=4.5, submit=0.5)       # 5.0
        s.add_ok("nvrhi", "d3d11", "tex_mut", 8, 1, render=4.5, submit=0.5)       # no native D3D11 at 8 threads
        by_key = self.build()
        # D3D11 rows: the native D3D11 baseline (4.0), never the native D3D12 one (2.0).
        d = by_key[("nvrhi", "d3d11", "tex_mut", 1)]["derived"]
        self.assertAlmostEqual(d["overhead_vs_native_pct"], 25.0)
        self.assertEqual(d["native_baseline"], "native D3D11")
        self.assertIsNone(d["overhead_vs_native_d3d12_cross_api_pct"])
        d = by_key[("nvrhi", "d3d11", "tex_mut", 8)]["derived"]
        self.assertIsNone(d["overhead_vs_native_pct"])
        self.assertIsNone(d["overhead_vs_native_ns_per_draw"])
        self.assertIsNone(d["overhead_vs_native_d3d12_cross_api_pct"])
        self.assertEqual(d["native_baseline"], "")
        # Vulkan rows: native D3D12 only as an explicitly labelled cross-API reference.
        d = by_key[("nvrhi", "vk", "tex_mut", 1)]["derived"]
        self.assertIsNone(d["overhead_vs_native_pct"])
        self.assertAlmostEqual(d["overhead_vs_native_d3d12_cross_api_pct"], -1.0)
        self.assertAlmostEqual(d["overhead_vs_native_d3d12_cross_api_ns_per_draw"], -0.4)
        self.assertEqual(d["native_baseline"], "native D3D12 (cross-API reference)")
        row = self.summary()[("nvrhi", "d3d11", "tex_mut", 8)]
        self.assertEqual(row["overhead_vs_native_pct"], "")
        self.assertNotIn("overhead_vs_native_d3d12_pct", row)

    def test_overhead_uses_the_same_thread_count(self):
        by_key = self.build()
        d = by_key[("nvrhi", "d3d12", "tex_mut", 8)]["derived"]
        self.assertAlmostEqual(d["overhead_vs_native_pct"], 10.0)            # 0.55 vs 0.5
        self.assertAlmostEqual(d["overhead_vs_native_ns_per_draw"], 1.0)

    def test_native_baseline_has_no_overhead_columns(self):
        by_key = self.build()
        for key in (("native", "d3d12", "original", 1), ("native", "d3d11", "original", 1)):
            d = by_key[key]["derived"]
            self.assertIsNone(d["overhead_vs_native_pct"])
            self.assertIsNone(d["overhead_vs_native_d3d12_cross_api_pct"])
            self.assertEqual(d["native_comparison"], "")

    def test_native_comparison_labels_and_tables(self):
        s = self.session
        s.add_ok("nvrhi", "d3d12", "bindless", 1, 1, render=1.6, submit=0.5)      # 2.1
        s.add_ok("nvrhi", "vk", "bindless", 1, 1, render=1.9, submit=0.5)         # 2.4
        s.add_ok("nvrhi", "d3d11", "mut", 1, 1, render=4.5, submit=0.5)           # 5.0
        by_key = self.build()
        self.assertEqual(by_key[("nvrhi", "d3d12", "bindless", 1)]["derived"]["native_comparison"], analyze.LAYER_OVERHEAD)
        self.assertEqual(by_key[("nvrhi", "vk", "bindless", 1)]["derived"]["native_comparison"], analyze.LAYER_OVERHEAD)
        self.assertEqual(by_key[("nvrhi", "d3d12", "tex_mut", 1)]["derived"]["native_comparison"], analyze.BINDING_MODEL_COST)
        self.assertEqual(by_key[("nvrhi", "vk", "tex_mut", 1)]["derived"]["native_comparison"], analyze.BINDING_MODEL_COST)
        self.assertEqual(by_key[("nvrhi", "d3d11", "mut", 1)]["derived"]["native_comparison"], analyze.D3D11_LAYER_OVERHEAD)
        layer = self.table("layer_overhead_vs_native.csv")
        cost = self.table("binding_model_cost_vs_native.csv")
        self.assertEqual({(r["renderer"], r["api"], r["binding"]) for r in layer},
                         {("nvrhi", "d3d12", "bindless"), ("nvrhi", "vk", "bindless"), ("nvrhi", "d3d11", "mut")})
        self.assertTrue(all(r["comparison"] == analyze.BINDING_MODEL_COST for r in cost))
        self.assertNotIn("bindless", {r["binding"] for r in cost})
        row = next(r for r in layer if r["api"] == "d3d12")
        self.assertAlmostEqual(float(row["overhead_cpu_render_pct"]), 5.0)
        self.assertAlmostEqual(float(row["native_cpu_render_ms"]), 2.0)
        self.assertEqual(row["native_baseline"], "native D3D12")
        row = next(r for r in layer if r["api"] == "vk")
        self.assertEqual(row["native_baseline"], "native D3D12 (cross-API reference)")
        self.assertAlmostEqual(float(row["overhead_cpu_render_pct"]), 20.0)

    def test_thread_scaling(self):
        by_key = self.build()
        self.assertAlmostEqual(by_key[("nvrhi", "d3d12", "tex_mut", 8)]["derived"]["speedup_vs_1_thread"], 4.0)
        self.assertAlmostEqual(by_key[("native", "d3d12", "original", 8)]["derived"]["speedup_vs_1_thread"], 4.0)
        self.assertAlmostEqual(by_key[("nvrhi", "d3d12", "tex_mut", 1)]["derived"]["speedup_vs_1_thread"], 1.0)

    def test_vulkan_versus_d3d12(self):
        by_key = self.build()
        self.assertAlmostEqual(by_key[("nvrhi", "vk", "tex_mut", 1)]["derived"]["vk_over_d3d12_cpu_render"], 0.9)
        self.assertIsNone(by_key[("nvrhi", "d3d12", "tex_mut", 1)]["derived"]["vk_over_d3d12_cpu_render"])

    def test_ns_per_draw_uses_the_recorded_draw_count(self):
        # 50,001 draws per frame is what the renderers record (asteroids + skybox): no warning, no assumption.
        self.session.add_ok("native", "d3d12", "original", 1, 1, render=1.5, submit=0.5, draws=50001)
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=1.7, submit=0.5, draws=50001)
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]
        self.assertAlmostEqual(c["derived"]["overhead_vs_native_ns_per_draw"], 0.2e6 / 50001)
        self.assertAlmostEqual(c["derived"]["ns_per_draw"], 2.2e6 / 50001)
        self.assertEqual(result["validation"]["warnings"], [])
        self.assertEqual(self.summary()[("nvrhi", "d3d12", "tex_mut", 1)]["draw_count"], "50001")

    def test_other_recorded_draw_count(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1, render=1.5, submit=0.5, draws=25000)
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=1.7, submit=0.5, draws=25000)
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        d = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]["derived"]
        self.assertAlmostEqual(d["overhead_vs_native_ns_per_draw"], 8.0)
        self.assertEqual(result["validation"]["warnings"], [])

    def test_no_ns_per_draw_overhead_when_the_draw_counts_differ(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1, render=1.5, submit=0.5, draws=50000)
        self.session.add_ok("native", "d3d12", "original", 1, 2, render=1.5, submit=0.5, draws=50000)
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=1.7, submit=0.5, draws=25000)
        code, result = self.run_analysis()
        d = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]["derived"]
        self.assertAlmostEqual(d["overhead_vs_native_pct"], 10.0)
        self.assertIsNone(d["overhead_vs_native_ns_per_draw"])
        self.assertIn("draw_count_differs", [w["code"] for w in result["validation"]["warnings"]])

    def test_draws_override(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1, render=1.5, submit=0.5)
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=1.7, submit=0.5)
        _, result = self.run_analysis(draws=100000)
        d = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]["derived"]
        self.assertAlmostEqual(d["overhead_vs_native_ns_per_draw"], 2.0)

    def test_gpu_over_frame(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1, gpu=2.0)  # frame_ms is 10 ms
        _, result = self.run_analysis()
        self.assertAlmostEqual(result["by_key"][("native", "d3d12", "original", 1)]["derived"]["gpu_over_frame"], 0.2)


class GpuTimingTests(SessionCase):
    def test_gpu_timed_runs_are_excluded_from_every_headline_metric(self):
        s = self.session
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=1.0, submit=0.5)
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 2, render=1.2, submit=0.5)
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 3, render=9.0, submit=0.5, gpu=4.0, duration=4.0)   # GPU-timed, 20 ms frames
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]
        self.assertEqual((c["runs_ok"], c["runs_gpu_timed"], len(c["runs"])), (3, 1, 2))
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["median"], 1.6)
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["max"], 1.7)
        self.assertAlmostEqual(c["metrics"]["render_ms"]["max"], 1.2)
        self.assertAlmostEqual(c["metrics"]["frame_ms"]["median"], 10.0)
        self.assertEqual(c["metrics"]["cpu_render_ms"]["runs"], 2)
        # The CPU-bound check uses the GPU-timed run only, against its own frame time.
        self.assertAlmostEqual(c["metrics"]["gpu_ms"]["median"], 4.0)
        self.assertAlmostEqual(c["derived"]["gpu_over_frame"], 0.2)
        self.assertIn("gpu_timing_mixed", [w["code"] for w in result["validation"]["warnings"]])
        row = self.summary()[("nvrhi", "d3d12", "tex_mut", 1)]
        self.assertEqual(row["runs_gpu_timed"], "1")
        self.assertAlmostEqual(float(row["cpu_render_ms"]), 1.6)

    def test_gpu_timed_session_has_no_headline_metrics(self):
        self.session.add_ok("native", "d3d12", "original", 8, 1, gpu=9.0)
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 8, 1, gpu=2.0)
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "tex_mut", 8)]
        self.assertEqual(c["status"], "ok")
        self.assertIsNone(c["metrics"]["cpu_render_ms"])
        self.assertIsNone(c["metrics"]["frame_ms"])
        self.assertIsNone(c["derived"]["overhead_vs_native_pct"])
        self.assertAlmostEqual(c["derived"]["gpu_over_frame"], 0.2)
        self.assertEqual(result["validation"]["warnings"], [])
        rows = {(r["renderer"], r["api"]): r for r in self.table("cpu_bound_check.csv")}
        self.assertAlmostEqual(float(rows[("nvrhi", "d3d12")]["gpu_over_frame"]), 0.2)
        self.assertEqual(rows[("nvrhi", "d3d12")]["suspect_gpu_bound"], "")
        self.assertIn("90 %", rows[("native", "d3d12")]["suspect_gpu_bound"])

    def test_cpu_bound_check_from_separate_sessions(self):
        self.session.add_ok("native", "d3d12", "original", 8, 1, duration=2.0)             # 10 ms frames
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 8, 1, duration=2.0)
        gpu = type(self.session)(os.path.join(self.tmp, "gpu"))
        half = type(self.session)(os.path.join(self.tmp, "half"))
        for aux in (gpu, half):
            os.makedirs(aux.root)
        gpu.add_ok("native", "d3d12", "original", 8, 1, gpu=9.5)
        gpu.add_ok("nvrhi", "d3d12", "tex_mut", 8, 1, gpu=3.0)
        half.add_ok("native", "d3d12", "original", 8, 1, duration=1.0, width=540, height=360)   # 5 ms frames
        half.add_ok("nvrhi", "d3d12", "tex_mut", 8, 1, duration=2.0, width=540, height=360)
        for aux in (gpu, half):
            aux.write_manifest()
        code, result = self.run_analysis(gpu_session=gpu.root, half_res_session=half.root)
        self.assertEqual(code, 0, result["log"])
        self.assertIsNone(result["by_key"][("nvrhi", "d3d12", "tex_mut", 8)]["metrics"]["gpu_ms"])
        rows = {(r["renderer"], r["api"]): r for r in self.table("cpu_bound_check.csv")}
        ok, bad = rows[("nvrhi", "d3d12")], rows[("native", "d3d12")]
        self.assertAlmostEqual(float(ok["gpu_over_frame"]), 0.3)
        self.assertAlmostEqual(float(ok["half_res_over_full_frame"]), 1.0)
        self.assertEqual(ok["suspect_gpu_bound"], "")
        self.assertAlmostEqual(float(bad["gpu_over_frame"]), 0.95)
        self.assertAlmostEqual(float(bad["half_res_over_full_frame"]), 0.5)
        self.assertIn("gpu_ms is 95 %", bad["suspect_gpu_bound"])
        self.assertIn("50 % at half resolution", bad["suspect_gpu_bound"])

    def test_no_cpu_bound_table_without_gpu_data(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        self.run_analysis()
        self.assertFalse(os.path.exists(os.path.join(self.out, "data", "cpu_bound_check.csv")))


class SensitivityTests(SessionCase):
    def build(self):
        s = self.session
        for rep, render in ((1, 1.5), (2, 1.5)):
            s.add_ok("nvrhi", "d3d12", "tex_mut", 1, rep, render=render, submit=0.5, gc=0.5)               # 2.0, +gc 2.5
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=2.5, submit=0.5, gc=0.5, variant="always_set_state")   # 3.0
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=1.5, submit=0.5, gc=1.5, variant="track_liveness")     # 2.0, +gc 3.5
        s.add_ok("nvrhi", "vk", "tex_mut", 8, 1, render=0.5, submit=0.5, variant="no_auto_barriers")              # no default run
        return self.run_analysis()

    def test_variants_never_enter_the_main_matrix(self):
        code, result = self.build()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]
        self.assertEqual(c["runs_ok"], 2)
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["median"], 2.0)
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["max"], 2.0)
        self.assertNotIn(("nvrhi", "vk", "tex_mut", 8), result["by_key"])
        self.assertEqual(set(self.summary()), {("nvrhi", "d3d12", "tex_mut", 1)})
        self.assertEqual(sorted(v["variant"] for v in result["sensitivity"]),
                         ["always_set_state", "no_auto_barriers", "track_liveness"])
        self.assertFalse(any(v["headline"] for v in result["sensitivity"]))

    def test_sensitivity_table(self):
        code, result = self.build()
        rows = {(r["api"], int(r["threads"]), r["variant"]): r for r in self.table("sensitivity.csv")}
        self.assertEqual(set(rows), {("d3d12", 1, "default"), ("d3d12", 1, "always_set_state"), ("d3d12", 1, "track_liveness"),
                                     ("vk", 8, "no_auto_barriers")})
        self.assertAlmostEqual(float(rows[("d3d12", 1, "default")]["cpu_render_ms"]), 2.0)
        self.assertEqual(rows[("d3d12", 1, "default")]["variant_over_default_cpu_render"], "")
        self.assertAlmostEqual(float(rows[("d3d12", 1, "always_set_state")]["variant_over_default_cpu_render"]), 1.5)
        self.assertAlmostEqual(float(rows[("d3d12", 1, "track_liveness")]["variant_over_default_cpu_render"]), 1.0)
        self.assertAlmostEqual(float(rows[("d3d12", 1, "track_liveness")]["variant_over_default_render_gc"]), 1.4)
        self.assertEqual(rows[("vk", 8, "no_auto_barriers")]["variant_over_default_cpu_render"], "")

    def test_no_sensitivity_table_without_variants(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        code, result = self.run_analysis()
        self.assertEqual(result["sensitivity"], [])
        self.assertFalse(os.path.exists(os.path.join(self.out, "data", "sensitivity.csv")))

    def test_switch_in_the_wrong_directory_is_rejected(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        # a run of the main matrix that was really taken with a switch, and a variant directory without it
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, extra={"always_set_state": "true", "track_liveness": "false"})
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 8, 1, variant="no_auto_barriers", extra={"no_auto_barriers": "false"})
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertEqual([i["code"] for i in result["validation"]["errors"]], ["variant_mismatch", "variant_mismatch"])
        self.assertEqual(result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]["runs_ok"], 0)


class FailedRunTests(SessionCase):
    def test_failed_run_is_excluded_but_reported(self):
        self.session.add_ok("nvrhi", "d3d12", "mut", 1, 1, render=1.0, submit=1.0)
        self.session.add_failed("nvrhi", "d3d12", "mut", 1, 2, reason="device removed")
        self.session.add_ok("nvrhi", "d3d12", "mut", 1, 3, render=2.0, submit=1.0)
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "mut", 1)]
        self.assertEqual((c["runs_ok"], c["runs_failed"]), (2, 1))
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["median"], 2.5)
        not_ok = result["validation"]["runs_not_ok"]
        self.assertEqual(len(not_ok), 1)
        self.assertEqual(not_ok[0]["status"], "failed")
        self.assertIn("device removed", not_ok[0]["reason"])
        self.assertIn("device removed", result["log"])
        with open(os.path.join(self.out, "data", "validation.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["run_counts"], {"ok": 2, "failed": 1})

    def test_status_is_ok_only_with_a_majority_of_valid_repetitions(self):
        s = self.session
        s.add_ok("native", "d3d12", "original", 1, 1)
        for rep in (1, 2, 3):
            s.add_ok("nvrhi", "d3d12", "mut", 1, rep)
        for rep in (4, 5):
            s.add_failed("nvrhi", "d3d12", "mut", 1, rep)                          # 3 of 5: ok
        s.add_ok("nvrhi", "d3d12", "mut", 2, 1, render=3.0)
        s.add_failed("nvrhi", "d3d12", "mut", 2, 2, reason="device removed")
        s.add_manifest_only("nvrhi", "d3d12", "mut", 2, 3, status="pending", reason="")   # 1 of 3: partial
        s.add_ok("nvrhi", "d3d12", "mut", 4, 1)
        s.add_failed("nvrhi", "d3d12", "mut", 4, 2)                                # 1 of 2: not a majority
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        by_key = result["by_key"]
        self.assertEqual(by_key[("nvrhi", "d3d12", "mut", 1)]["status"], "ok")
        partial = by_key[("nvrhi", "d3d12", "mut", 2)]
        self.assertEqual((partial["status"], partial["runs_scheduled"], partial["runs_ok"]), ("partial", 3, 1))
        self.assertAlmostEqual(partial["metrics"]["cpu_render_ms"]["median"], 3.5)   # the valid run is still reported
        self.assertEqual(by_key[("nvrhi", "d3d12", "mut", 4)]["status"], "partial")
        row = self.summary()[("nvrhi", "d3d12", "mut", 2)]
        self.assertEqual((row["status"], row["runs_scheduled"], row["reason"]), ("partial", "3", "device removed"))
        self.assertEqual(result["validation"]["config_status_counts"], {"ok": 2, "partial": 2})

    def test_strict_turns_failed_runs_into_an_error(self):
        self.session.add_ok("nvrhi", "d3d12", "mut", 1, 1)
        self.session.add_failed("nvrhi", "d3d12", "mut", 1, 2)
        self.assertEqual(self.run_analysis(strict=True)[0], 1)

    def test_configuration_with_only_failed_runs_stays_in_the_summary(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        self.session.add_failed("nvrhi", "vk", "mut", 4, 1, reason="out of descriptors")
        code, result = self.run_analysis()
        self.assertEqual(code, 0)
        row = self.summary()[("nvrhi", "vk", "mut", 4)]
        self.assertEqual(row["status"], "failed")
        self.assertEqual(row["cpu_render_ms"], "")
        self.assertEqual(row["runs_failed"], "1")
        self.assertEqual(row["reason"], "out of descriptors")

    def test_unsupported_is_not_applicable_not_failed(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        self.session.add_failed("nvrhi", "d3d12", "dyn", 1, 1, kind="unsupported", reason="dyn is not expressible in nvrhi")
        code, result = self.run_analysis(strict=True)
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "dyn", 1)]
        self.assertEqual(c["status"], "n/a")
        self.assertEqual((c["runs_failed"], c["runs_unsupported"]), (0, 1))

    def test_timeout_without_run_json_comes_from_the_manifest(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        self.session.add_manifest_only("nvrhi", "vk", "bindless", 15, 1, status="timeout", reason="killed after 95 s")
        self.session.add_manifest_only("nvrhi", "vk", "bindless", 15, 2, status="pending", reason="")
        code, result = self.run_analysis()
        self.assertEqual(code, 0)
        c = result["by_key"][("nvrhi", "vk", "bindless", 15)]
        self.assertEqual((c["runs_ok"], c["runs_failed"], c["runs_missing"]), (0, 1, 1))
        self.assertEqual(c["status"], "failed")
        self.assertIn("timeout: killed after 95 s", c["reason"])
        self.assertEqual(self.run_analysis(strict=True)[0], 1)

    def test_run_json_deleted_after_an_ok_manifest_entry_is_missing(self):
        path = self.session.add_ok("native", "d3d12", "original", 1, 1)
        self.session.add_ok("native", "d3d12", "original", 1, 2)
        os.remove(os.path.join(path, "run.json"))
        _, result = self.run_analysis()
        statuses = sorted(r.status for r in result["runs"])
        self.assertEqual(statuses, ["missing", "ok"])

    def test_no_valid_run_is_an_error(self):
        self.session.add_failed("nvrhi", "d3d12", "mut", 1, 1)
        self.assertEqual(self.run_analysis()[0], 1)

    def test_session_without_manifest(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        self.session.add_failed("nvrhi", "d3d12", "mut", 1, 1)
        log = io.StringIO()
        code, result = analyze.analyze(self.session.root, self.out, charts=False, log=lambda s: log.write(s))
        self.assertEqual(code, 0)
        self.assertEqual(sorted(r.status for r in result["runs"]), ["failed", "ok"])

    def test_missing_session_directory(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(analyze.main([os.path.join(self.tmp, "nope"), "--out", self.out, "--no-charts"]), 2)


class ValidationTests(SessionCase):
    def codes(self, result):
        return [i["code"] for i in result["validation"]["errors"]]

    def test_scene_hash_mismatch_is_detected(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1, render=1.5, submit=0.5)
        self.session.add_ok("nvrhi", "vk", "tex_mut", 1, 1, render=2.5, submit=0.5)
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=1.7, submit=0.5, scene_hash="0x0000000000000bad")
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertIn("scene_hash_mismatch", self.codes(result))
        self.assertEqual(result["validation"]["reference_static_scene_hash"], GOOD_HASH)
        bad = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]
        self.assertEqual((bad["runs_ok"], bad["runs_invalid"], bad["status"]), (0, 1, "failed"))
        self.assertIsNone(bad["metrics"]["cpu_render_ms"])
        self.assertEqual(result["by_key"][("nvrhi", "vk", "tex_mut", 1)]["runs_ok"], 1)
        self.assertIn("0x0000000000000bad", result["log"])

    def test_reference_hash_is_native_d3d12_even_when_outnumbered(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1, scene_hash="0x000000000000000a")
        for rep in (1, 2, 3):
            self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, rep, scene_hash="0x000000000000000b")
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertEqual(result["validation"]["reference_static_scene_hash"], "0x000000000000000a")
        self.assertEqual(result["by_key"][("native", "d3d12", "original", 1)]["runs_ok"], 1)
        self.assertEqual(result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]["runs_invalid"], 3)

    def test_identical_hashes_pass(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        self.session.add_ok("nvrhi", "vk", "tex_mut", 1, 1)
        code, result = self.run_analysis()
        self.assertEqual(code, 0)
        self.assertEqual(self.codes(result), [])

    def test_frame_count_must_match_run_json(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1, frame_count=500)
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertIn("frame_count_mismatch", self.codes(result))

    def test_too_few_frames(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1, frames=5)
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertIn("too_few_frames", self.codes(result))

    def test_frames_must_cover_the_measured_duration(self):
        path = self.session.add_ok("native", "d3d12", "original", 1, 1, frames=200, duration=2.0)
        with open(os.path.join(path, "run.json"), encoding="utf-8") as f:
            meta = json.load(f)
        meta["duration_seconds"] = 20.0  # the frames only add up to 2 s
        with open(os.path.join(path, "run.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f)
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertIn("duration_implausible", self.codes(result))

    def test_truncated_frames_csv(self):
        path = self.session.add_ok("native", "d3d12", "original", 1, 1)
        with open(os.path.join(path, "frames.csv"), "a", encoding="utf-8") as f:
            f.write("300,0.25,1.0\n")
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertIn("frames_unreadable", self.codes(result))

    def test_missing_frames_csv_and_bad_header(self):
        a = self.session.add_ok("native", "d3d12", "original", 1, 1)
        b = self.session.add_ok("native", "d3d12", "original", 1, 2)
        self.session.add_ok("native", "d3d12", "original", 1, 3)
        os.remove(os.path.join(a, "frames.csv"))
        with open(os.path.join(b, "frames.csv"), "w", encoding="utf-8") as f:
            f.write("frame,render_ms\n1,2\n")
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertEqual(sorted(self.codes(result)), ["frames_missing", "frames_unreadable"])
        self.assertEqual(result["by_key"][("native", "d3d12", "original", 1)]["runs_ok"], 1)

    def test_contract_violations_in_run_json(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        self.session.add_ok("nvrhi", "d3d12", "mut", 8, 1, used_threads=4)
        self.session.add_ok("nvrhi", "d3d12", "mut", 8, 2, build="debug")
        self.session.add_ok("nvrhi", "d3d12", "mut", 8, 3, validation=True)
        self.session.add_ok("nvrhi", "d3d12", "mut", 8, 4, adapter="AMD Radeon 780M Graphics")
        self.session.add_ok("nvrhi", "d3d12", "mut", 8, 5, width=540, height=360)
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertEqual(sorted(self.codes(result)),
                         ["adapter_mismatch", "not_release", "resolution_mismatch", "thread_mismatch", "validation_on"])
        self.assertEqual(result["by_key"][("nvrhi", "d3d12", "mut", 8)]["runs_invalid"], 5)

    def test_run_json_in_the_wrong_directory(self):
        path = self.session.add_ok("nvrhi", "d3d12", "mut", 1, 1)
        with open(os.path.join(path, "run.json"), encoding="utf-8") as f:
            meta = json.load(f)
        meta["api"] = "vk"
        with open(os.path.join(path, "run.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f)
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertIn("config_mismatch", self.codes(result))

    def test_bom_and_non_run_directories_are_tolerated(self):
        path = self.session.add_ok("native", "d3d12", "original", 1, 1)
        with open(os.path.join(path, "run.json"), encoding="utf-8") as f:
            text = f.read()
        with open(os.path.join(path, "run.json"), "w", encoding="utf-8-sig") as f:
            f.write(text)
        os.makedirs(os.path.join(self.session.root, "profiles", "x"))
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        self.assertEqual(len(result["runs"]), 1)


HZ = {"display_refresh_hz": "100"}   # refresh period 10 ms: a synthetic run of 200 frames in 2 s sits exactly on it


class PacingTests(SessionCase):
    def test_pacing_multiple(self):
        f = analyze.pacing_multiple
        period = 1000.0 / 120.0
        self.assertEqual(f(period, 5.5, 120), 1)
        self.assertEqual(f(period * 1.019, 5.5, 120), 1)          # within 2 %
        self.assertIsNone(f(period * 1.03, 5.5, 120))             # outside
        self.assertEqual(f(period / 2, 1.0, 120), 0.5)            # two frames per refresh
        self.assertEqual(f(period * 2, 3.0, 120), 2)
        self.assertIsNone(f(period, 8.2, 120))                    # the renderer itself fills the frame
        self.assertIsNone(f(5.8, 5.5, 120))
        self.assertIsNone(f(period, 5.5, None))                   # refresh rate unknown: no verdict
        self.assertIsNone(f(period, 5.5, 0))
        # The slack must be absorbed by the presentation: wait + present falls when the renderer takes longer.
        self.assertEqual(f(period, 5.5, 120, 2.8, -0.8), 1)
        self.assertIsNone(f(period, 7.8, 120, 0.4, 0.15))         # a renderer that happens to need 8.3 ms per frame
        self.assertIsNone(f(period, 5.5, 120, 2.8, 0.0))
        self.assertEqual(f(period, 5.5, 120, 2.8, None), 1)       # constant columns: the share of the slack decides
        self.assertIsNone(f(period, 5.5, 120, 0.4, None))

    def test_correlation(self):
        self.assertAlmostEqual(analyze.correlation([1, 2, 3, 4], [8, 6, 4, 2]), -1.0)
        self.assertAlmostEqual(analyze.correlation([1, 2, 3, 4], [1, 2, 3, 4]), 1.0)
        self.assertIsNone(analyze.correlation([1, 1, 1, 1], [1, 2, 3, 4]))
        self.assertIsNone(analyze.correlation([1, 2], [2, 1]))

    def test_frame_on_the_refresh_period_without_absorbed_slack_is_not_paced(self):
        # 10 ms frames at 100 Hz, 8.75 ms busy, and a Present that costs the same 0.8 ms whatever the renderer does.
        rows = [(8.0 + 0.1 * (i % 3), 0.4) for i in range(200)]
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, rows=rows, present=0.8, extra=dict(HZ))
        code, result = self.run_analysis()
        self.assertEqual(result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]["runs_paced"], 0)
        self.assertEqual(result["validation"]["warnings"], [])

    def build(self):
        s = self.session
        # Vulkan tex_mut: two paced runs (10 ms frames at 100 Hz) and one unpaced run (7 ms frames).
        s.add_ok("nvrhi", "vk", "tex_mut", 1, 1, render=3.0, submit=0.5, present=6.0, extra=dict(HZ))
        s.add_ok("nvrhi", "vk", "tex_mut", 1, 2, render=3.2, submit=0.5, present=6.0, extra=dict(HZ))
        s.add_ok("nvrhi", "vk", "tex_mut", 1, 3, render=3.4, submit=0.5, present=0.1, duration=1.4, extra=dict(HZ))
        # Vulkan mut: never paced (12 ms frames).
        for rep in (1, 2):
            s.add_ok("nvrhi", "vk", "mut", 1, rep, render=7.0, submit=0.5, duration=2.4, extra=dict(HZ))
        return self.run_analysis()

    def test_paced_runs_leave_the_frame_metrics_but_not_render_submit(self):
        code, result = self.build()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "vk", "tex_mut", 1)]
        self.assertEqual((c["runs_ok"], c["runs_paced"], len(c["unpaced_runs"])), (3, 2, 1))
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["median"], 3.7)      # all three runs
        self.assertEqual(c["metrics"]["cpu_render_ms"]["runs"], 3)
        self.assertAlmostEqual(c["metrics"]["frame_ms"]["median"], 7.0)           # the unpaced run only
        self.assertEqual(c["metrics"]["frame_ms"]["runs"], 1)
        self.assertAlmostEqual(c["metrics"]["present_ms"]["median"], 0.1)
        self.assertAlmostEqual(c["metrics"]["cpu_render_present_ms"]["median"], 4.0)
        self.assertEqual(c["metrics"]["wait_ms"]["runs"], 1)
        n = result["by_key"][("nvrhi", "vk", "mut", 1)]
        self.assertEqual(n["runs_paced"], 0)
        self.assertAlmostEqual(n["metrics"]["frame_ms"]["median"], 12.0)
        self.assertEqual([w["code"] for w in result["validation"]["warnings"]], ["paced", "paced"])
        self.assertEqual(result["validation"]["paced_runs"], 2)
        self.assertEqual(self.summary()[("nvrhi", "vk", "tex_mut", 1)]["runs_paced"], "2")
        runs = {(r["config"], r["rep"]): r for r in self.table("runs.csv")}
        self.assertEqual(runs[("nvrhi_vk_tex_mut_t01", "rep1")]["paced"], "1")
        self.assertEqual(runs[("nvrhi_vk_tex_mut_t01", "rep1")]["pacing_multiple"], "1")
        self.assertEqual(runs[("nvrhi_vk_tex_mut_t01", "rep3")]["paced"], "0")
        self.assertEqual(runs[("nvrhi_vk_mut_t01", "rep1")]["paced"], "0")

    def test_configuration_with_only_paced_runs_has_no_frame_metrics(self):
        s = self.session
        s.add_ok("nvrhi", "vk", "tex_mut", 8, 1, render=1.0, submit=0.2, present=8.4, extra=dict(HZ))
        s.add_ok("nvrhi", "vk", "mut", 8, 1, render=2.0, submit=0.4, duration=0.6, extra=dict(HZ))   # 3 ms frames
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "vk", "tex_mut", 8)]
        self.assertEqual(c["status"], "ok")
        self.assertIsNone(c["metrics"]["frame_ms"])
        self.assertIsNone(c["metrics"]["present_ms"])
        self.assertIsNone(c["metrics"]["cpu_render_present_ms"])
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["median"], 1.2)
        self.assertAlmostEqual(result["by_key"][("nvrhi", "vk", "mut", 8)]["metrics"]["frame_ms"]["median"], 3.0)
        self.assertIn("paced_config", [w["code"] for w in result["validation"]["warnings"]])
        self.assertEqual(self.summary()[("nvrhi", "vk", "tex_mut", 8)]["frame_ms"], "")

    def test_frame_on_the_refresh_period_but_filled_by_the_renderer_is_not_paced(self):
        self.session.add_ok("nvrhi", "vk", "mut", 1, 1, render=9.3, submit=0.3, extra=dict(HZ))   # busy 9.85 of 10 ms
        code, result = self.run_analysis()
        c = result["by_key"][("nvrhi", "vk", "mut", 1)]
        self.assertEqual(c["runs_paced"], 0)
        self.assertAlmostEqual(c["metrics"]["frame_ms"]["median"], 10.0)
        self.assertEqual(result["validation"]["warnings"], [])

    def test_no_verdict_without_a_refresh_rate_unless_given(self):
        self.session.add_ok("nvrhi", "vk", "tex_mut", 1, 1, present=8.0)  # 10 ms frames, no display_refresh_hz
        code, result = self.run_analysis()
        self.assertEqual(result["by_key"][("nvrhi", "vk", "tex_mut", 1)]["runs_paced"], 0)
        shutil.rmtree(self.out, True)
        code, result = self.run_analysis(refresh_hz=100.0)
        self.assertEqual(result["by_key"][("nvrhi", "vk", "tex_mut", 1)]["runs_paced"], 1)
        self.assertIsNone(result["by_key"][("nvrhi", "vk", "tex_mut", 1)]["metrics"]["frame_ms"])

    def test_paced_runs_are_excluded_from_the_cpu_bound_check(self):
        self.session.add_ok("nvrhi", "vk", "tex_mut", 8, 1, present=8.0, extra=dict(HZ))            # paced
        self.session.add_ok("nvrhi", "vk", "mut", 8, 1, duration=1.4, extra=dict(HZ))               # 7 ms frames
        half = type(self.session)(os.path.join(self.tmp, "half"))
        os.makedirs(half.root)
        half.add_ok("nvrhi", "vk", "tex_mut", 8, 1, present=8.0, width=540, height=360, extra=dict(HZ))   # paced as well
        half.add_ok("nvrhi", "vk", "mut", 8, 1, duration=1.4, width=540, height=360, extra=dict(HZ))
        half.write_manifest()
        code, result = self.run_analysis(half_res_session=half.root)
        self.assertEqual(code, 0, result["log"])
        rows = {(r["renderer"], r["api"], r["binding"]): r for r in self.table("cpu_bound_check.csv")}
        self.assertEqual(rows[("nvrhi", "vk", "tex_mut")]["frame_ms"], "")
        self.assertEqual(rows[("nvrhi", "vk", "tex_mut")]["half_res_over_full_frame"], "")
        self.assertIn("paced or not displayed", rows[("nvrhi", "vk", "tex_mut")]["suspect_gpu_bound"])
        self.assertIn("half-resolution runs", rows[("nvrhi", "vk", "tex_mut")]["suspect_gpu_bound"])
        self.assertAlmostEqual(float(rows[("nvrhi", "vk", "mut")]["half_res_over_full_frame"]), 1.0)
        self.assertEqual(rows[("nvrhi", "vk", "mut")]["suspect_gpu_bound"], "")

    def test_gpu_timed_paced_run_gives_no_gpu_share(self):
        self.session.add_ok("nvrhi", "vk", "tex_mut", 8, 1, gpu=2.0, present=8.0, extra=dict(HZ))      # paced, GPU-timed
        code, result = self.run_analysis()
        c = result["by_key"][("nvrhi", "vk", "tex_mut", 8)]
        self.assertAlmostEqual(c["metrics"]["gpu_ms"]["median"], 2.0)
        self.assertIsNone(c["derived"]["gpu_over_frame"])

    def test_window_hidden_unlike_the_rest_of_the_session(self):
        s = self.session
        shown = {"window_visible_fraction": "1.00"}
        for rep in (1, 2):
            s.add_ok("native", "d3d12", "original", 8, rep, extra=dict(shown))
        s.add_ok("native", "d3d12", "original", 8, 3, duration=1.0, extra={"window_visible_fraction": "0.00"})   # 5 ms frames
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("native", "d3d12", "original", 8)]
        self.assertEqual((c["runs_ok"], c["runs_pacing_excluded"], c["runs_paced"]), (3, 1, 0))
        self.assertEqual(c["metrics"]["frame_ms"]["runs"], 2)
        self.assertAlmostEqual(c["metrics"]["frame_ms"]["median"], 10.0)
        self.assertEqual(c["metrics"]["cpu_render_ms"]["runs"], 3)
        self.assertEqual([w["code"] for w in result["validation"]["warnings"]], ["window_visibility_differs"])

    def test_covered_session_is_consistent(self):
        for rep in (1, 2):
            self.session.add_ok("native", "d3d12", "original", 8, rep, extra={"window_visible_fraction": "0.00"})
        code, result = self.run_analysis()
        c = result["by_key"][("native", "d3d12", "original", 8)]
        self.assertEqual(c["runs_pacing_excluded"], 0)
        self.assertEqual(result["validation"]["warnings"], [])


class CbPageSplitTests(SessionCase):
    @staticmethod
    def extra(split, lists=None, **more):
        e = {"cb_page_split": "true" if split else "false",
             "cb_page_split_command_lists": str(lists if lists is not None else (1 if split else 0))}
        e.update(more)
        return e

    def build(self):
        s = self.session
        for rep, render in ((1, 5.0), (2, 5.2), (3, 5.4)):
            s.add_ok("nvrhi", "d3d12", "tex_mut", 1, rep, render=render, submit=0.5, extra=self.extra(False))
        for rep, render in ((4, 9.0), (5, 9.4)):
            s.add_ok("nvrhi", "d3d12", "tex_mut", 1, rep, render=render, submit=0.5, extra=self.extra(True))
        return self.run_analysis()

    def test_populations_are_not_pooled(self):
        code, result = self.build()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]
        self.assertEqual((c["runs_ok"], c["runs_cb_page_split"], len(c["runs"])), (5, 2, 3))
        self.assertEqual(c["status"], "ok")
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["median"], 5.7)       # unaffected runs only
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["max"], 5.9)
        self.assertEqual(c["population"], "cb_page_split=false (3 of 5 runs)")
        self.assertIn("cb_page_split_mixed", [w["code"] for w in result["validation"]["warnings"]])
        row = self.summary()[("nvrhi", "d3d12", "tex_mut", 1)]
        self.assertEqual(row["runs_cb_page_split"], "2")
        self.assertAlmostEqual(float(row["cb_page_split_cpu_render_ms"]), 9.7)
        self.assertAlmostEqual(float(row["cb_page_split_over_unaffected"]), 9.7 / 5.7, places=4)
        table = self.table("cb_page_split.csv")
        self.assertEqual(len(table), 1)
        self.assertEqual((table[0]["runs_unaffected"], table[0]["runs_affected"], table[0]["variant"]), ("3", "2", "default"))
        self.assertAlmostEqual(float(table[0]["cpu_render_ms_unaffected"]), 5.7)
        self.assertAlmostEqual(float(table[0]["cpu_render_ms_affected"]), 9.7)
        self.assertAlmostEqual(float(table[0]["cpu_render_ms_affected_max"]), 9.9)
        self.assertAlmostEqual(float(table[0]["affected_over_unaffected"]), 9.7 / 5.7, places=4)
        runs = {(r["config"], r["rep"]): r for r in self.table("runs.csv")}
        self.assertEqual(runs[("nvrhi_d3d12_tex_mut_t01", "rep4")]["cb_page_split"], "true")
        self.assertEqual(runs[("nvrhi_d3d12_tex_mut_t01", "rep1")]["cb_page_split"], "false")

    def test_only_affected_runs(self):
        for rep in (1, 2):
            self.session.add_ok("nvrhi", "d3d12", "tex_mut", 8, rep, render=1.5, submit=0.3, extra=self.extra(True, lists=3))
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        c = result["by_key"][("nvrhi", "d3d12", "tex_mut", 8)]
        self.assertAlmostEqual(c["metrics"]["cpu_render_ms"]["median"], 1.8)
        self.assertEqual(c["population"], "cb_page_split=true (no unaffected run)")
        self.assertEqual([w["code"] for w in result["validation"]["warnings"]], ["cb_page_split_all"])
        table = self.table("cb_page_split.csv")
        self.assertEqual((table[0]["runs_unaffected"], table[0]["affected_command_lists_max"]), ("0", "3"))

    def test_no_table_without_affected_runs(self):
        self.session.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, extra=self.extra(False))
        self.session.add_ok("nvrhi", "vk", "tex_mut", 1, 1, extra={"cb_page_split": "n/a"})
        code, result = self.run_analysis()
        self.assertEqual(code, 0, result["log"])
        self.assertFalse(os.path.exists(os.path.join(self.out, "data", "cb_page_split.csv")))
        self.assertEqual(result["by_key"][("nvrhi", "d3d12", "tex_mut", 1)]["population"], "")
        self.assertEqual(result["validation"]["warnings"], [])

    def test_placement_switches_are_variants(self):
        s = self.session
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=5.0, submit=0.5, extra=self.extra(False))
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=5.0, submit=0.5, variant="cb_page_split_avoid",
                 extra=self.extra(False, cb_page_split_avoid="true", cb_page_split_force="false"))
        s.add_ok("nvrhi", "d3d12", "tex_mut", 1, 1, render=9.0, submit=0.5, variant="cb_page_split_force",
                 extra=self.extra(True, cb_page_split_avoid="false", cb_page_split_force="true"))
        # a forced placement in the directory of the main matrix is rejected
        s.add_ok("nvrhi", "d3d12", "tex_mut", 8, 1, extra=self.extra(True, cb_page_split_force="true"))
        code, result = self.run_analysis()
        self.assertEqual(code, 1)
        self.assertEqual([i["code"] for i in result["validation"]["errors"]], ["variant_mismatch"])
        rows = {r["variant"]: r for r in self.table("sensitivity.csv") if r["threads"] == "1"}
        self.assertEqual(set(rows), {"default", "cb_page_split_avoid", "cb_page_split_force"})
        self.assertAlmostEqual(float(rows["cb_page_split_force"]["variant_over_default_cpu_render"]), 9.5 / 5.5, places=4)
        self.assertAlmostEqual(float(rows["cb_page_split_avoid"]["variant_over_default_cpu_render"]), 1.0)
        table = {r["variant"]: r for r in self.table("cb_page_split.csv")}
        self.assertEqual(set(table), {"cb_page_split_force"})
        # a variant made of affected runs only is what was asked for: no warning
        self.assertEqual(result["validation"]["warnings"], [])


class OutputTests(SessionCase):
    def build_matrix(self):
        cost = {"native": 2.0, "nvrhi": 2.4}
        for threads in (1, 2, 8):
            for api in ("d3d11", "d3d12", "vk"):
                for renderer, bindings in (("native", ["original"]), ("nvrhi", ["mut", "tex_mut", "tex_mut_pc"])):
                    if renderer == "native" and api == "vk":
                        continue
                    for b, binding in enumerate(bindings):
                        for rep in (1, 2):
                            value = cost[renderer] * (1 + 0.1 * b) * (0.9 if api == "vk" else 1.0) / threads + 0.01 * rep
                            self.session.add_ok(renderer, api, binding, threads, rep, render=value * 0.8,
                                                submit=value * 0.2, frames=60)
        self.session.add_failed("nvrhi", "d3d12", "dyn", 1, 1, kind="unsupported", reason="not expressible")

    def test_command_line_writes_summary_and_run_tables(self):
        self.build_matrix()
        self.session.write_manifest()
        with contextlib.redirect_stdout(io.StringIO()):
            code = analyze.main([self.session.root, "--out", self.out, "--no-charts"])
        self.assertEqual(code, 0)
        summary = self.summary()
        self.assertEqual(len(summary), 3 * (2 + 9) + 1)  # per thread count: 2 native, 9 nvrhi; plus the n/a row
        self.assertEqual(summary[("nvrhi", "d3d12", "dyn", 1)]["status"], "n/a")
        with open(os.path.join(self.out, "data", "summary.csv"), newline="", encoding="utf-8") as f:
            header = next(csv.reader(f))
        for column in ("cpu_render_ms", "cpu_render_ms_min", "cpu_render_ms_max", "cpu_render_ms_p1", "cpu_render_ms_p99",
                       "cpu_render_present_ms", "cpu_render_gc_ms", "gc_ms", "frame_ms", "gpu_ms", "runs_scheduled",
                       "runs_gpu_timed", "native_baseline", "native_comparison", "overhead_vs_native_pct",
                       "overhead_vs_native_ns_per_draw", "overhead_vs_native_render_present_pct",
                       "overhead_vs_native_render_gc_pct", "overhead_vs_native_d3d12_cross_api_pct",
                       "overhead_vs_native_d3d12_cross_api_ns_per_draw", "speedup_vs_1_thread",
                       "vk_over_d3d12_cpu_render"):
            self.assertIn(column, header)
        for name in ("layer_overhead_vs_native.csv", "binding_model_cost_vs_native.csv"):
            self.assertTrue(os.path.isfile(os.path.join(self.out, "data", name)), name)
        with open(os.path.join(self.out, "data", "runs.csv"), newline="", encoding="utf-8") as f:
            self.assertEqual(len(list(csv.DictReader(f))), len(self.session.manifest_runs))

    @unittest.skipUnless(HAVE_MATPLOTLIB, "matplotlib is not installed")
    def test_charts_are_written(self):
        self.build_matrix()
        code, result = self.run_analysis(charts=True)
        self.assertEqual(code, 0, result["log"])
        names = sorted(os.path.basename(p) for p in result["charts"])
        self.assertEqual(names, ["binding_modes_t01.png", "binding_modes_t08.png", "frame_time_distribution.png",
                                 "render_cost_by_configuration_t01.png",
                                 "render_cost_by_configuration_t08.png", "thread_scaling.png", "thread_speedup.png",
                                 "vulkan_vs_d3d12.png"])
        for path in result["charts"]:
            self.assertGreater(os.path.getsize(path), 5000, path)

    @unittest.skipIf(HAVE_MATPLOTLIB, "matplotlib is installed")
    def test_missing_matplotlib_is_reported_not_hidden(self):
        self.session.add_ok("native", "d3d12", "original", 1, 1)
        code, result = self.run_analysis(charts=True)
        self.assertEqual(code, 1)
        self.assertIn("charts were NOT written", result["log"])
        self.assertTrue(os.path.isfile(os.path.join(self.out, "data", "summary.csv")))


if __name__ == "__main__":
    unittest.main()
