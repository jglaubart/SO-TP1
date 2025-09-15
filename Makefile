# === Toolchain / flags ===
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -pedantic -Iinclude
LDFLAGS := -pthread
NCURSES := -lncurses

# --- Forzar 256 colores en todo lo que ejecute make ---
TERM ?= xterm-256color
export TERM

# === Ejecutables ===
VIEW    := src/view
PLAYER  := src/player
MASTER  := src/master

# === Objetos intermedios ===
OBJS_PLAYER := src/player.o src/player_strategies.o src/shared_mem.o src/sync_utils.o src/game_utils.o
OBJS_VIEW   := src/view.o src/shared_mem.o src/sync_utils.o src/game_utils.o
OBJS_MASTER := src/master.o src/shared_mem.o src/sync_utils.o src/game_utils.o

.PHONY: all clean deps deps-reset check-colors run runcat

# Compila todo
all: clean deps $(VIEW) $(PLAYER) $(MASTER)

# ===== Dependencias del sistema (idempotente con stamp) =====
DEB_PKGS    := libncurses-dev ncurses-term
DEPS_STAMP  := /var/.tp1_deps.stamp

deps: $(DEPS_STAMP)

$(DEPS_STAMP):
	@set -e; \
	missing=0; \
	for pkg in $(DEB_PKGS); do \
	  if ! dpkg -s $$pkg >/dev/null 2>&1; then \
	    echo "Falta $$pkg"; missing=1; \
	  fi; \
	done; \
	if [ $$missing -eq 1 ]; then \
	  echo "Instalando: $(DEB_PKGS)"; \
	  apt-get update && apt-get install -y $(DEB_PKGS); \
	else \
	  echo "Dependencias ncurses ya presentes"; \
	fi; \
	# Garantizar xterm-256color
	if ! infocmp xterm-256color >/dev/null 2>&1; then \
	  echo "Instalando terminfo xterm-256color (ncurses-term)..."; \
	  apt-get update && apt-get install -y ncurses-term; \
	fi; \
	echo "TERM=$$TERM  colors=$$(tput -T xterm-256color colors 2>/dev/null || echo N/A)"; \
	touch $@

deps-reset:
	@rm -f $(DEPS_STAMP)

check-colors:
	@echo "TERM=$(TERM)  colors=$$(tput -T xterm-256color colors 2>/dev/null || echo N/A)"

# --- Player ---
$(PLAYER): $(OBJS_PLAYER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/player.o: src/player.c include/player_strategies.h include/shared_mem.h include/sync_utils.h include/game_utils.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/player_strategies.o: src/player_strategies.c include/player_strategies.h
	$(CC) $(CFLAGS) -c -o $@ $<

# View objects
src/view.o: src/view.c include/shared_mem.h include/sync_utils.h include/game_utils.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Libs usadas por view
src/%.o: src/%.c include/%.h
	$(CC) $(CFLAGS) -c -o $@ $<

# --- View (depende de deps para tener 256 colores) ---
$(VIEW): $(OBJS_VIEW) | deps
	$(CC) $(CFLAGS) -o $@ $(OBJS_VIEW) $(LDFLAGS) $(NCURSES)

# --- Master ---
$(MASTER): $(OBJS_MASTER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/master.o: src/master.c include/shared_mem.h include/sync_utils.h include/game_utils.h
	$(CC) $(CFLAGS) -c -o $@ $<


# --- Entrar a un contenedor Docker temporal con la imagen de la cátedra ---
docker_cont:
	docker run --rm -it -v "$$(pwd)":/root -w /root agodio/itba-so-multi-platform:3.0

# --- Ejecutar con máster ---
run:
	./src/master -w 10 -h 10 -d 200 -t 10 \
		-v ./src/view -p ./src/player ./src/player ./src/player

# --- Clean ---
clean:
	rm -f $(VIEW) $(PLAYER) $(MASTER) $(OBJS_PLAYER) $(OBJS_VIEW) $(OBJS_MASTER)

