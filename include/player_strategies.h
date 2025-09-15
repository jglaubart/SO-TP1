// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#ifndef PLAYER_STRATEGIES_H
#define PLAYER_STRATEGIES_H

#pragma once
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h> 
#include <stdio.h>
#include "shared_mem.h"   // game_state_t, player_t
#include "game_utils.h"   // DX/DY, idx_wh, in_bounds_wh

// Estrategias disponibles
typedef enum {
    STRAT_GREEDY_PLUS = 0,      // recompensa + movilidad local
    STRAT_SPACE_MAX,            // maximiza maniobrabilidad
    STRAT_CENTER_CONTROL,       // favorece centro y celdas altas
    STRAT_CUTOFF,               // intenta quitar movilidad a rivales cercanos
    STRAT_TWO_PLY_LIGHT,        // 2-ply muy liviano (tu jugada + movilidad resultante)
    STRAT_ENDGAME_HARVEST,      // en endgame, prioriza valores altos cercanos
    STRAT_RANDOM_TIEBREAK       // igual a greedy pero rompe empates al azar
} strategy_t;

// Decide estrategia inicial dado tablero/jugadores/índice
strategy_t choose_strategy(unsigned short W, unsigned short H, unsigned int num_players, int myi);

// ¿Conviene pasar a endgame?
bool should_switch_to_endgame(unsigned int free_cells, unsigned int total_cells);

// API principal: dir 0..7 o 255 si no hay jugada válida
unsigned char pick_move_strategy(strategy_t strat, const game_state_t *gs, int player_idx);

#endif
