// player_strategies.c
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include "player_strategies.h"

// === acceso al estado global (provisto por el player principal) ===
game_state_t *gs = NULL; // será “extern” en el player; acá definimos por si se linkea aparte

// === direcciones compatibles con tu player ===
/*Direcciones (unsigned char) 0..7:
   0=arriba, luego horario: 1=arriba-der, 2=der, 3=abajo-der,
   4=abajo, 5=abajo-izq, 6=izq, 7=arriba-izq. */
const int DX[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
const int DY[8] = {-1,-1, 0, 1, 1, 1, 0,-1 };

// === helpers de celda ===
static inline bool cell_free_reward(int v){ return v > 0; } // 1..9 libres
static inline bool cell_taken(int v){ return v <= 0; }       // 0..-8 ocupadas

// Cuenta cuántas celdas libres hay “alrededor” (1 anillo)
static int mobility_1(int x, int y){
    int free_n = 0;
    for (int k=0;k<8;k++){
        int mx = x + DX[k], my = y + DY[k];
        if (!in_bounds_int(mx,my)) continue;
        if (cell_free_reward(gs->board[idx_int(mx,my)])) free_n++;
    }
    return free_n;
}

// Cuenta libres en dos anillos (vecinas de vecinas)
static int mobility_2(int x, int y){
    int seen = 0, free_n = 0;
    // bitset barato para no contar dos veces (W,H <= 255 típicamente; guardamos a ojo)
    // como es chico, evitamos malloc: caja de 5x5 centrada (overcount leve si bordes)
    for (int k=0;k<8;k++){
        int nx = x + DX[k], ny = y + DY[k];
        if (!in_bounds_int(nx,ny)) continue;
        if (!cell_free_reward(gs->board[idx_int(nx,ny)])) continue;
        free_n++;
        for (int t=0;t<8;t++){
            int mx = nx + DX[t], my = ny + DY[t];
            if (!in_bounds_int(mx,my)) continue;
            int v = gs->board[idx_int(mx,my)];
            if (cell_free_reward(v)) seen++; // aproximado
        }
    }
    return free_n + seen;
}

static int dist_to_wall(int x, int y){
    int W = (int)gs->width, H = (int)gs->height;
    int dx = x < (W-1-x) ? x : (W-1-x);
    int dy = y < (H-1-y) ? y : (H-1-y);
    return dx < dy ? dx : dy;
}

static int center_bias(int x, int y){
    // cuanto más cerca del centro, mayor puntaje
    double cx = (gs->width-1)/2.0, cy = (gs->height-1)/2.0;
    double dx = x - cx, dy = y - cy;
    double d2 = dx*dx + dy*dy;
    // invertimos distancia: menor d2 => mayor score
    // escala fija: con tableros grandes sigue siendo suave
    return (int)(10000.0 / (1.0 + d2));
}

// presión rival: penaliza moverte a casillas cercanas a oponentes (r <= 2)
static int opponent_pressure(int x, int y){
    int np = (int)gs->num_players; if (np > 9) np = 9;
    int mepid = getpid();
    int pen = 0;
    for (int i=0;i<np;i++){
        if (gs->players[i].pid == mepid) continue;
        int ox = gs->players[i].x, oy = gs->players[i].y;
        int dx = ox - x, dy = oy - y;
        int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
        int manh = adx + ady; // métrica simple
        if (manh <= 1) pen += 30;        // muy cerca: alto riesgo
        else if (manh == 2) pen += 10;   // medio
        else if (manh == 3) pen += 4;    // leve
    }
    return pen; // penalización (positiva); restamos al score
}

// ¿la celda (x,y) crea cuello de botella? (menos de 3 salidas libres alrededor)
static int corridor_bonus(int x, int y){
    int m = mobility_1(x,y);
    return (m <= 2) ? 8 : ((m == 3) ? 3 : 0);
}

// === family: evaluadores con pesos (sin heap, O(8×8) máx) ===
typedef struct {
    int w_reward;
    int w_mob1;
    int w_mob2;
    int w_center;
    int w_wallneg;   // ojo: se RESTA dist_to_wall*w_wallneg => “pegarse a pared” si es negativo
    int w_oppneg;    // penalización por presión rival
    int w_corridor;  // bonus por cortar pasillos
    bool random_tiebreak;
} weights_t;

static unsigned int fast_rand(void){
    static unsigned int s = 0;
    if (!s) s = (unsigned int)(time(NULL) ^ (getpid()<<16));
    s ^= s<<13; s ^= s>>17; s ^= s<<5;
    return s;
}

static unsigned char eval_and_pick(const weights_t *W, unsigned short x, unsigned short y){
    int best = -1e9, best_dir = -1, best_noise = 0;
    for (unsigned char dir=0; dir<8; dir++){
        int nx = (int)x + DX[dir];
        int ny = (int)y + DY[dir];
        if (!in_bounds_int(nx,ny)) continue;
        int v = gs->board[idx_int(nx,ny)];
        if (cell_taken(v)) continue;

        int sc = 0;
        if (W->w_reward)  sc += W->w_reward  * v;
        if (W->w_mob1)    sc += W->w_mob1    * mobility_1(nx,ny);
        if (W->w_mob2)    sc += W->w_mob2    * mobility_2(nx,ny);
        if (W->w_center)  sc += W->w_center  * center_bias(nx,ny);
        if (W->w_wallneg) sc -= W->w_wallneg * dist_to_wall(nx,ny);
        if (W->w_oppneg)  sc -= W->w_oppneg  * opponent_pressure(nx,ny);
        if (W->w_corridor) sc += W->w_corridor * corridor_bonus(nx,ny);

        int noise = 0;
        if (W->random_tiebreak) noise = (int)(fast_rand() & 7); // rompo empates suaves

        if (sc > best || (sc == best && noise > best_noise)){
            best = sc;
            best_noise = noise;
            best_dir = dir;
        }
    }
    return (best_dir >= 0) ? (unsigned char)best_dir : 255;
}

// === estrategias concretas (solo setean pesos) ===
static unsigned char pick_greedy_plus(unsigned short x, unsigned short y){
    const weights_t w = { .w_reward=50, .w_mob1=8, .w_mob2=0, .w_center=0, .w_wallneg=0, .w_oppneg=0, .w_corridor=0, .random_tiebreak=false };
    return eval_and_pick(&w, x, y);
}

static unsigned char pick_space_max(unsigned short x, unsigned short y){
    const weights_t w = { .w_reward=25, .w_mob1=8, .w_mob2=3, .w_center=0, .w_wallneg=0, .w_oppneg=2, .w_corridor=1, .random_tiebreak=false };
    return eval_and_pick(&w, x, y);
}

static unsigned char pick_wall_hug(unsigned short x, unsigned short y){
    // “pegarse” a pared: restamos dist_to_wall (w_wallneg alto)
    const weights_t w = { .w_reward=20, .w_mob1=6, .w_mob2=0, .w_center=0, .w_wallneg=10, .w_oppneg=2, .w_corridor=2, .random_tiebreak=false };
    return eval_and_pick(&w, x, y);
}

static unsigned char pick_center_control(unsigned short x, unsigned short y){
    const weights_t w = { .w_reward=30, .w_mob1=4, .w_mob2=2, .w_center=2, .w_wallneg=0, .w_oppneg=1, .w_corridor=0, .random_tiebreak=false };
    return eval_and_pick(&w, x, y);
}

static unsigned char pick_lane_runner(unsigned short x, unsigned short y){
    // en tableros “angostos”: priorizamos movilidad y corredor
    const weights_t w = { .w_reward=18, .w_mob1=7, .w_mob2=2, .w_center=0, .w_wallneg=6, .w_oppneg=2, .w_corridor=3, .random_tiebreak=false };
    return eval_and_pick(&w, x, y);
}

static unsigned char pick_cutoff(unsigned short x, unsigned short y){
    // busca cuellos de botella y castiga acercarse a rivales
    const weights_t w = { .w_reward=15, .w_mob1=5, .w_mob2=3, .w_center=0, .w_wallneg=0, .w_oppneg=4, .w_corridor=6, .random_tiebreak=false };
    return eval_and_pick(&w, x, y);
}

static unsigned char pick_two_ply_light(unsigned short x, unsigned short y){
    // aproximación: reward + movilidad2 con un plus fuerte
    const weights_t w = { .w_reward=22, .w_mob1=6, .w_mob2=4, .w_center=0, .w_wallneg=0, .w_oppneg=2, .w_corridor=2, .random_tiebreak=false };
    return eval_and_pick(&w, x, y);
}

static unsigned char pick_endgame_harvest(unsigned short x, unsigned short y){
    // cuando queda poco: ir fuerte por valor y movilidad inmediata
    const weights_t w = { .w_reward=60, .w_mob1=6, .w_mob2=0, .w_center=0, .w_wallneg=0, .w_oppneg=0, .w_corridor=0, .random_tiebreak=false };
    return eval_and_pick(&w, x, y);
}

static unsigned char pick_random_tiebreak(unsigned short x, unsigned short y){
    // greedy con desempate aleatorio para evitar simetrías
    const weights_t w = { .w_reward=50, .w_mob1=8, .w_mob2=0, .w_center=0, .w_wallneg=0, .w_oppneg=0, .w_corridor=0, .random_tiebreak=true };
    return eval_and_pick(&w, x, y);
}

// === selector público ===
unsigned char pick_move_strategy(strategy_t s, unsigned short x, unsigned short y){
    switch (s){
        case STRAT_GREEDY_PLUS:      return pick_greedy_plus(x,y);
        case STRAT_SPACE_MAX:        return pick_space_max(x,y);
        case STRAT_WALL_HUG:         return pick_wall_hug(x,y);
        case STRAT_CENTER_CONTROL:   return pick_center_control(x,y);
        case STRAT_LANE_RUNNER:      return pick_lane_runner(x,y);
        case STRAT_CUTOFF:           return pick_cutoff(x,y);
        case STRAT_TWO_PLY_LIGHT:    return pick_two_ply_light(x,y);
        case STRAT_ENDGAME_HARVEST:  return pick_endgame_harvest(x,y);
        case STRAT_RANDOM_TIEBREAK:  return pick_random_tiebreak(x,y);
        default:                     return pick_greedy_plus(x,y);
    }
}

// === política de selección según régimen del juego ===
static bool is_skinny(unsigned short W, unsigned short H){
    unsigned short a = (W>H?W:H), b = (W>H?H:W);
    return (b*3 <= a); // si lado corto *3 <= lado largo => “angosto”
}

strategy_t choose_strategy(unsigned short W, unsigned short H, unsigned int num_players, int myi){
    (void)myi; // por ahora no usamos mi orden/pos
    if (num_players >= 6) {
        // súper poblado: cortar pasillos o pared
        return is_skinny(W,H) ? STRAT_LANE_RUNNER : STRAT_CUTOFF;
    }
    if (W*H <= 100){ // tableros chicos (≈<=10x10)
        return (num_players >= 4) ? STRAT_WALL_HUG : STRAT_SPACE_MAX;
    }
    if (W*H <= 400){ // medianos (≈<=20x20)
        if (is_skinny(W,H)) return STRAT_LANE_RUNNER;
        return (num_players <= 3) ? STRAT_TWO_PLY_LIGHT : STRAT_SPACE_MAX;
    }
    // grandes (>= ~21x21)
    if (num_players <= 3) return STRAT_CENTER_CONTROL;
    return STRAT_SPACE_MAX;
}

// switch a endgame cuando queda poco “combustible”
bool should_switch_to_endgame(unsigned int free_cells, unsigned int total_cells){
    // si queda <= 12% libres, pasamos a cosecha agresiva
    return (free_cells * 100 <= total_cells * 12);
}
