// player_ff.c  (Flood Fill + heurística avanzada)

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include "structs.h"

//memorias compartidas
#define SHM_STATE "/game_state"
#define SHM_SYNC  "/game_sync"

//punteros a memorias compartidas
static game_state_t *gs = NULL;
static game_sync_t  *gx = NULL;

/* ================= utilitarios ================= */

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    _exit(1);
}

static void sem_wait_intr(sem_t *s) {
    for (;;) {
        if (sem_wait(s) == 0) return;
        if (errno != EINTR) die("sem_wait: %s", strerror(errno));
    }
}

/* Lectores–Escritores con preferencia al escritor (master) */
static void reader_enter(void) {
    sem_wait_intr(&gx->C);
    sem_wait_intr(&gx->E);
    gx->F++;
    if (gx->F == 1) sem_wait_intr(&gx->D);
    if (sem_post(&gx->E) == -1) die("sem_post(E): %s", strerror(errno));
    if (sem_post(&gx->C) == -1) die("sem_post(C): %s", strerror(errno));
}

static void reader_exit(void) {
    sem_wait_intr(&gx->E);
    if (gx->F == 0) die("reader_exit: F underflow");
    gx->F--;
    if (gx->F == 0 && sem_post(&gx->D) == -1)
        die("sem_post(D): %s", strerror(errno));
    if (sem_post(&gx->E) == -1) die("sem_post(E): %s", strerror(errno));
}

static inline int in_bounds(int x, int y) {
    return x >= 0 && y >= 0 && x < (int)gs->width && y < (int)gs->height;
}
static inline int idx(int x, int y) { return y * (int)gs->width + x; }

static int my_index_by_pid(pid_t me) {
    unsigned int np = gs->num_players;
    if (np > 9) np = 9;
    for (unsigned int i = 0; i < np; i++) {
        if (gs->players[i].pid == me) return (int)i;
    }
    return -1;
}


/* ================= Algoritmo de movimiento ================= */

static const int DX[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
static const int DY[8] = {-1,-1, 0, 1, 1, 1, 0,-1 };

static inline bool cell_is_taken(int v) { return v <= 0; }

// calcula suma de recompensas en un radio alrededor
static int local_heat(const game_state_t *gs, int x, int y, int radius) {
    int sum = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int nx = x + dx, ny = y + dy;
            if (!in_bounds(nx, ny)) continue;
            int v = gs->board[idx(nx, ny)];
            if (v > 0) sum += v;
        }
    }
    return sum;
}

static unsigned char pick_move(unsigned short x, unsigned short y) {
    int best_dir = -1;
    int best_reward = -1;
    int best_heat = -1;

    for (unsigned char dir = 0; dir < 8; dir++) {
        int nx = x + DX[dir];
        int ny = y + DY[dir];
        if (!in_bounds(nx, ny)) continue;

        int v = gs->board[idx(nx, ny)];
        if (cell_is_taken(v)) continue; // ocupada

        if (v > best_reward) {
            best_reward = v;
            best_heat = local_heat(gs, nx, ny, 3);
            best_dir = dir;
        } else if (v == best_reward) {
            int heat = local_heat(gs, nx, ny, 3);
            if (heat > best_heat) {
                best_heat = heat;
                best_dir = dir;
            }
        }
    }

    return (best_dir >= 0) ? (unsigned char)best_dir : 0;
}




/* ================= main ================= */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <width> <height>\n", argv[0]);
        return 1;
    }
    int arg_width  = atoi(argv[1]);
    int arg_height = atoi(argv[2]);
    (void)arg_width; (void)arg_height;

    fprintf(stderr, "[%d] Jugador-FF iniciado con tablero %dx%d\n",
            getpid(), arg_width, arg_height);

    // abrir shm
    int fd_state = shm_open(SHM_STATE, O_RDONLY, 0);
    if (fd_state == -1) die("shm_open(%s): %s", SHM_STATE, strerror(errno));
    int fd_sync  = shm_open(SHM_SYNC,  O_RDWR,  0);
    if (fd_sync  == -1) die("shm_open(%s): %s", SHM_SYNC, strerror(errno));

    struct stat st_state, st_sync;
    if (fstat(fd_state, &st_state) == -1) die("fstat(state): %s", strerror(errno));
    if (fstat(fd_sync,  &st_sync ) == -1) die("fstat(sync ): %s", strerror(errno));

    gs = mmap(NULL, (size_t)st_state.st_size, PROT_READ, MAP_SHARED, fd_state, 0);
    if (gs == MAP_FAILED) die("mmap(state): %s", strerror(errno));
    gx = mmap(NULL, (size_t)st_sync.st_size,  PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync,  0);
    if (gx == MAP_FAILED) die("mmap(sync): %s", strerror(errno));
    close(fd_state);
    close(fd_sync);

    // buscar mi índice
    pid_t me = getpid();
    int myi = -1;
    for (int tries = 0; tries < 2000 && myi < 0; tries++) {
        reader_enter();
        myi = my_index_by_pid(me);
        reader_exit();
        if (myi < 0) usleep(1000);
    }
    if (myi < 0) die("no encontré mi pid en game_state");

    // loop principal
    for (;;) {
        reader_enter();
        bool finished = gs->finished;
        unsigned short x = gs->players[myi].x;
        unsigned short y = gs->players[myi].y;
        reader_exit();
        if (finished) break;

        sem_wait_intr(&gx->G[myi]);

        reader_enter();
        x = gs->players[myi].x;
        y = gs->players[myi].y;
        unsigned char dir = pick_move(x, y);
        reader_exit();

        ssize_t w = write(STDOUT_FILENO, &dir, 1);
        if (w != 1) break;
    }

    if (gs) munmap(gs, (size_t)st_state.st_size);
    if (gx) munmap(gx, (size_t)st_sync.st_size);
    return 0;
}
