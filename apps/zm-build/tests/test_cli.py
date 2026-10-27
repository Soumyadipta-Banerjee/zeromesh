import unittest

from zm_build.cli import build_auto_parser, build_cmake_parser, build_make_parser


class CliParserTest(unittest.TestCase):
    def test_cmake_defaults(self):
        args = build_cmake_parser().parse_args([])
        self.assertEqual(str(args.source_dir), ".")
        self.assertEqual(str(args.build_dir), "build")
        self.assertEqual(args.server, "127.0.0.1:8980")
        self.assertEqual(args.jobs, 0)

    def test_cmake_jobs_and_target(self):
        args = build_cmake_parser().parse_args(
            ["-S", "src", "-B", "out", "-j", "4", "--target", "hello"]
        )
        self.assertEqual(str(args.source_dir), "src")
        self.assertEqual(str(args.build_dir), "out")
        self.assertEqual(args.jobs, 4)
        self.assertEqual(args.targets, ["hello"])

    def test_make_directory(self):
        args = build_make_parser().parse_args(["-C", "proj", "all"])
        self.assertEqual(str(args.source_dir), "proj")
        self.assertEqual(args.targets, ["all"])

    def test_auto_path(self):
        args = build_auto_parser().parse_args(["/tmp/proj", "-j", "2"])
        self.assertEqual(str(args.path), "/tmp/proj")
        self.assertEqual(args.jobs, 2)


if __name__ == "__main__":
    unittest.main()
