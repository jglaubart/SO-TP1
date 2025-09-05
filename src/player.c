// player.c
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
#include "shared_mem.h"
#include "sync_utils.h"
#include "game_utils.h"
#include "player_strategies.h"


//puntero a memorias compartidas
static game_sync_t  *gx = NULL;
game_state_t *gs = NULL;   

//función para contar libres rápido
static unsigned int count_free_cells(void);

static int my_index_by_pid(pid_t me);

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

    size_t GS_BYTES = 0;
    if (gs_open_ro(&gs, &GS_BYTES) != 0) die("gs_open_ro: %s", strerror(errno));
    if (gx_open_rw(&gx) != 0)            die("gx_open_rw: %s", strerror(errno));


    // Buscar mi índice por PID en players[]
    pid_t me = getpid();
    int myi = -1;

    // Es realmente necesario? creo q debeira ser suficiente con el semáforo.
    for (int tries = 0; tries < 2000 && myi < 0; tries++) { // ~2s en total antes de fallar con 1ms sleep entre busqueda
        reader_enter(gx);
        myi = my_index_by_pid(me);
        reader_exit(gx);
        if (myi < 0) usleep(1000);
    }
    if (myi < 0) die("no encontré mi pid en game_state");

   // ELEGIR ESTRATEGIA INICIAL
   strategy_t strat = choose_strategy(gs->width, gs->height, gs->num_players, myi);

   for (;;) {
       reader_enter(gx);
       bool finished = gs->finished;
       // (opcional) switch dinámico a endgame
       unsigned int total = (unsigned int)gs->width * (unsigned int)gs->height;
       unsigned int freec = count_free_cells();
       bool to_end = should_switch_to_endgame(freec, total);
       reader_exit(gx);

       if (finished) break;

       sem_wait_intr(&gx->movement[myi]);

       reader_enter(gx);
       if (to_end) strat = STRAT_ENDGAME_HARVEST; // una vez activado, queda
       unsigned char dir = pick_move_strategy(strat, gs, myi);
       reader_exit(gx);

       if (dir == 255) { close(STDOUT_FILENO); break; }
       if (write(STDOUT_FILENO, &dir, 1) != 1) break;
   }

    // Limpieza
    gs_close(gs, GS_BYTES);
    gx_close(gx);

    return 0;
}

/* ================= funciones ================= */
static unsigned int count_free_cells(void){
    unsigned int W = gs->width, H = gs->height;
    unsigned int tot = W*H, freec = 0;
    for (unsigned int i=0;i<tot;i++){
        if (gs->board[i] > 0) freec++;
    }
    return freec;
}

static int my_index_by_pid(pid_t me) {
    unsigned int np = gs->num_players;
    if (np > 9) np = 9;
    for (unsigned int i = 0; i < np; i++) {
        if (gs->players[i].pid == me) return (int)i;
    }
    return -1;
}

