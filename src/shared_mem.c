#define _DEFAULT_SOURCE
#include "shared_mem.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

int gs_create_and_init(int W, int H, unsigned nplayers,
                       game_state_t **gs_out, size_t *gs_bytes_out)
{
    if (!gs_out || !gs_bytes_out) return -1;
    *gs_out = NULL; *gs_bytes_out = 0;

    size_t bytes = sizeof(game_state_t) + (size_t)W * (size_t)H * sizeof(int);

    /* crear shm (unlink previo para asegurar creación limpia) */
    shm_unlink(SHM_STATE);
    int fd = shm_open(SHM_STATE, O_CREAT|O_EXCL|O_RDWR, 0666);
    if (fd == -1) return -1;
    if (ftruncate(fd, (off_t)bytes) == -1) { close(fd); return -1; }

    game_state_t *gs = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (gs == MAP_FAILED) return -1;

    /* init básico */
    memset(gs, 0, bytes);
    gs->width  = (unsigned short)W;
    gs->height = (unsigned short)H;
    gs->num_players = nplayers;
    gs->finished = false;

    *gs_out = gs;
    *gs_bytes_out = bytes;
    return 0;
}

int gs_open_ro(game_state_t **gs_out, size_t *gs_bytes_out)
{
    if (!gs_out || !gs_bytes_out) return -1;
    *gs_out = NULL; *gs_bytes_out = 0;

    int fd = shm_open(SHM_STATE, O_RDONLY, 0);
    if (fd == -1) return -1;

    struct stat st;
    if (fstat(fd, &st) == -1) { close(fd); return -1; }

    game_state_t *gs = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (gs == MAP_FAILED) return -1;

    *gs_out = gs;
    *gs_bytes_out = (size_t)st.st_size;
    return 0;
}

void gs_close(game_state_t *gs, size_t gs_bytes)
{
    if (gs && gs_bytes) munmap(gs, gs_bytes);
}

/* ===== utilitarias ligadas al estado ===== */

void gs_init_board_rewards(int *board, int W, int H, unsigned seed)
{
    /* valores 1..9 (libres), caller se encarga de marcar capturas negativas */
    srand(seed ? seed : (unsigned)time(NULL));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            board[y*W + x] = 1 + (rand() % 9);
}

int gs_place_players(game_state_t *gs)
{
    if (!gs) return -1;
    int W = (int)gs->width, H = (int)gs->height, n = (int)gs->num_players;
    int total = W * H;
    if (n < 0 || n > 9 || n > total || total > 10000) return -1;

    int positions[10000];
    for (int i = 0; i < total; ++i) positions[i] = i;

    /* Fisher–Yates */
    for (int i = total - 1; i > 0; --i) {
        int j = rand() % (i + 1);
        int tmp = positions[i]; positions[i] = positions[j]; positions[j] = tmp;
    }

    for (int p = 0; p < n; ++p) {
        int pos = positions[p];
        int x = pos % W, y = pos / W;
        gs->players[p].x = (unsigned short)x;
        gs->players[p].y = (unsigned short)y;
        gs->players[p].score = 0;
        gs->players[p].valid_moves = 0;
        gs->players[p].invalid_moves = 0;
        gs->players[p].blocked = false;
        gs->board[y*W + x] = -p; /* capturada por el jugador p */
    }
    return 0;
}
