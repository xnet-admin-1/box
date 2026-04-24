/* lb — TCP round-robin load balancer
 * Usage: lb <listen-port> <backend1-port> <backend2-port> ...
 * Binds to 127.0.0.1, round-robins connections to backends on 127.0.0.1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <poll.h>

static int g_backends[16];
static int g_nbackends;
static int g_next;

struct relay_arg { int a; int b; };

static void *relay(void *arg) {
    struct relay_arg *r = arg;
    char buf[16384];
    while (1) {
        int n = recv(r->a, buf, sizeof(buf), 0);
        if (n <= 0) break;
        int s = 0;
        while (s < n) { int w = send(r->b, buf + s, n - s, 0); if (w <= 0) goto out; s += w; }
    }
out:
    shutdown(r->a, SHUT_RD);
    shutdown(r->b, SHUT_WR);
    free(r);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: lb <port> <backend-port> ...\n"); return 1; }
    int listen_port = atoi(argv[1]);
    g_nbackends = argc - 2;
    for (int i = 0; i < g_nbackends; i++) g_backends[i] = atoi(argv[2 + i]);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001), .sin_port = htons(listen_port) };
    bind(sfd, (struct sockaddr *)&sa, sizeof(sa));
    listen(sfd, 64);

    while (1) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) continue;
        int port = g_backends[g_next++ % g_nbackends];
        int bfd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ba = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001), .sin_port = htons(port) };
        if (connect(bfd, (struct sockaddr *)&ba, sizeof(ba)) < 0) { close(cfd); close(bfd); continue; }

        struct relay_arg *r1 = malloc(sizeof(*r1)); r1->a = cfd; r1->b = bfd;
        struct relay_arg *r2 = malloc(sizeof(*r2)); r2->a = bfd; r2->b = cfd;
        pthread_t t1, t2;
        pthread_create(&t1, NULL, relay, r1); pthread_detach(t1);
        pthread_create(&t2, NULL, relay, r2); pthread_detach(t2);
    }
}
