// structs.h
#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdbool.h>
#include <semaphore.h>
#include <sys/types.h>

// nombres de las memorias compartidas
#define SHM_STATE "/game_state"
#define SHM_SYNC  "/game_sync"

// información de un jugador
typedef struct {
    char name[16];              // nombre del jugador
    unsigned int score;         // puntaje
    unsigned int invalid_moves; // movimientos inválidos realizados
    unsigned int valid_moves;   // movimientos válidos realizados
    unsigned short x, y;        // coordenadas actuales
    pid_t pid;                  // pid del proceso jugador
    bool blocked;               // bloqueado (EOF en su pipe)
} player_t;

// estado global del juego
typedef struct {
    unsigned short width;       // ancho
    unsigned short height;      // alto
    unsigned int num_players;   // cantidad de jugadores (<= 9)
    player_t players[9];        // jugadores
    bool finished;              // juego finalizado
    int board[];                // tablero: fila-0, fila-1, ..., fila-(h-1)
} game_state_t;


// A: master -> vista (hay cambios)
// B: vista -> master (impresión lista)
// C: mutex para evitar inanición del escritor (master)
// D: mutex de acceso de escritores al estado
// E: mutex del contador de lectores, siguiente variable
// F: contador de jugadores leyendo el estado
// G[i]: permiso a cada jugador para enviar 1 movimiento
typedef struct {
    sem_t changes;
    sem_t print;
    sem_t master;
    sem_t writer;
    sem_t reader;
    unsigned int player;
    sem_t movement[9];
} game_sync_t;

#endif // STRUCTS_H
