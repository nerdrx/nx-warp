import unittest
from summarize_pipeline_latency import summarize


def row(event, frame, time, stream=0, wire=None):
    return list(map(str, [event, frame, time, stream] + ([] if wire is None else [wire])))


class PipelineLatency(unittest.TestCase):
    def fixture(self):
        return [row('encode_begin', 10, 1_000_000), row('encode_begin', 12, 5_000_000),
                row('nx_frame_map', 10, 2_000_000, wire=65535),
                row('nx_frame_map', 12, 6_000_000, wire=0),
                row('receive_begin', 65535, 3_000_000), row('blit', 65535, 4_000_000),
                row('receive_begin', 65536, 7_000_000), row('blit', 65536, 8_000_000)]

    def test_dropped_outer_and_wire_wrap(self):
        result = summarize(self.fixture(), 'nx', 0)['stages']
        self.assertEqual(result['blit'], {'count': 2, 'p50_p95_p99_ms': [3, 3, 3]})
        self.assertEqual(result['receive_begin']['p50_p95_p99_ms'], [2, 2, 2])

    def test_min_duplicates_and_stream_isolation(self):
        rows = self.fixture() + [row('blit', 65535, 99_000_000),
            row('encode_begin', 10, 0, 255), row('encode_begin', 10, 0, 1),
            row('nx_frame_map', 10, 1, 1, 100)]
        self.assertEqual(summarize(rows, 'nx', 0), summarize(self.fixture(), 'nx', 0))

    def test_missing_and_conflicting_maps(self):
        changes = [lambda rows: [r for r in rows if r[0] != 'nx_frame_map'],
            lambda rows: rows + [row('receive_begin', 65537, 9_000_000)],
            lambda rows: rows + [row('nx_frame_map', 10, 10_000_000, wire=1)],
            lambda rows: rows + [row('nx_frame_map', 13, 10_000_000, wire=0)]]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                summarize(change(self.fixture()), 'nx', 0)

    def test_hevc_and_warmup(self):
        rows = [row('encode_begin', 1, 1_000_000), row('receive_begin', 1, 2_000_000),
                row('blit', 1, 4_000_000), row('encode_begin', 2, 20_000_000),
                row('receive_begin', 2, 22_000_000), row('blit', 2, 25_000_000)]
        self.assertEqual(summarize(rows, 'hevc', .01)['stages']['blit'],
                         {'count': 1, 'p50_p95_p99_ms': [5, 5, 5]})

    def test_missing_encode_and_negative_duration(self):
        for rows in ([r for r in self.fixture() if r[:2] != ['encode_begin', '10']],
                     self.fixture() + [row('blit', 65535, 0)]):
            with self.assertRaises(ValueError):
                summarize(rows, 'nx', 0)


if __name__ == '__main__':
    unittest.main()
