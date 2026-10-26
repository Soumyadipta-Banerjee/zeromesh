# ZeroMesh Subprojects & Wrap Files

This directory contains Meson subproject wrap files (`*.wrap`) for fetching and building external dependencies when system packages are not found.

### Key Dependencies
- `protobuf.wrap`: Google Protocol Buffers compiler and runtime.
- `grpc.wrap`: Google Remote Procedure Call (gRPC) C++ framework.
- `ftxui.wrap`: Modern C++ terminal UI library for `src/ui/tui.cpp`.
- `cli11.wrap`: Header-only CLI argument parser for `src/main.cpp`.
- `openssl.wrap` / `crypto`: Cryptographic hashing for CAS SHA-256 Merkle trees.

### Using System Packages
If your system already provides these libraries via `pkg-config`, Meson will prioritize the system versions automatically.
