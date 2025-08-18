#!/usr/bin/env bash
set -euo pipefail

# Ajustá si tu carpeta está en otro lado
HOST_DIR="${HOST_DIR:-$HOME/Documents/itba/SO/SO-TP1}"
CONT_DIR="/SO-TP1"
IMG="agodio/itba-so-multi-platform:3.0"

# Parámetros del juego (podés sobreescribir con env: W=20 ./run.sh)
W="${W:-10}"
H="${H:-10}"
D="${D:-200}"
T="${T:-10}"
SEED="${SEED:-}"
VIEW="${VIEW:-}"

# Cantidad de players (default 3)
NUM_PLAYERS="${1:-3}"

# Generar "./player ./player ..."
PLAYERS=""
for _ in $(seq 1 "$NUM_PLAYERS"); do
  PLAYERS="$PLAYERS ./player"
done

# 1) Compilar en Docker (quiet)
docker run --rm -v "$HOST_DIR:$CONT_DIR" -w "$CONT_DIR" "$IMG" \
  bash -lc 'make -s clean all'

# 2) Ejecutar en Docker (sin prints extra)
docker run -it --rm -v "$HOST_DIR:$CONT_DIR" -w "$CONT_DIR" "$IMG" \
  bash -lc '
    set -e
    CMD="./masterCatedra -w '"$W"' -h '"$H"' -d '"$D"' -t '"$T"'"
    [ -n "'"$SEED"'" ] && CMD="$CMD -s '"$SEED"'"
    [ -n "'"$VIEW"'" ] && CMD="$CMD -v '"$VIEW"'"
    # Ejecutar; nota: el master imprime su propia config/resumen.
    eval "$CMD -p '"$PLAYERS"'" 2>/dev/null

  '
