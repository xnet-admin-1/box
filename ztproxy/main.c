/*
 * ztproxy — ZeroTier userspace TCP proxy (single-threaded event loop)
 *
 * All zts_* calls happen on one thread. Local sockets use epoll.
 * zts sockets polled with non-blocking recv/send.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <android/log.h>

#include "ZeroTier.h"

#define TAG "ztproxy"
#define LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, TAG, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##__VA_ARGS__)

#define MAX_CONNS 64
#define BUF_SIZE 16384

static volatile int g_running = 1;
static volatile int g_online = 0;
static char g_zt_addr[64];
static uint8_t g_upstream_ip[4];
static uint16_t g_upstream_port = 0;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void zt_callback(struct zts_callback_msg *msg) {
    if (msg->eventCode == 144 && msg->addr) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&msg->addr->addr;
        inet_ntop(AF_INET, &sa->sin_addr, g_zt_addr, sizeof(g_zt_addr));
        LOG("ZT address: %s", g_zt_addr);
    }
    if (msg->eventCode == 2 || msg->eventCode == 35 || msg->eventCode == 37)
        g_online = 1;
}

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* Connection states */
enum {
    S_GREETING,       /* reading SOCKS5 greeting from client */
    S_REQUEST,        /* reading SOCKS5 request from client */
    S_ZT_CONNECTING,  /* zts_connect in progress (worker thread) */
    S_CONNECTED,      /* worker done, ready for relay */
    S_UPSTREAM_GREET, /* sent greeting to upstream, waiting reply */
    S_UPSTREAM_REQ,   /* sent CONNECT to upstream, waiting reply */
    S_RELAY,          /* bidirectional relay */
    S_DEAD,
};

struct conn {
    int state;
    int cfd;          /* local client fd */
    int zfd;          /* zts fd (-1 if not yet) */
    uint8_t buf[512]; /* small buffer for handshake */
    int buf_len;
    /* Relay buffers */
    uint8_t c2z[BUF_SIZE]; /* client → zt */
    int c2z_len, c2z_off;
    uint8_t z2c[BUF_SIZE]; /* zt → client */
    int z2c_len, z2c_off;
    /* Request info for upstream */
    uint8_t atyp;
    uint8_t dst_ip[4];
    uint8_t domain[260];
    int domain_len;
    uint16_t dst_port;
    char dst_str[256];
};

static struct conn conns[MAX_CONNS];
static int epfd;

static struct conn *alloc_conn(int cfd) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].state == S_DEAD && conns[i].cfd == -1) {
            memset(&conns[i], 0, sizeof(struct conn));
            conns[i].cfd = cfd;
            conns[i].zfd = -1;
            conns[i].state = S_GREETING;
            return &conns[i];
        }
    }
    return NULL;
}

static void free_conn(struct conn *c) {
    if (c->cfd >= 0) { epoll_ctl(epfd, EPOLL_CTL_DEL, c->cfd, NULL); close(c->cfd); }
    if (c->zfd >= 0) zts_close(c->zfd);
    c->cfd = -1;
    c->zfd = -1;
    c->state = S_DEAD;
}

static void epoll_mod(int fd, uint32_t events, void *ptr) {
    struct epoll_event ev = { .events = events, .data.ptr = ptr };
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

/* Build lwip sockaddr */
static void make_lwip_sa(uint8_t *sa, const uint8_t *ip, uint16_t port) {
    memset(sa, 0, 16);
    sa[0] = 16;
    sa[1] = AF_INET;
    sa[2] = (port >> 8) & 0xff;
    sa[3] = port & 0xff;
    memcpy(&sa[4], ip, 4);
}

/* Process SOCKS5 greeting */
static void on_greeting(struct conn *c) {
    int n = recv(c->cfd, c->buf + c->buf_len, sizeof(c->buf) - c->buf_len, 0);
    if (n <= 0) { free_conn(c); return; }
    c->buf_len += n;
    if (c->buf_len < 2) return;
    if (c->buf[0] != 0x05) { free_conn(c); return; }
    int need = 2 + c->buf[1];
    if (c->buf_len < need) return;
    /* Send no-auth reply */
    uint8_t reply[] = {0x05, 0x00};
    send(c->cfd, reply, 2, MSG_NOSIGNAL);
    c->buf_len = 0;
    c->state = S_REQUEST;
}

/* Connect worker — runs zts_connect + upstream handshake off the main loop */
static void *connect_worker(void *arg) {
    struct conn *c = (struct conn *)arg;

    uint8_t connect_ip[4];
    uint16_t connect_port;
    if (g_upstream_port) {
        memcpy(connect_ip, g_upstream_ip, 4);
        connect_port = g_upstream_port;
    } else {
        memcpy(connect_ip, c->dst_ip, 4);
        connect_port = c->dst_port;
    }

    uint8_t lwip_sa[16];
    make_lwip_sa(lwip_sa, connect_ip, connect_port);

    if (zts_connect(c->zfd, (struct sockaddr *)lwip_sa, 16) < 0) {
        ERR("zt connect %s:%d failed", c->dst_str, c->dst_port);
        c->state = S_DEAD; /* signal main loop to clean up */
        return NULL;
    }

    if (g_upstream_port) {
        uint8_t buf[300];
        /* Upstream greeting */
        buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00;
        zts_send(c->zfd, buf, 3, 0);
        if (zts_recv(c->zfd, buf, 2, 0) < 2 || buf[1] != 0x00) {
            ERR("upstream auth failed for %s:%d", c->dst_str, c->dst_port);
            c->state = S_DEAD;
            return NULL;
        }
        /* Upstream CONNECT */
        buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00; buf[3] = c->atyp;
        int len = 4;
        if (c->atyp == 0x01) { memcpy(buf+4, c->dst_ip, 4); len += 4; }
        else { memcpy(buf+4, c->domain, c->domain_len); len += c->domain_len; }
        uint16_t np = htons(c->dst_port);
        memcpy(buf+len, &np, 2); len += 2;
        zts_send(c->zfd, buf, len, 0);
        if (zts_recv(c->zfd, buf, 10, 0) < 4 || buf[1] != 0x00) {
            ERR("upstream connect %s:%d failed", c->dst_str, c->dst_port);
            c->state = S_DEAD;
            return NULL;
        }
    }

    LOG("proxying %s:%d", c->dst_str, c->dst_port);
    c->state = S_CONNECTED; /* signal main loop */
    return NULL;
}

/* Process SOCKS5 request */
static void on_request(struct conn *c) {
    int n = recv(c->cfd, c->buf + c->buf_len, sizeof(c->buf) - c->buf_len, 0);
    if (n <= 0) { free_conn(c); return; }
    c->buf_len += n;
    if (c->buf_len < 4) return;
    if (c->buf[0] != 0x05 || c->buf[1] != 0x01) { free_conn(c); return; }

    c->atyp = c->buf[3];
    int need;
    if (c->atyp == 0x01) {
        need = 10; /* 4 header + 4 ip + 2 port */
    } else if (c->atyp == 0x03) {
        if (c->buf_len < 5) return;
        need = 4 + 1 + c->buf[4] + 2;
    } else {
        free_conn(c); return;
    }
    if (c->buf_len < need) return;

    if (c->atyp == 0x01) {
        memcpy(c->dst_ip, c->buf + 4, 4);
        c->dst_port = (c->buf[8] << 8) | c->buf[9];
        snprintf(c->dst_str, sizeof(c->dst_str), "%d.%d.%d.%d",
            c->dst_ip[0], c->dst_ip[1], c->dst_ip[2], c->dst_ip[3]);
    } else {
        int dlen = c->buf[4];
        c->domain[0] = dlen;
        memcpy(c->domain + 1, c->buf + 5, dlen);
        c->domain_len = 1 + dlen;
        c->dst_port = (c->buf[5 + dlen] << 8) | c->buf[6 + dlen];
        memcpy(c->dst_str, c->buf + 5, dlen);
        c->dst_str[dlen] = 0;
        if (!g_upstream_port) {
            uint8_t fail[] = {0x05, 0x04, 0,0,0,0,0,0,0,0};
            send(c->cfd, fail, 10, MSG_NOSIGNAL);
            free_conn(c); return;
        }
    }

    LOG("CONNECT %s:%d", c->dst_str, c->dst_port);

    /* Create zts socket */
    c->zfd = zts_socket(AF_INET, SOCK_STREAM, 0);
    if (c->zfd < 0) {
        uint8_t fail[] = {0x05, 0x01, 0,0,0,0,0,0,0,0};
        send(c->cfd, fail, 10, MSG_NOSIGNAL);
        free_conn(c); return;
    }

    /* Send success early so client can pipeline */
    uint8_t ok[] = {0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0};
    send(c->cfd, ok, 10, MSG_NOSIGNAL);

    /* Stop watching client, spawn connect worker */
    epoll_mod(c->cfd, 0, c);
    c->state = S_ZT_CONNECTING;
    pthread_t tid;
    pthread_create(&tid, NULL, connect_worker, c);
    pthread_detach(tid);
}

/* Process upstream greeting reply */
static void on_upstream_greet(struct conn *c) {
    int n = zts_recv(c->zfd, c->buf + c->buf_len, 2 - c->buf_len, MSG_DONTWAIT);
    if (n == 0) { free_conn(c); return; }
    if (n < 0) return; /* EAGAIN */
    c->buf_len += n;
    if (c->buf_len < 2) return;
    if (c->buf[1] != 0x00) { ERR("upstream auth failed"); free_conn(c); return; }

    /* Send CONNECT request */
    uint8_t req[300];
    req[0] = 0x05; req[1] = 0x01; req[2] = 0x00; req[3] = c->atyp;
    int len = 4;
    if (c->atyp == 0x01) {
        memcpy(req + 4, c->dst_ip, 4); len += 4;
    } else {
        memcpy(req + 4, c->domain, c->domain_len); len += c->domain_len;
    }
    uint16_t np = htons(c->dst_port);
    memcpy(req + len, &np, 2); len += 2;
    zts_send(c->zfd, req, len, 0);
    c->buf_len = 0;
    c->state = S_UPSTREAM_REQ;
}

/* Process upstream CONNECT reply */
static void on_upstream_req(struct conn *c) {
    int n = zts_recv(c->zfd, c->buf + c->buf_len, 10 - c->buf_len, MSG_DONTWAIT);
    if (n == 0) { free_conn(c); return; }
    if (n < 0) return; /* EAGAIN */
    c->buf_len += n;
    if (c->buf_len < 4) return;
    /* Might get variable length reply, but we need at least 4 bytes to check status */
    if (c->buf[1] != 0x00) {
        ERR("upstream connect %s:%d failed (code %d)", c->dst_str, c->dst_port, c->buf[1]);
        free_conn(c); return;
    }
    /* Consume rest of reply (up to 10 bytes for IPv4) */
    if (c->buf_len < 10) return;

    LOG("proxying %s:%d", c->dst_str, c->dst_port);
    c->state = S_RELAY;
    c->c2z_len = c->c2z_off = 0;
    c->z2c_len = c->z2c_off = 0;
    epoll_mod(c->cfd, EPOLLIN, c);
}

/* Relay data */
static void relay_step(struct conn *c, int client_readable) {
    /* Client → ZT */
    for (;;) {
        if (c->c2z_len == 0) {
            int n = recv(c->cfd, c->c2z, BUF_SIZE, MSG_DONTWAIT);
            if (n == 0) { free_conn(c); return; }
            if (n < 0) break;
            c->c2z_len = n; c->c2z_off = 0;
        }
        while (c->c2z_off < c->c2z_len) {
            int w = zts_send(c->zfd, c->c2z + c->c2z_off, c->c2z_len - c->c2z_off, MSG_DONTWAIT);
            if (w <= 0) goto c2z_done;
            c->c2z_off += w;
        }
        c->c2z_len = 0; c->c2z_off = 0;
    }
c2z_done:
    if (c->c2z_off == c->c2z_len) { c->c2z_len = 0; c->c2z_off = 0; }

    /* ZT → Client */
    for (;;) {
        if (c->z2c_len == 0) {
            int n = zts_recv(c->zfd, c->z2c, BUF_SIZE, MSG_DONTWAIT);
            if (n == 0) { free_conn(c); return; }
            if (n < 0) break;
            c->z2c_len = n; c->z2c_off = 0;
        }
        while (c->z2c_off < c->z2c_len) {
            int w = send(c->cfd, c->z2c + c->z2c_off, c->z2c_len - c->z2c_off, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w <= 0) goto z2c_done;
            c->z2c_off += w;
        }
        c->z2c_len = 0; c->z2c_off = 0;
    }
z2c_done:
    if (c->z2c_off == c->z2c_len) { c->z2c_len = 0; c->z2c_off = 0; }
}

/* ── Local controller ────────────────────────────────────────── */

static uint64_t create_local_network(const char *zt_path, uint64_t node_id) {
    uint64_t nwid = (node_id << 24) | 0xffff01ULL;
    char nwid_str[20], dir[512], path[512], tmp[512];
    snprintf(nwid_str, sizeof(nwid_str), "%016llx", (unsigned long long)nwid);
    snprintf(tmp, sizeof(tmp), "%s/controller.d", zt_path);
    mkdir(tmp, 0700);
    snprintf(dir, sizeof(dir), "%s/controller.d/network", zt_path);
    mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/%s.json", dir, nwid_str);
    if (access(path, F_OK) == 0) return nwid;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f,
        "{\"id\":\"%s\",\"nwid\":\"%s\",\"name\":\"box-local\",\"private\":false,"
        "\"v4AssignMode\":{\"zt\":true},"
        "\"ipAssignmentPools\":[{\"ipRangeStart\":\"172.29.0.1\",\"ipRangeEnd\":\"172.29.255.254\"}],"
        "\"routes\":[{\"target\":\"172.29.0.0/16\"}],\"revision\":1,\"objtype\":\"network\","
        "\"creationTime\":%lld,\"rules\":[{\"type\":\"ACTION_ACCEPT\"}]}",
        nwid_str, nwid_str, (long long)time(NULL) * 1000);
    fclose(f);
    return nwid;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    char *zt_path = NULL;
    uint64_t nwid = 0;
    int zt_port = 29994;
    int socks_port = 0;
    int local_controller = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:n:P:s:lu:")) != -1) {
        switch (opt) {
            case 'p': zt_path = optarg; break;
            case 'n': nwid = strtoull(optarg, NULL, 16); break;
            case 'P': zt_port = atoi(optarg); break;
            case 's': socks_port = atoi(optarg); break;
            case 'l': local_controller = 1; break;
            case 'u': {
                char tmp[64];
                strncpy(tmp, optarg, sizeof(tmp)-1); tmp[63] = 0;
                char *colon = strrchr(tmp, ':');
                if (colon) { *colon = 0; inet_pton(AF_INET, tmp, g_upstream_ip); g_upstream_port = atoi(colon+1); }
                break;
            }
            default: goto usage;
        }
    }
    if (!zt_path || (!nwid && !local_controller) || !socks_port) {
usage:
        fprintf(stderr, "usage: ztproxy -p <path> -n <nwid> -s <socks-port> [-u upstream:port] [-P zt-port]\n"
                        "       ztproxy -p <path> -l -s <socks-port> [-P zt-port]\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    LOG("starting: path=%s nwid=%llx zt_port=%d socks=%d", zt_path, (unsigned long long)nwid, zt_port, socks_port);

    if (local_controller) {
        char id_path[512];
        snprintf(id_path, sizeof(id_path), "%s/identity.public", zt_path);
        FILE *idf = fopen(id_path, "r");
        if (idf) {
            char id_buf[256];
            if (fgets(id_buf, sizeof(id_buf), idf)) {
                char *colon = strchr(id_buf, ':');
                if (colon) *colon = 0;
                uint64_t nid = strtoull(id_buf, NULL, 16);
                if (nid) nwid = create_local_network(zt_path, nid);
            }
            fclose(idf);
        }
    }

    if (zts_start(zt_path, zt_callback, zt_port) != 0) { ERR("zts_start failed"); return 1; }

    LOG("waiting for online...");
    for (int i = 0; i < 30 && g_running && !g_online; i++) sleep(1);
    if (!g_online) { ERR("timeout"); return 1; }

    uint64_t node_id = zts_get_node_id();
    LOG("node %llx online", (unsigned long long)node_id);

    if (local_controller && !nwid) {
        nwid = create_local_network(zt_path, node_id);
        if (!nwid) return 1;
        zts_stop();
        return 2;
    }

    zts_join(nwid);
    LOG("waiting for address...");
    for (int i = 0; i < 60 && g_running && !g_zt_addr[0]; i++) sleep(1);
    if (!g_zt_addr[0]) { ERR("no address"); return 1; }
    LOG("ready: %s socks=%d", g_zt_addr, socks_port);

    /* Init connections */
    for (int i = 0; i < MAX_CONNS; i++) { conns[i].cfd = -1; conns[i].zfd = -1; conns[i].state = S_DEAD; }

    /* SOCKS5 listener */
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(socks_port) };
    if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { ERR("bind: %s", strerror(errno)); return 1; }
    listen(sfd, 64);
    set_nonblock(sfd);

    epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL }; /* NULL = listener */
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);

    LOG("SOCKS5 on 127.0.0.1:%d", socks_port);

    struct epoll_event events[64];

    while (g_running) {
        /* Short timeout so we can poll zts sockets */
        int nev = epoll_wait(epfd, events, 64, 1);

        for (int i = 0; i < nev; i++) {
            struct conn *c = events[i].data.ptr;
            if (!c) {
                /* Listener: accept */
                while (1) {
                    int cfd = accept(sfd, NULL, NULL);
                    if (cfd < 0) break;
                    set_nonblock(cfd);
                    struct conn *nc = alloc_conn(cfd);
                    if (!nc) { close(cfd); continue; }
                    struct epoll_event cev = { .events = EPOLLIN, .data.ptr = nc };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
                continue;
            }
            if (events[i].events & (EPOLLERR | EPOLLHUP)) { free_conn(c); continue; }
            if (c->state == S_GREETING) on_greeting(c);
            else if (c->state == S_REQUEST) on_request(c);
            else if (c->state == S_RELAY) relay_step(c, 1);
        }

        /* Poll zts-side for all active connections */
        for (int i = 0; i < MAX_CONNS; i++) {
            struct conn *c = &conns[i];
            if (c->state == S_CONNECTED) {
                /* Worker finished — transition to relay */
                c->state = S_RELAY;
                c->c2z_len = c->c2z_off = 0;
                c->z2c_len = c->z2c_off = 0;
                epoll_mod(c->cfd, EPOLLIN, c);
            } else if (c->state == S_DEAD && c->cfd >= 0) {
                /* Worker failed */
                free_conn(c);
            } else if (c->state == S_RELAY) {
                relay_step(c, 0);
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < MAX_CONNS; i++) if (conns[i].state != S_DEAD) free_conn(&conns[i]);
    close(sfd);
    close(epfd);
    zts_leave(nwid);
    zts_stop();
    return 0;
}
