# ==== Configuración general ====
CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2
LDFLAGS  := -pthread
TARGETS  := player           # agregá 'view master' cuando los tengas

# Imagen y paths para usar Docker desde el host
DOCKER_IMG := agodio/itba-so-multi-platform:3.0
# Cambiá esta ruta al path real de tu repo en tu máquina:
HOST_DIR   := $(HOME)/Documents/itba/SO/SO-TP1
CONT_DIR   := /SO-TP1

# ==== Binarios ====
all: $(TARGETS)

player: player.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean run-local docker-sh docker-build docker-run docker-clean

# ==== Conveniencias para Docker DESDE EL HOST ====
# Abre una shell dentro del contenedor con la carpeta montada
docker-sh:
	docker run -it --rm -v "$(HOST_DIR):$(CONT_DIR)" -w "$(CONT_DIR)" $(DOCKER_IMG) bash

# Compila adentro del contenedor (equivalente a hacer: docker-sh + make)
docker-build:
	docker run --rm -v "$(HOST_DIR):$(CONT_DIR)" -w "$(CONT_DIR)" $(DOCKER_IMG) \
		bash -lc 'make clean && make all'

# Ejecuta el máster de la cátedra con 3 players compilados (todo adentro del contenedor)
# Parámetros ajustables por variables:
#   W=10 H=10 D=200 T=10 SEED= (vacío usa time(NULL)) VIEW= (ruta bin vista o vacío)
W ?= 10
H ?= 10
D ?= 200
T ?= 10
SEED ?=
VIEW ?=
PLAYERS ?= ./player ./player ./player

docker-run:
	docker run --rm -it -v "$(HOST_DIR):$(CONT_DIR)" -w "$(CONT_DIR)" $(DOCKER_IMG) \
		bash -lc '\
			if [ ! -x ./player ]; then echo "[ERROR] Falta compilar ./player. Corré: make docker-build"; exit 1; fi; \
			CMD="./masterCatedra -w $(W) -h $(H) -d $(D) -t $(T)"; \
			[ -n "$(SEED)" ] && CMD="$$CMD -s $(SEED)"; \
			[ -n "$(VIEW)" ] && CMD="$$CMD -v $(VIEW)"; \
			echo "$$CMD -p $(PLAYERS)"; \
			$$CMD -p $(PLAYERS) \
		'

# Limpia binarios desde dentro del contenedor (opcional)
docker-clean:
	docker run --rm -v "$(HOST_DIR):$(CONT_DIR)" -w "$(CONT_DIR)" $(DOCKER_IMG) \
		bash -lc 'make clean'
