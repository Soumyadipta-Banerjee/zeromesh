#!/usr/bin/env python3
"""Simple Hello World example using ZeroMesh SDK."""

import sys
import zeromesh

def main():
    try:
        zm = zeromesh.connect()
    except zeromesh.DaemonNotRunning as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    print("Connected to zmd. Running distributed task...")
    job = zm.map(
        argv=["echo", "Hello from ZeroMesh task {}!"],
        items=["A", "B", "C"],
        name="hello_job",
    )

    for event in job.events():
        if event.log_chunk:
            print(event.log_chunk.decode("utf-8", errors="replace"), end="")

    status = job.wait()
    print(f"Job completed: {len(status.results)} tasks succeeded.")

if __name__ == "__main__":
    main()
