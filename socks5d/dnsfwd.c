/* dnsfwd — UDP+TCP DNS forwarder. Binds to <ip>:53, forwards to 127.0.0.53 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static struct sockaddr_in g_upstream;

static void *tcp_conn(void *arg) {
    int cfd = (int)(long)arg;
    struct timeval tv = {5, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[4096];
    while (1) {
        uint16_t len;
        if (recv(cfd, &len, 2, MSG_WAITALL) != 2) break;
        len = ntohs(len);
        if (len > sizeof(buf)) break;
        if (recv(cfd, buf, len, MSG_WAITALL) != len) break;
        int ufd = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(ufd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sendto(ufd, buf, len, 0, (struct sockaddr*)&g_upstream, sizeof(g_upstream));
        int rn = recv(ufd, buf, sizeof(buf), 0);
        close(ufd);
        if (rn <= 0) break;
        uint16_t rlen = htons(rn);
        send(cfd, &rlen, 2, 0);
        send(cfd, buf, rn, 0);
    }
    close(cfd);
    return NULL;
}

static void *tcp_accept(void *arg) {
    int tfd = (int)(long)arg;
    while (1) {
        int c = accept(tfd, NULL, NULL);
        if (c < 0) continue;
        pthread_t t;
        pthread_create(&t, NULL, tcp_conn, (void*)(long)c);
        pthread_detach(t);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: dnsfwd <bind-ip>\n"); return 1; }
    int one = 1;
    g_upstream = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(53)};
    inet_pton(AF_INET, "127.0.0.53", &g_upstream.sin_addr);

    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(53)};
    inet_pton(AF_INET, argv[1], &sa.sin_addr);

    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(ufd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(ufd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("udp bind"); return 1; }

    int tfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(tfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(tfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("tcp bind"); return 1; }
    listen(tfd, 32);

    printf("dnsfwd on %s:53 (udp+tcp) -> 127.0.0.53:53\n", argv[1]);

    pthread_t tid;
    pthread_create(&tid, NULL, tcp_accept, (void*)(long)tfd);

    char buf[4096];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int n = recvfrom(ufd, buf, sizeof(buf), 0, (struct sockaddr*)&client, &clen);
        if (n <= 0) continue;
        int fwd = socket(AF_INET, SOCK_DGRAM, 0);
        struct timeval tv = {5, 0};
        setsockopt(fwd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sendto(fwd, buf, n, 0, (struct sockaddr*)&g_upstream, sizeof(g_upstream));
        int rn = recv(fwd, buf, sizeof(buf), 0);
        close(fwd);
        if (rn > 0) sendto(ufd, buf, rn, 0, (struct sockaddr*)&client, clen);
    }
}
