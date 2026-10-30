import numpy as np
import pytest

from stats import (bootstrap_ci, diff_verdict, holm, ratio, ratio_verdict,
                   wilcoxon_p)

rng = np.random.default_rng(7)


def test_identical_never_better():
    r = rng.lognormal(0, 0.02, 20)
    assert ratio_verdict(r, wilcoxon_p(np.log(r))) in ("EQUIVALENT", "INCONCLUSIVE")


def test_30pct_speedup_better():
    r = 0.70 * rng.lognormal(0, 0.05, 20)
    assert ratio_verdict(r, 1e-4) == "BETTER"


def test_30pct_slowdown_worse():
    r = (1 / 0.70) * rng.lognormal(0, 0.05, 20)
    assert ratio_verdict(r, 1e-4) == "WORSE"


def test_noisy_inconclusive():
    r = rng.lognormal(0, 0.5, 20)
    assert ratio_verdict(r, wilcoxon_p(np.log(r))) == "INCONCLUSIVE"


def test_holm_known_values():
    assert holm([0.01, 0.04, 0.03]) == pytest.approx([0.03, 0.06, 0.06])


def test_ratio_failure_sentinels():
    assert ratio(5, 10, False, False, True) == 1e9
    assert ratio(5, 10, False, True, False) == 1e-9
    assert ratio(5, 10, False, False, False) == 1.0
    assert ratio(5, 10, False, True, True) == 0.5
    assert ratio(5, 10, True, True, True) == 2.0


def test_diff_verdict_margin():
    assert diff_verdict(rng.normal(-5, 0.5, 20), 1e-4) == "BETTER"
    assert diff_verdict(rng.normal(0, 0.3, 20), 0.5) == "EQUIVALENT"


def test_ratio_verdict_inclusive_boundaries():
    assert ratio_verdict(np.full(20, 0.95), 1e-4) == "BETTER"
    assert ratio_verdict(np.full(20, 1.05), 1e-4) == "WORSE"
    assert ratio_verdict(np.full(20, 0.951), 1e-4) == "EQUIVALENT"
    assert ratio_verdict(np.full(20, 1.049), 1e-4) == "EQUIVALENT"
    assert ratio_verdict(np.full(20, 0.96), 0.5) == "EQUIVALENT"


def test_diff_verdict_inclusive_boundaries():
    assert diff_verdict(np.full(20, -2.0), 1e-4) == "BETTER"
    assert diff_verdict(np.full(20, 2.0), 1e-4) == "WORSE"
    assert diff_verdict(np.full(20, -1.9), 1e-4) == "EQUIVALENT"
    assert diff_verdict(np.full(20, 1.9), 1e-4) == "EQUIVALENT"
