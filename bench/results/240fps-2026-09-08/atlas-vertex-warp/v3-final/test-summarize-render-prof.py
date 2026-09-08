#!/usr/bin/env python3
import importlib.util, json, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).parent
spec = importlib.util.spec_from_file_location("render_prof", HERE / "summarize-render-prof.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

class ParserTest(unittest.TestCase):
    def test_cross_timestamp_duplicate_and_incomplete(self):
        lines = [
            "[2026-01-01 00:00:00.000] render: 10 iterations in 2.0 s (5/s), 9 new-source,",
            "[2026-01-01 00:00:00.200] render: this app's own GPU pass 4.0 ms per iteration",
            "[2026-01-01 00:00:00.400] render: defoveate 2160x2160 per eye x2 = 9.33 Mpx/frame ; 1 re-presented from the cache",
            "[2026-01-01 00:00:00.500] render: this app's own GPU pass 4.1 ms per iteration",
            "[2026-01-01 00:00:02.000] render: 8 iterations in 2.0 s (4/s), 8 new-source,",
            "[2026-01-01 00:00:02.200] render: this app's own GPU pass 3.0 ms per iteration",
        ]
        with tempfile.NamedTemporaryFile("w", suffix=".log") as f:
            f.write("\n".join(lines)); f.flush()
            got = mod.parse(Path(f.name), warmup=0)
        self.assertEqual(got["window_count"], 1)
        self.assertEqual(got["incomplete_blocks"], 1)
        self.assertEqual(got["windows"][0]["cache_hits"], 1)
        self.assertEqual(got["windows"][0]["gpu_ms_per_iteration"], 4.0)

if __name__ == "__main__":
    unittest.main()
