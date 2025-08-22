# === Toolchain / flags ===
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -pedantic
LDFLAGS := -pthread

# === Ejecutables ===
VIEW := src/view
P1   := src/player_greedy
MASTER := src/master

all: $(VIEW) $(P1) $(MASTER)

# --- Jugador (no usa ncurses) ---
$(P1): src/player_greedy.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# --- Vista (usa ncurses) ---
$(VIEW): src/view.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lncurses

$(MASTER): src/master.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)


# --- Ejecutar con el máster de la cátedra ---    BORRAR EL RUN EN LA ENTREGA
runcat:
	./src/masterCatedra -w 10 -h 10 -d 200 -t 10 -v ./src/view -p ./src/player_greedy ./src/player_greedy ./src/player_greedy

run:
	./src/master -w 10 -h 10 -d 200 -t 10 -v ./src/view -p ./src/player_greedy ./src/player_greedy ./src/player_greedy
clean:
	rm -f $(VIEW) $(P1) $(MASTER)

.PHONY: all deps run clean
