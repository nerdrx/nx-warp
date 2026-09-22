import unittest
import numpy as np
import direct_blocks as codec


class DirectBlocksTest(unittest.TestCase):
    def test_packed_sampling_matches_reference(self):
        rng = np.random.default_rng(73)
        # Stereo fixture exercises both eyes, tile order, all three scales,
        # endpoint quantization and selector word boundaries.
        src = rng.integers(0, 256, (256, 512, 3), dtype=np.uint8)
        descriptors, packed, reference = codec.encode(src)
        desc = np.frombuffer(descriptors, dtype="<u4")
        words = np.frombuffer(packed, dtype="<u4")
        y, x = np.mgrid[:256, :512]
        d = desc[(y // 32) * 16 + x // 32]
        shift = d >> 30
        self.assertEqual(set(np.unique(shift)), {0, 1, 2})
        qx, qy = (x % 32) >> shift, (y % 32) >> shift
        offset = (d & 0x3fffffff) + 5 * ((qy // 8) * (4 >> shift) + qx // 8)
        ends = words[offset]
        def expand(v):
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            return np.stack(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)), -1)
        index = (qy % 8) * 8 + qx % 8
        k = ((words[offset + 1 + index // 16] >> (2 * (index % 16))) & 3)[..., None]
        sampled = ((3-k)*expand(ends & 65535)+k*expand(ends >> 16)+1)//3
        np.testing.assert_array_equal(sampled.astype(np.uint8), reference)

    def test_solid_primary_colours(self):
        for colour in ((255,0,0), (0,255,0), (0,0,255), (0,0,0), (255,255,255)):
            src = np.empty((64,128,3), dtype=np.uint8)
            src[:] = colour
            _, _, decoded = codec.encode(src)
            np.testing.assert_array_equal(src, decoded)

if __name__ == "__main__":
    unittest.main()
