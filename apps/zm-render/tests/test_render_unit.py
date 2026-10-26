import tempfile
import unittest
from pathlib import Path
from unittest import mock

from zm_render.frames import FrameRange, split_chunks
from zm_render.render import (
    blender_argv,
    collect_frames,
    expected_frame_path,
)


class BlenderArgvTest(unittest.TestCase):
    def test_chunk_argv(self):
        ch = split_chunks(FrameRange(5, 12), 10)[0]
        argv = blender_argv("shot.blend", ch)
        self.assertEqual(argv[0], "blender")
        self.assertEqual(argv[argv.index("-b") + 1], "shot.blend")
        self.assertIn("-noaudio", argv)
        self.assertNotIn("-E", argv)
        self.assertEqual(argv[argv.index("-s") + 1], "5")
        self.assertEqual(argv[argv.index("-e") + 1], "12")
        self.assertIn("-a", argv)

    def test_optional_engine(self):
        ch = split_chunks(FrameRange(1, 1), 1)[0]
        argv = blender_argv("shot.blend", ch, engine="CYCLES")
        self.assertEqual(argv[argv.index("-E") + 1], "CYCLES")


class CollectFramesTest(unittest.TestCase):
    def test_all_present(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            for n in (1, 2, 3):
                expected_frame_path(out, n).write_bytes(b"png")
            r = collect_frames(out, FrameRange(1, 3))
            self.assertEqual(r.frames_ok, [1, 2, 3])
            self.assertEqual(r.frames_missing, [])

    def test_missing(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            expected_frame_path(out, 1).write_bytes(b"png")
            # frame 2 missing
            expected_frame_path(out, 3).write_bytes(b"png")
            r = collect_frames(out, FrameRange(1, 3))
            self.assertEqual(r.frames_missing, [2])


class BuildJobSpecTest(unittest.TestCase):
    def test_one_task_per_chunk(self):
        # Avoid importing generated stubs; stub common_pb2 shapes.
        class Dig:
            pass

        class EnvSpec:
            def __init__(self, name):
                self.name = name

        class Resources:
            def __init__(self, cores):
                self.cores = cores

        class TaskSpec:
            def __init__(self, **kw):
                self.__dict__.update(kw)

        class JobSpec:
            def __init__(self, name, tasks):
                self.name = name
                self.tasks = tasks

        common = mock.Mock()
        common.EnvSpec = EnvSpec
        common.Resources = Resources
        common.TaskSpec = TaskSpec
        common.JobSpec = JobSpec

        from zm_render.render import build_job_spec

        chunks = split_chunks(FrameRange(1, 5), 2)
        spec = build_job_spec(
            input_root=Dig(),
            blend_rel="s.blend",
            chunks=chunks,
            env="host",
            cores=1,
            blender_bin="./blender",
            common_pb2=common,
        )
        self.assertEqual(len(spec.tasks), 3)
        self.assertEqual(spec.tasks[0].name, "frames_1_2")
        self.assertEqual(spec.tasks[0].argv[spec.tasks[0].argv.index("-s") + 1], "1")
        self.assertEqual(spec.tasks[0].argv[spec.tasks[0].argv.index("-e") + 1], "2")
        self.assertEqual(spec.tasks[-1].name, "frames_5_5")
        self.assertEqual(spec.tasks[0].outputs, ["out/"])


if __name__ == "__main__":
    unittest.main()
