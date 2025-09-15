// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
// This is a personal academic project. Dear PVS-Studio, please check it.
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ncurses.h>

#include "shared_mem.h"
#include "sync_utils.h"
#include "game_utils.h"

// shm pointers
static game_state_t *gs = NULL;
static game_sync_t  *gx = NULL;

// --- escala visual (celda = CELL_H x CELL_W chars; ratio 2:1 para "cuadrado" visual) ---
#define CELL_SCALE 2
#define CELL_W (2*CELL_SCALE) 
#define CELL_H (1*CELL_SCALE) 

//================= Declaraciones de funciones =====================
// --- helpers ui ---
static void draw_rect(int y0, int x0, int h, int w, attr_t attr);
static void draw_centered_char(int y0, int x0, int h, int w, attr_t attr, char ch);
static void draw_box(int y0, int x0, int h, int w);

// ---- utils ----
static void die_ncurses(const char *fmt, ...) __attribute__((noreturn));

// Pares: 10..18 cuerpo, 20..28 cabeza, 30..38 ojos
static int pair_body(int idx);
static int pair_head(int idx);
static int pair_eyes(int idx);
static int pair_reward(void);

// --- colores (ASUME 256 colores; aborta si no hay) ---
static bool g_has_color = false;

// Paleta xterm-256 fija: 9 pares (cuerpo pastel / cabeza más intensa) por jugador A..I
// Elegidos para ser distinguibles y mantener "misma familia" de color.
static const short BODY_BG[9];
static const short HEAD_BG[9];

static void setup_colors(void);
static void render_board_and_stats(void);

//========================= main ========================= 
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // Fuerza ncurses con 256 colores para la vista
    setenv("TERM", "xterm-256color", 1);
    
    initscr();  // comienza ncurses
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0); 
    setup_colors();  // inicia paleta de colores, aborta si no hay 256

    // conecta memorias compartidas
    size_t GS_BYTES = 0;
    if (gs_open_ro(&gs, &GS_BYTES) != 0) die_ncurses("gs_open_ro: %s", strerror(errno));
    if (gx_open_rw(&gx) != 0) die_ncurses("gx_open_rw: %s", strerror(errno));

    // loop de vista
    for (;;) {
        sem_wait_intr(&gx->state_changed); //master lo despierta por cambios
        reader_enter(gx);
        bool finished = gs->finished;
        render_board_and_stats();  //actualiza tablero
        reader_exit(gx);
        if (sem_post(&gx->state_rendered) == -1) die_ncurses("sem_post(state_rendered): %s", strerror(errno));
        if (finished) break;
    }

    // Bloquea en getch() para que el usuario pueda ver el estado final.
    {
        int term_h, term_w; 
        getmaxyx(stdscr, term_h, term_w);
        const char *msg = "Fin del juego. Presione cualquier tecla para continuar";
        int msg_x = (term_w - (int)strlen(msg)) / 2; 
        if (msg_x < 0) msg_x = 0;
        int msg_y = term_h - 2; 
        if (msg_y < 0) msg_y = 0;

        // Dibuja una línea y el mensaje
        mvhline(msg_y - 1, 0, ' ', term_w);
        mvprintw(msg_y, msg_x, "%s", msg);
        refresh();

        nodelay(stdscr, FALSE);
        getch();
    }

    endwin();  //finaliza ncurses
    gs_close(gs, GS_BYTES);
    gx_close(gx);

    return 0;
}



//------------- funciones -------------------
static void draw_rect(int y0, int x0, int h, int w, attr_t attr) {
    attron(attr);
    for (int r = 0; r < h; r++) {
        move(y0 + r, x0);
        for (int c = 0; c < w; c++) addch(' ');
    }
    attroff(attr);
}
static void draw_centered_char(int y0, int x0, int h, int w, attr_t attr, char ch) {
    int cy = y0 + h/2;
    int cx = x0 + (w-1)/2;
    attron(attr);
    mvaddch(cy, cx, ch);
    attroff(attr);
}
static void draw_box(int y0, int x0, int h, int w) {
    mvaddch(y0, x0, '+');
    mvaddch(y0, x0 + w - 1, '+');
    mvaddch(y0 + h - 1, x0, '+');
    mvaddch(y0 + h - 1, x0 + w - 1, '+');
    for (int x = 1; x < w - 1; x++) {
        mvaddch(y0, x0 + x, '-');
        mvaddch(y0 + h - 1, x0 + x, '-');
    }
    for (int y = 1; y < h - 1; y++) {
        mvaddch(y0 + y, x0, '|');
        mvaddch(y0 + y, x0 + w - 1, '|');
    }
}

static void die_ncurses(const char *fmt, ...) {
    endwin();
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    _exit(1);
}

static const short BODY_BG[9] = { 159, 114, 229, 183, 110, 217, 252, 223, 147 };
static const short HEAD_BG[9] = {  51,  46, 220, 201,  21, 196, 231, 208,  93 };

static int pair_body(int idx) { return COLOR_PAIR(10 + idx) | A_DIM; }
static int pair_head(int idx) { return COLOR_PAIR(20 + idx) | A_BOLD; }
static int pair_eyes(int idx) { return COLOR_PAIR(30 + idx) | A_BOLD; }
static int pair_reward(void)   { return COLOR_PAIR(1) | A_DIM; }

static void setup_colors(void) {
    g_has_color = has_colors();
    if (!g_has_color) die_ncurses("ncurses: no hay colores");
    start_color();
    use_default_colors();
    assume_default_colors(-1, -1);

    if (COLORS < 256)
        die_ncurses("Se requieren 256 colores (TERM=xterm-256color). COLORS=%d", COLORS);

    // Recompensas: texto tenue sobre fondo default
    init_pair(1, COLOR_WHITE, -1);

    init_pair(2, COLOR_RED, -1); // Rojo sobre fondo default

    // Definir pares de cuerpo/cabeza/ojos para 9 jugadores
    for (int i = 0; i < 9; i++) {
        init_pair(10 + i, COLOR_BLACK, BODY_BG[i]);  // cuerpo (pastel)
        init_pair(20 + i, COLOR_BLACK, HEAD_BG[i]);  // cabeza (más intensa)
        init_pair(30 + i, COLOR_BLACK, HEAD_BG[i]);  // ojos negros sobre cabeza
    }
}

static void render_board_and_stats(void) {
    // lee informacion de la partida
    unsigned short W = gs->width, H = gs->height;
    unsigned int np = gs->num_players; 
    if (np > 9) np = 9;

    // Dimensiones del tablero en caracteres
    int grid_w = (int)W * CELL_W;
    int grid_h = (int)H * CELL_H;

    // Tamaño pantalla y centrado
    int term_h, term_w; 
    getmaxyx(stdscr, term_h, term_w);
    int box_w = grid_w + 2, box_h = grid_h + 2;

    int top  = (term_h - (box_h + 2 + (int)np)) / 2; if (top  < 0) top  = 0;
    int left = (term_w -  box_w) / 2;                 if (left < 0) left = 0;

    clear(); 
    mvprintw(top > 0 ? top - 1 : 0, left, "ChompChamps  %hux%hu", W, H);

    // Marco tablero
    int y0 = top, x0 = left;
    draw_box(y0, x0, box_h, box_w);

    // Área interna
    int grid_y0 = y0 + 1, grid_x0 = x0 + 1;

    // Pintar celdas
    for (int gy = 0; gy < (int)H; gy++) {
        for (int gx = 0; gx < (int)W; gx++) {
            int v = gs->board[idx_wh(gx, gy, gs->width)]; //valor de la celda
            int cell_y = grid_y0 + gy * CELL_H;
            int cell_x = grid_x0 + gx * CELL_W;

            if (v > 0) { //celda libre
                draw_rect(cell_y, cell_x, CELL_H, CELL_W, A_NORMAL); //limpia el fondo
                draw_centered_char(cell_y, cell_x, CELL_H, CELL_W, pair_reward(), (char)('0' + (v % 10))); //imprime valor
            } else { //celda ocupada
                int owner = -v; 
                if (owner > 8) owner = 8;
                draw_rect(cell_y, cell_x, CELL_H, CELL_W, pair_body(owner)); //pinta con color del jugador
            }
        }
    }

    // Dibuja cabezas + ojos
    for (unsigned int i = 0; i < np; i++) {
        const player_t *p = &gs->players[i];
        char eye = '.';
        if (p->blocked) eye = 'x';   //ojos de jugador bloqueado

        int cell_y = grid_y0 + (int)p->y * CELL_H;
        int cell_x = grid_x0 + (int)p->x * CELL_W;

        // cabeza más intensa
        draw_rect(cell_y, cell_x, CELL_H, CELL_W, pair_head((int)i));

        // ojos negros centrados
        int cy = cell_y + CELL_H/4;
        int mid = cell_x + CELL_W/2;
        int x1 = (CELL_W >= 4) ? (mid - 1) : cell_x ;
        int x2 = (CELL_W >= 4) ? (mid) : (cell_x + CELL_W - 1);
        if (x1 < cell_x) x1 = cell_x;
        if (x2 >= cell_x + CELL_W) x2 = cell_x + CELL_W - 1;

        attron(pair_eyes((int)i));
        mvaddch(cy, x1, eye);
        mvaddch(cy, x2, eye);
        attroff(pair_eyes((int)i));
    }

    // Stats abajo
    char buf[256];
    int max_linew = 0;
    for (unsigned int i = 0; i < np; i++) {
        const player_t *p = &gs->players[i];
        int len = snprintf(buf, sizeof buf,
                           "%c name=%-10s score=%-4u valid=%-3u invalid=%-3u pos=(%u,%u) %s",
                           'A' + (int)i, p->name, p->score, p->valid_moves, p->invalid_moves,
                           (unsigned)p->x, (unsigned)p->y, p->blocked ? "blk" : "   ");
        if (len > max_linew) max_linew = len;
    }
    int inner_needed = 2 + 2 + 1 + max_linew + 2; // margen + chip + espacio + texto + margen
    int needed_total = inner_needed + 2;          // + bordes
    int stats_w = box_w; 
    if (needed_total > stats_w) stats_w = needed_total; 
    if (stats_w > term_w) stats_w = term_w;

    int board_center_x = left + box_w / 2;
    int stats_x0 = board_center_x - stats_w / 2;
    if (stats_x0 < 0) stats_x0 = 0;
    if (stats_x0 + stats_w > term_w) stats_x0 = term_w - stats_w;

    int rows = (int)np + 2;
    int stats_h = rows + 2;
    int stats_y0 = y0 + box_h + 1;

    draw_box(stats_y0, stats_x0, stats_h, stats_w + 8);

    const char *title = "Players";
    int title_x = stats_x0 + (stats_w - (int)strlen(title)) / 2;
    mvprintw(stats_y0 + 1, title_x, "%s", title);

    int rstats = stats_y0 + 2;
    int inner_left = stats_x0 + 2;
    for (unsigned int i = 0; i < np; i++) {
        const player_t *p = &gs->players[i];
        attron(COLOR_PAIR(20 + (int)i)); 
        mvprintw(rstats, inner_left, "  "); 
        attroff(COLOR_PAIR(20 + (int)i));
        if (p->blocked) {
            attron(COLOR_PAIR(2) | A_BOLD);
        }
        mvprintw(rstats, inner_left + 3,
                 "%c name=%-7s ID=%-2d score=%-4u valid=%-3u invalid=%-3u pos=(%u,%u) %s",
                 'A' + (int)i, p->name, (int)i, p->score, p->valid_moves, p->invalid_moves,
                 (unsigned)p->x, (unsigned)p->y, p->blocked ? "Blocked" : "Active");
         if (p->blocked) {
            attroff(COLOR_PAIR(2) | A_BOLD);
        }
        rstats++;
    }

    refresh();  //imprime
}
