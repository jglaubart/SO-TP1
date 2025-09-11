// master.c
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>

#include "game_utils.h"   // die, base_name, set_cloexec, DX/DY, idx_wh/in_bounds_wh 
#include "shared_mem.h"   // game_state_t, gs_create_and_init, gs_init_board_rewards, gs_place_players 
#include "sync_utils.h"   // game_sync_t, gx_create_and_init, writer_enter/exit, sem_wait_intr, notify   

// --- ANSI colors para A..I, igual que la vista ---
#define ANSI_RESET   "\x1b[0m"
static const char *ansi_player_color(int idx) {
    // A..I => 0..8, mapeo: CYAN, GREEN, YELLOW, MAGENTA, BLUE, RED, WHITE, CYAN, GREEN
    static const char *map[MAXP] = {
        "\x1b[1;36m", // A CYAN
        "\x1b[1;32m", // B GREEN
        "\x1b[1;33m", // C YELLOW
        "\x1b[1;35m", // D MAGENTA
        "\x1b[1;34m", // E BLUE
        "\x1b[1;31m", // F RED
        "\x1b[1;37m", // G WHITE
        "\x1b[1;36m", // H CYAN
        "\x1b[1;32m", // I GREEN
    };
    if (idx < 0 || idx > 8) return "\x1b[1m"; // fallback bold
    return map[idx];
}

// ============= util =============
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig);

// ============= shm globals =============
static game_state_t *gs = NULL;
static game_sync_t  *gx = NULL;
static size_t GS_BYTES = 0;

// ============= argv parsing =============
typedef struct {
    int w, h;             // dimensiones del tablero
    int delay_ms;         // ms entre renders / ticks
    int timeout_s;        // cortar por inactividad global
    unsigned int seed;    // semilla para rewards/colocación
    const char *view_path;// binario de vista (NULL => sin vista)
    int nplayers;         // cantidad de jugadores
    const char *pbin[MAXP]; // rutas a binarios de jugadores
} opts_t;

static void parse_opts(int argc, char **argv, opts_t *o);

static void print_config(const opts_t *o);

// ============= view notify =============
static bool g_has_view = false;

// ============= cleanup =============
typedef struct {
    pid_t view_pid;
    pid_t pids[MAXP];
    int   nplayers;
    int   pipes_r[MAXP]; // read ends in master
} proc_set_t;
static proc_set_t P = { .view_pid = -1, .nplayers = 0 };

static void cleanup(void);
// Drena lo que quede en los pipes de jugadores y espera a que cierren (EOF).
// Mantiene los read-end abiertos para evitar SIGPIPE en los hijos.
static void drain_players_until_exit(int nplayers, int grace_ms);

// ============= spawn helpers =============
static void spawn_view(const opts_t *o);
static void spawn_players(const opts_t *o, int pipes[][2]);

// ============= procesamiento de un movimiento =============
static void apply_move_rr(int i, unsigned char dir, int W, int H, int *b, struct timespec *last_valid);



// ============= main =============
int main(int argc, char **argv){
    // 1) señales / cleanup
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    atexit(cleanup);  //limpiar al terminar el proceso

    // 2) parsear
    opts_t O; parse_opts(argc, argv, &O);
    print_config(&O);

    // 3-4) crear memorias compartidas
    if (gs_create_and_init(O.w, O.h, (unsigned)O.nplayers, &gs, &GS_BYTES) != 0) // ancho, alto, cantidad jugadores, salida **gs y *bytes
        die("gs_create_and_init"); 
    if (gx_create_and_init(&gx) != 0)
        die("gx_create_and_init");

    // 5) inicializar tablero y jugadores
    gs_init_board_rewards(gs->board, O.w, O.h, O.seed);
    if (gs_place_players(gs) != 0) die("gs_place_players"); //ubica jugadores en celdas válidas, limpia contadores

    
    // 6) lanzar vista y jugadores
    spawn_view(&O);
    int pipes[MAXP][2]; 
    memset(pipes, -1, sizeof(pipes));
    P.nplayers = O.nplayers; 
    for(int i=0;i<P.nplayers;i++){
        P.pipes_r[i] = -1;
    }
    spawn_players(&O, pipes);
    

   // 7) primer render + habilitar 1 solicitud a cada jugador
    sync_notify_view_and_delay(gx, g_has_view, O.delay_ms, &g_stop);
    writer_enter(gx);
    gs_mark_blocked_players(gs);
    writer_exit(gx);

    for (int i = 0; i < O.nplayers; i++) {
        bool blk;
        reader_enter(gx);
        blk = gs->players[i].blocked;
        reader_exit(gx);

        // No habilitar bloqueados
        if (!blk) {
            if (sync_allow_one_move(gx, i) == -1) { //activa el semaforo para permitir un movimeinto
                die("sync_allow_one_move(%d): %s", i, strerror(errno));
            }
        }
    }
    

    // 8) loop principal (RR con select)
    struct timespec last_valid; 
    clock_gettime(CLOCK_MONOTONIC, &last_valid);

    int rr = 0; // round-robin cursor
    while (!g_stop) {
        // a) calcula cuánto falta para que se pase el timeout
        struct timespec now; 
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed_ms = (now.tv_sec - last_valid.tv_sec)*1000LL + (now.tv_nsec - last_valid.tv_nsec)/1000000LL;
        long long remaining_ms = (long long)O.timeout_s*1000LL - elapsed_ms;
        if (remaining_ms <= 0) break;

        // b) armar fd_set y hacer select
        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = -1, alive = 0;
        for (int i=0;i<O.nplayers;i++){
            if (P.pipes_r[i] >= 0) {
                FD_SET(P.pipes_r[i], &rfds);
                if (P.pipes_r[i] > maxfd) maxfd = P.pipes_r[i];
                alive++;
            }
        }
        if (alive == 0) break; // no queda nadie escribiendo

        struct timeval tv;
        tv.tv_sec = (remaining_ms/1000);
        tv.tv_usec = (remaining_ms%1000)*1000;
        int rv = select(maxfd+1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            die("select: %s", strerror(errno));
        }
        if (rv == 0) {
            // se venció el tv => se corta por inactividad
            break;
        }

        // c) atender SOLO 1 jugador por iteración
        int processed = -1;
        for (int off=0; off<O.nplayers; ++off) {
            int i = (rr + off) % O.nplayers;
            if (P.pipes_r[i] < 0) continue;
            if (!FD_ISSET(P.pipes_r[i], &rfds)) continue;

            unsigned char dir;
            int pr = proto_read_dir(P.pipes_r[i], &dir);
            if (pr == 0) {
                apply_move_rr(i, dir, O.w, O.h, gs->board, &last_valid); // aplica el movimiento
                // si el movimiento encerró a alguien, marcarlo como "blk"
                writer_enter(gx);
                gs_mark_blocked_players(gs);
                writer_exit(gx);

                sync_notify_view_and_delay(gx, g_has_view, O.delay_ms, &g_stop);
                // habilitar nueva solicitud a ese jugador
                if (sync_allow_one_move(gx, i) == -1) die("sync_allow_one_move(%d): %s", i, strerror(errno));
            } else if (pr == 1) {
                // EOF, jugador bloqueado
                writer_enter(gx);
                gs->players[i].blocked = true;
                writer_exit(gx);
                close(P.pipes_r[i]); //cierra FD
                P.pipes_r[i] = -1;
                sync_notify_view_and_delay(gx, g_has_view, O.delay_ms, &g_stop);
            } else {
                // error de lectura => cerrar FD
                close(P.pipes_r[i]);
                P.pipes_r[i] = -1;
            }
            processed = i;
            break;
        }
        if (processed >= 0) rr = (processed + 1) % O.nplayers;

        // d) si estan todos bloqueados, termina
        reader_enter(gx);
        bool can_move = gs_any_player_can_move(gs);
        reader_exit(gx);
        if (!can_move) break;
    }
    // 9) finalizar juego (sin cerrar los pipes)
    writer_enter(gx);
    gs->finished = true;
    writer_exit(gx);
    sync_notify_view_and_delay(gx, g_has_view, O.delay_ms, &g_stop);

    // despertar jugadores para que vean que finalizo y salgan
    for (int i = 0; i < O.nplayers; ++i){
        sync_allow_one_move(gx, i);
    }

    // 10) sacar lo que queda y recién después cerrar los FDs (espera 2000 ms para que cierren los jugadores)
    drain_players_until_exit(O.nplayers, 2000);
    
    // 11) esperar hijos e imprimir resultados
    int status;
    if (P.view_pid > 0) {
        if (waitpid(P.view_pid, &status, 0) > 0) {
            if (WIFEXITED(status))
                fprintf(stderr, "View exited (%d)\n", WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                fprintf(stderr, "View killed by signal (%d)\n", WTERMSIG(status));
        }
    }
    for (int i = 0; i < O.nplayers; ++i) {
        if (gs->players[i].pid <= 0) continue;
        if (waitpid(gs->players[i].pid, &status, 0) <= 0) continue;

        const char *c1 = "", *c0 = "";
        if (isatty(STDERR_FILENO)) { 
            c1 = ansi_player_color(i); 
            c0 = ANSI_RESET; 
        }

        unsigned s  = gs->players[i].score;
        unsigned v  = gs->players[i].valid_moves;
        unsigned iv = gs->players[i].invalid_moves;

        char letter = 'A' + i;
        if (WIFEXITED(status)) {
            // colorea solo "Player <letra> <nombre>"
            fprintf(stderr, "%sPlayer %c %s%s%s (%d)%s exited (%d) with a score of %u / %u / %u\n",
                    c1, letter, c1, gs->players[i].name, c0, i, c0,
                    WEXITSTATUS(status), s, v, iv);
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "%sPlayer %c %s%s%s (%d)%s killed by signal (%d) with a score of %u / %u / %u\n",
                    c1, letter, c1, gs->players[i].name, c0, i, c0,
                    WTERMSIG(status), s, v, iv);
        }
    }
}


// ============= funciones auxiliares =============
static void on_signal(int sig){ (void)sig; g_stop = 1; }

static void parse_opts(int argc, char **argv, opts_t *o){
    o->w = 10; o->h = 10;
    o->delay_ms = 200;
    o->timeout_s = 10;
    o->seed = (unsigned)time(NULL);
    o->view_path = NULL;
    o->nplayers = 0;

    int opt;
    while ((opt = getopt(argc, argv, "w:h:d:t:s:v:p:")) != -1) {
        switch (opt) {
        case 'w': o->w = atoi(optarg); break;
        case 'h': o->h = atoi(optarg); break;
        case 'd': o->delay_ms = atoi(optarg); break;
        case 't': o->timeout_s = atoi(optarg); break;
        case 's': o->seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'v': o->view_path = optarg; break;
        case 'p':
            // -p player1 player2 ...
            o->pbin[o->nplayers++] = optarg;
            while (optind < argc && argv[optind][0] != '-') {
                if (o->nplayers >= MAXP) die("Demasiados jugadores (max 9)");
                o->pbin[o->nplayers++] = argv[optind++];
            }
            break;
        default: die("Uso: master [-w W] [-h H] [-d delay_ms] [-t timeout_s] [-s seed] [-v view] -p player1 [player2 ...]");
        }
    }
    if (o->w < 10) o->w = 10;
    if (o->h < 10) o->h = 10;
    if (o->nplayers < 1) die("Error: At least one player must be specified using -p.");
}

static void print_config(const opts_t *o){
    printf("width: %d\n",   o->w);
    printf("height: %d\n",  o->h);
    printf("delay: %d\n",   o->delay_ms);
    printf("timeout: %d\n", o->timeout_s);
    printf("seed: %u\n",    o->seed);
    printf("view: %s\n",    o->view_path ? o->view_path : "-");
    printf("num_players: %d\n", o->nplayers);
    for (int i = 0; i < o->nplayers; ++i)
        printf("  %s\n", o->pbin[i]);
    fflush(stdout);

    // Mostrarlo la informacion antes de lanzar vista/jugadores
    if (o->view_path) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 500*1000000L };
        nanosleep(&ts, NULL);
    }
}

// ============= cleanup =============
static void cleanup(void){
    if (gs) {
        gx_destroy_sems(gx);   // destruye todos los semáforos G/A/B/C/D/E   
        gs_close(gs, GS_BYTES); // munmap estado                           
        gx_close(gx);           // munmap sync                               
        gs = NULL; gx = NULL;
    }
    shm_unlink(SHM_STATE);
    shm_unlink(SHM_SYNC);

    for (int i=0; i<P.nplayers; i++){
        if (P.pipes_r[i] >= 0) close(P.pipes_r[i]);
    }
}

static void drain_players_until_exit(int nplayers, int grace_ms) {
    int abiertos = 0;
    for (int i = 0; i < nplayers; ++i)
     if (P.pipes_r[i] >= 0) abiertos++;

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (abiertos > 0) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed = (now.tv_sec - start.tv_sec)*1000LL +
                            (now.tv_nsec - start.tv_nsec)/1000000LL;
        if (elapsed > grace_ms && grace_ms >= 0) break;

        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < nplayers; ++i) {
            if (P.pipes_r[i] >= 0) {
                FD_SET(P.pipes_r[i], &rfds);
                if (P.pipes_r[i] > maxfd) maxfd = P.pipes_r[i];
            }
        }
        if (maxfd < 0) break;

        struct timeval tv = { .tv_sec = 0, .tv_usec = 100*1000 }; // 100 ms
        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rv == 0) continue;

        for (int i = 0; i < nplayers; ++i) {
            if (P.pipes_r[i] < 0) continue;
            if (!FD_ISSET(P.pipes_r[i], &rfds)) continue;

            unsigned char dummy;
            ssize_t r = read(P.pipes_r[i], &dummy, 1);
            if (r == 0) {
                // EOF: el jugador cerró su extremo -> marcar y cerrar FD
                writer_enter(gx);
                gs->players[i].blocked = true;
                writer_exit(gx);
                close(P.pipes_r[i]);
                P.pipes_r[i] = -1;
                abiertos--;
            } else if (r < 0 && errno != EINTR && errno != EAGAIN) {
                close(P.pipes_r[i]);
                P.pipes_r[i] = -1;
                abiertos--;
            }
            // si r==1, descarta el byte (llegó justo antes de ver "finished")
        }
    }
}

// ============= spawn helpers =============
static void spawn_view(const opts_t *o){
    if (!o->view_path) { g_has_view = false; return; }
    g_has_view = true;
    pid_t pid = fork();
    if (pid < 0) die("fork(view): %s", strerror(errno));
    if (pid == 0) {
        // hijo: exec view
        char wbuf[16], hbuf[16];
        snprintf(wbuf, sizeof wbuf, "%d", o->w);
        snprintf(hbuf, sizeof hbuf, "%d", o->h);
        execl(o->view_path, o->view_path, wbuf, hbuf, (char*)NULL);
        //fallo exec
        die_fast("exec(view '%s'): %s", o->view_path, strerror(errno));
    }
    P.view_pid = pid;
}

static void spawn_players(const opts_t *o, int pipes[][2]){
    // crear todos los pipes
    for (int i=0; i<o->nplayers; i++){
        if (pipe(pipes[i]) == -1) die("pipe: %s", strerror(errno));
        // master solo lee -> marcar write-end CLOEXEC=1 en el padre
        set_cloexec(pipes[i][0], 0); // read-end queda abierto en padre
        set_cloexec(pipes[i][1], 0); // el hijo lo usa para stdout
    }

    for (int i=0; i<o->nplayers; i++){
        pid_t pid = fork();
        if (pid < 0) die("fork(player): %s", strerror(errno));
        if (pid == 0) {
            // CHILD i: cerrar todo excepto write-end, y dup2 a STDOUT
            // write-end : pipes[i][1]
            for (int j=0; j<o->nplayers; j++){
                if (j == i) continue;
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(pipes[i][0]);
            // Juagdor escribe direcciones por stdout
            if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                die_fast("dup2(player[%d]->stdout): %s", i, strerror(errno));
            }
            close(pipes[i][1]);
            // exec jugador
            char wbuf[16], hbuf[16];
            snprintf(wbuf, sizeof wbuf, "%d", o->w);
            snprintf(hbuf, sizeof hbuf, "%d", o->h);
            execl(o->pbin[i], o->pbin[i], wbuf, hbuf, (char*)NULL);
            // fallo exec de jugador i
            die_fast("exec(player '%s'): %s", o->pbin[i], strerror(errno));
        }
        // PARENT:
        close(pipes[i][1]);                  // no escribe
        P.pipes_r[i] = pipes[i][0];          // guarda read-end
        set_cloexec(P.pipes_r[i], 1);
        gs->players[i].pid = pid;
        // nombre visible (hasta 15 chars, null terminated)
        memset(gs->players[i].name, 0, sizeof(gs->players[i].name));
        strncpy(gs->players[i].name, base_name(o->pbin[i]), sizeof(gs->players[i].name)-1);
    }
}

// ============= procesamiento de un movimiento =============
static void apply_move_rr(int i, unsigned char dir, int W, int H, int *b, struct timespec *last_valid){
    // validar dir 0..7
    if (dir > 7) {
        writer_enter(gx);
        gs->players[i].invalid_moves++;
        writer_exit(gx);
        return;
    }
    int x = gs->players[i].x, y = gs->players[i].y;
    int nx = x + DX[dir], ny = y + DY[dir];
    bool valid = in_bounds_wh(nx,ny,W,H) && b[idx_wh(nx,ny,W)] > 0;

    writer_enter(gx);
    if (!valid || gs->players[i].blocked) {
        gs->players[i].invalid_moves++;
        writer_exit(gx);
        return;
    }
    // mover: sumar recompensa, marcar celda, actualizar pos, contadores
    int reward = b[idx_wh(nx,ny,W)];
    gs->players[i].score += (unsigned)reward;
    gs->players[i].valid_moves++;
    gs->players[i].x = (unsigned short)nx;
    gs->players[i].y = (unsigned short)ny;
    b[idx_wh(nx,ny,W)] = -i;  // capturada por jugador i
    writer_exit(gx);

    // reset del timer de inactividad
    clock_gettime(CLOCK_MONOTONIC, last_valid);
}
