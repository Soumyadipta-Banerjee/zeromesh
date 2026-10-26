#!/usr/bin/env python3
"""Parameter sweep example using ZeroMesh SDK."""

import os
import tempfile
import zeromesh

def main():
    try:
        zm = zeromesh.connect()
    except zeromesh.DaemonNotRunning as e:
        print(f"Error: {e}")
        return

    # Create dummy scene input directory
    with tempfile.TemporaryDirectory() as tmp_dir:
        input_file = os.path.join(tmp_dir, "params.txt")
        with open(input_file, "w") as f:
            f.write("simulation_constant=42\n")

        print("Uploading input directory to CAS...")
        root = zm.upload(tmp_dir)
        print(f"Root digest: {root.hash} ({root.size_bytes} bytes)")

        # Map 10 simulation runs
        print("Submitting parameter sweep...")
        job = zm.map(
            argv=["sh", "-c", "echo 'Result for parameter {}:' $(cat params.txt) > result_{}.txt"],
            items=range(1, 11),
            input_root=root,
            outputs=["result_{}.txt"],
            name="param_sweep",
        )

        status = job.wait()
        print(f"Sweep done! Status: {len(status.results)} tasks finished.")

        out_dir = os.path.join(tmp_dir, "outputs")
        for i in range(10):
            task_name = f"task_{i}"
            files = job.fetch(task_name, out_dir)
            print(f"Fetched {task_name}: {files}")

if __name__ == "__main__":
    main()
