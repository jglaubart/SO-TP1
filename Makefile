# === Toolchain / flags ===
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -pedantic
LDFLAGS := -pthread
NCURSES := -lncurses

# === Ejecutables ===
VIEW    := src/view
PLAYER  := src/player
MASTER  := src/master

# === Objetos intermedios ===
OBJS_PLAYER := src/player.o src/player_strategies.o

.PHONY: all clean deps run runcat

# Compila todo
all: $(VIEW) $(PLAYER) $(MASTER)

# --- Dependencias (instala headers de ncurses si faltan) ---
deps:
	@missing_headers=0; \
	if [ ! -f /usr/include/ncurses.h ] && [ ! -f /usr/include/ncurses/ncurses.h ]; then \
		missing_headers=1; \
	fi; \
	if [ "$$missing_headers" -eq 0 ]; then \
		echo "ncurses headers already present"; \
		exit 0; \
	fi; \
	if command -v dpkg >/dev/null 2>&1; then \
		echo "Installing libncurses-dev via apt..."; \
		apt-get update && apt-get install -y libncurses-dev; \
	elif command -v apk >/dev/null 2>&1; then \
		echo "Installing ncurses-dev via apk..."; \
		apk add --no-cache ncurses-dev; \
	elif command -v dnf >/dev/null 2>&1; then \
		echo "Installing ncurses-devel via dnf..."; \
		dnf install -y ncurses-devel; \
	elif command -v yum >/dev/null 2>&1; then \
		echo "Installing ncurses-devel via yum..."; \
		yum install -y ncurses-devel; \
	elif command -v pacman >/dev/null 2>&1; then \
		echo "Installing ncurses via pacman..."; \
		pacman -Sy --noconfirm ncurses; \
	else \
		echo "No known package manager found. Please install ncurses headers manually."; \
		exit 1; \
	fi

# --- Player ---
$(PLAYER): $(OBJS_PLAYER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/player.o: src/player.c src/player_strategies.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/player_strategies.o: src/player_strategies.c src/player_strategies.h
	$(CC) $(CFLAGS) -c -o $@ $<

# --- View ---
$(VIEW): src/view.c | deps
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(NCURSES)

# --- Master ---
$(MASTER): src/master.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# --- Ejecutar con máster de la cátedra (debug) ---
runcat:
	./src/masterCatedra -w 10 -h 10 -d 200 -t 10 \
		-v ./src/view -p ./src/player ./src/player ./src/player

# --- Ejecutar con tu máster ---
run:
	./src/master -w 10 -h 10 -d 200 -t 10 \
		-v ./src/view -p ./src/player ./src/player ./src/player

# --- Clean ---
clean:
	rm -f $(VIEW) $(PLAYER) $(MASTER) $(OBJS_PLAYER)
