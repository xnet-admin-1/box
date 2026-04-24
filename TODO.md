# Box — TODO

## Status

### Done ✅
- PRoot + talloc + libarchive cross-compiled for Android arm64 (16KB aligned)
- CLI (`box` shell script): run, exec, stop, rm, ps, logs, inspect, pull, images, up, down
- Boxfile compose format: SERVICE, FROM, PACKAGES, ENV, EXPOSE, VOLUME, WORKDIR, CMD
- Alpine 3.21 + Ubuntu 24.04 rootfs images built and released
- CI: native.yml (binaries), rootfs-alpine.yml, rootfs-ubuntu.yml
- Mirrored networking (host stack shared)
- ztproxy daemon: TCP proxy via libzt, mmap mapping file, epoll loop
- ztproxy embedded ZT controller: creates local network on-device, auto-assigns IPs
- ztproxy tested: `nc 127.0.0.1:10000` → ZT peer, 3.6MB stripped binary

### In Progress
- libzt integration into Box containers (PRoot extension + CLI wiring)

---

## 1. ztproxy hardening
- [ ] Dynamic port allocation: allocate proxy ports on-demand instead of pre-mapped slots
- [ ] Connection cleanup: timeout idle connections, handle zt_fd read in separate thread or poll
- [ ] Multiple concurrent connections per mapping slot
- [ ] Graceful shutdown: drain connections before exit
- [ ] Write ztproxy PID file for lifecycle management
- [ ] Auto-restart on first run (currently exits with code 2, caller must restart)

## 2. PRoot extension (zt_net)
- [ ] Create `src/extension/zt_net/zt_net.c` in Termux PRoot fork
- [ ] Intercept `connect()` syscall via ptrace
- [ ] Rewrite non-loopback sockaddr_in to `127.0.0.1:<proxy_port>`
- [ ] Write original destination to shared mapping file (mmap)
- [ ] Allocate proxy port per unique destination (port pool 10000-10999)
- [ ] Register extension in `src/cli/proot.c`
- [ ] Pass mapping file path and enable flag via PRoot env vars
- [ ] Skip rewrite for loopback/local destinations
- [ ] Test: `curl` inside container reaches ZT peer through ztproxy

## 3. CLI integration
- [ ] `--zt-net <network_id>` flag on `box run` — starts ztproxy, enables PRoot extension
- [ ] `--zt-local` flag on `box run` — uses embedded controller instead of remote network
- [ ] `NETWORK <network_id>` directive in Boxfile
- [ ] `NETWORK local` directive — embedded controller mode
- [ ] ztproxy lifecycle: start on `box run`, stop on `box stop`, cleanup on `box rm`
- [ ] Per-container ZT identity stored at `$BOX_CONTAINERS/$name/zt/`
- [ ] Per-container mapping file at `$BOX_RUN/$name.zt.map`
- [ ] Conditional resolv.conf: `127.0.0.53` when ZT enabled (DNS proxy)
- [ ] `box network` subcommand: show ZT status, node ID, address, peers
- [ ] `box network join <nwid>` / `box network leave <nwid>` per container

## 4. Build system
- [ ] Add libzt submodule to box repo (or reference xnet-zt's copy)
- [ ] Add ztproxy cross-compile to `build.sh`
- [ ] Add ztproxy to `native.yml` CI — produce `libztproxy.so` artifact
- [ ] Ship ztproxy in native-latest release alongside proot/bsdtar/talloc
- [ ] Clean build artifacts from git (ztproxy/build/ currently tracked)
- [ ] Add .gitignore for ztproxy/build/

## 5. Android app (Haven)
- [ ] Create Android app project (package: `sh.haven`)
- [ ] Bundle native binaries: proot, talloc, bsdtar, ztproxy in jniLibs
- [ ] Bundle `box` CLI script in assets
- [ ] Terminal emulator activity (or integrate with existing terminal app)
- [ ] First-run setup: extract binaries, pull default rootfs
- [ ] VPN-free networking via ztproxy (no VpnService needed)
- [ ] Notification for running containers
- [ ] File provider for container ↔ Android file sharing

## 6. DNS
- [ ] DNS proxy in ztproxy: listen on `127.0.0.53:53` (UDP)
- [ ] Forward DNS queries over ZT to configured resolver
- [ ] Or resolve via system DNS and return to container
- [ ] Intercept DNS in PRoot extension (rewrite UDP connect to 53)

## 7. Inbound connections
- [ ] ztproxy listens on ZT IP for inbound TCP
- [ ] Forward inbound to container's localhost port
- [ ] `EXPOSE` in Boxfile maps container port → ZT-reachable port
- [ ] `box run -p 8080` actually works over ZT (not just metadata)

## 8. UDP support
- [ ] UDP proxy in ztproxy (zts_sendto/zts_recvfrom)
- [ ] PRoot extension intercepts `sendto()` for UDP
- [ ] Mapping file entries for UDP destinations

## 9. Multi-container networking
- [ ] Containers on same local controller network can reach each other
- [ ] Container A `curl container-b:8080` resolves via local DNS
- [ ] Service discovery: write container names + IPs to shared hosts file
- [ ] Or minimal DNS server in ztproxy for `*.box.local` domain

## 10. Performance & reliability
- [ ] Benchmark: throughput through ztproxy vs direct
- [ ] Connection pooling: reuse zt_fd for same destination
- [ ] Splice/sendfile for zero-copy proxy where possible
- [ ] Watchdog: restart ztproxy if it crashes
- [ ] Metrics: connection count, bytes proxied, latency

## 11. Security
- [ ] Per-container ZT identity isolation (separate keys)
- [ ] Private network mode: controller requires authorization
- [ ] Network ACLs via ZT flow rules
- [ ] Restrict proxy to container's PID namespace (verify caller)

## 12. Documentation
- [ ] Update README with ZT networking section
- [ ] Update LIBZT_INTEGRATION.md with actual implementation details
- [ ] Usage examples: container-to-server, container-to-container, Boxfile with NETWORK
- [ ] Architecture diagram with data flow
