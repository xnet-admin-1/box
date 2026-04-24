#include <stddef.h>
typedef unsigned int u32_t;
typedef unsigned char u8_t;
struct sio_fd_s;
typedef struct sio_fd_s* sio_fd_t;
sio_fd_t sio_open(u8_t d) { (void)d; return NULL; }
void sio_send(u8_t c, sio_fd_t f) { (void)c; (void)f; }
u8_t sio_recv(sio_fd_t f) { (void)f; return 0; }
u32_t sio_read(sio_fd_t f, u8_t *b, u32_t s) { (void)f; (void)b; (void)s; return 0; }
u32_t sio_tryread(sio_fd_t f, u8_t *b, u32_t s) { (void)f; (void)b; (void)s; return 0; }
