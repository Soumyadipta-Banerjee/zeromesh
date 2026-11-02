#!/usr/bin/env python3
"""The smallest adapter: one item per name, each writing a greeting file.

    python3 hello_world.py Ada Grace Linus        # outputs in ./zm-out/
    python3 hello_world.py --remote-only Ada      # on another machine of the team
"""

from zeromesh.adapter import Adapter, Item, Out, main


class Hello(Adapter):
    name = "zm-hello"
    kind = "hello"

    def add_args(self, p):
        p.add_argument("names", nargs="+", help="who to greet")

    def items(self, a):
        for n in a.names:
            yield Item(id=n, argv=["sh", "-c", f'echo "Hello, {n}, from $ZM_NODE_ID" > {n}.txt'],
                       outputs=[Out(f"{n}.txt")])


if __name__ == "__main__":
    main(Hello)
