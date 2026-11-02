import unittest

from zm_render.frames import FrameRange, frame_filename, parse_frames


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


class FrameFilenameTest(unittest.TestCase):
    def test_padding(self):
        self.assertEqual(frame_filename(1), "frame_0001.png")
        self.assertEqual(frame_filename(240), "frame_0240.png")


if __name__ == "__main__":
    unittest.main()
