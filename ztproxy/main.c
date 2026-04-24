/*
 * ztproxy — ZeroTier userspace TCP proxy for Box containers
 *
 * Accepts local TCP connections on proxy ports (10000+), looks up the
 * real destination in a shared mapping file, connects via libzt, and
 * proxies bidirectionally.
 *
 * Usage: ztproxy -p <zt-data-path> -n <network-id> [-P <zt-port>]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <android/log.h>

#include "ZeroTier.h"

#define TAG "ztproxy"
#define LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, TAG, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##__VA_ARGS__)

/* Mapping file: 1000 slots, mmap'd, shared with PRoot extension */
#define MAP_SLOTS    1000
#define MAP_PORT_BASE 10000

struct map_entry {
    uint16_t proxy_port;   /* local port (MAP_PORT_BASE + index) */
    uint8_t  addr[4];      /* destination IPv4 */
    uint16_t port;          /* destination port */
    uint8_t  flags;         /* 1=active, 0=free */
    uint8_t  _pad;
};

/* Per-connection state */
#define MAX_CONNS 256
#define BUF_SIZE  4096

struct conn {
    int local_fd;   /* kernel TCP (from container via localhost) */
    int zt_fd;      /* libzt socket (to ZT destination) */
    int active;
};

static volatile int g_running = 1;
static volatile int g_online = 0;
static struct map_entry *g_map;
static int g_epfd;
static struct conn g_conns[MAX_CONNS];
static int g_listen_fds[MAP_SLOTS]; /* one listener per active mapping */
static char g_zt_addr[64];

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* Build lwip-compatible sockaddr (has sin_len at offset 0) */
static void make_lwip_sa(uint8_t *sa, const uint8_t *ip4, uint16_t port) {
    memset(sa, 0, 16);
    sa[0] = 16;                     /* sin_len */
    sa[1] = AF_INET;                /* sin_family */
    sa[2] = (port >> 8) & 0xff;    /* sin_port high */
    sa[3] = port & 0xff;            /* sin_port low */
    memcpy(&sa[4], ip4, 4);         /* sin_addr */
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Find a free conn slot */
static struct conn *alloc_conn(void) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!g_conns[i].active) {
            memset(&g_conns[i], 0, sizeof(struct conn));
            g_conns[i].active = 1;
            g_conns[i].local_fd = -1;
            g_conns[i].zt_fd = -1;
            return &g_conns[i];
        }
    }
    return NULL;
}

static void free_conn(struct conn *c) {
    if (c->local_fd >= 0) { epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->local_fd, NULL); close(c->local_fd); }
    if (c->zt_fd >= 0) zts_close(c->zt_fd);
    c->active = 0;
}

/* Look up mapping for a proxy port */
static struct map_entry *lookup_map(uint16_t proxy_port) {
    int idx = proxy_port - MAP_PORT_BASE;
    if (idx < 0 || idx >= MAP_SLOTS) return NULL;
    struct map_entry *e = &g_map[idx];
    return (e->flags & 1) ? e : NULL;
}

/* Create a listener on a proxy port */
static int create_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    set_nonblock(fd);
    return fd;
}

/* Accept a local connection and connect to ZT destination */
static void handle_accept(int listen_fd, uint16_t proxy_port) {
    int local_fd = accept(listen_fd, NULL, NULL);
    if (local_fd < 0) return;

    struct map_entry *e = lookup_map(proxy_port);
    if (!e) { close(local_fd); return; }

    struct conn *c = alloc_conn();
    if (!c) { close(local_fd); return; }

    char dst[32];
    snprintf(dst, sizeof(dst), "%d.%d.%d.%d", e->addr[0], e->addr[1], e->addr[2], e->addr[3]);
    LOG("proxy %d → %s:%d", proxy_port, dst, e->port);

    /* Connect via libzt */
    int zt_fd = zts_socket(AF_INET, SOCK_STREAM, 0);
    if (zt_fd < 0) { close(local_fd); c->active = 0; return; }

    uint8_t sa[16];
    make_lwip_sa(sa, e->addr, e->port);
    if (zts_connect(zt_fd, (struct sockaddr *)sa, 16) < 0) {
        ERR("zt connect to %s:%d failed", dst, e->port);
        close(local_fd);
        zts_close(zt_fd);
        c->active = 0;
        return;
    }

    c->local_fd = local_fd;
    c->zt_fd = zt_fd;

    set_nonblock(local_fd);

    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, local_fd, &ev);

    LOG("connected %d↔zt(%s:%d)", local_fd, dst, e->port);
}

/* Proxy data between local and zt */
static void handle_data(struct conn *c) {
    char buf[BUF_SIZE];

    /* local → zt */
    int n = read(c->local_fd, buf, sizeof(buf));
    if (n > 0) {
        zts_send(c->zt_fd, buf, n, 0);
    } else if (n == 0) {
        free_conn(c);
        return;
    }

    /* zt → local (non-blocking poll) */
    n = zts_recv(c->zt_fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0) {
        write(c->local_fd, buf, n);
    } else if (n == 0) {
        free_conn(c);
    }
}

/* libzt event callback */
static void zt_callback(struct zts_callback_msg *msg) {
    if (msg->eventCode == 144 && msg->addr) { /* ADDR_ADDED_IP4 */
        struct sockaddr_in *sa = (struct sockaddr_in *)&msg->addr->addr;
        inet_ntop(AF_INET, &sa->sin_addr, g_zt_addr, sizeof(g_zt_addr));
        LOG("ZT address: %s", g_zt_addr);
    }
    if (msg->eventCode == 2 || msg->eventCode == 35 || msg->eventCode == 37)
        g_online = 1;
    LOG("event %d", msg->eventCode);
}

/* Open or create mapping file */
static struct map_entry *open_map(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return NULL;
    size_t sz = MAP_SLOTS * sizeof(struct map_entry);
    ftruncate(fd, sz);
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (p == MAP_FAILED) ? NULL : (struct map_entry *)p;
}

int main(int argc, char **argv) {
    char *zt_path = NULL;
    uint64_t nwid = 0;
    int zt_port = 29994;
    char *map_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:n:P:m:")) != -1) {
        switch (opt) {
            case 'p': zt_path = optarg; break;
            case 'n': nwid = strtoull(optarg, NULL, 16); break;
            case 'P': zt_port = atoi(optarg); break;
            case 'm': map_path = optarg; break;
            default:
                fprintf(stderr, "usage: ztproxy -p <zt-path> -n <network-id> [-P port] [-m map-file]\n");
                return 1;
        }
    }

    if (!zt_path || !nwid) {
        fprintf(stderr, "usage: ztproxy -p <zt-path> -n <network-id> [-P port] [-m map-file]\n");
        return 1;
    }

    if (!map_path) {
        static char default_map[256];
        snprintf(default_map, sizeof(default_map), "%s/ztproxy.map", zt_path);
        map_path = default_map;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    LOG("starting: path=%s nwid=%llx port=%d map=%s", zt_path, (unsigned long long)nwid, zt_port, map_path);

    /* Open mapping file */
    g_map = open_map(map_path);
    if (!g_map) { ERR("failed to open map: %s", map_path); return 1; }

    /* Init libzt */
    if (zts_start(zt_path, zt_callback, zt_port) != 0) {
        ERR("zts_start failed"); return 1;
    }

    /* Wait for online */
    LOG("waiting for node online...");
    for (int i = 0; i < 30 && g_running && !g_online; i++) sleep(1);
    if (!g_online) { ERR("node never came online"); return 1; }

    /* Join network */
    LOG("joining %llx...", (unsigned long long)nwid);
    zts_join(nwid);
    LOG("node id: %llx", (unsigned long long)zts_get_node_id());

    /* Wait for address */
    LOG("waiting for address...");
    for (int i = 0; i < 60 && g_running && g_zt_addr[0] == 0; i++) sleep(1);
    if (g_zt_addr[0] == 0) { ERR("no address after 60s"); return 1; }
    LOG("ready: %s", g_zt_addr);

    /* Setup epoll */
    g_epfd = epoll_create1(0);
    memset(g_listen_fds, -1, sizeof(g_listen_fds));

    /* Main loop */
    struct epoll_event events[64];
    while (g_running) {
        /* Scan mapping file for new entries, create listeners */
        for (int i = 0; i < MAP_SLOTS; i++) {
            if ((g_map[i].flags & 1) && g_listen_fds[i] < 0) {
                uint16_t port = MAP_PORT_BASE + i;
                int fd = create_listener(port);
                if (fd >= 0) {
                    g_listen_fds[i] = fd;
                    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = port };
                    epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev);
                    LOG("listening on 127.0.0.1:%d → %d.%d.%d.%d:%d",
                        port, g_map[i].addr[0], g_map[i].addr[1],
                        g_map[i].addr[2], g_map[i].addr[3], g_map[i].port);
                }
            }
        }

        int nev = epoll_wait(g_epfd, events, 64, 1000);
        for (int i = 0; i < nev; i++) {
            if (events[i].data.u32 >= MAP_PORT_BASE &&
                events[i].data.u32 < MAP_PORT_BASE + MAP_SLOTS) {
                /* Listener accept */
                handle_accept(g_listen_fds[events[i].data.u32 - MAP_PORT_BASE],
                              events[i].data.u32);
            } else if (events[i].data.ptr) {
                /* Data on proxied connection */
                handle_data((struct conn *)events[i].data.ptr);
            }
        }
    }

    LOG("shutting down...");
    zts_leave(nwid);
    zts_stop();
    return 0;
}
