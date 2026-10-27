"""Unit tests for zm_task.tasks — run.sh parsing and grouping."""

import pytest
from zm_task.tasks import TaskLine, parse_run_text, split_task_groups


class TestParseRunText:
    def test_simple_commands(self):
        text = "echo hello\necho world\n"
        tasks = parse_run_text(text)
        assert len(tasks) == 2
        assert tasks[0].command == "echo hello"
        assert tasks[0].index == 0
        assert tasks[1].command == "echo world"
        assert tasks[1].index == 1

    def test_skips_blank_lines(self):
        text = "\n\necho hello\n\necho world\n\n"
        tasks = parse_run_text(text)
        assert len(tasks) == 2

    def test_skips_comments(self):
        text = "# this is a comment\necho hello\n# another comment\necho world\n"
        tasks = parse_run_text(text)
        assert len(tasks) == 2

    def test_skips_shebang(self):
        text = "#!/bin/bash\necho hello\n"
        tasks = parse_run_text(text)
        assert len(tasks) == 1
        assert tasks[0].command == "echo hello"

    def test_label_directive(self):
        text = "# @label my_task\necho hello\n"
        tasks = parse_run_text(text)
        assert len(tasks) == 1
        assert tasks[0].label == "my_task"
        assert tasks[0].command == "echo hello"

    def test_label_only_applies_to_next(self):
        text = "# @label first\necho one\necho two\n"
        tasks = parse_run_text(text)
        assert tasks[0].label == "first"
        assert tasks[1].label == "task_1"  # default label

    def test_line_continuation(self):
        text = "echo hello \\\n  world\n"
        tasks = parse_run_text(text)
        assert len(tasks) == 1
        assert "hello" in tasks[0].command
        assert "world" in tasks[0].command

    def test_preserves_set_commands(self):
        text = "set -e\necho hello\nset -x\necho world\n"
        tasks = parse_run_text(text)
        assert len(tasks) == 4
        assert tasks[0].command == "set -e"

    def test_empty_script(self):
        text = "# only comments\n\n# more comments\n"
        tasks = parse_run_text(text)
        assert len(tasks) == 0

    def test_task_names_unique(self):
        text = "a\nb\nc\nd\ne\n"
        tasks = parse_run_text(text)
        names = [t.name for t in tasks]
        assert len(names) == len(set(names))

    def test_complex_commands(self):
        text = (
            "python train.py --lr 0.001 --epochs 10\n"
            "ffmpeg -i in.mp4 -vf 'scale=1080:720' out.mp4\n"
            "gcc -O2 -o main main.c && ./main > out/result.txt\n"
        )
        tasks = parse_run_text(text)
        assert len(tasks) == 3
        assert "--lr 0.001" in tasks[0].command
        assert "ffmpeg" in tasks[1].command
        assert "&&" in tasks[2].command


class TestSplitTaskGroups:
    def _make_tasks(self, n: int) -> list[TaskLine]:
        return [TaskLine(index=i, command=f"cmd_{i}", label=f"t_{i}") for i in range(n)]

    def test_group_size_1(self):
        tasks = self._make_tasks(5)
        groups = split_task_groups(tasks, 1)
        assert len(groups) == 5
        assert all(len(g) == 1 for g in groups)

    def test_group_size_2(self):
        tasks = self._make_tasks(5)
        groups = split_task_groups(tasks, 2)
        assert len(groups) == 3
        assert len(groups[0]) == 2
        assert len(groups[1]) == 2
        assert len(groups[2]) == 1  # remainder

    def test_group_size_larger_than_tasks(self):
        tasks = self._make_tasks(3)
        groups = split_task_groups(tasks, 10)
        assert len(groups) == 1
        assert len(groups[0]) == 3

    def test_group_size_zero_becomes_1(self):
        tasks = self._make_tasks(3)
        groups = split_task_groups(tasks, 0)
        assert len(groups) == 3

    def test_empty(self):
        groups = split_task_groups([], 5)
        assert len(groups) == 0
