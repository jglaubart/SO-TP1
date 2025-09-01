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
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>

#include "structs.h"

// --- ANSI colors para A..I, igual que la vista ---
#define ANSI_RESET   "\x1b[0m"
static const char *ansi_player_color(int idx) {
    // A..I => 0..8, mapeo: CYAN, GREEN, YELLOW, MAGENTA, BLUE, RED, WHITE, CYAN, GREEN
    static const char *map[9] = {
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

//chequear que el stream es tty (terminal interactiva para usar colores)
static int stream_isatty(FILE *f) {
    int fd = fileno(f);
    return (fd >= 0) && isatty(fd);
}


// ============= util =============
#define MAXP 9
static volatile sig_atomic_t g_stop = 0;
static void die(const char *fmt, ...) __attribute__((noreturn));
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
static void on_signal(int sig){ (void)sig; g_stop = 1; }

static void sem_wait_intr(sem_t *s) {
    for (;;) {
        if (sem_wait(s) == 0) return;
        if (errno != EINTR) die("sem_wait: %s", strerror(errno));
    }
}

// ============= dir helpers =============
static const int DX[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
static const int DY[8] = {-1,-1, 0, 1, 1, 1, 0,-1 };
static inline int in_bounds(int x,int y,int W,int H){ return x>=0 && y>=0 && x<W && y<H; }
static inline int idx(int x,int y,int W){ return y*W + x; }

// ============= shm globals =============
static game_state_t *gs = NULL;
static game_sync_t  *gx = NULL;
static size_t GS_BYTES = 0;

// ============= writer (master) =============
// Writers (master): wait(C); wait(D); ...; post(D); post(C)
static void writer_enter(void){
    sem_wait_intr(&gx->master);
    sem_wait_intr(&gx->writer);
}
static void writer_exit(void){
    if (sem_post(&gx->writer)==-1) die("sem_post(writer): %s", strerror(errno));
    if (sem_post(&gx->master)==-1) die("sem_post(master): %s", strerror(errno));
}

// ============= argv parsing =============
typedef struct {
    int w, h;
    int delay_ms;
    int timeout_s;
    unsigned int seed;
    const char *view_path;   // NULL = sin vista
    int nplayers;
    const char *pbin[MAXP];
} opts_t;

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
            // lo que queda en argv desde optind-1 (optarg) hasta el final son players
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

    // Mostrarlo "por unos segundos" antes de lanzar vista/jugadores
    struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 }; // 2 segundos
    nanosleep(&ts, NULL);
}


// ============= board init =============
static void init_board_rewards(int *b, int W, int H, unsigned seed){
    srand(seed);
    for (int y=0; y<H; y++)
        for (int x=0; x<W; x++)
            b[idx(x,y,W)] = 1 + (rand() % 9);
}

// posiciones iniciales: centros de una grilla r x c ~ sqrt(n)
static void place_players(int W,int H,int n){
    int r = 1, c = n;
    for (int k = 1; k*k <= n; ++k) { r = k; }
    c = (n + r - 1) / r;
    int cellW = W / c;
    int cellH = H / r;
    int p = 0;
    for (int i=0; i<r && p<n; ++i){
        for (int j=0; j<c && p<n; ++j){
            int cx = j*cellW + cellW/2;
            int cy = i*cellH + cellH/2;
            if (cx >= W) cx = W-1;
            if (cy >= H) cy = H-1;
            gs->players[p].x = (unsigned short)cx;
            gs->players[p].y = (unsigned short)cy;
            gs->players[p].score = 0;
            gs->players[p].valid_moves = 0;
            gs->players[p].invalid_moves = 0;
            gs->players[p].blocked = false;
            // celda inicial queda capturada por el jugador p (no suma recompensa)
            gs->board[idx(cx,cy,W)] = -p;
            p++;
        }
    }
}

// ¿queda algun mov válido para (x,y)?
static bool has_valid_move_from(int x,int y,int W,int H,int *b){
    for (int d=0; d<8; ++d){
        int nx = x + DX[d], ny = y + DY[d];
        if (in_bounds(nx,ny,W,H) && b[idx(nx,ny,W)] > 0) return true;
    }
    return false;
}
static bool any_player_can_move(int W,int H,int n,int *b){
    for (int i=0;i<n;i++){
        int x = gs->players[i].x, y = gs->players[i].y;
        if (has_valid_move_from(x,y,W,H,b)) return true;
    }
    return false;
}

// ============= view notify =============
static bool g_has_view = false;
static void notify_view_and_delay(int delay_ms){
    if (g_has_view) {
        if (sem_post(&gx->changes) == -1) die("sem_post(changes): %s", strerror(errno));
        if (!g_stop) {
            sem_wait_intr(&gx->print);
        } else {
            // abortando: no bloquea si la vista ya murió
            (void)sem_trywait(&gx->print);
        }
    }
    if (delay_ms > 0) {
        struct timespec ts = { delay_ms/1000, (delay_ms%1000)*1000000L };
        nanosleep(&ts, NULL);
    }
}

// ============= cleanup =============
typedef struct {
    pid_t view_pid;
    pid_t pids[MAXP];
    int   nplayers;
    int   pipes_r[MAXP]; // read ends in master
} proc_set_t;
static proc_set_t P = { .view_pid = -1, .nplayers = 0 };

static void cleanup(void){
    // intenta no tirar errores si ya se llamó
    if (gs) {
        // destruir semáforos
        sem_destroy(&gx->changes);
        sem_destroy(&gx->print);
        sem_destroy(&gx->master);
        sem_destroy(&gx->writer);
        sem_destroy(&gx->reader);
        for (int i=0;i<MAXP;i++) sem_destroy(&gx->movement[i]);
        munmap(gs, GS_BYTES);
        munmap(gx, sizeof(*gx));
        gs = NULL; gx = NULL;
    }
    shm_unlink(SHM_STATE);
    shm_unlink(SHM_SYNC);

    for (int i=0;i<P.nplayers;i++) if (P.pipes_r[i] >= 0) close(P.pipes_r[i]);
}
// Drena lo que quede en los pipes de jugadores y espera a que cierren (EOF).
// Mantiene los read-end abiertos para evitar SIGPIPE en los hijos.
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
                // EOF: el jugador cerró su extremo -> marcar y cerrar nuestro FD
                writer_enter();
                gs->players[i].blocked = true;
                writer_exit();
                close(P.pipes_r[i]);
                P.pipes_r[i] = -1;
                abiertos--;
            } else if (r < 0 && errno != EINTR && errno != EAGAIN) {
                close(P.pipes_r[i]);
                P.pipes_r[i] = -1;
                abiertos--;
            }
            // si r==1, descartamos el byte (llegó justo antes de ver "finished")
        }
    }
}

// --- marca como bloqueados a los que no tienen movimientos válidos ---
static void mark_blocked_players(int W, int H, int n, int *b) {
    writer_enter();
    for (int i = 0; i < n; ++i) {
        if (gs->players[i].blocked) continue; // ya bloqueado (EOF u otra causa)
        int x = gs->players[i].x, y = gs->players[i].y;
        bool stuck = true;
        for (int d = 0; d < 8; ++d) {
            int nx = x + DX[d], ny = y + DY[d];
            if (in_bounds(nx, ny, W, H) && b[idx(nx,ny,W)] > 0) { stuck = false; break; }
        }
        if (stuck) gs->players[i].blocked = true;
    }
    writer_exit();
}


// ============= spawn helpers =============
static const char* base_name(const char *path){
    const char *s = strrchr(path, '/'); return s? s+1 : path;
}

static void set_cloexec(int fd, int on){
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) die("fcntl(F_GETFD): %s", strerror(errno));
    if (on) flags |= FD_CLOEXEC; else flags &= ~FD_CLOEXEC;
    if (fcntl(fd, F_SETFD, flags) == -1) die("fcntl(F_SETFD): %s", strerror(errno));
}

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
        perror("exec(view)");
        _exit(127);
    }
    P.view_pid = pid;
}

static void spawn_players(const opts_t *o, int pipes[][2]){
    // crear todos los pipes
    for (int i=0;i<o->nplayers;i++){
        if (pipe(pipes[i]) == -1) die("pipe: %s", strerror(errno));
        // master solo lee -> marca write-end CLOEXEC en el padre
        set_cloexec(pipes[i][0], 0); // read-end queda abierto en padre
        set_cloexec(pipes[i][1], 0); // clear aún, el hijo lo usará para stdout
    }

    for (int i=0;i<o->nplayers;i++){
        pid_t pid = fork();
        if (pid < 0) die("fork(player): %s", strerror(errno));
        if (pid == 0) {
            // CHILD i: cerrar todo excepto mi write-end, y dup2 a STDOUT
            for (int j=0;j<o->nplayers;j++){
                if (j == i) continue;
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(pipes[i][0]);
            if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                perror("dup2(player-stdout)"); _exit(127);
            }
            close(pipes[i][1]);
            // exec jugador
            char wbuf[16], hbuf[16];
            snprintf(wbuf, sizeof wbuf, "%d", o->w);
            snprintf(hbuf, sizeof hbuf, "%d", o->h);
            execl(o->pbin[i], o->pbin[i], wbuf, hbuf, (char*)NULL);
            perror("exec(player)");
            _exit(127);
        }
        // PARENT:
        close(pipes[i][1]);                  // no escribimos
        P.pipes_r[i] = pipes[i][0];          // guardamos read-end
        // opcional: no-bloqueante (select igual nos protege)
        // int fl = fcntl(P.pipes_r[i], F_GETFL); fcntl(P.pipes_r[i], F_SETFL, fl|O_NONBLOCK);
        gs->players[i].pid = pid;
        // nombre visible (hasta 15 chars, cero-terminado)
        memset(gs->players[i].name, 0, sizeof(gs->players[i].name));
        strncpy(gs->players[i].name, base_name(o->pbin[i]), sizeof(gs->players[i].name)-1);
    }
}

// ============= procesamiento de un movimiento =============
static void apply_move_rr(int i, unsigned char dir, int W, int H, int *b, struct timespec *last_valid){
    // validar dir 0..7
    if (dir > 7) {
        writer_enter();
        gs->players[i].invalid_moves++;
        writer_exit();
        return;
    }
    int x = gs->players[i].x, y = gs->players[i].y;
    int nx = x + DX[dir], ny = y + DY[dir];
    bool valid = in_bounds(nx,ny,W,H) && b[idx(nx,ny,W)] > 0;

    writer_enter();
    if (!valid || gs->players[i].blocked) {
        gs->players[i].invalid_moves++;
        writer_exit();
        return;
    }
    // mover: sumar recompensa, marcar celda, actualizar pos, contadores
    int reward = b[idx(nx,ny,W)];
    gs->players[i].score += (unsigned)reward;
    gs->players[i].valid_moves++;
    gs->players[i].x = (unsigned short)nx;
    gs->players[i].y = (unsigned short)ny;
    b[idx(nx,ny,W)] = -i;  // capturada por jugador i
    writer_exit();

    // reset del timer de inactividad
    clock_gettime(CLOCK_MONOTONIC, last_valid);
}

// ============= main =============
int main(int argc, char **argv){
    // 1) señales / cleanup
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    atexit(cleanup);

    // 2) parsear
    opts_t O; parse_opts(argc, argv, &O);

    print_config(&O);

    // 3) shm unlink + permisos generosos
    shm_unlink(SHM_STATE);
    shm_unlink(SHM_SYNC);
    umask(000);

    // 4) crear shm state (flexible array)
    GS_BYTES = sizeof(game_state_t) + (size_t)O.w * (size_t)O.h * sizeof(int);
    int fd_state = shm_open(SHM_STATE, O_CREAT|O_EXCL|O_RDWR, 0666);
    if (fd_state == -1) die("shm_open(state): %s", strerror(errno));
    if (ftruncate(fd_state, (off_t)GS_BYTES) == -1) die("ftruncate(state): %s", strerror(errno));
    gs = mmap(NULL, GS_BYTES, PROT_READ|PROT_WRITE, MAP_SHARED, fd_state, 0);
    if (gs == MAP_FAILED) die("mmap(state): %s", strerror(errno));
    close(fd_state);

    // 5) crear shm sync
    int fd_sync = shm_open(SHM_SYNC, O_CREAT|O_EXCL|O_RDWR, 0666);
    if (fd_sync == -1) die("shm_open(sync): %s", strerror(errno));
    if (ftruncate(fd_sync, (off_t)sizeof(game_sync_t)) == -1) die("ftruncate(sync): %s", strerror(errno));
    gx = mmap(NULL, sizeof(game_sync_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd_sync, 0);
    if (gx == MAP_FAILED) die("mmap(sync): %s", strerror(errno));
    close(fd_sync);

    // 6) inicializar structs
    memset(gs, 0, GS_BYTES);
    memset(gx, 0, sizeof(*gx));
    gs->width  = (unsigned short)O.w;
    gs->height = (unsigned short)O.h;
    gs->num_players = (unsigned)O.nplayers;
    gs->finished = false;

    init_board_rewards(gs->board, O.w, O.h, O.seed);
    place_players(O.w, O.h, O.nplayers);

    // semáforos (pshared=1)
    if (sem_init(&gx->changes, 1, 0) == -1) die("sem_init(changes): %s", strerror(errno));
    if (sem_init(&gx->print, 1, 0) == -1) die("sem_init(print): %s", strerror(errno));
    if (sem_init(&gx->master, 1, 1) == -1) die("sem_init(master): %s", strerror(errno));
    if (sem_init(&gx->writer, 1, 1) == -1) die("sem_init(writer): %s", strerror(errno));
    if (sem_init(&gx->reader, 1, 1) == -1) die("sem_init(reader): %s", strerror(errno));
    gx->player = 0;
    for (int i=0;i<MAXP;i++)
        if (sem_init(&gx->movement[i], 1, 0) == -1) die("sem_init(movement): %s", strerror(errno));

    // 7) lanzar vista y jugadores
    spawn_view(&O);
    int pipes[MAXP][2]; memset(pipes, -1, sizeof(pipes));
    P.nplayers = O.nplayers; for(int i=0;i<P.nplayers;i++) P.pipes_r[i] = -1;
    spawn_players(&O, pipes);
    mark_blocked_players(O.w, O.h, O.nplayers, gs->board);


    // 8) primer render + habilitar 1 solicitud a cada jugador
    notify_view_and_delay(O.delay_ms);
    for (int i=0;i<O.nplayers;i++) {
        if (!gs->players[i].blocked) { // <-- NO habilites a los bloqueados
            // habilitar nueva solicitud solo si no quedó bloqueado
            writer_enter();
            bool blk = gs->players[i].blocked;
            writer_exit();
            if (!blk) {
                if (sem_post(&gx->movement[i]) == -1) die("sem_post(movement[i]): %s", strerror(errno));
            }
        }
    }
    

    // 9) loop principal (RR con select)
    struct timespec last_valid; clock_gettime(CLOCK_MONOTONIC, &last_valid);

    int rr = 0; // round-robin cursor
    while (!g_stop) {
        // a) timeout restante (inactividad)
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed_ms = (now.tv_sec - last_valid.tv_sec)*1000LL +
                               (now.tv_nsec - last_valid.tv_nsec)/1000000LL;
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

        // c) atender SOLO 1 jugador por iteración siguiendo RR
        int processed = -1;
        for (int off=0; off<O.nplayers; ++off) {
            int i = (rr + off) % O.nplayers;
            if (P.pipes_r[i] < 0) continue;
            if (!FD_ISSET(P.pipes_r[i], &rfds)) continue;

            unsigned char dir;
            ssize_t r = read(P.pipes_r[i], &dir, 1);
            if (r == 1) {
                apply_move_rr(i, dir, O.w, O.h, gs->board, &last_valid);
                // si el movimiento encerró a alguien, marcarlo como "blk"
                mark_blocked_players(O.w, O.h, O.nplayers, gs->board);
                notify_view_and_delay(O.delay_ms);
                // habilitar nueva solicitud a ese jugador
                if (sem_post(&gx->movement[i]) == -1) die("sem_post(movement[i]): %s", strerror(errno));
            } else if (r == 0) {
                // EOF => marcar bloqueado y cerrar fd
                writer_enter();
                gs->players[i].blocked = true;
                writer_exit();
                close(P.pipes_r[i]);
                P.pipes_r[i] = -1;
                notify_view_and_delay(O.delay_ms);
            } else {
                if (errno != EINTR && errno != EAGAIN) {
                    // error de lectura => cerrar
                    close(P.pipes_r[i]);
                    P.pipes_r[i] = -1;
                }
            }
            processed = i;
            break;
        }
        if (processed >= 0) rr = (processed + 1) % O.nplayers;

        // d) si nadie puede moverse, terminar
        if (!any_player_can_move(O.w, O.h, O.nplayers, gs->board))
            break;
    }
    // 10) finalizar juego (sin cerrar aún los pipes)
    writer_enter();
    gs->finished = true;
    writer_exit();
    notify_view_and_delay(O.delay_ms);

    // despertar a todos para que puedan ver 'finished' y salir
    for (int i = 0; i < O.nplayers; ++i) sem_post(&gx->movement[i]);

    // 11) drenar lo que quede y recién después cerrar nuestros FDs
    //    (2000 ms de gracia suele ser suficiente)
    drain_players_until_exit(O.nplayers, 2000);
    
    // 12) esperar hijos e imprimir resultados
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
        if (stream_isatty(stderr)) { c1 = ansi_player_color(i); c0 = ANSI_RESET; }

        unsigned s  = gs->players[i].score;
        unsigned v  = gs->players[i].valid_moves;
        unsigned iv = gs->players[i].invalid_moves;

        char letter = 'A' + i;
        if (WIFEXITED(status)) {
            // coloreo solo "Player <letra> <nombre>"
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

