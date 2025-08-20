# === Imagen obligatoria de la cátedra ===
IMG      := agodio/itba-so-multi-platform:3.0
CONT     := SO                     # nombre de tu contenedor persistente
CONT_DIR := /root
HOST_DIR := $(shell pwd)

# === Flags de compilación ===
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2
LDFLAGS := -pthread -lncurses

#agregar master cuando se haga
TARGETS := src/player_greedy src/view src/player_greedyPlus

# Por defecto: compilar en el contenedor persistente
all: docker-build

# Compilación dentro del contenedor persistente
inner-build: $(TARGETS)

src/view: src/view.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

src/player_greedyPlus: src/player_greedyPlus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

src/player_greedy: src/player_greedy.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# --- Destapar cuando existan esos archivos ---
# src/master: src/master.c
#	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f src/player_greedy src/player_greedyPlus src/view src/master 

# ---- targets docker ----
docker-build:
	docker exec -it $(CONT) bash -lc "cd $(CONT_DIR) && make clean && make inner-build"

run:
	docker exec -it $(CONT) bash -lc "cd $(CONT_DIR)/src && ./masterCatedra -w 15 -h 15 -d 200 -t 15 -v ./view -p ./player_greedyPlus ./player_greedy ./player_greedy ./player_greedy ./player_greedy"


.PHONY: all inner-build clean docker-build run run-view
