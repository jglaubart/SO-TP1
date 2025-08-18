#!/usr/bin/env bash
set -euo pipefail

# Ajustá este path a tu carpeta del TP
HOST_DIR="${HOME}/Documents/itba/SO/SO-TP1"
CONT_DIR="/SO-TP1"
IMG="agodio/itba-so-multi-platform:3.0"

# Parámetros del juego (podés cambiarlos por flags si querés)
W=10
H=10
D=200
T=10
SEED=""
VIEW=""   # ej: ./vista

echo "[1/2] Compilando dentro de Docker…"
docker run --rm -v "${HOST_DIR}:${CONT_DIR}" -w "${CONT_DIR}" "${IMG}" \
  bash -lc 'make clean && make all'

echo "[2/2] Ejecutando máster con 3 players…"
docker run -it --rm -v "${HOST_DIR}:${CONT_DIR}" -w "${CONT_DIR}" "${IMG}" \
  bash -lc "\
    CMD=\"./masterCatedra -w ${W} -h ${H} -d ${D} -t ${T}\"; \
    [ -n \"${SEED}\" ] && CMD=\"\$CMD -s ${SEED}\"; \
    [ -n \"${VIEW}\" ] && CMD=\"\$CMD -v ${VIEW}\"; \
    echo \"\$CMD -p ./player ./player ./player\"; \
    \$CMD -p ./player ./player ./player \
  "
