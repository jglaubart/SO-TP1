# === Imagen obligatoria de la cátedra ===
IMG      := agodio/itba-so-multi-platform:3.0
CONT_DIR := /SO-TP1
HOST_DIR := $(shell pwd)

# === Flags de compilación (dentro del contenedor) ===
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2
LDFLAGS := -pthread

TARGETS := player
# TARGETS := master view player

all: docker-build

docker-pull:
	@docker pull $(IMG) >/dev/null

docker-build: docker-pull
	@docker run --rm -v "$(HOST_DIR):$(CONT_DIR)" -w "$(CONT_DIR)" $(IMG) \
		bash -lc 'make -s clean && make -s inner-build'

# === Compilación "interna" (se ejecuta dentro del contenedor) ===
inner-build: $(TARGETS)

# --- Destapar cuando existan esos archivos ---
# master: src/master.c
# 	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
#
# view: src/view.c
# 	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

player: src/player.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f master view player

#---- esto es para que se ejecute en docker, se hace automaticamente con 3 players ----#
run:
	docker run --rm -it -v "$(PWD):/SO-TP1" -w /SO-TP1 $(IMG) \
	  bash -lc './src/masterCatedra -w 10 -h 10 -d 200 -t 10 -p ./player ./player ./player ./player'

.PHONY: all docker-pull docker-build inner-build clean

