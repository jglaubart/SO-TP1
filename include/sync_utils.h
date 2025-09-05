#pragma once
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>

/* ===== Segmento de SINCRONIZACIÓN ===== */
#define SHM_SYNC "/game_sync"

typedef struct {
    sem_t state_changed;            /* A: máster → vista — hay cambios */
    sem_t state_rendered;           /* B: vista  → máster — terminó de imprimir */
    sem_t writer_starvation_mutex;  /* C: evita inanición del máster (preferencia escritor) */
    sem_t state_write_lock;         /* D: exclusión de escritura sobre el estado */
    sem_t readers_count_lock;       /* E: mutex del contador de lectores */
    unsigned int readers_count;     /* F: # lectores activos (jugadores/vista) */
    sem_t movement[9];              /* G[i]: permiso a jugador i para 1 movimiento */
} game_sync_t;

/* ============ API sync (SHM /game_sync) ============ */

/* Crea + init semáforos (solo master). Devuelve 0 si ok. */
int gx_create_and_init(game_sync_t **gx_out);

/* Abre en RW (view y player). Devuelve 0 si ok. */
int gx_open_rw(game_sync_t **gx_out);

/* Cierre/unmap. */
void gx_close(game_sync_t *gx);

/* Destruye todos los semáforos (solo master, antes de cerrar). */
void gx_destroy_sems(game_sync_t *gx);

/* ===== Esperas robustas y esquema lectores–escritor ===== */
int  sem_wait_intr(sem_t *s); /* reintenta si EINTR, devuelve 0 si ok */
void reader_enter(game_sync_t *gx);
void reader_exit (game_sync_t *gx);
void writer_enter(game_sync_t *gx);
void writer_exit (game_sync_t *gx);

/* ===== Máster ↔ Vista + delay ===== */
void sync_notify_view_and_delay(game_sync_t *gx, bool has_view, int delay_ms,
                                volatile sig_atomic_t *g_stop);

/* ===== Turnos jugador (G[i]) ===== */
int  sync_allow_one_move(game_sync_t *gx, int i); /* post movement[i] */
int  sync_wait_my_turn  (game_sync_t *gx, int i); /* wait movement[i] */
