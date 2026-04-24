/* socks5d — minimal high-performance SOCKS5 server
 * Binds to a specific IP:port, relays TCP via splice where possible.
 * Usage: socks5d <bind-ip> <port>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define BUF 65536

static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL)|O_NONBLOCK); }

static void relay(int a, int b) {
    char buf[BUF];
    while (1) {
        int n = recv(a, buf, BUF, 0);
        if (n <= 0) break;
        int off = 0;
        while (off < n) {
            int w = send(b, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) return;
            off += w;
        }
    }
}

struct relay_pair { int from, to; };
static void *relay_thread(void *arg) {
    struct relay_pair *p = arg;
    relay(p->from, p->to);
    shutdown(p->from, SHUT_RD);
    shutdown(p->to, SHUT_WR);
    free(p);
    return NULL;
}

static void *handle(void *arg) {
    int cfd = (int)(long)arg;
    unsigned char buf[512];
    int n;

    /* Set TCP_NODELAY */
    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct timeval tv = {30, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Greeting */
    n = recv(cfd, buf, 2, 0);
    if (n < 2 || buf[0] != 5) goto done;
    if (buf[1] > 0) recv(cfd, buf, buf[1], 0);
    buf[0] = 5; buf[1] = 0;
    send(cfd, buf, 2, MSG_NOSIGNAL);

    /* Request */
    n = recv(cfd, buf, 4, 0);
    if (n < 4 || buf[0] != 5 || buf[1] != 1) goto done;

    char host[256];
    uint16_t port;
    int atyp = buf[3];

    if (atyp == 1) {
        recv(cfd, buf, 4, 0);
        snprintf(host, sizeof(host), "%d.%d.%d.%d", buf[0], buf[1], buf[2], buf[3]);
        recv(cfd, &port, 2, 0);
        port = ntohs(port);
    } else if (atyp == 3) {
        unsigned char dlen;
        recv(cfd, &dlen, 1, 0);
        recv(cfd, host, dlen, 0);
        host[dlen] = 0;
        recv(cfd, &port, 2, 0);
        port = ntohs(port);
    } else goto done;

    /* Resolve + connect */
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        buf[0]=5; buf[1]=4; memset(buf+2,0,8);
        send(cfd, buf, 10, MSG_NOSIGNAL);
        goto done;
    }

    int rfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(rfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct timeval ctv = {10, 0};
    setsockopt(rfd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));

    if (connect(rfd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(rfd);
        buf[0]=5; buf[1]=5; memset(buf+2,0,8);
        send(cfd, buf, 10, MSG_NOSIGNAL);
        goto done;
    }
    freeaddrinfo(res);

    /* Success */
    buf[0]=5; buf[1]=0; buf[2]=0; buf[3]=1; memset(buf+4,0,6);
    send(cfd, buf, 10, MSG_NOSIGNAL);

    /* Relay with longer timeout */
    tv.tv_sec = 300;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(rfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Bidirectional relay */
    pthread_t tid;
    struct relay_pair *rp = malloc(sizeof(*rp));
    rp->from = rfd; rp->to = cfd;
    pthread_create(&tid, NULL, relay_thread, rp);

    relay(cfd, rfd);
    shutdown(cfd, SHUT_RD);
    shutdown(rfd, SHUT_WR);
    pthread_join(tid, NULL);
    close(rfd);

done:
    close(cfd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: socks5d <ip> <port>\n"); return 1; }

    signal(SIGPIPE, SIG_IGN);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(atoi(argv[2]))};
    inet_pton(AF_INET, argv[1], &sa.sin_addr);

    if (bind(sfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    listen(sfd, 256);
    printf("socks5d on %s:%s\n", argv[1], argv[2]);

    while (1) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) continue;
        pthread_t tid;
        pthread_create(&tid, NULL, handle, (void*)(long)cfd);
        pthread_detach(tid);
    }
}
