import sys
from pathlib import Path

import pytest

VERDICT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VERDICT_DIR))


@pytest.fixture
def repo() -> Path:
    return VERDICT_DIR.parents[1]


# ---- synthetic-data helpers for test_analyze.py ----------------------------------------------
import csv  # noqa: E402

import numpy as np  # noqa: E402

from analyze import COLUMNS  # noqa: E402


def make_row(**kw) -> dict:
    row = {c: None for c in COLUMNS}
    row.update(notes="", variant="")
    row.update(kw)
    return row


def make_rows(blocks: int, wall: dict, noise: float = 0.02, seed: int = 0,
              bad: frozenset = frozenset()) -> list:
    """Synthetic results for blocks 0..blocks-1 (no warm-up rows).

    wall[(workload, system)] is the base value of the workload's scored metric(s):
    seconds for w1/w2/w3/w4/w5 (tx_mb and reexec_task_s mirror it), and the p99 under load in ms
    for w6 (idle p99 is 1.0 ms, wall_s is 60). Each row is multiplied by (1 + noise * N(0,1)).
    w4 rows use step "rerun", w5 rows variant "kill", the rest step "main". Every row has total=10
    and ok=10, except (block, workload, system) in `bad`, which gets ok=9.
    """
    rng = np.random.default_rng(seed)
    rows = []
    for b in range(blocks):
        order = 0
        for (wl, system), v in wall.items():
            f = 1.0 + noise * float(rng.standard_normal())
            x = v * f
            row = make_row(block=b, order=order, workload=wl, system=system,
                           step="rerun" if wl == "w4" else "main",
                           variant="kill" if wl == "w5" else "",
                           wall_s=x, rx_mb=x, tx_mb=x, ok=10, total=10, retries=0,
                           ttfr_s=0.1 * x, reexec_task_s=x, tasks_done=10)
            if wl == "w6":
                row.update(wall_s=60.0, probe_p99_ms=x, probe_idle_p99_ms=1.0,
                           rx_mb=None, tx_mb=None, reexec_task_s=None, ttfr_s=None)
            if (b, wl, system) in bad:
                row["ok"] = 9
            rows.append(row)
            order += 1
    return rows


def write_csv(rows: list, path) -> None:
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        w.writeheader()
        for r in rows:
            w.writerow({c: ("" if r.get(c) is None else r[c]) for c in COLUMNS})
