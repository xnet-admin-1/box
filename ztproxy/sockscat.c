/* sockscat — connect through SOCKS5 and relay stdin/stdout
 * Usage: sockscat <socks-host> <socks-port> <dest-host> <dest-port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: sockscat socks-ip socks-port dest-ip dest-port\n"); return 1; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[2])) };
    inet_pton(AF_INET, argv[1], &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("connect"); return 1; }

    /* SOCKS5 greeting: version=5, 1 method, no-auth */
    uint8_t buf[512];
    buf[0]=5; buf[1]=1; buf[2]=0;
    write(fd, buf, 3);
    read(fd, buf, 2);
    if (buf[0]!=5 || buf[1]!=0) { fprintf(stderr, "socks auth failed\n"); return 1; }

    /* SOCKS5 CONNECT */
    uint8_t dst_ip[4];
    inet_pton(AF_INET, argv[3], dst_ip);
    uint16_t dst_port = htons(atoi(argv[4]));
    buf[0]=5; buf[1]=1; buf[2]=0; buf[3]=1;
    memcpy(buf+4, dst_ip, 4);
    memcpy(buf+8, &dst_port, 2);
    write(fd, buf, 10);
    read(fd, buf, 10);
    if (buf[1]!=0) { fprintf(stderr, "socks connect failed: %d\n", buf[1]); return 1; }

    /* Relay stdin/stdout ↔ socket */
    struct pollfd pfd[2] = {{ .fd=0, .events=POLLIN }, { .fd=fd, .events=POLLIN }};
    while (1) {
        int r = poll(pfd, 2, -1);
        if (r <= 0) break;
        if (pfd[0].revents & POLLIN) {
            int n = read(0, buf, sizeof(buf));
            if (n <= 0) break;
            write(fd, buf, n);
        }
        if (pfd[1].revents & POLLIN) {
            int n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            write(1, buf, n);
        }
        if (pfd[0].revents & POLLHUP || pfd[1].revents & POLLHUP) break;
    }
    close(fd);
    return 0;
}
