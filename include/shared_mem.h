// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#ifndef SHARED_MEM_H
#define SHARED_MEM_H

#pragma once
#include <stdbool.h>
#include <sys/types.h>
#include <stddef.h>

//Segmento de ESTADO del juego 
#define SHM_STATE "/game_state"

//Información de un jugador (igual a tu structs.h, sin cambios de campos)
typedef struct {
    char name[16];
    unsigned int score;
    unsigned int invalid_moves;
    unsigned int valid_moves;
    unsigned short x, y;
    pid_t pid;
    bool blocked;
} player_t;

// Estado global del juego (flexible array al final)
typedef struct {
    unsigned short width;
    unsigned short height;
    unsigned int num_players;   /* <= 9 */
    player_t players[9];
    bool finished;
    int board[];       /* fila-0, fila-1, ..., fila-(h-1) */
} game_state_t;

/* API estado (SHM /game_state) */

/* Crea, trunca e inicializa el estado (solo master). Devuelve 0 si ok. */
int gs_create_and_init(int W, int H, unsigned nplayers, game_state_t **gs_out, size_t *gs_bytes_out);

/* Abre en solo-lectura el estado (view y player). Devuelve 0 si ok. */
int gs_open_ro(game_state_t **gs_out, size_t *gs_bytes_out);

/* Unmap/cierre simétrico del estado. */
void gs_close(game_state_t *gs, size_t gs_bytes);

/* Inicializa recompensas del board [1..9]. */
void gs_init_board_rewards(int *board, int W, int H, unsigned seed);

/* Embaraja y posiciona jugadores en celdas libres */
int gs_place_players(game_state_t *gs); /* usa width/height/num_players/board */

/* Queries/ops sobre el estado */
bool gs_has_valid_move_from(const game_state_t *gs, int x, int y);
bool gs_any_player_can_move(const game_state_t *gs);
void gs_mark_blocked_players(game_state_t *gs);
unsigned int gs_count_free_cells(const game_state_t *gs);

#endif