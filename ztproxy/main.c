/*
 * ztproxy — ZeroTier userspace TCP proxy
 *
 * Modes:
 *   -m <map-file>   Mapping file mode (for PRoot extension)
 *   -s <port>       SOCKS5 mode (standalone proxy)
 *
 * Usage:
 *   ztproxy -p <path> -n <nwid> -s 1080          # SOCKS5 proxy
 *   ztproxy -p <path> -n <nwid> -m map.bin        # mapping file mode
 *   ztproxy -p <path> -l -s 1080                  # local controller + SOCKS5
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <android/log.h>
#include <semaphore.h>

#include "ZeroTier.h"

#define TAG "ztproxy"
#define LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, TAG, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##__VA_ARGS__)

#define BUF_SIZE 16384
#define MAX_CONCURRENT 8

static volatile int g_running = 1;
static volatile int g_online = 0;
static char g_zt_addr[64];
static uint8_t g_upstream_ip[4];
static uint16_t g_upstream_port = 0;
static sem_t g_conn_sem;
static pthread_mutex_t g_zts_lock = PTHREAD_MUTEX_INITIALIZER;

/* Thread-safe wrappers for zts calls */
static int safe_zts_socket(int af, int type, int proto) {
    pthread_mutex_lock(&g_zts_lock);
    int r = zts_socket(af, type, proto);
    pthread_mutex_unlock(&g_zts_lock);
    return r;
}
static int safe_zts_connect(int fd, const struct sockaddr *sa, int len) {
    pthread_mutex_lock(&g_zts_lock);
    int r = zts_connect(fd, sa, len);
    pthread_mutex_unlock(&g_zts_lock);
    return r;
}
static int safe_zts_send(int fd, const void *buf, int len, int flags) {
    int total = 0;
    while (total < len) {
        pthread_mutex_lock(&g_zts_lock);
        int r = zts_send(fd, (const char*)buf + total, len - total, flags | MSG_DONTWAIT);
        pthread_mutex_unlock(&g_zts_lock);
        if (r > 0) { total += r; continue; }
        if (r == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
        return -1;
    }
    return total;
}
static int safe_zts_recv(int fd, void *buf, int len, int flags) {
    while (1) {
        pthread_mutex_lock(&g_zts_lock);
        int r = zts_recv(fd, buf, len, flags | MSG_DONTWAIT);
        pthread_mutex_unlock(&g_zts_lock);
        if (r > 0 || r == 0) return r;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
        return -1;
    }
}
static int safe_zts_close(int fd) {
    pthread_mutex_lock(&g_zts_lock);
    int r = zts_close(fd);
    pthread_mutex_unlock(&g_zts_lock);
    return r;
}

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

/* ── SOCKS5 handler (one thread per connection) ─────────────── */

struct relay_ctx { int from_fd; int to_fd; volatile int *done; };

static void *relay_zt_to_local(void *arg) {
    struct relay_ctx *r = (struct relay_ctx *)arg;
    uint8_t rbuf[BUF_SIZE];
    while (1) {
        int n = safe_zts_recv(r->from_fd, rbuf, sizeof(rbuf), 0);
        if (n <= 0) break;
        int sent = 0;
        while (sent < n) {
            int w = write(r->to_fd, rbuf + sent, n - sent);
            if (w <= 0) goto out;
            sent += w;
        }
    }
out:
    *r->done = 1;
    free(r);
    return NULL;
}

struct socks_arg {
    int client_fd;
};

static void *socks5_thread(void *arg) {
    struct socks_arg *sa = (struct socks_arg *)arg;
    int cfd = sa->client_fd;
    free(sa);

    sem_wait(&g_conn_sem);

    uint8_t buf[BUF_SIZE];

    /* SOCKS5 greeting */
    int n = recv(cfd, buf, 2, 0);
    if (n < 2 || buf[0] != 0x05) goto done;
    int nmethods = buf[1];
    if (nmethods > 0) recv(cfd, buf, nmethods, 0); /* consume methods */
    buf[0] = 0x05; buf[1] = 0x00; /* no auth */
    send(cfd, buf, 2, 0);

    /* SOCKS5 request */
    n = recv(cfd, buf, 4, 0);
    if (n < 4 || buf[0] != 0x05 || buf[1] != 0x01) goto done; /* only CONNECT */

    uint8_t atyp = buf[3];
    uint8_t dst_ip[4];
    uint16_t dst_port;
    char dst_str[256];
    uint8_t domain_buf[260]; /* for atyp=3: len + domain */
    int domain_len = 0;

    if (atyp == 0x01) { /* IPv4 */
        recv(cfd, dst_ip, 4, 0);
        recv(cfd, &dst_port, 2, 0);
        dst_port = ntohs(dst_port);
        snprintf(dst_str, sizeof(dst_str), "%d.%d.%d.%d", dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
    } else if (atyp == 0x03) { /* Domain */
        uint8_t dlen;
        recv(cfd, &dlen, 1, 0);
        recv(cfd, domain_buf + 1, dlen, 0);
        domain_buf[0] = dlen;
        domain_len = 1 + dlen;
        recv(cfd, &dst_port, 2, 0);
        dst_port = ntohs(dst_port);
        memcpy(dst_str, domain_buf + 1, dlen);
        dst_str[dlen] = 0;
        if (!g_upstream_port) {
            /* No upstream — can't resolve domains */
            buf[0] = 0x05; buf[1] = 0x04; memset(buf+2, 0, 8);
            send(cfd, buf, 10, 0);
            goto done;
        }
    } else {
        goto done;
    }

    LOG("SOCKS5 CONNECT %s:%d", dst_str, dst_port);

    /* Connect via libzt — to upstream SOCKS5 or directly */
    uint8_t connect_ip[4];
    uint16_t connect_port;
    if (g_upstream_port) {
        memcpy(connect_ip, g_upstream_ip, 4);
        connect_port = g_upstream_port;
    } else {
        memcpy(connect_ip, dst_ip, 4);
        connect_port = dst_port;
    }

    int zt_fd = safe_zts_socket(AF_INET, SOCK_STREAM, 0);
    if (zt_fd < 0) {
        buf[0] = 0x05; buf[1] = 0x01; memset(buf+2, 0, 8);
        send(cfd, buf, 10, 0);
        goto done;
    }

    /* lwip sockaddr: sin_len + sin_family + sin_port(BE) + sin_addr */
    uint8_t lwip_sa[16];
    memset(lwip_sa, 0, 16);
    lwip_sa[0] = 16;
    lwip_sa[1] = AF_INET;
    lwip_sa[2] = (connect_port >> 8) & 0xff;
    lwip_sa[3] = connect_port & 0xff;
    memcpy(&lwip_sa[4], connect_ip, 4);

    if (safe_zts_connect(zt_fd, (struct sockaddr *)lwip_sa, 16) < 0) {
        ERR("zt connect %s:%d failed", dst_str, dst_port);
        safe_zts_close(zt_fd);
        buf[0] = 0x05; buf[1] = 0x05; memset(buf+2, 0, 8);
        send(cfd, buf, 10, 0);
        goto done;
    }

    /* SOCKS5 success reply */
    buf[0] = 0x05; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
    memset(buf+4, 0, 6);
    send(cfd, buf, 10, 0);

    /* If upstream, do SOCKS5 handshake with upstream */
    if (g_upstream_port) {
        /* greeting */
        buf[0] = 0x05; buf[1] = 1; buf[2] = 0;
        safe_zts_send(zt_fd, buf, 3, 0);
        if (safe_zts_recv(zt_fd, buf, 2, 0) < 2 || buf[1] != 0) {
            ERR("upstream auth failed"); safe_zts_close(zt_fd); goto done;
        }
        /* CONNECT request */
        buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00;
        buf[3] = atyp;
        int reqlen = 4;
        if (atyp == 0x01) {
            memcpy(buf + 4, dst_ip, 4); reqlen += 4;
        } else if (atyp == 0x03) {
            memcpy(buf + 4, domain_buf, domain_len); reqlen += domain_len;
        }
        uint16_t np = htons(dst_port);
        memcpy(buf + reqlen, &np, 2); reqlen += 2;
        safe_zts_send(zt_fd, buf, reqlen, 0);
        if (safe_zts_recv(zt_fd, buf, 10, 0) < 4 || buf[1] != 0) {
            ERR("upstream connect %s:%d failed", dst_str, dst_port);
            safe_zts_close(zt_fd); goto done;
        }
    }

    LOG("proxying %s:%d", dst_str, dst_port);

    /* Bidirectional proxy — two threads */
    volatile int relay_done = 0;

    struct relay_ctx *rctx = malloc(sizeof(struct relay_ctx));
    rctx->from_fd = zt_fd; rctx->to_fd = cfd; rctx->done = &relay_done;

    pthread_t relay_tid;
    pthread_create(&relay_tid, NULL, relay_zt_to_local, rctx);
    pthread_detach(relay_tid);

    /* This thread: local → zt */
    while (!relay_done) {
        n = recv(cfd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        int sent = 0;
        while (sent < n) {
            int w = safe_zts_send(zt_fd, buf + sent, n - sent, 0);
            if (w <= 0) goto proxy_done;
            sent += w;
        }
    }

proxy_done:
    safe_zts_close(zt_fd);
done:
    close(cfd);
    sem_post(&g_conn_sem);
    return NULL;
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
    if (access(path, F_OK) == 0) { LOG("network %s exists", nwid_str); return nwid; }
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
    LOG("created network %s", nwid_str);
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
                strncpy(tmp, optarg, sizeof(tmp)-1);
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

    sem_init(&g_conn_sem, 0, MAX_CONCURRENT);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    LOG("starting: path=%s nwid=%llx zt_port=%d socks=%d", zt_path, (unsigned long long)nwid, zt_port, socks_port);

    /* Local controller: pre-create network */
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

    /* Start libzt */
    if (zts_start(zt_path, zt_callback, zt_port) != 0) { ERR("zts_start failed"); return 1; }

    LOG("waiting for online...");
    for (int i = 0; i < 30 && g_running && !g_online; i++) sleep(1);
    if (!g_online) { ERR("timeout"); return 1; }

    uint64_t node_id = zts_get_node_id();
    LOG("node %llx online", (unsigned long long)node_id);

    if (local_controller && !nwid) {
        nwid = create_local_network(zt_path, node_id);
        if (!nwid) return 1;
        LOG("first run — restart to load network");
        zts_stop();
        return 2;
    }

    LOG("joining %llx...", (unsigned long long)nwid);
    zts_join(nwid);

    LOG("waiting for address...");
    for (int i = 0; i < 60 && g_running && !g_zt_addr[0]; i++) sleep(1);
    if (!g_zt_addr[0]) { ERR("no address"); return 1; }
    LOG("ready: %s on port %d, SOCKS5 on :%d", g_zt_addr, zt_port, socks_port);

    /* SOCKS5 listener */
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(socks_port) };
    if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { ERR("bind %d: %s", socks_port, strerror(errno)); return 1; }
    listen(sfd, 32);
    LOG("SOCKS5 listening on 127.0.0.1:%d", socks_port);

    while (g_running) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(sfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) { if (errno == EINTR) continue; break; }

        struct socks_arg *arg = malloc(sizeof(struct socks_arg));
        arg->client_fd = cfd;
        pthread_t tid;
        pthread_create(&tid, NULL, socks5_thread, arg);
        pthread_detach(tid);
    }

    close(sfd);
    zts_leave(nwid);
    zts_stop();
    return 0;
}
