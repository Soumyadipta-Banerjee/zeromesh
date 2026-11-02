import argparse
import unittest
from pathlib import Path

from zeromesh import cli

from zm_render.cli import Render
from zm_render.render import BLENDER_ENV, blender_argv

DATA = Path(__file__).resolve().parent / "data"


def parsed(*argv):
    p = argparse.ArgumentParser()
    adapter = Render()
    adapter.add_args(p)
    cli.add_shared_flags(p)
    a = p.parse_args(list(argv))
    adapter.check(a)
    return adapter, a


class BlenderArgvTest(unittest.TestCase):
    def test_frame_range(self):
        argv = blender_argv("shot.blend", "5", "12")
        self.assertEqual(argv[:4], ["blender", "-b", "shot.blend", "-noaudio"])
        self.assertNotIn("-E", argv)
        self.assertEqual(argv[argv.index("-o") + 1], "//out/frame_####")
        self.assertEqual(argv[argv.index("-s") + 1], "5")
        self.assertEqual(argv[argv.index("-e") + 1], "12")
        self.assertEqual(argv[-1], "-a")

    def test_optional_engine(self):
        argv = blender_argv("shot.blend", "1", "1", engine="CYCLES")
        self.assertEqual(argv[argv.index("-E") + 1], "CYCLES")


class RenderAdapterTest(unittest.TestCase):
    def test_one_item_per_frame_and_a_combine_template(self):
        adapter, a = parsed(str(DATA / "cube.blend"), "--frames", "3-5")
        items = list(adapter.items(a))
        self.assertEqual([(i.id, i.seq) for i in items], [("frame_0003", 3), ("frame_0004", 4), ("frame_0005", 5)])
        out = items[0].outputs[0]
        self.assertEqual((out.path, out.dest), ("out/frame_0003.png", "frame_0003.png"))
        combine = adapter.combine(a)
        self.assertEqual(combine[combine.index("-s") + 1], "{first}")
        self.assertEqual(combine[combine.index("-e") + 1], "{last}")
        self.assertEqual(adapter.workspace(a), str(DATA))

    def test_environment_follows_the_blender_flag(self):
        adapter, _ = parsed(str(DATA / "cube.blend"), "--frames", "1")
        self.assertEqual([(p.name, p.version) for p in adapter.env.programs], [("blender", ">=0")])
        self.assertEqual(adapter.env.vars, BLENDER_ENV)
        adapter, _ = parsed(str(DATA / "cube.blend"), "--frames", "1", "--blender-version", ">=4.2")
        self.assertEqual(adapter.env.programs[0].version, ">=4.2")
        adapter, _ = parsed(str(DATA / "cube.blend"), "--frames", "1", "--blender", "./blender")
        self.assertEqual(adapter.env.programs, ())  # travels with the scene
        adapter, _ = parsed(str(DATA / "cube.blend"), "--frames", "2", "--env", "capture")
        probe = adapter.env.probes[0]
        self.assertEqual(probe[probe.index("-b") + 1], str(DATA / "cube.blend"))
        self.assertTrue(probe[probe.index("-o") + 1].startswith("/"))  # never into the user's scene dir
        self.assertEqual(probe[probe.index("-s") + 1], "2")


if __name__ == "__main__":
    unittest.main()
