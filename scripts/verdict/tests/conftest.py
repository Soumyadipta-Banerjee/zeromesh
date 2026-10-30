import sys
from pathlib import Path

import pytest

VERDICT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VERDICT_DIR))


@pytest.fixture
def repo() -> Path:
    return VERDICT_DIR.parents[1]
