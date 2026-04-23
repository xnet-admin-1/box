# Box + libzt Integration Plan

## Overview

Integrate libzt (ZeroTier userspace TCP/IP stack) as an optional container network stack. Gives containers their own ZeroTier IP without VPN, tun, or root.

## Architecture

```
Container Process
    │ connect("10.147.17.20:8080")
    │ (syscall intercepted by ptrace)
    ▼
PRoot Extension (zt_net)
    │ rewrites dest → 127.0.0.1:10042
    │ writes {10042, 10.147.17.20, 8080} to mapping file
    ▼
Kernel TCP (localhost)
    │ fast path — no ptrace on data plane
    ▼
ztproxy Daemon
    │ reads mapping, connects via libzt
    │ proxies bidirectionally
    ▼
ZeroTier Network (encrypted, P2P)
```

## Components

### 1. ztproxy daemon (`ztproxy/main.c`, ~1500 LOC)
- Statically links `libzt.a`, ships as `libztproxy.so`
- Single libzt instance per container
- Listens on proxy ports 10000-10999
- Reads mmap'd mapping file for connect() destinations
- Handles DNS over ZeroTier (listens on 127.0.0.53:53)
- Per-container ZT identity at `$BOX_CONTAINERS/$name/zt-identity/`

### 2. PRoot extension (`src/extension/zt_net/zt_net.c`, ~400 LOC)
- Modeled on existing `port_switch` extension
- Intercepts `connect()` only (control plane)
- Rewrites sockaddr to `127.0.0.1:<unique_proxy_port>`
- Writes original destination to shared mapping file
- No data-plane involvement — read/write/poll go through kernel

### 3. Shared mapping file (`$BOX_RUN/$name.zt.map`)
- mmap'd fixed-size array, 1000 slots × 32 bytes
- Entry: `{uint16_t proxy_port, uint8_t addr[16], uint16_t port, uint8_t flags}`
- Lock-free: PRoot writes, daemon reads, atomic flag for slot state

### 4. CLI changes (~100 LOC delta in `cli/box`)
- `--zt-net <network_id>` flag on `box run`
- `NETWORK <network_id>` directive in Boxfile
- Daemon lifecycle in cmd_run/cmd_stop/cmd_rm
- Conditional resolv.conf (127.0.0.53 when ZT enabled)

## Implementation Order

### Phase 1: Build infrastructure
1. Add libzt cross-compile to `build.sh` (already done — `libzt.a` built for aarch64)
2. Add `ztproxy/CMakeLists.txt`, link against `libzt.a`
3. Add ztproxy to `native.yml` CI

### Phase 2: Core proxy
4. Write ztproxy daemon — init libzt, join network, accept proxy connections
5. Implement mapping file reader — match proxy port to real destination
6. Implement bidirectional TCP proxy (epoll-based)
7. Test standalone: manual mapping file + nc through proxy to ZT peer

### Phase 3: PRoot integration
8. Create `src/extension/zt_net/zt_net.c` — intercept connect(), rewrite sockaddr
9. Implement mapping file writer in extension
10. Register extension in `src/cli/proot.c`
11. Test: container curl through ztproxy to ZT peer

### Phase 4: CLI + polish
12. Add `--zt-net` flag and daemon lifecycle to `cli/box`
13. Add `NETWORK` to Boxfile parser
14. Conditional DNS (resolv.conf → 127.0.0.53)
15. Add `box network` subcommand for status/identity info

### Phase 5: Advanced
16. UDP support in ztproxy
17. Inbound connections (listen on ZT IP, forward to container)
18. `EXPOSE` actually works — maps container port to ZT-reachable port
19. Per-container ZT identity management (box network join/leave)

## Key Design Decisions

- **Control plane only in PRoot** — data flows through kernel TCP on localhost, not ptrace. This is critical for performance.
- **One daemon per container** — isolates ZT identities, limits blast radius, clean lifecycle.
- **Static linking** — ztproxy is a single binary with no runtime deps. Ships as libztproxy.so for Android APK compat.
- **Mapping file over IPC** — mmap is simpler and faster than unix sockets or pipes for the PRoot↔daemon interface.

## Resource Budget

| Component | Memory | CPU |
|-----------|--------|-----|
| ztproxy daemon | ~15-25MB | idle when no traffic |
| PRoot extension | ~0 (in-process) | negligible per connect() |
| Mapping file | 32KB | mmap'd, no copies |

## Dependencies

- libzt.a (built, 23MB static lib for aarch64)
- Android NDK 27+ (already in build system)
- No new runtime dependencies
