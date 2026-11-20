"""Real zmd + FFmpeg execution, output validation, and cache re-placement."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import zmtest  # noqa: E402
from zeromesh import report  # noqa: E402
from zm_ffmpeg.cli import main  # noqa: E402

HAS_FFMPEG = shutil.which("ffmpeg") is not None and shutil.which("ffprobe") is not None


@unittest.skipUnless(HAS_FFMPEG, "ffmpeg and ffprobe are required")
class FfmpegMeshTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.zmd = zmtest.Zmd()

    @classmethod
    def tearDownClass(cls):
        cls.zmd.close()

    def test_real_conversion_and_cached_replacement(self):
        with tempfile.TemporaryDirectory(prefix="zm-ffmpeg-") as temp:
            root = Path(temp)
            work = root / "work"
            out = root / "out"
            work.mkdir()
            source = work / "input.avi"
            subprocess.run([
                "ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i",
                "testsrc2=size=64x64:rate=4:duration=0.25", "-frames:v", "1", "-c:v", "rawvideo", str(source),
            ], check=True, capture_output=True, text=True)

            argv = [
                "--socket", self.zmd.socket,
                "--local-only",
                "-o", str(out),
                "input.avi",
                "--suffix", ".mkv",
                "--",
                "-an", "-c:v", "ffv1",
            ]

            def convert():
                old = os.getcwd()
                os.chdir(work)
                try:
                    with self.assertRaises(SystemExit) as raised:
                        main(argv)
                    self.assertEqual(raised.exception.code, 0, self.zmd.log())
                finally:
                    os.chdir(old)
                return report.load(str(out / "report.json"))

            first = convert()
            self.assertEqual(first.run.succeeded, 1)
            self.assertFalse(first.items[0].cache.hit)
            converted = out / "input.mkv"
            self.assertTrue(converted.is_file())
            self.assertGreater(converted.stat().st_size, 0)

            probe = subprocess.run([
                "ffprobe", "-v", "error", "-show_entries", "stream=codec_name,codec_type", "-of", "json",
                str(converted),
            ], check=True, capture_output=True, text=True)
            streams = json.loads(probe.stdout)["streams"]
            self.assertEqual(streams, [{"codec_name": "ffv1", "codec_type": "video"}])

            converted.unlink()
            second = convert()
            self.assertTrue(second.items[0].cache.hit, self.zmd.log())
            self.assertTrue(converted.is_file())  # placed from the original result, not converted again
            self.assertEqual(second.run.cached, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
