# ZeroMesh BitTorrent-Style Directory Swarm (GTK4 GUI)

An example application located in `tests/examples/` demonstrating how to export any directory into a BitTorrent-style magnet link (`magnet:?xt=urn:zm:<info_hash>`), distribute it across computers using ZeroMesh's Kademlia DHT submodules, and allow peer computers to download and reconstruct the entire directory in their own working directory while re-seeding the chunks to the mesh.

---

## Architecture Overview

```
                      ┌──────────────────────────────────────┐
                      │    GTK4 GUI (tests/examples)         │
                      │  - Export & Seed Tab (Current Dir)   │
                      │  - Download & Reconstruct Tab        │
                      │  - Swarm Telemetry & Mesh Stats      │
                      └──────────────────┬───────────────────┘
                                         │
                 ┌───────────────────────┴───────────────────────┐
                 ▼                                               ▼
     ┌───────────────────────┐                       ┌───────────────────────┐
     │  TorrentManifest      │                       │  TorrentEngine        │
     │  - Slices files into  │                       │  - Coordinates DHT,   │
     │    256 KB chunks      │                       │    ChunkFetcher, CAS  │
     │  - Computes InfoHash  │                       │  - Manages Seeder /   │
     │  - BitTorrent Magnet  │                       │    Leecher State      │
     └───────────┬───────────┘                       └───────────┬───────────┘
                 │                                               │
                 ├───────────────────────┬───────────────────────┤
                 ▼                       ▼                       ▼
      ┌─────────────────────┐ ┌─────────────────────┐ ┌─────────────────────┐
      │  Kademlia DHT Engine│ │  ChunkFetcher & TCP │ │  CasStore & Cache   │
      │  (src/dht/kademlia) │ │  (src/vfs/fetcher)  │ │  (src/cas/store)    │
      │  - ANNOUNCE_CHUNK   │ │  - Direct TCP pull  │ │  - SHA-256 storage  │
      │  - GET_PROVIDERS    │ │  - SHA-256 integrity│ │  - LRU disk cache   │
      │  - STORE/FIND_VALUE │ │  - Local TCP server │ │    eviction         │
      └─────────────────────┘ └─────────────────────┘ └─────────────────────┘
```

---

## Features

1. **Automatic Working Directory Detection**:
   When run, the application defaults to the directory it is executed from (`std::filesystem::current_path()`).
2. **Magnet Link Creation & Seeding**:
   - Recursively traverses all files in the directory.
   - Slices files into content-addressed chunks (default 256 KB).
   - Computes SHA-256 digests and stages chunks into local CAS.
   - Announces every chunk hash and the root manifest InfoHash to the Kademlia DHT network.
   - Generates a Magnet URI: `magnet:?xt=urn:zm:<64-char-infohash>&dn=<name>&xl=<total_bytes>`
   - Provides a 1-click **Copy Magnet** button.
   - Activates a background TCP chunk server to seed chunks to peer nodes.
3. **Directory Downloading & Reconstruction**:
   - Paste any Magnet URI or raw 64-char InfoHash.
   - Queries the DHT to discover chunk providers.
   - Streams chunks concurrently over TCP directly from seeders.
   - Cryptographically verifies SHA-256 integrity of each downloaded chunk.
   - Reconstructs nested subdirectories and files in the destination directory.
   - Automatically becomes a **Seeder** upon completion, announcing downloaded chunks to the DHT.
4. **Live Diagnostics & Telemetry**:
   - Active mesh peer count via mDNS and DHT routing table.
   - Total bytes seeded and downloaded.
   - Real-time chunking and download progress bars with speed and status logs.

---

## Compilation

Build using Meson and Ninja:

```bash
# From the repository root
meson setup build
ninja -C build
```

The binary will be compiled to `build/zm-torrent-gui`.

---

## Running the Application

### 1. On Computer A (Seeder / Exporter)
Navigate to the directory you want to share and launch the app:

```bash
cd ~/my_project
/path/to/zeromesh/build/zm-torrent-gui
```

- Click **🚀 Create Magnet & Start Seeding**.
- Click **📋 Copy Magnet** once generated.

### 2. On Computer B (Downloader / Peer)
On another computer connected to the same LAN (or routing through the DHT mesh):

```bash
cd ~/downloads
/path/to/zeromesh/build/zm-torrent-gui
```

- Switch to the **📥 Download & Reconstruct** tab.
- Paste the Magnet URI into the entry field.
- Click **⬇ Fetch & Reconstruct**.
- The entire folder will be downloaded and verified in `~/downloads/<folder_name>`.

### 3. Testing on a Single Machine
To simulate two distinct computers on the same system, specify distinct DHT and chunk ports:

**Terminal 1 (Computer A):**
```bash
cd /tmp/folder_a
/path/to/zeromesh/build/zm-torrent-gui --dht-port 8982 --chunk-port 8984
```

**Terminal 2 (Computer B):**
```bash
cd /tmp/folder_b
/path/to/zeromesh/build/zm-torrent-gui --dht-port 9982 --chunk-port 9984
```
Paste the magnet link from Terminal 1 into Terminal 2 and click fetch!
