#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>



/* ===== errores/señales genéricos ===== */
void die(const char *fmt, ...) __attribute__((noreturn));

/* ===== procesos/FD ===== */
const char* base_name(const char *path);
void set_cloexec(int fd, int on);

/* ===== geometría del tablero ===== */
extern const int DX[8];
extern const int DY[8];

static inline int in_bounds_wh(int x, int y, int W, int H){
    return x >= 0 && y >= 0 && x < W && y < H;
}
static inline int idx_wh(int x, int y, int W){
    return y * W + x;
}

/* ===== protocolo por pipe: 1 byte dirección (0..7) ===== */
int  proto_read_dir (int fd, unsigned char *dir_out);   /* read robusto */
int  proto_write_dir(int fd, unsigned char dir);        /* write robusto */
static inline int dir_is_valid(unsigned char d){ return d <= 7; }
