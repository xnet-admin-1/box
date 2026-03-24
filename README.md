# 📦 Box

Lightweight, rootless container runtime for Android and ARM SBCs. PRoot-based — no root, no kernel modules, no setup. Mirrored networking (like WSL2 mirrored mode).

Box cross-compiles the full native toolchain (PRoot + talloc + libarchive) and builds repackaged rootfs images, producing release artifacts that consumer apps like [MCPShell](https://github.com/xnet-admin-1/mcpshell) download at runtime.

## Components

```
box/
├── talloc/          # talloc — hierarchical memory allocator (PRoot dependency)
├── libarchive/      # libarchive — archive handling (PRoot dependency, also produces bsdtar)
├── proot/           # Termux-patched PRoot — ptrace-based chroot/bind/binfmt_misc
├── rootfs/          # Base images repackaged for proot
└── .github/workflows/
    ├── native.yml          # CI: cross-compile talloc + libarchive + proot
    ├── rootfs-ubuntu.yml   # CI: Ubuntu 24.04 base image
    └── rootfs-alpine.yml   # CI: Alpine 3.21 base image
```

## Base Images

| Image | Size | libc | Package Manager | Use Case |
|-------|------|------|-----------------|----------|
| Alpine 3.21 | ~3MB | musl | apk | Minimal, fast bootstrap, dev containers |
| Ubuntu 24.04 | ~30MB | glibc | apt | Full compatibility, prebuilt binary support |

## Release Artifacts

### Native Binaries (`native-latest`)

| File | Source | Description |
|------|--------|-------------|
| `libproot-xed.so` | PRoot (Termux-patched) | The proot binary |
| `libproot.so` | PRoot loader | W^X bypass loader (64-bit) |
| `libproot32.so` | PRoot loader | W^X bypass loader (32-bit) |
| `libtalloc.so` | talloc | PRoot dependency |
| `libbsdtar.so` | libarchive | PRoot dependency + archive extraction tool |

### Alpine Rootfs (`rootfs-alpine-*`)

| File | Description |
|------|-------------|
| `box-alpine-3.21-aarch64.tar.xz` | Alpine 3.21 minirootfs |
| `box-alpine-3.21-aarch64.tar.xz.sha256` | SHA256 checksum |

### Ubuntu Rootfs (`rootfs-ubuntu-*`)

| File | Description |
|------|-------------|
| `box-ubuntu-24.04-aarch64.tar.xz` | Ubuntu 24.04 base rootfs |
| `box-ubuntu-24.04-aarch64.tar.xz.sha256` | SHA256 checksum |

## Networking

Box uses mirrored networking — the container shares the host's network stack directly. No NAT, no virtual bridge, no port forwarding. Bind to `0.0.0.0:3000` inside the container, it's reachable on the device IP at port 3000. Same model as WSL2 mirrored networking mode.

## Build Locally

Native binaries (requires Android NDK in xnet-dev container):

```bash
docker exec -it xnet-dev bash /workspace/box/build.sh
```

## Dependencies

PRoot has two dependencies:
- **talloc** — hierarchical memory allocator
- **libarchive** — multi-format archive library

Both are cross-compiled for Android ARM targets using the NDK toolchain.

## License

[GPL-2.0](LICENSE) (PRoot), various for dependencies.
