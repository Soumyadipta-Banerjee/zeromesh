"""Pure statistics primitives for the verdict suite (numpy/scipy only)."""
import numpy as np
from scipy import stats as _sps

ALPHA = 0.05


def ratio(zm: float, base: float, higher_is_better: bool,
          zm_ok: bool, base_ok: bool) -> float:
    """ZeroMesh/baseline ratio oriented so that < 1 is always good for ZeroMesh."""
    if not zm_ok and base_ok:
        return 1e9
    if zm_ok and not base_ok:
        return 1e-9
    if not zm_ok and not base_ok:
        return 1.0
    return base / zm if higher_is_better else zm / base


def bootstrap_ci(x, stat=np.median, n: int = 10000, seed: int = 0,
                 level: float = 0.95) -> tuple[float, float]:
    """Percentile bootstrap CI of stat(x), resampling blocks (elements) of x."""
    x = np.asarray(x, dtype=float)
    rng = np.random.default_rng(seed)
    idx = rng.integers(0, len(x), size=(n, len(x)))
    boots = np.array([stat(row) for row in x[idx]])
    tail = (1.0 - level) / 2.0 * 100.0
    lo, hi = np.percentile(boots, [tail, 100.0 - tail])
    return float(lo), float(hi)


def wilcoxon_p(x) -> float:
    """Two-sided Wilcoxon signed-rank p vs 0; callers pass log ratios or diffs."""
    x = np.asarray(x, dtype=float)
    if not np.any(x != 0):
        return 1.0
    return float(_sps.wilcoxon(x, alternative="two-sided").pvalue)


def holm(pvals: list[float]) -> list[float]:
    """Holm step-down adjusted p-values, monotone, capped at 1, in input order."""
    m = len(pvals)
    order = sorted(range(m), key=lambda i: pvals[i])
    out = [0.0] * m
    running = 0.0
    for rank, i in enumerate(order):
        running = max(running, (m - rank) * pvals[i])
        out[i] = min(1.0, running)
    return out


def ratio_verdict(ratios, p_adj: float, margin: float = 0.05,
                  seed: int = 0, n_boot: int = 10000) -> str:
    """Ratios are oriented so < 1 is good. margin is a relative equivalence band."""
    r = np.asarray(ratios, dtype=float)
    med = float(np.median(r))
    lo, hi = bootstrap_ci(r, n=n_boot, seed=seed)
    low_edge, high_edge = 1.0 / (1.0 + margin), 1.0 + margin
    if p_adj < ALPHA and hi < 1.0 and med < low_edge:
        return "BETTER"
    if p_adj < ALPHA and lo > 1.0 and med > high_edge:
        return "WORSE"
    if lo >= low_edge and hi <= high_edge:
        return "EQUIVALENT"
    return "INCONCLUSIVE"


def diff_verdict(diffs_ms, p_adj: float, margin_ms: float = 2.0,
                 seed: int = 0, n_boot: int = 10000) -> str:
    """Diffs are zm - base in ms; negative is good. margin_ms is an absolute band."""
    d = np.asarray(diffs_ms, dtype=float)
    med = float(np.median(d))
    lo, hi = bootstrap_ci(d, n=n_boot, seed=seed)
    if p_adj < ALPHA and hi < 0.0 and med < -margin_ms:
        return "BETTER"
    if p_adj < ALPHA and lo > 0.0 and med > margin_ms:
        return "WORSE"
    if lo >= -margin_ms and hi <= margin_ms:
        return "EQUIVALENT"
    return "INCONCLUSIVE"
