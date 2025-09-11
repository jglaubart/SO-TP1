#define _DEFAULT_SOURCE
#include "game_utils.h"
#include <string.h>
#include <fcntl.h>
#include <errno.h>

/*die con exit*/
void die(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/*die con _exit para procesos hijos*/
void die_fast(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    _exit(1);
}



/* helpers procesos/FD */
const char* base_name(const char *path){
    const char *s = strrchr(path, '/'); return s ? s + 1 : path;
}
void set_cloexec(int fd, int on){
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) die("fcntl(F_GETFD): %s", strerror(errno));
    if (on) flags |= FD_CLOEXEC; else flags &= ~FD_CLOEXEC;
    if (fcntl(fd, F_SETFD, flags) == -1) die("fcntl(F_SETFD): %s", strerror(errno));
}

/* direcciones 8-neighborhood (igual convenio que tu código) */
const int DX[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
const int DY[8] = {-1,-1, 0, 1, 1, 1, 0,-1 };

/* protocolo 1 byte (EINTR/EAGAIN) */
int proto_read_dir(int fd, unsigned char *dir_out){
    if (!dir_out) return -1;
    for (;;) {
        ssize_t r = read(fd, dir_out, 1);
        if (r == 1) return 0;         /* ok */
        if (r == 0) return 1;         /* EOF */
        if (r < 0 && (errno == EINTR)) continue;
        if (r < 0 && (errno == EAGAIN)) { usleep(1000); continue; }
        return -1;                    /* error */
    }
}

int proto_write_dir(int fd, unsigned char dir){
    const unsigned char b = dir;
    for (;;) {
        ssize_t w = write(fd, &b, 1);
        if (w == 1) return 0;         /* ok */
        if (w < 0 && (errno == EINTR)) continue;
        if (w < 0 && (errno == EAGAIN)) { usleep(1000); continue; }
        return -1;                    /* error */
    }
}
