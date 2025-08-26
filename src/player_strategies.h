// player_strategies.h
#pragma once
#include <stdbool.h>
#include <sys/types.h>
#include "structs.h"

// Compartimos estas del player:
extern game_state_t *gs;

// Direcciones 0..7 (mismo convenio que tu player)
extern const int DX[8];
extern const int DY[8];

// Estrategias disponibles
typedef enum {
    STRAT_GREEDY_PLUS = 0,      // tu heurística base (recompensa + movilidad local)
    STRAT_SPACE_MAX,            // maximiza maniobrabilidad (2 anillos)
    STRAT_WALL_HUG,             // bordes/pared para reducir exposición
    STRAT_CENTER_CONTROL,       // prioriza centro y celdas altas
    STRAT_LANE_RUNNER,          // en tableros “angostos”, avanza por eje largo
    STRAT_CUTOFF,               // bloquea corredores y le quita movilidad a rivales cercanos
    STRAT_TWO_PLY_LIGHT,        // mirada 2-ply súper liviana (tu jugada + movilidad resultante)
    STRAT_ENDGAME_HARVEST,      // endgame: farm de valores altos cercanos
    STRAT_RANDOM_TIEBREAK       // igual a greedy pero rompe empates con random
} strategy_t;

// Dado tamaño del tablero, #jugadores y mi índice, decide la estrategia inicial
strategy_t choose_strategy(unsigned short W, unsigned short H, unsigned int num_players, int myi);

// Permite “cambiar” a estrategia de endgame cuando se vacía el tablero
bool should_switch_to_endgame(unsigned int free_cells, unsigned int total_cells);

// API principal: dado (x,y) y una estrategia, devuelve dir 0..7 o 255 si no hay jugada válida
unsigned char pick_move_strategy(strategy_t s, unsigned short x, unsigned short y);

// Helpers útiles si querés depurar
static inline bool in_bounds_int(int x, int y) {
    return x >= 0 && y >= 0 && x < (int)gs->width && y < (int)gs->height;
}
static inline int idx_int(int x, int y) { return y * (int)gs->width + x; }
