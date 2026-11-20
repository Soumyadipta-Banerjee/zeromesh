"""Planner, validation, and public dry-run coverage for zm-ffmpeg."""

from __future__ import annotations

import argparse
import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from zeromesh import cli

from zm_ffmpeg.cli import Ffmpeg, main
from zm_ffmpeg.convert import Conversion, check_workspace_file, ffmpeg_argv, plan_conversions


def parsed(ffmpeg, *argv):
    parser = argparse.ArgumentParser()
    ffmpeg.add_args(parser)
    cli.add_shared_flags(parser)
    args = parser.parse_args(list(argv))
    ffmpeg.check(args)
    return ffmpeg, args


class ConversionPlanTests(unittest.TestCase):
    def test_parents_are_preserved_and_order_is_stable(self):
        plan = plan_conversions([Path("z/b.mov"), Path("a/b.mkv")], ".mp4")
        self.assertEqual([(c.input.as_posix(), c.output.as_posix()) for c in plan],
                         [("a/b.mkv", "a/b.mp4"), ("z/b.mov", "z/b.mp4")])
        for conversion in plan:
            self.assertLessEqual(len(conversion.id.encode()), 200)
            self.assertNotIn(" ", conversion.id)

    def test_aliases_cannot_create_duplicate_inputs_or_output_collisions(self):
        with self.assertRaisesRegex(ValueError, "duplicate input"):
            plan_conversions([Path("a.mov"), Path("nested/../a.mov")], ".mp4")
        with self.assertRaisesRegex(ValueError, "output collision"):
            plan_conversions([Path("a.mov"), Path("nested/../a.avi")], ".mp4")
        with self.assertRaisesRegex(ValueError, "output collision"):
            plan_conversions([Path("a.mov"), Path("a.avi")], ".mp4")

    def test_unsafe_paths_and_overwrites_are_rejected(self):
        for source in (Path("/tmp/a.mov"), Path("../a.mov"), Path("nested/../../a.mov")):
            with self.subTest(source=source), self.assertRaises(ValueError):
                plan_conversions([source], ".mp4")
        with self.assertRaisesRegex(ValueError, "same path"):
            plan_conversions([Path("a.mov")], ".mov")
        with self.assertRaisesRegex(ValueError, "suffix"):
            plan_conversions([Path("a.mov")], "mp4")

    def test_ids_are_stable_and_do_not_trust_sanitized_names(self):
        one = plan_conversions([Path("two words.mov")], ".mp4")[0]
        again = plan_conversions([Path("./two words.mov")], ".mkv")[0]
        self.assertEqual(one.id, again.id)
        self.assertNotEqual(one.output, again.output)

    def test_argv_preserves_option_boundaries_without_a_shell(self):
        conversion = Conversion(Path("in file.mov"), Path("out file.mp4"), "item")
        argv = ffmpeg_argv("ffmpeg", conversion, ["-metadata", "title=two words"])
        self.assertEqual(argv[:5], ["ffmpeg", "-nostdin", "-y", "-i", "in file.mov"])
        self.assertEqual(argv[-3:], ["-metadata", "title=two words", "out file.mp4"])
        self.assertNotIn("sh", argv[:2])


class FfmpegAdapterTests(unittest.TestCase):
    def test_environment_and_items_follow_the_executable_contract(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(Path, "cwd", return_value=Path(temp)):
            Path(temp, "input.mov").touch()
            adapter, args = parsed(Ffmpeg(), "input.mov")
            self.assertEqual(adapter.workspace(args), temp)
            self.assertEqual(adapter.env.programs[0].name, "ffmpeg")
            self.assertEqual(adapter.env.programs[0].version, "")  # exact build by default
            self.assertEqual(list(adapter.items(args))[0].outputs[0].path, "input.mp4")

            adapter, _ = parsed(Ffmpeg(), "input.mov", "--env", "capture")
            self.assertEqual(adapter.env.probes, (("ffmpeg", "-version"),))

    def test_relative_custom_ffmpeg_travels_with_the_workspace(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(Path, "cwd", return_value=Path(temp)):
            Path(temp, "input.mov").touch()
            Path(temp, "tools").mkdir()
            Path(temp, "tools", "ffmpeg").touch()
            adapter, _ = parsed(Ffmpeg(), "input.mov", "--ffmpeg", "tools/ffmpeg")
            self.assertEqual(adapter.env.programs, ())

    def test_external_symlink_is_not_an_implicit_upload(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "work"
            outside = Path(temp) / "outside.mov"
            root.mkdir()
            outside.touch()
            (root / "input.mov").symlink_to(outside)
            conversion = plan_conversions([Path("input.mov")], ".mp4")[0]
            with self.assertRaisesRegex(ValueError, "outside the workspace"):
                check_workspace_file(root, conversion)

    def test_public_dry_run_needs_no_daemon(self):
        with tempfile.TemporaryDirectory() as temp:
            old = Path.cwd()
            os.chdir(temp)
            Path("input.mov").touch()
            stdout, stderr = io.StringIO(), io.StringIO()
            try:
                with redirect_stdout(stdout), redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
                    main(["input.mov", "--dry-run", "--", "-an", "-c:v", "ffv1"])
            finally:
                os.chdir(old)
            self.assertEqual(raised.exception.code, 0)
            self.assertIn("input.mp4", stdout.getvalue())
            self.assertIn("ffv1", stdout.getvalue())
            self.assertIn("1 item(s)", stdout.getvalue())
            self.assertNotIn("zmd is not running", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
