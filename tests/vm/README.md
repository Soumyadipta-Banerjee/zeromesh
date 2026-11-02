# Testing ZeroMesh across machines with QEMU VMs

Unit and integration tests run every daemon on one kernel. These scripts run the product the way
users do: separate machines, discovery by multicast, mTLS over a real network, donors that lack
software, links with latency and loss, machines that leave.

The team is this machine plus two Ubuntu 24.04 VMs (`zm-a`, `zm-b`) on libvirt's `default` NAT
network (`virbr0`, 192.168.122.0/24). The VMs cannot join a Wi-Fi network directly: a Wi-Fi client
interface does not bridge, and macvtap on it was tried (`iwlwifi`, 2026-11-02): the access point
never answered the VM's DHCP. Instead each VM's link is shaped with `tc netem` to behave like Wi-Fi.

## Once: the VMs

Any Ubuntu 24.04 cloud image with a user that has passwordless sudo and your SSH key works
(cloud-init `users:` entry). `tests/vm/node.sh HOST setup` installs what the scenarios need
(rsync, strace, a Python venv with grpcio). Leave compilers and tools off a VM to test donors
that lack them.

```bash
virsh start zm-a; virsh start zm-b
virsh domifaddr zm-a; virsh domifaddr zm-b         # their 192.168.122.x addresses
export ZM_VM_KEY=~/Desktop/zm-qemu/id_zm             # the key cloud-init installed (default)
tests/vm/node.sh 192.168.122.254 setup
```

## Each change: stage, deploy, form the team

```bash
ninja -C build
tests/vm/stage.sh                                    # build/vm-stage: zmd, zm, bwrap, libs, SDK, apps
for h in 192.168.122.254 192.168.122.174; do tests/vm/node.sh $h deploy; done   # fresh state each time
tests/vm/host.sh wipe; tests/vm/host.sh start        # this machine, state in build/vm-host
phrase=$(tests/vm/host.sh zm team create | tr -d "'")
for h in 192.168.122.254 192.168.122.174; do tests/vm/node.sh $h zm team join "$phrase"; done
tests/vm/host.sh zm status                           # both VMs admitted
for h in 192.168.122.254 192.168.122.174; do tests/vm/node.sh $h shape wifi; done
```

`node.sh HOST shape wifi|bad|off`: what the VM sends is delayed 5 ms ± 3 ms with 0.3 % loss at
60 Mbit/s (`wifi`), or 40 ms ± 20 ms, 3 % loss, 8 Mbit/s (`bad`). Shaping both VMs shapes VM↔VM
traffic both ways and this machine↔VM traffic one way.

`eval "$(tests/vm/host.sh env)"` points `zm`, the SDK and the apps in this shell at the test node.

## What was checked this way (2026-11-02), and what it found

- Discovery and admission of both VMs from a laptop whose default route is Wi-Fi. Discovery used to
  announce only on the default route's interface (`ip route get 239.255.42.99` -> `wlp5s0`), so the
  VMs were never found; it now announces on, and listens to, every multicast-capable interface.
- `zm doctor` on all three nodes; its warnings matched the setup (no memory controller delegated).
- Fan-out: 12 items on the VMs' 4 cores in three waves, 6 per VM.
- `zm-cc` on this repository's `core/` (24 C++ units with gRPC headers):
  - the snapshot shipped the daemon's own store and the build's binaries (481 MB); the daemon now
    leaves its state out and zm-cc leaves out ELF files and build caches (14 MB);
  - `ccache c++ ...` was taken for a compiler named ccache; launchers are stripped;
  - with the donors' own compiler, the units that need headers only the laptop has fail and say which
    (`sodium.h: No such file or directory`); with `--env capture` all 24 compile on the VMs;
  - memory: wait4 on bwrap reports bwrap's own 2 MB, never the compiler's, so every learned
    reservation was ~5x too small; two 470-760 MB compiles started on a 1.5 GB VM, which thrashed
    (5 MB available, no swap) and froze for ~37 s, past the 10 s keepalive, and the requester gave
    both donors up. Memory is now measured from the cgroup or the process tree, and a start counts
    what running batches have yet to grow into. Two consecutive cold rebuilds then compiled 24/24
    with at least 228 MB left and no stall;
  - a batch whose only donors were busy *this second* was failed as "no node can run this batch":
    eligibility compared against free memory instead of total; it now waits. Nodes excluded after a
    loss are tried again when nothing else can run the batch, and an unplaceable batch says why.
