# 📦 Box

Lightweight, rootless container runtime for Android and ARM SBCs. PRoot-based — no root, no kernel modules, no setup. Mirrored networking (like WSL2 mirrored mode).

Box cross-compiles the full native toolchain (PRoot + talloc + libarchive), builds repackaged rootfs images, and provides a CLI to manage containers from Android sh — like `docker` but for PRoot.

## CLI

The `box` command runs in Android sh (toybox/mksh) and manages PRoot containers. No root required.

```bash
box pull alpine                              # download rootfs
box run alpine -n dev -- sh -l               # interactive container
box run ubuntu -d -n web -- python3 -m http.server 8080  # background
box exec dev -- apk add python3 git          # run in existing container
box ps                                       # list containers
box stop web                                 # stop a container
box rm web                                   # remove a container
box up Boxfile                               # start services from Boxfile
box down                                     # stop all Boxfile services
```

### Boxfile (compose-style)

Dockerfile-inspired, line-based format. Parseable by shell — no YAML.

```
SERVICE dev
FROM ubuntu
PACKAGES python3 git curl
ENV LANG=C.UTF-8
VOLUME $DATA/workspace:/workspace
WORKDIR /workspace
CMD bash

SERVICE mcp
FROM alpine
PACKAGES nodejs npm
EXPOSE 39811
CMD node /srv/mcp-server.js
```

### Volume Shortcuts

| Shorthand | Resolves To |
|-----------|-------------|
| `$DATA` | App filesDir/box |
| `$EXT` | App-scoped external storage |
| `$CACHE` | App cacheDir/box |

PRoot uses ptrace-based path translation (not real bind mounts), so volumes work without root. Limited to paths the app can access: its own data dirs, app-scoped external storage, and read-only system paths.

## Components

```
box/
├── cli/
│   ├── box              # CLI script (Android sh)
│   └── Boxfile.example  # Sample compose file
├── talloc/              # talloc — hierarchical memory allocator (PRoot dependency)
├── libarchive/          # libarchive — archive handling (PRoot dependency, also produces bsdtar)
├── proot/               # Termux-patched PRoot — ptrace-based chroot/bind/binfmt_misc
├── rootfs/              # Base images repackaged for proot
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
