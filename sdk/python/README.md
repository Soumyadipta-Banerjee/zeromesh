# ZeroMesh Python SDK

The `zeromesh` Python package provides a clean, ergonomic client for submitting, mapping, and monitoring compute jobs across a ZeroMesh peer network.

It talks directly to the local ZeroMesh daemon (`zmd`) over its UNIX domain socket using the protobuf `ClientApi`.

## Installation

```bash
pip install ./sdk/python
```

For development (pytest, regenerating protos on `pip install -e`):

```bash
pip install './sdk/python[dev]'
```

Ensure `zmd` is running locally before submitting jobs:
```bash
zmd &
```

## Quick Start: Distributed Parameter Mapping

```python
import zeromesh

# 1. Connect to local daemon (raises zeromesh.DaemonNotRunning if daemon is offline)
zm = zeromesh.connect()

# 2. Upload workspace / scene tree to Content-Addressable Storage (CAS)
root = zm.upload("./scene")

# 3. Map commands across parameters or items
job = zm.map(
    argv=["blender", "-b", "shot.blend", "-f", "{}"],
    items=range(1, 241),
    input_root=root,
    outputs=["out/"],
    env="host",
    cores=1,
)

# 4. Stream real-time events
for event in job.events():
    if event.log_chunk:
        print(event.log_chunk.decode("utf-8", errors="replace"), end="")

# 5. Wait for completion (raises TaskFailed if any task returns non-zero exit code)
status = job.wait()

# 6. Fetch outputs from completed tasks
for task_name in status.results:
    job.fetch(task_name, "renders/")

print(f"Job {job.job_id} completed successfully!")
```

## Advanced DAG Workflows

For complex Directed Acyclic Graph (DAG) pipelines with task dependencies and intermediate outputs:

```python
import zeromesh
from zeromesh.v1 import common_pb2

zm = zeromesh.connect()

# Define upstream task A
task_a = common_pb2.TaskSpec(
    name="build",
    argv=["gcc", "-c", "main.c", "-o", "main.o"],
    outputs=["main.o"],
    env_spec=common_pb2.EnvSpec(name="host"),
)

# Define downstream task B that depends on task A
task_b = common_pb2.TaskSpec(
    name="link",
    argv=["gcc", "main.o", "-o", "app"],
    outputs=["app"],
    env_spec=common_pb2.EnvSpec(name="host"),
    depends_on=["build"],
    inputs_from=[common_pb2.InputFrom(task="build", path=".")],
)

# Submit DAG
job = zm.submit(common_pb2.JobSpec(name="build_pipeline", tasks=[task_a, task_b]))
job.wait()
job.fetch("link", "./bin")
```

## Peer Discovery & Team Management

```python
peers = zm.peers()
for peer in peers:
    print(f"Peer: {peer.hostname} ({peer.node_id[:8]}) - State: {peer.state} - Admitted: {peer.admitted}")

info = zm.team_info()
print(f"Team ID: {info.team_id}, Active: {info.in_team}, Pinned peers: {info.peers_pinned}")
```

## Saving Partial Results (`commit_unit`)

A task made of many units (frames, sweep cases) can save each one as it finishes. If the donor leaves or the
task is cut off, the next attempt starts with those units already present, so only the rest runs again.

```python
import zeromesh

for i in range(100):
    if zeromesh.is_committed(f"case{i}"):
        continue                                   # an earlier attempt finished it
    result = run_case(i)
    zeromesh.commit_unit(f"case{i}", result)       # bytes, text, or a file path (copied); atomic
```

`commit_unit` works only inside a task (it writes to `$ZM_UNITS_DIR`). Unit ids are 1-128 characters of
`A-Z a-z 0-9 . _ -` and may not start with a dot. Job events carry a `unit_committed` field as units arrive.

## Error Handling

- `zeromesh.DaemonNotRunning`: Raised if `zmd` is not running or socket is unreachable.
- `zeromesh.TaskFailed`: Raised by `job.wait()` on task failure. Contains `exit_code`, `stderr_head`, and `result`.
- `zeromesh.JobCancelled`: Raised if job was aborted.
- `KeyboardInterrupt`: Safely traps interrupt, cancels the remote job on `zmd`, and exits.
