import tempfile
import unittest
from pathlib import Path

from zm_render.blend_paths import find_external_absolute_paths, warn_external_paths


class BlendPathsTest(unittest.TestCase):
    def test_detects_external(self):
        with tempfile.TemporaryDirectory() as td:
            scene = Path(td)
            blend = scene / "shot.blend"
            # Embed a fake absolute texture path outside the scene dir.
            blend.write_bytes(
                b"BLENDER-v293\x00"
                b"/home/other/textures/wood.png\x00"
                b"/usr/share/blender/datafiles/colormanagement\x00"
                b"trailer"
            )
            found = find_external_absolute_paths(blend)
            self.assertTrue(any(p.endswith("wood.png") for p in found))
            self.assertTrue(any("/usr/share/blender" in p for p in found))

    def test_ignores_paths_inside_scene(self):
        with tempfile.TemporaryDirectory() as td:
            scene = Path(td)
            blend = scene / "shot.blend"
            inside = str((scene / "tex" / "a.png").resolve())
            blend.write_bytes(b"HDR\x00" + inside.encode() + b"\x00")
            self.assertEqual(find_external_absolute_paths(blend), [])

    def test_warn_prints(self):
        with tempfile.TemporaryDirectory() as td:
            blend = Path(td) / "s.blend"
            blend.write_bytes(b"/opt/assets/big.exr\x00")
            import io

            buf = io.StringIO()
            paths = warn_external_paths(blend, file=buf)
            self.assertTrue(paths)
            self.assertIn("absolute paths", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
