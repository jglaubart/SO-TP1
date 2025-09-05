#define _DEFAULT_SOURCE
#include "sync_utils.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

int gx_create_and_init(game_sync_t **gx_out)
{
    if (!gx_out) return -1;
    *gx_out = NULL;

    shm_unlink(SHM_SYNC);
    int fd = shm_open(SHM_SYNC, O_CREAT|O_EXCL|O_RDWR, 0666);
    if (fd == -1) return -1;
    if (ftruncate(fd, (off_t)sizeof(game_sync_t)) == -1) { close(fd); return -1; }

    game_sync_t *gx = mmap(NULL, sizeof(game_sync_t),
                           PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (gx == MAP_FAILED) return -1;

    memset(gx, 0, sizeof(*gx));

    /* sem_init(pshared=1) */
    if (sem_init(&gx->state_changed,           1, 0) == -1) return -1;
    if (sem_init(&gx->state_rendered,          1, 0) == -1) return -1;
    if (sem_init(&gx->writer_starvation_mutex, 1, 1) == -1) return -1;
    if (sem_init(&gx->state_write_lock,        1, 1) == -1) return -1;
    if (sem_init(&gx->readers_count_lock,      1, 1) == -1) return -1;
    gx->readers_count = 0;
    for (int i = 0; i < 9; ++i)
        if (sem_init(&gx->movement[i], 1, 0) == -1) return -1;

    *gx_out = gx;
    return 0;
}

int gx_open_rw(game_sync_t **gx_out)
{
    if (!gx_out) return -1;
    *gx_out = NULL;

    int fd = shm_open(SHM_SYNC, O_RDWR, 0);
    if (fd == -1) return -1;

    struct stat st;
    if (fstat(fd, &st) == -1) { close(fd); return -1; }
    if ((size_t)st.st_size < sizeof(game_sync_t)) { close(fd); return -1; }

    game_sync_t *gx = mmap(NULL, sizeof(game_sync_t),
                           PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (gx == MAP_FAILED) return -1;

    *gx_out = gx;
    return 0;
}

void gx_close(game_sync_t *gx)
{
    if (gx) munmap(gx, sizeof(*gx));
}

void gx_destroy_sems(game_sync_t *gx)
{
    if (!gx) return;
    sem_destroy(&gx->state_changed);
    sem_destroy(&gx->state_rendered);
    sem_destroy(&gx->writer_starvation_mutex);
    sem_destroy(&gx->state_write_lock);
    sem_destroy(&gx->readers_count_lock);
    for (int i = 0; i < 9; ++i) sem_destroy(&gx->movement[i]);
}

int sem_wait_intr(sem_t *s)
{
    for (;;) {
        if (sem_wait(s) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

/* Lectores–Escritor con preferencia al escritor */
void reader_enter(game_sync_t *gx)
{
    sem_wait_intr(&gx->writer_starvation_mutex);
    sem_wait_intr(&gx->readers_count_lock);
    gx->readers_count++;
    if (gx->readers_count == 1) sem_wait_intr(&gx->state_write_lock);
    sem_post(&gx->readers_count_lock);
    sem_post(&gx->writer_starvation_mutex);
}

void reader_exit(game_sync_t *gx)
{
    sem_wait_intr(&gx->readers_count_lock);
    if (gx->readers_count > 0) gx->readers_count--;
    if (gx->readers_count == 0) sem_post(&gx->state_write_lock);
    sem_post(&gx->readers_count_lock);
}

void writer_enter(game_sync_t *gx)
{
    sem_wait_intr(&gx->writer_starvation_mutex);
    sem_wait_intr(&gx->state_write_lock);
}

void writer_exit(game_sync_t *gx)
{
    sem_post(&gx->state_write_lock);
    sem_post(&gx->writer_starvation_mutex);
}

/* Máster ↔ Vista (A/B) + delay */
void sync_notify_view_and_delay(game_sync_t *gx, bool has_view, int delay_ms,
                                volatile sig_atomic_t *g_stop)
{
    if (has_view) {
        sem_post(&gx->state_changed);  /* A */
        if (!g_stop || !(*g_stop)) {
            sem_wait_intr(&gx->state_rendered); /* B */
        } else {
            (void)sem_trywait(&gx->state_rendered);
        }
    }
    if (delay_ms > 0) {
        struct timespec ts = { delay_ms/1000, (delay_ms%1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
}

/* Turnos de jugador */
int sync_allow_one_move(game_sync_t *gx, int i)
{
    if (!gx || i < 0 || i >= 9) return -1;
    return sem_post(&gx->movement[i]);
}

int sync_wait_my_turn(game_sync_t *gx, int i)
{
    if (!gx || i < 0 || i >= 9) return -1;
    return sem_wait_intr(&gx->movement[i]);
}
