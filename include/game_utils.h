#ifndef GAME_UTILS_H
#define GAME_UTILS_H

#pragma once
#include <stdio.h>
#include <unistd.h>

/* Errores/señales genéricos */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf,1,2)));
void die_fast(const char *fmt, ...) __attribute__((noreturn, format(printf,1,2)));

/* Procesos/FD */
//devuelve el último componente del path
const char* base_name(const char *path);
// setea/limpia el flag FD_CLOEXEC
void set_cloexec(int fd, int on);

/* Geometría del tablero */
extern const int DX[8];
extern const int DY[8];
typedef enum {
    DIR_N=0, DIR_NE=1, DIR_E=2, DIR_SE=3, DIR_S=4, DIR_SW=5, DIR_W=6, DIR_NW=7
} Direction;


static inline int in_bounds_wh(int x, int y, int W, int H){
    return x >= 0 && y >= 0 && x < W && y < H;
}
static inline int idx_wh(int x, int y, int W){
    return y * W + x;
}

/* Protocolo por pipe: 1 byte dirección (0..7) */
int  proto_read_dir (int fd, unsigned char *dir_out);
int  proto_write_dir(int fd, unsigned char dir);
static inline int dir_is_valid(unsigned char d){ return d <= 7; }

#endif
