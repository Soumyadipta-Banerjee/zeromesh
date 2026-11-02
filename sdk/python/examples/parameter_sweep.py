#!/usr/bin/env python3
"""A parameter sweep: the same command over a range of values, all reading the workspace's
params.txt, results gathered into one table by finish() on this machine.

    echo "constant=42" > params.txt
    python3 parameter_sweep.py --from 1 --to 20 --keep-going
"""

import os

from zeromesh.adapter import Adapter, AdapterError, Item, Out, Policy, Res, main


class Sweep(Adapter):
    name = "zm-sweep"
    kind = "sweep"
    res = Res(cores=1)
    policy = Policy(timeout=600)

    def add_args(self, p):
        p.add_argument("--from", dest="first", type=int, default=1)
        p.add_argument("--to", dest="last", type=int, default=10)

    def check(self, a):
        if not os.path.exists("params.txt"):
            raise AdapterError("no params.txt here (echo 'constant=42' > params.txt)")
        if a.last < a.first:
            raise AdapterError("--to is before --from")

    def items(self, a):
        for x in range(a.first, a.last + 1):
            cmd = f'echo "x={x} $(cat params.txt) y=$(( {x} * {x} ))" > result.txt'
            yield Item(id=f"x{x}", argv=["sh", "-c", cmd], outputs=[Out("result.txt", dest=f"results/x{x}.txt")],
                       inputs=["params.txt"])

    def finish(self, a, run):
        rows = []
        for item_id in sorted(run.succeeded, key=lambda i: int(i[1:])):
            for path in run.placed(item_id):
                with open(path) as f:
                    rows.append(f.read().strip())
        table = os.path.join(run.out_dir, "table.txt")
        with open(table, "w") as f:
            f.write("\n".join(rows) + "\n")
        print(f"{len(rows)} result(s) in {table}")


if __name__ == "__main__":
    main(Sweep)
