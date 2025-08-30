# === Toolchain / flags ===
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -pedantic
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
OBJS_PLAYER := src/player.o src/player_strategies.o

.PHONY: all clean deps deps-reset check-colors run runcat

# Compila todo
all: clean $(VIEW) $(PLAYER) $(MASTER)

# ===== Dependencias del sistema (idempotente con stamp) =====
DEB_PKGS    := libncurses-dev ncurses-term
DEPS_STAMP  := .deps.stamp

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
	# Garantizar terminfo xterm-256color
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

src/player.o: src/player.c src/player_strategies.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/player_strategies.o: src/player_strategies.c src/player_strategies.h
	$(CC) $(CFLAGS) -c -o $@ $<

# --- View (depende de deps para tener terminfo 256) ---
$(VIEW): src/view.c | deps
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(NCURSES)

# --- Master ---
$(MASTER): src/master.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# --- Ejecutar con máster de la cátedra (debug) ---
runcat:
	./src/masterCatedra -w 10 -h 10 -d 300 -t 10 \
		-v ./src/view -p ./src/player ./src/player ./src/player

# --- Ejecutar con tu máster ---
run:
	./src/master -w 15 -h 15 -d 300 -t 10 \
		-v ./src/view -p ./src/player ./src/player ./src/player ./src/player ./src/player ./src/player

# --- Clean ---
clean:
	rm -f $(VIEW) $(PLAYER) $(MASTER) $(OBJS_PLAYER)

