// src/view.c
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <semaphore.h>
#include <errno.h>
#include <ncurses.h>

#include "structs.h"

// shm pointers
static game_state_t *gs = NULL;
static game_sync_t  *gx = NULL;

// --- escala visual (celda = CELL_H x CELL_W chars; ratio 2:1 para "cuadrado" visual) ---
#define CELL_SCALE 3
#define CELL_W (2*CELL_SCALE)  // 6
#define CELL_H (1*CELL_SCALE)  // 3

// --- helpers ui ---
static inline void draw_rect(int y0, int x0, int h, int w, attr_t attr) {
    attron(attr);
    for (int r = 0; r < h; r++) {
        move(y0 + r, x0);
        for (int c = 0; c < w; c++) addch(' ');
    }
    attroff(attr);
}
static inline void draw_centered_char(int y0, int x0, int h, int w, attr_t attr, char ch) {
    int cy = y0 + h/2;
    int cx = x0 + (w-1)/2;
    attron(attr);
    mvaddch(cy, cx, ch);
    attroff(attr);
}
static inline void draw_box(int y0, int x0, int h, int w) {
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

// ---- utils ----
static void die(const char *fmt, ...) {
    endwin();
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

// Lectores–Escritores con preferencia al escritor (master)
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
    if (gx->player == 0 && sem_post(&gx->writer) == -1) die("sem_post(writer): %s", strerror(errno));
    if (sem_post(&gx->reader) == -1) die("sem_post(reader): %s", strerror(errno));
}

static inline int idx(int x, int y) { return y * (int)gs->width + x; }

// --- colores (ASUME 256 colores; aborta si no hay) ---
static bool g_has_color = false;

// Paleta xterm-256 fija: 9 pares (cuerpo pastel / cabeza más intensa) por jugador A..I
// Elegidos para ser distinguibles y mantener "misma familia" de color.
static const short BODY_BG[9] = { 159, 114, 229, 183, 110, 217, 252, 223, 147 };
static const short HEAD_BG[9] = {  51,  46, 220, 201,  21, 196, 231, 208,  93 };
// Pares: 10..18 cuerpo, 20..28 cabeza, 30..38 ojos (fg negro sobre bg de cabeza)
static inline int pair_body(int idx) { return COLOR_PAIR(10 + idx) | A_DIM; }
static inline int pair_head(int idx) { return COLOR_PAIR(20 + idx) | A_BOLD; }
static inline int pair_eyes(int idx) { return COLOR_PAIR(30 + idx) | A_BOLD; }
static inline int pair_reward(void)   { return COLOR_PAIR(1) | A_DIM; }

static void setup_colors(void) {
    g_has_color = has_colors();
    if (!g_has_color) die("ncurses: no hay colores");
    start_color();
    use_default_colors();
    assume_default_colors(-1, -1);

    if (COLORS < 256)
        die("Se requieren 256 colores (TERM=xterm-256color). COLORS=%d", COLORS);

    // Recompensas: texto tenue sobre fondo default
    init_pair(1, COLOR_WHITE, -1);

    // Definir pares de cuerpo/cabeza/ojos para 9 jugadores
    for (int i = 0; i < 9; i++) {
        init_pair(10 + i, COLOR_BLACK, BODY_BG[i]);  // cuerpo (pastel)
        init_pair(20 + i, COLOR_BLACK, HEAD_BG[i]);  // cabeza (más intensa)
        init_pair(30 + i, COLOR_BLACK, HEAD_BG[i]);  // ojos negros sobre cabeza
    }
}

// ---- render ----
static void render_board_and_stats(void) {
    unsigned short W = gs->width, H = gs->height;
    unsigned int np = gs->num_players; if (np > 9) np = 9;

    // Dimensiones del tablero en caracteres
    int grid_w = (int)W * CELL_W;
    int grid_h = (int)H * CELL_H;

    // Tamaño pantalla y centrado
    int term_h, term_w; getmaxyx(stdscr, term_h, term_w);
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
            int v = gs->board[idx(gx, gy)];
            int cell_y = grid_y0 + gy * CELL_H;
            int cell_x = grid_x0 + gx * CELL_W;

            if (v > 0) {
                draw_rect(cell_y, cell_x, CELL_H, CELL_W, A_NORMAL);
                draw_centered_char(cell_y, cell_x, CELL_H, CELL_W, pair_reward(), (char)('0' + (v % 10)));
            } else {
                int owner = -v; if (owner < 0) owner = 0; if (owner > 8) owner = 8;
                draw_rect(cell_y, cell_x, CELL_H, CELL_W, pair_body(owner));
            }
        }
    }

    // Cabezas + ojos (encima del cuerpo)
    for (unsigned int i = 0; i < np; i++) {
        const player_t *p = &gs->players[i];
        if (p->blocked) continue;

        int cell_y = grid_y0 + (int)p->y * CELL_H;
        int cell_x = grid_x0 + (int)p->x * CELL_W;

        // cabeza más intensa
        draw_rect(cell_y, cell_x, CELL_H, CELL_W, pair_head((int)i));

        // ojos negros centrados
        int cy = cell_y + CELL_H/2;
        int mid = cell_x + CELL_W/2;
        int ex1 = (CELL_W >= 4) ? (mid - 1) : cell_x;
        int ex2 = (CELL_W >= 4) ? (mid)     : (cell_x + CELL_W - 1);
        if (ex1 < cell_x) ex1 = cell_x;
        if (ex2 >= cell_x + CELL_W) ex2 = cell_x + CELL_W - 1;

        attron(pair_eyes((int)i));
        mvaddch(cy, ex1, '.');
        mvaddch(cy, ex2, '.');
        attroff(pair_eyes((int)i));
    }

    // === Stats abajo, centradas y con marco ===
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
    int stats_w = box_w; if (needed_total > stats_w) stats_w = needed_total; if (stats_w > term_w) stats_w = term_w;

    int board_center_x = left + box_w / 2;
    int stats_x0 = board_center_x - stats_w / 2;
    if (stats_x0 < 0) stats_x0 = 0;
    if (stats_x0 + stats_w > term_w) stats_x0 = term_w - stats_w;

    int rows = (int)np + 2;
    int stats_h = rows + 2;
    int stats_y0 = y0 + box_h + 1;

    draw_box(stats_y0, stats_x0, stats_h, stats_w);

    const char *title = "Players";
    int title_x = stats_x0 + (stats_w - (int)strlen(title)) / 2;
    mvprintw(stats_y0 + 1, title_x, "%s", title);

    int rstats = stats_y0 + 2;
    int inner_left = stats_x0 + 2;
    for (unsigned int i = 0; i < np; i++) {
        const player_t *p = &gs->players[i];
        attron(COLOR_PAIR(20 + (int)i)); mvprintw(rstats, inner_left, "  "); attroff(COLOR_PAIR(20 + (int)i));
        mvprintw(rstats, inner_left + 3,
                 "%c name=%-10s score=%-4u valid=%-3u invalid=%-3u pos=(%u,%u) %s",
                 'A' + (int)i, p->name, p->score, p->valid_moves, p->invalid_moves,
                 (unsigned)p->x, (unsigned)p->y, p->blocked ? "blk" : "   ");
        rstats++;
    }

    refresh();
}

//------------- main -----------------------
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // Fuerza 256 colores para esta vista, sin depender del shell del profe
    setenv("TERM", "xterm-256color", 1);
    
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    setup_colors();  // aborta si no hay 256

    // abrir shm (del master)
    int fd_state = shm_open(SHM_STATE, O_RDONLY, 0);
    if (fd_state == -1) die("shm_open(%s): %s", SHM_STATE, strerror(errno));
    int fd_sync  = shm_open(SHM_SYNC,  O_RDWR,  0);
    if (fd_sync  == -1) die("shm_open(%s): %s", SHM_SYNC,  strerror(errno));

    struct stat st_state, st_sync;
    if (fstat(fd_state, &st_state) == -1) die("fstat(state): %s", strerror(errno));
    if (fstat(fd_sync,  &st_sync ) == -1) die("fstat(sync ): %s", strerror(errno));

    gs = mmap(NULL, (size_t)st_state.st_size, PROT_READ,              MAP_SHARED, fd_state, 0);
    if (gs == MAP_FAILED) die("mmap(state): %s", strerror(errno));
    gx = mmap(NULL, (size_t)st_sync.st_size,  PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync,  0);
    if (gx == MAP_FAILED) die("mmap(sync): %s", strerror(errno));
    close(fd_state);
    close(fd_sync);

    // loop de vista
    for (;;) {
        sem_wait_intr(&gx->changes);
        reader_enter();
        bool finished = gs->finished;
        render_board_and_stats();
        reader_exit();
        if (sem_post(&gx->print) == -1) die("sem_post(print): %s", strerror(errno));
        if (finished) break;
    }

    endwin();
    if (gs) munmap(gs, (size_t)st_state.st_size);
    if (gx) munmap(gx, (size_t)st_sync.st_size);
    return 0;
}
