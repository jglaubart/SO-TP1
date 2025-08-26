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
#include <curses.h>   // ncurses

#include "structs.h"  // SHM_STATE/SHM_SYNC + player_t, game_state_t, game_sync_t

// shm pointers
static game_state_t *gs = NULL;
static game_sync_t  *gx = NULL;

// ---- utils ----
static void die(const char *fmt, ...) {
    endwin(); // por las dudas, si ya inicializamos ncurses
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

// Lectores–Escritores con preferencia al escritor (master):
// Readers: wait(C); wait(E); if (++F == 1) wait(D); post(E); post(C);
// Readers exit: wait(E); if (--F == 0) post(D); post(E);
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

// --- colores y helpers de render ---
static bool g_has_color = false;

static void setup_colors(void) {
    g_has_color = has_colors();
    if (!g_has_color) return;
    start_color();
    use_default_colors();
    // 1: recompensas, 2..10: jugadores A..I
    init_pair(1, COLOR_WHITE,  -1);  // números
    init_pair(2, COLOR_CYAN,   -1);  // A
    init_pair(3, COLOR_GREEN,  -1);  // B
    init_pair(4, COLOR_YELLOW, -1);  // C
    init_pair(5, COLOR_MAGENTA,-1);  // D
    init_pair(6, COLOR_BLUE,   -1);  // E
    init_pair(7, COLOR_RED,    -1);  // F
    init_pair(8, COLOR_WHITE,  -1);  // G
    init_pair(9, COLOR_CYAN,   -1);  // H
    init_pair(10,COLOR_GREEN,  -1);  // I
}

static int color_pair_for_player(int owner_idx) {
    if (!g_has_color) return 0;
    int p = 2 + owner_idx;
    if (p < 1)  p = 1;
    if (p > 10) p = 10;
    return COLOR_PAIR(p) | A_BOLD;
}

static int color_pair_for_reward(void) {
    return g_has_color ? (COLOR_PAIR(1) | A_DIM) : A_NORMAL;
}

// ---- render ----
static void render_board_and_stats(void) {
    unsigned short w = gs->width, h = gs->height;
    unsigned int np = gs->num_players; if (np > 9) np = 9;

    clear();
    // Título
    mvprintw(0, 0, "ChompChamps  %ux%u", w, h);

    // Layout:
    // - Tablero dentro de un marco desde fila 2, col 0.
    // - Cada celda ocupa 2 caracteres (p.ej. " 1", " A").
    // - Stats a la derecha del tablero.
    int cellw = 2;
    int top = 2, left = 0;
    int box_w = (int)w * cellw + 2;   // bordes laterales
    int box_h = (int)h + 2;           // bordes superior/inferior

    // Dibuja marco
    // Esquinas
    mvaddch(top, left, '+');
    mvaddch(top, left + box_w - 1, '+');
    mvaddch(top + box_h - 1, left, '+');
    mvaddch(top + box_h - 1, left + box_w - 1, '+');
    // Horizontales
    for (int x = 1; x < box_w - 1; x++) {
        mvaddch(top, left + x, '-');
        mvaddch(top + box_h - 1, left + x, '-');
    }
    // Verticales
    for (int y = 1; y < box_h - 1; y++) {
        mvaddch(top + y, left, '|');
        mvaddch(top + y, left + box_w - 1, '|');
    }

    // Relleno de celdas
    int grid_y0 = top + 1;
    int grid_x0 = left + 1;
    for (unsigned short y = 0; y < h; y++) {
        int sy = grid_y0 + (int)y;
        for (unsigned short x = 0; x < w; x++) {
            int sx = grid_x0 + (int)x * cellw;
            int v = gs->board[idx((int)x, (int)y)];
            if (v > 0) {
                // recompensa 1..9
                attr_t attr = color_pair_for_reward();
                attron(attr);
                // 2 columnas: espacio + dígito para alinear
                mvaddch(sy, sx, ' ');
                mvaddch(sy, sx + 1, (char)('0' + (v % 10)));
                attroff(attr);
            } else {
                int owner = -v; // 0..8
                char c = (owner >= 0 && owner <= 8) ? ('A' + owner) : '?';
                attr_t attr = color_pair_for_player(owner);
                attron(attr);
                mvaddch(sy, sx, ' ');
                mvaddch(sy, sx + 1, c);
                attroff(attr);
            }
        }
    }

    // Panel de stats a la derecha
    int stats_x = left + box_w + 2;
    int r = top;
    mvprintw(r++, stats_x, "Players:");
    for (unsigned int i = 0; i < np; i++) {
        const player_t *p = &gs->players[i];
        // etiqueta colorizada con la letra del jugador
        attr_t attr = color_pair_for_player((int)i);
        attron(attr);
        mvprintw(r, stats_x, "%c", 'A' + (int)i);
        attroff(attr);
        // resto en normal
        mvprintw(r++, stats_x + 3, "name=%-10s score=%-4u valid=%-3u invalid=%-3u pos=(%u,%u) %s",
                 p->name, p->score, p->valid_moves, p->invalid_moves,
                 (unsigned)p->x, (unsigned)p->y, p->blocked ? "blk" : "   ");
    }

    refresh();
}

//------------- main -----------------------
int main(int argc, char **argv) {
    // master nos pasa width/height como argv[1], argv[2]; no los necesitamos acá
    (void)argc; (void)argv;

    // asegurar TERM para ncurses (válido según docentes)
    if (!getenv("TERM")) setenv("TERM", "xterm-256color", 1);

    // abrir shm creadas por master
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

    // iniciar ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    setup_colors();

    // bucle: esperar A, leer/mostrar, postear B, salir si finished
    for (;;) {
        sem_wait_intr(&gx->changes);          // master: hay nueva foto del estado
        reader_enter();
        bool finished = gs->finished;
        render_board_and_stats();
        reader_exit();
        if (sem_post(&gx->print) == -1) die("sem_post(print): %s", strerror(errno)); // aviso "ya imprimí"
        if (finished) break;
    }

    // fin
    endwin();
    if (gs) munmap(gs, (size_t)st_state.st_size);
    if (gx) munmap(gx, (size_t)st_sync.st_size);
    return 0;
}