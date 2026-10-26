import unittest

from zm_render.frames import (
    FrameRange,
    frame_filename,
    parse_frames,
    split_chunks,
)


class ParseFramesTest(unittest.TestCase):
    def test_single(self):
        r = parse_frames("7")
        self.assertEqual(r, FrameRange(7, 7))
        self.assertEqual(r.count, 1)

    def test_range(self):
        r = parse_frames("1-240")
        self.assertEqual(r.start, 1)
        self.assertEqual(r.end, 240)
        self.assertEqual(r.count, 240)

    def test_whitespace(self):
        self.assertEqual(parse_frames(" 3 - 5 "), FrameRange(3, 5))

    def test_invalid(self):
        for bad in ("", "a-b", "5-3", "0-1", "-1"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    parse_frames(bad)


class SplitChunksTest(unittest.TestCase):
    def test_exact(self):
        chunks = split_chunks(FrameRange(1, 10), 5)
        self.assertEqual([(c.start, c.end) for c in chunks], [(1, 5), (6, 10)])

    def test_remainder(self):
        chunks = split_chunks(FrameRange(1, 8), 3)
        self.assertEqual(
            [(c.start, c.end) for c in chunks],
            [(1, 3), (4, 6), (7, 8)],
        )

    def test_chunk_one(self):
        chunks = split_chunks(FrameRange(2, 4), 1)
        self.assertEqual([(c.start, c.end, c.name) for c in chunks], [
            (2, 2, "frames_2_2"),
            (3, 3, "frames_3_3"),
            (4, 4, "frames_4_4"),
        ])

    def test_bad_chunk(self):
        with self.assertRaises(ValueError):
            split_chunks(FrameRange(1, 2), 0)


class FrameFilenameTest(unittest.TestCase):
    def test_padding(self):
        self.assertEqual(frame_filename(1), "frame_0001.png")
        self.assertEqual(frame_filename(240), "frame_0240.png")


if __name__ == "__main__":
    unittest.main()
