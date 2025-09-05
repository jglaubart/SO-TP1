#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <string.h>
#include "player_strategies.h"
#include "game_utils.h"
#include "shared_mem.h"

// ----------------- helpers comunes -----------------
static inline bool valid_dest(const game_state_t *gs, int nx, int ny) {
    return in_bounds_wh(nx, ny, gs->width, gs->height) &&
           gs->board[idx_wh(nx, ny, gs->width)] > 0;
}

static inline int mobility_from(const game_state_t *gs, int x, int y) {
    int m = 0;
    for (int d = 0; d < 8; ++d) {
        int nx = x + DX[d], ny = y + DY[d];
        if (valid_dest(gs, nx, ny)) m++;
    }
    return m;
}

// Cuánta “libertad” hay si me muevo a (nx,ny): anillo 1 y 2
static int space_2rings(const game_state_t *gs, int nx, int ny) {
    static const int R2DX[16] = { 0,1,2,2,2,1,0,-1,-2,-2,-2,-1,0,1, 0,-1 };
    static const int R2DY[16] = {-2,-2,-2,-1,0,1,2,2, 2, 1, 0,-1,0,1,-2,-1 };
    int sc = 0;
    // anillo 1
    sc += mobility_from(gs, nx, ny) * 3;
    // anillo 2 (mide “aire” alrededor)
    for (int i = 0; i < 16; ++i) {
        int ax = nx + R2DX[i], ay = ny + R2DY[i];
        if (in_bounds_wh(ax, ay, gs->width, gs->height) &&
            gs->board[idx_wh(ax, ay, gs->width)] > 0) sc++;
    }
    return sc;
}

static inline int cell_value(const game_state_t *gs, int x, int y) {
    return gs->board[idx_wh(x, y, gs->width)];
}

static int center_bias(const game_state_t *gs, int x, int y) {
    // Penaliza distancia al centro (cuanto más cerca, mejor)
    double cx = (gs->width  - 1) / 2.0;
    double cy = (gs->height - 1) / 2.0;
    double dx = x - cx, dy = y - cy;
    double dist2 = dx*dx + dy*dy;
    // Escala a entero con signo negativo (menor dist -> mayor puntaje)
    return (int)(-dist2);
}

static int cutoff_score(const game_state_t *gs, int me, int nx, int ny) {
    // Heurística simple: restar la movilidad promedio de rivales cercanos
    int impact = 0, cnt = 0;
    for (unsigned p = 0; p < gs->num_players; ++p) {
        if ((int)p == me) continue;
        const player_t *op = &gs->players[p];
        if (op->blocked) continue;
        int dx = (int)op->x - nx, dy = (int)op->y - ny;
        int r2 = dx*dx + dy*dy;
        if (r2 <= 10) { // solo rivales “cercanos”
            impact += mobility_from(gs, op->x, op->y);
            cnt++;
        }
    }
    if (!cnt) return 0;
    return - (impact / cnt); // menos movilidad rival es mejor
}

// 2-ply liviano: evalúa mis 8 jugadas, para cada una calcula mi movilidad resultante
static int two_ply_light_score(const game_state_t *gs, int nx, int ny) {
    return mobility_from(gs, nx, ny) * 5 + cell_value(gs, nx, ny);
}

// ----------------- selección por estrategia -----------------
static unsigned char best_dir_greedy_plus(const game_state_t *gs, int x, int y, bool rnd_tiebreak) {
    int best = INT_MIN, bestd = -1;

    for (int d = 0; d < 8; ++d) {
        int nx = x + DX[d], ny = y + DY[d];
        if (!valid_dest(gs, nx, ny)) continue;

        int sc = cell_value(gs, nx, ny) * 10 + mobility_from(gs, nx, ny);
        if (sc > best || (rnd_tiebreak && sc == best && (rand() & 1))) {
            best = sc; bestd = d;
        }
    }
    return (bestd < 0) ? 255 : (unsigned char)bestd;
}

static unsigned char best_dir_space_max(const game_state_t *gs, int me, int x, int y) {
    (void)me;
    int best = INT_MIN, bestd = -1;
    for (int d = 0; d < 8; ++d) {
        int nx = x + DX[d], ny = y + DY[d];
        if (!valid_dest(gs, nx, ny)) continue;
        int sc = space_2rings(gs, nx, ny);
        if (sc > best) { best = sc; bestd = d; }
    }
    return (bestd < 0) ? 255 : (unsigned char)bestd;
}

static unsigned char best_dir_center_control(const game_state_t *gs, int me, int x, int y) {
    (void)me;
    int best = INT_MIN, bestd = -1;
    for (int d = 0; d < 8; ++d) {
        int nx = x + DX[d], ny = y + DY[d];
        if (!valid_dest(gs, nx, ny)) continue;
        int sc = cell_value(gs, nx, ny) * 6 + center_bias(gs, nx, ny);
        if (sc > best) { best = sc; bestd = d; }
    }
    return (bestd < 0) ? 255 : (unsigned char)bestd;
}

static unsigned char best_dir_cutoff(const game_state_t *gs, int me, int x, int y) {
    int best = INT_MIN, bestd = -1;
    for (int d = 0; d < 8; ++d) {
        int nx = x + DX[d], ny = y + DY[d];
        if (!valid_dest(gs, nx, ny)) continue;
        int sc = cell_value(gs, nx, ny) * 5 + cutoff_score(gs, me, nx, ny);
        if (sc > best) { best = sc; bestd = d; }
    }
    return (bestd < 0) ? 255 : (unsigned char)bestd;
}

static unsigned char best_dir_two_ply_light(const game_state_t *gs, int me, int x, int y) {
    (void)me;
    int best = INT_MIN, bestd = -1;
    for (int d = 0; d < 8; ++d) {
        int nx = x + DX[d], ny = y + DY[d];
        if (!valid_dest(gs, nx, ny)) continue;
        int sc = two_ply_light_score(gs, nx, ny);
        if (sc > best) { best = sc; bestd = d; }
    }
    return (bestd < 0) ? 255 : (unsigned char)bestd;
}

static unsigned char best_dir_endgame_harvest(const game_state_t *gs, int me, int x, int y) {
    (void)me;
    int best = INT_MIN, bestd = -1;
    for (int d = 0; d < 8; ++d) {
        int nx = x + DX[d], ny = y + DY[d];
        if (!valid_dest(gs, nx, ny)) continue;
        // endgame: prioridad altísima al valor de celda, leve preferencia a movilidad
        int sc = cell_value(gs, nx, ny) * 20 + mobility_from(gs, nx, ny);
        if (sc > best) { best = sc; bestd = d; }
    }
    return (bestd < 0) ? 255 : (unsigned char)bestd;
}

// ----------------- API -----------------
strategy_t choose_strategy(unsigned short W, unsigned short H,
                           unsigned int num_players, int myi)
{
    (void)myi;
    // Heurística simple por forma del tablero y cantidad de jugadores
    if (W*H >= 120 && num_players >= 3) return STRAT_SPACE_MAX;
    if (W >= H*2 || H >= W*2)           return STRAT_CENTER_CONTROL; // tableros “alargados”
    if (num_players == 2)               return STRAT_TWO_PLY_LIGHT;  // 1v1 más “táctico”
    return STRAT_GREEDY_PLUS;
}

bool should_switch_to_endgame(unsigned int free_cells, unsigned int total_cells) {
    // Cambiar a endgame cuando queda <=15% de libres
    return free_cells * 100u <= total_cells * 15u;
}

unsigned char pick_move_strategy(strategy_t strat, const game_state_t *gs, int player_idx){
    const player_t *me = &gs->players[player_idx];
    if (me->blocked) return 255;

    int x = (int)me->x, y = (int)me->y;

    switch (strat) {
        case STRAT_GREEDY_PLUS:     return best_dir_greedy_plus(gs, x, y, false);
        case STRAT_RANDOM_TIEBREAK: return best_dir_greedy_plus(gs, x, y, true);
        case STRAT_SPACE_MAX:       return best_dir_space_max(gs, player_idx, x, y);
        case STRAT_CENTER_CONTROL:  return best_dir_center_control(gs, player_idx, x, y);
        case STRAT_CUTOFF:          return best_dir_cutoff(gs, player_idx, x, y);
        case STRAT_TWO_PLY_LIGHT:   return best_dir_two_ply_light(gs, player_idx, x, y);
        case STRAT_ENDGAME_HARVEST: return best_dir_endgame_harvest(gs, player_idx, x, y);
        default:                    return 255;
    }
}

