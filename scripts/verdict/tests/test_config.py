from config import load, run_order, shell_vars


def test_shell_vars_format(repo):
    lines = shell_vars(load(repo / "scripts/verdict/verdict.toml")).splitlines()
    assert "VD_RUN_BLOCKS=20" in lines


def test_run_order_deterministic_and_varies():
    s = ["zm", "condor-default", "condor-tuned"]
    assert run_order(1, 3, s) == run_order(1, 3, s)
    assert len({tuple(run_order(1, b, s)) for b in range(1, 21)}) > 1
    assert sorted(run_order(1, 3, s)) == sorted(s)
