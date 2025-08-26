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
#include "player_strategies.h"

//puntero a memorias compartidas
static game_sync_t  *gx = NULL;

//función para contar libres rápido
static unsigned int count_free_cells(void){
    unsigned int W = gs->width, H = gs->height;
    unsigned int tot = W*H, freec = 0;
    for (unsigned int i=0;i<tot;i++){
        if (gs->board[i] > 0) freec++;
    }
    return freec;
}

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
    sem_wait_intr(&gx->master);
    sem_wait_intr(&gx->reader);
    gx->player++;
    if (gx->player == 1) sem_wait_intr(&gx->writer);
    if (sem_post(&gx->reader) == -1) die("sem_post(reader): %s", strerror(errno));
    if (sem_post(&gx->master) == -1) die("sem_post(master): %s", strerror(errno));
}

static void reader_exit(void) {
    sem_wait_intr(&gx->reader);
    if (gx->player == 0) die("reader_exit: player underflow");
    gx->player--;
    if (gx->player == 0 && sem_post(&gx->writer) == -1)
        die("sem_post(writer): %s", strerror(errno));
    if (sem_post(&gx->reader) == -1) die("sem_post(reader): %s", strerror(errno));
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

   // ELEGIR ESTRATEGIA INICIAL
   strategy_t strat = choose_strategy(gs->width, gs->height, gs->num_players, myi);

   for (;;) {
       reader_enter();
       bool finished = gs->finished;
       unsigned short x = gs->players[myi].x;
       unsigned short y = gs->players[myi].y;
       // (opcional) switch dinámico a endgame
       unsigned int total = (unsigned int)gs->width * (unsigned int)gs->height;
       unsigned int freec = count_free_cells();
       bool to_end = should_switch_to_endgame(freec, total);
       reader_exit();

       if (finished) break;

       sem_wait_intr(&gx->movement[myi]);

       reader_enter();
       x = gs->players[myi].x;
       y = gs->players[myi].y;
       if (to_end) strat = STRAT_ENDGAME_HARVEST; // una vez activado, queda
       unsigned char dir = pick_move_strategy(strat, x, y);
       reader_exit();

       if (dir == 255) { close(STDOUT_FILENO); break; }
       if (write(STDOUT_FILENO, &dir, 1) != 1) break;
   }

    // Limpieza
    if (gs) munmap(gs, (size_t)st_state.st_size);
    if (gx) munmap(gx, (size_t)st_sync.st_size);
    return 0;
}
