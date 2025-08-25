// player_greedy.c
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

//punteros a memorias compartidas
static game_state_t *gs = NULL;
static game_sync_t  *gx = NULL;

/* ================= funciones ================= */

//imprime a stderr un mensaje formateado y termina con _exit(1)
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

/* Lectores–Escritores con preferencia al escritor (master):
   Readers: wait(C); wait(E); if (++F == 1) wait(D); post(E); post(C);
   Readers exit: wait(E); if (--F == 0) post(D); post(E);
   Writers (master): wait(C); wait(D); ...; post(D); post(C); */
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

/* ================= Algoritmo de movimiento =================
   Direcciones (unsigned char) 0..7:
   0=arriba, luego horario: 1=arriba-der, 2=der, 3=abajo-der,
   4=abajo, 5=abajo-izq, 6=izq, 7=arriba-izq. */
static const int DX[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
static const int DY[8] = {-1,-1, 0, 1, 1, 1, 0,-1 };

static inline bool cell_is_free_reward(int v) { return v > 0; }  // 1..9
static inline bool cell_is_taken(int v) { return v <= 0; }        // 0..-8 (capturada)

/* Heurística “greedy-plus”:
   1) Entre las 8 vecinas libres, elegir la de MAYOR recompensa (1..9).
   2) En empate, preferir la que deja más vecinas libres alrededor (espacio de maniobra).
   3) Si no hay libres, devolver 0. */
   static unsigned char pick_move(unsigned short x, unsigned short y) {
    int best_dir = -1;
    int best_reward = -1;
    int best_free_neighbors = -1;

    for (unsigned char dir = 0; dir < 8; dir++) {
        int nx = (int)x + DX[dir];
        int ny = (int)y + DY[dir];
        if (!in_bounds(nx, ny)) continue;

        int v = gs->board[idx(nx, ny)];
        if (cell_is_taken(v)) continue;  // solo celdas libres con recompensa 1..9 (válidas)
        // si es peor que lo mejor visto, ni calculo free_n
        if (v < best_reward) continue;

        int free_n = 0;
        // empate en valor, miro cantidad de celdas vecinas libres
        for (int k = 0; k < 8; k++) {
            int mx = nx + DX[k], my = ny + DY[k];
            if (!in_bounds(mx, my)) continue;
            int mv = gs->board[idx(mx, my)];
            if (cell_is_free_reward(mv)) free_n++;
        }

        // criterio: mayor reward; si empata, mayor free_n
        if (v > best_reward || (v == best_reward && free_n > best_free_neighbors)) {
            best_reward = v;
            best_free_neighbors = free_n;
            best_dir = dir;
        }
    }

    // devuelve 0..7 si hay jugada; 255 si no hay ninguna vecina libre
    return (best_dir >= 0) ? (unsigned char)best_dir : 255;

}


/* ================= main ================= */

int main(int argc, char **argv) {
    // El master invoca: ./player <width> <height>
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <width> <height>\n", argv[0]);
        return 1; //error
    }
    int arg_width  = atoi(argv[1]);
    int arg_height = atoi(argv[2]);
    (void)arg_width; (void)arg_height; // evita warnings de unused si no se pasan (no son obligatorios)
    fprintf(stderr, "[%d] Jugador iniciado con tablero %dx%d\n",
            getpid(), arg_width, arg_height); //chequeo de funcionamiento

    // Abrir shm: estado solo lectura; sync lectura/escritura. ---> muere y lanza error si falla
    int fd_state = shm_open(SHM_STATE, O_RDONLY, 0);
    if (fd_state == -1) die("shm_open(%s): %s", SHM_STATE, strerror(errno));
    int fd_sync  = shm_open(SHM_SYNC,  O_RDWR,  0);
    if (fd_sync  == -1) die("shm_open(%s): %s", SHM_SYNC,  strerror(errno));

    // Mapear
    struct stat st_state, st_sync;
    if (fstat(fd_state, &st_state) == -1) die("fstat(state): %s", strerror(errno));
    if (fstat(fd_sync,  &st_sync ) == -1) die("fstat(sync ): %s", strerror(errno));

    gs = mmap(NULL, (size_t)st_state.st_size, PROT_READ, MAP_SHARED, fd_state, 0);
    if (gs == MAP_FAILED) die("mmap(state): %s", strerror(errno));
    gx = mmap(NULL, (size_t)st_sync.st_size,  PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync,  0);
    if (gx == MAP_FAILED) die("mmap(sync): %s", strerror(errno));
    close(fd_state);
    close(fd_sync);

    // Buscar mi índice por PID en players[]
    pid_t me = getpid();
    int myi = -1;
    for (int tries = 0; tries < 2000 && myi < 0; tries++) { // ~2s en total antes de fallar con 1ms sleep entre busqueda
        reader_enter();
        myi = my_index_by_pid(me);
        reader_exit();
        if (myi < 0) usleep(1000);
    }
    if (myi < 0) die("no encontré mi pid en game_state");

    // Bucle infinito principal
    for (;;) {
        // Chequear fin de juego
        reader_enter();
        bool finished = gs->finished;
        unsigned short x = gs->players[myi].x;
        unsigned short y = gs->players[myi].y;
        reader_exit();
        if (finished) break;  //salir si termino el juego

        // Esperar permiso para 1 movimiento (semaforo)
        sem_wait_intr(&gx->G[myi]);

        // Releer estado y decidir
        reader_enter();
        x = gs->players[myi].x;
        y = gs->players[myi].y;
        unsigned char dir = pick_move(x, y);
        reader_exit();

        // Si no hay jugadas posibles: cerrar stdout => el máster verá EOF y marcará "blk"
        if (dir == 255) {
            close(STDOUT_FILENO);   // provoca EOF en el máster
            break;
        }

        // Enviar 1 byte por stdout (pipe al master)
        ssize_t w = write(STDOUT_FILENO, &dir, 1);
        if (w != 1) {
            break; // falla de pipe => salir
        }
    }

    // Limpieza
    if (gs) munmap(gs, (size_t)st_state.st_size);
    if (gx) munmap(gx, (size_t)st_sync.st_size);
    return 0;
}
