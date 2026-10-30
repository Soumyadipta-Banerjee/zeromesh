#!/usr/bin/env python3
"""Load verdict.toml, export it as shell variables, and derive seeded run orders."""
import random
import sys
import tomllib


def load(path) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)


def _quote(v) -> str:
    if isinstance(v, str):
        return "'" + v.replace("'", "'\\''") + "'"
    return str(v)


def shell_vars(cfg: dict) -> str:
    return "\n".join(
        f"VD_{section.upper()}_{key.upper()}={_quote(v)}"
        for section, table in cfg.items()
        for key, v in table.items()
    )


def run_order(seed: int, block: int, systems: list[str]) -> list[str]:
    order = list(systems)
    random.Random(seed * 1000 + block).shuffle(order)
    return order


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "shell":
        print(shell_vars(load(argv[2])))
        return 0
    if len(argv) >= 5 and argv[1] == "order":
        print(" ".join(run_order(int(argv[2]), int(argv[3]), argv[4:])))
        return 0
    print("usage: config.py shell FILE | config.py order SEED BLOCK sys...", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
