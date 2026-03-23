# PRoot

User-space chroot, mount --bind, and binfmt_misc via ptrace. Termux-patched.

- Source: https://github.com/termux/proot (master)
- Patches: link2symlink, sysvipc, ashmem_memfd, W^X loader bypass
- Dependencies: talloc, libarchive
- Cross-compiled for `aarch64-linux-android` (API 26)

Produces:
- `libproot-xed.so` — the proot binary
- `libproot.so` — 64-bit loader (W^X bypass)
- `libproot32.so` — 32-bit loader
