# ZeroMesh Packaging

ZeroMesh delivers binary packages and systemd user services for seamless zero-setup deployment on Linux.

## Installation

### Ubuntu / Debian (.deb)

1. Build the Debian package (requires `dpkg-deb`):
   ```bash
   make build
   ./packaging/debian/build_deb.sh
   ```

2. Install the package:
   ```bash
   sudo dpkg -i zeromesh_0.1.0_amd64.deb
   sudo apt-get install -f  # resolves any missing dependencies
   ```

### Arch Linux (AUR / PKGBUILD)

1. Build and install with `makepkg`:
   ```bash
   cd packaging/arch
   makepkg -si
   ```

---

## Daemon & Service Management

ZeroMesh runs as a non-privileged **systemd user service**:

```bash
# Reload user daemon configuration
systemctl --user daemon-reload

# Start zmd immediately and enable on login
systemctl --user enable --now zmd

# Check daemon status
systemctl --user status zmd

# Check mesh node status
zm status
```

### Donating While Logged Out (Linger)

To allow `zmd` to start on boot and keep donating compute when you are not logged in:

```bash
loginctl enable-linger $USER
```

---

## Network & Firewall Configuration

ZeroMesh requires two network ports to communicate across nodes on your LAN:

| Port | Protocol | Purpose |
|---|---|---|
| **8981** | TCP | `NodeApi` remote execution, task placement, and blob transfer over mTLS |
| **8988** | UDP | Team-scoped multicast discovery and health beacons |

### UFW Example
```bash
sudo ufw allow 8981/tcp comment "ZeroMesh NodeApi"
sudo ufw allow 8988/udp comment "ZeroMesh Discovery"
```

### firewalld Example
```bash
sudo firewall-cmd --add-port=8981/tcp --permanent
sudo firewall-cmd --add-port=8988/udp --permanent
sudo firewall-cmd --reload
```
