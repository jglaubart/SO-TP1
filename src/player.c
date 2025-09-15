// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
// This is a personal academic project. Dear PVS-Studio, please check it.
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
    if (gx_open_rw(&gx) != 0) die("gx_open_rw: %s", strerror(errno));


    // Buscar mi índice por PID en players[]
    pid_t me = getpid();
    reader_enter(gx);
    int myi = my_index_by_pid(me);
    reader_exit(gx);

   // ELEGIR ESTRATEGIA INICIAL
   strategy_t strat = choose_strategy(gs->width, gs->height, gs->num_players, myi);

   for (;;) {
       reader_enter(gx);
       bool finished = gs->finished;
       unsigned int total = (unsigned int)gs->width * (unsigned int)gs->height;
       unsigned int freec = gs_count_free_cells(gs);
       bool to_end = should_switch_to_endgame(freec, total);
       reader_exit(gx);

       if (finished) break;

       if (sync_wait_my_turn(gx, myi) == -1) break;

       reader_enter(gx);
       if (to_end) strat = STRAT_ENDGAME_HARVEST; // una vez activado, queda
       unsigned char dir = pick_move_strategy(strat, gs, myi);
       reader_exit(gx);

       if (dir == 255) { close(STDOUT_FILENO); break; }
       if (proto_write_dir(STDOUT_FILENO, dir) != 0) break;
   }

    // Limpieza
    gs_close(gs, GS_BYTES);
    gx_close(gx);

    return 0;
}

/* ================= busca ID ================= */

static int my_index_by_pid(pid_t me) {
    unsigned int np = gs->num_players;
    if (np > 9) np = 9;
    for (unsigned int i = 0; i < np; i++) {
        if (gs->players[i].pid == me) return (int)i;
    }
    return -1;
}

