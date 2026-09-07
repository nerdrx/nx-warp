import hashlib
import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "summarize-vkdec.py"


def load_module():
    spec = importlib.util.spec_from_file_location("summarize_vkdec", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def run_cli(*args: str, input_text: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        input=input_text,
        text=True,
        capture_output=True,
        check=False,
    )


class SummarizeVkdecTests(unittest.TestCase):
    def test_summarize_real_stats_shape_and_deadlines(self):
        module = load_module()
        text = "\n".join(
            [
                "frame 0: 100 B, 2048 tiles (32 tskip, 4 lane group(s), 6 dispatches)  "
                "parse 1.000  submit 2.000  passA 0.000  passW 0.000  passB 0.000  "
                "gpu 0.000  total 1.000 ms",
                "frame 1: 200 B, 1024 tiles (16 tskip, 3 lane group(s), 5 dispatches)  "
                "parse 2.000  submit 3.000  passA 0.000  passW 0.000  passB 0.000  "
                "gpu 0.000  total 4.000 ms",
                "frame 2: 300 B, 1536 tiles (24 tskip, 4 lane group(s), 7 dispatches)  "
                "parse 3.000  submit 4.000  passA 0.000  passW 0.000  passB 0.000  "
                "gpu 0.000  total 8.000 ms",
                "frame 3: 400 B, 2048 tiles (48 tskip, 5 lane group(s), 8 dispatches)  "
                "parse 4.000  submit 5.000  passA 0.000  passW 0.000  passB 0.000  "
                "gpu 0.000  total 12.000 ms",
                "frame 4: 500 B, 2048 tiles (64 tskip, 6 lane group(s), 9 dispatches)  "
                "parse 5.000  submit 6.000  passA 0.000  passW 0.000  passB 0.000  "
                "gpu 0.000  total 20.000 ms",
                "5 frame(s), 1920x1080 yuv420p on RADV",
            ]
        )

        report = module.parse_report(text, warmup_frames=1)

        self.assertEqual(
            report["input_sha256"],
            hashlib.sha256(text.encode("utf-8", "surrogateescape")).hexdigest(),
        )
        self.assertEqual(report["sample_count"], 4)
        self.assertEqual(report["samples_seen"], 5)
        self.assertEqual(report["warmup_frames"], 1)
        self.assertTrue(report["pass_b_includes_pass_w"])
        self.assertFalse(report["timestamps_available"])
        self.assertEqual(
            report["metrics"]["parse_ms"],
            {
                "p50": 3.5,
                "p95": 4.85,
                "p99": 4.97,
                "available_samples": 4,
                "unavailable_samples": 0,
            },
        )
        self.assertEqual(
            report["metrics"]["submit_ms"],
            {
                "p50": 4.5,
                "p95": 5.85,
                "p99": 5.97,
                "available_samples": 4,
                "unavailable_samples": 0,
            },
        )
        self.assertEqual(
            report["metrics"]["total_ms"],
            {
                "p50": 10.0,
                "p95": 18.8,
                "p99": 19.76,
                "available_samples": 4,
                "unavailable_samples": 0,
            },
        )
        self.assertEqual(
            report["metrics"]["pass_a_ms"],
            {
                "p50": None,
                "p95": None,
                "p99": None,
                "available_samples": 0,
                "unavailable_samples": 4,
            },
        )
        self.assertEqual(
            report["counts"]["tiles"],
            {
                "p50": 1792.0,
                "p95": 2048.0,
                "p99": 2048.0,
                "available_samples": 4,
                "unavailable_samples": 0,
            },
        )
        self.assertEqual(
            report["counts"]["dispatches"],
            {
                "p50": 7.5,
                "p95": 8.85,
                "p99": 8.97,
                "available_samples": 4,
                "unavailable_samples": 0,
            },
        )
        self.assertEqual(
            report["deadline_miss_fraction_decoder_only"],
            {"90": 0.5, "120": 0.5, "144": 0.75, "180": 0.75, "240": 0.75},
        )
        self.assertEqual(
            report["unmeasured"],
            {
                "occupancy": None,
                "memory_traffic": None,
                "thermal": None,
                "quality": None,
                "end_to_end": None,
            },
        )
        self.assertEqual(
            report["warnings"],
            [
                "passA includes upload+atlas compose; timestamp 0 is before upload/compose and timestamp 1 is after entropy.",
                "nxvc-vkdec passB includes passW; do not add them together.",
            ],
        )

    def test_zero_gpu_row_keeps_host_metrics_and_excludes_gpu_metrics(self):
        module = load_module()
        text = "\n".join(
            [
                "frame 0: 1 B, 1 tiles (0 tskip, 1 lane group(s), 1 dispatches)  "
                "parse 1.0  submit 2.0  passA 9.0  passW 8.0  passB 7.0  "
                "gpu 0.0  total 3.0 ms",
                "[timing] frame 0 GPU timestamps unavailable; zero is not a performance result",
                "frame 1: 2 B, 2 tiles (0 tskip, 1 lane group(s), 2 dispatches)  "
                "parse 4.0  submit 5.0  passA 6.0  passW 1.0  passB 3.0  "
                "gpu 10.0  total 12.0 ms",
            ]
        )

        report = module.parse_report(text)

        self.assertEqual(report["sample_count"], 2)
        self.assertEqual(report["metrics"]["parse_ms"]["p50"], 2.5)
        self.assertEqual(report["metrics"]["submit_ms"]["p50"], 3.5)
        self.assertEqual(report["metrics"]["total_ms"]["p50"], 7.5)
        for name, value in {
            "pass_a_ms": 6.0,
            "pass_w_ms": 1.0,
            "pass_b_ms": 3.0,
            "gpu_ms": 10.0,
        }.items():
            self.assertEqual(report["metrics"][name]["p50"], value)
            self.assertEqual(report["metrics"][name]["available_samples"], 1)
            self.assertEqual(report["metrics"][name]["unavailable_samples"], 1)
        self.assertTrue(report["timestamps_available"])
        self.assertEqual(
            report["input_sha256"],
            hashlib.sha256(text.encode("utf-8", "surrogateescape")).hexdigest(),
        )

    def test_timing_warning_lines_are_accepted_by_cli(self):
        text = "\n".join(
            [
                "frame 0: 1 B, 2 tiles (0 tskip, 1 lane group(s), 2 dispatches)  "
                "parse 0.1  submit 0.2  passA 0.3  passW 0.4  passB 0.5  "
                "gpu 0.6  total 0.7 ms",
                "[timing] frame 0 GPU timestamps unavailable; zero is not a performance result",
            ]
        )
        proc = run_cli(input_text=text)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        report = json.loads(proc.stdout)
        self.assertEqual(
            report["input_sha256"],
            hashlib.sha256(text.encode("utf-8", "surrogateescape")).hexdigest(),
        )

    def test_rejects_bad_logs(self):
        module = load_module()
        cases = [
            (
                "\n".join(
                    [
                        "frame 0: 1 B, 1 tiles (0 tskip, 1 lane group(s), 2 dispatches)  "
                        "parse 0.1  submit 0.2  passA 0.3  passW 0.4  passB 0.5  "
                        "gpu 0.6  total 0.7 ms",
                        "frame 0: 2 B, 1 tiles (0 tskip, 1 lane group(s), 2 dispatches)  "
                        "parse 0.1  submit 0.2  passA 0.3  passW 0.4  passB 0.5  "
                        "gpu 0.6  total 0.7 ms",
                    ]
                ),
                "frame id reset/duplicate",
            ),
            ("frame 1: bad frame length 123", "frame 1: bad frame length 123"),
            ("benchmark_exit=2", "benchmark_exit=2"),
            ("5 frame(s), 1920x1080 yuv420p on RADV", "no samples"),
        ]
        for text, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(ValueError, expected):
                    module.parse_report(text)

    def test_cli_emits_json(self):
        path = Path(self._testMethodName + ".log")
        try:
            path.write_text(
                "\n".join(
                    [
                        "frame 0: 1 B, 2 tiles (0 tskip, 1 lane group(s), 2 dispatches)  "
                        "parse 0.1  submit 0.2  passA 0.0  passW 0.0  passB 0.0  "
                        "gpu 0.0  total 1.0 ms",
                        "1 frame(s), 100x50 yuv420p on lavapipe",
                    ]
                )
            )
            proc = run_cli(str(path), input_text="")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            data = json.loads(proc.stdout)
            self.assertEqual(data["sample_count"], 1)
            self.assertFalse(data["timestamps_available"])
        finally:
            if path.exists():
                path.unlink()


if __name__ == "__main__":
    unittest.main()
