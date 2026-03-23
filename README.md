# 📦 Box

Lightweight, rootless container runtime for Android and ARM SBCs. PRoot-based — no root, no kernel modules, no setup.

Box cross-compiles the full native toolchain (PRoot + talloc + libarchive) and builds repackaged rootfs images, producing release artifacts that consumer apps like [MCPShell](https://github.com/xnet-admin-1/mcpshell) download at runtime.

## Components

```
box/
├── talloc/          # talloc — hierarchical memory allocator (PRoot dependency)
├── libarchive/      # libarchive — archive handling (PRoot dependency, also produces bsdtar)
├── proot/           # Termux-patched PRoot — ptrace-based chroot/bind/binfmt_misc
├── rootfs/          # Ubuntu cloud image repackaged for proot
└── .github/workflows/
    ├── native.yml   # CI: cross-compile talloc + libarchive + proot → release artifacts
    └── rootfs.yml   # CI: download + repackage Ubuntu rootfs → release artifacts
```

## Release Artifacts

### Native Binaries (`native-arm64-v8a`)

| File | Source | Description |
|------|--------|-------------|
| `libproot-xed.so` | PRoot (Termux-patched) | The proot binary |
| `libproot.so` | PRoot loader | W^X bypass loader (64-bit) |
| `libproot32.so` | PRoot loader | W^X bypass loader (32-bit) |
| `libtalloc.so` | talloc | PRoot dependency |
| `libbsdtar.so` | libarchive | PRoot dependency + archive extraction tool |

### Rootfs (`ubuntu-24.04-aarch64`)

| File | Description |
|------|-------------|
| `ubuntu-24.04-aarch64.tar.xz` | Ubuntu 24.04 base rootfs for proot |
| `ubuntu-24.04-aarch64.tar.xz.sha256` | SHA256 checksum |

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
