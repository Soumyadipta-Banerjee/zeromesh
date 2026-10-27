import tempfile
import unittest
from pathlib import Path
from unittest import mock

from zm_render.frames import FrameRange, split_chunks
from zm_render.render import (
    blender_argv,
    build_job_spec,
    collect_frames,
    expected_frame_path,
    parse_dispatch_policy,
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
            def __init__(self, name, params=None):
                self.name = name
                self.params = params or {}

        class Resources:
            def __init__(self, cores):
                self.cores = cores

        class TaskSpec:
            def __init__(self, **kw):
                self.__dict__.update(kw)

        class DispatchPolicy:
            def __init__(self, kind, params=None):
                self.kind = kind
                self.params = params or {}

        class JobSpec:
            def __init__(self, name, tasks, dispatch=None):
                self.name = name
                self.tasks = tasks
                self.dispatch = dispatch

        common = mock.Mock()
        common.EnvSpec = EnvSpec
        common.Resources = Resources
        common.TaskSpec = TaskSpec
        common.JobSpec = JobSpec
        common.DispatchPolicy = DispatchPolicy
        common.Policy = mock.Mock()

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
        self.assertEqual(spec.dispatch.kind, "pull")
        self.assertEqual(spec.dispatch.params, {})

    def test_dispatch_policy_on_job(self):
        class Dig:
            pass

        class EnvSpec:
            def __init__(self, name, params=None):
                self.name = name
                self.params = params or {}

        class Resources:
            def __init__(self, cores):
                self.cores = cores

        class TaskSpec:
            def __init__(self, **kw):
                self.__dict__.update(kw)

        class DispatchPolicy:
            def __init__(self, kind, params=None):
                self.kind = kind
                self.params = params or {}

        class JobSpec:
            def __init__(self, name, tasks, dispatch=None):
                self.name = name
                self.tasks = tasks
                self.dispatch = dispatch

        common = mock.Mock()
        common.EnvSpec = EnvSpec
        common.Resources = Resources
        common.TaskSpec = TaskSpec
        common.JobSpec = JobSpec
        common.DispatchPolicy = DispatchPolicy
        common.Policy = mock.Mock()

        chunks = split_chunks(FrameRange(1, 2), 2)
        spec = build_job_spec(
            input_root=Dig(),
            blend_rel="s.blend",
            chunks=chunks,
            env="host",
            cores=0,
            blender_bin="blender",
            common_pb2=common,
            dispatch_kind="batch",
            dispatch_params={
                "prefetch": "3",
                "max_in_flight": "8",
            },
        )
        self.assertEqual(spec.tasks[0].resources.cores, 0)
        self.assertEqual(spec.dispatch.kind, "batch")
        self.assertEqual(spec.dispatch.params["prefetch"], "3")
        self.assertEqual(spec.dispatch.params["max_in_flight"], "8")


class ParseDispatchPolicyTest(unittest.TestCase):
    def test_defaults(self):
        self.assertEqual(parse_dispatch_policy(""), ("pull", {}))
        self.assertEqual(parse_dispatch_policy("pull"), ("pull", {}))
        self.assertEqual(parse_dispatch_policy("fast"), ("fast", {}))
        self.assertEqual(parse_dispatch_policy("my-plugin"), ("my-plugin", {}))

    def test_batch_prefetch(self):
        self.assertEqual(parse_dispatch_policy("batch:2"), ("batch", {"prefetch": "2"}))

    def test_batch_invalid(self):
        with self.assertRaises(ValueError):
            parse_dispatch_policy("batch:0")


class CliDispatchTest(unittest.TestCase):
    def test_cores_default_zero(self):
        from zm_render.cli import build_parser

        args = build_parser().parse_args(
            ["scene.blend", "--frames", "1-1"],
        )
        self.assertEqual(args.cores, 0)
        self.assertEqual(args.policy, "pull")

    def test_cli_to_dispatch_params(self):
        from zm_render.cli import build_parser

        args = build_parser().parse_args(
            [
                "scene.blend",
                "--frames",
                "1-1",
                "--policy",
                "batch:4",
                "--max-in-flight",
                "12",
            ],
        )
        kind, params = parse_dispatch_policy(args.policy)
        if args.max_in_flight is not None:
            params["max_in_flight"] = str(args.max_in_flight)
        self.assertEqual(kind, "batch")
        self.assertEqual(params, {"prefetch": "4", "max_in_flight": "12"})


if __name__ == "__main__":
    unittest.main()
