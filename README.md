# ChompChamps — TP1 de Sistemas Operativos

Este proyecto implementa un juego tipo *snake* multijugador llamado **ChompChamps**, como parte del Trabajo Práctico Nº1 de la materia Sistemas Operativos (ITBA).  
Está desarrollado en C y utiliza memoria compartida, semáforos y pipes para comunicar tres procesos distintos:

- `master`: coordina el juego y la sincronización
- `view`: renderiza el estado del juego en consola (usando `ncurses`)
- `player`: jugador automático que decide sus movimientos

## 🐧 Cómo compilar y correr (usando Docker)

1. Ingresar a un contenedor de docker con la imagen oficial de la cátedra `agodio/itba-so-multi-platform:3.0` o bien correr:

   ```bash
   make docker_cont
   ```

   Esto abre un contenedor temporal con la imagen y monta el proyecto en `/root`.

2. Una vez adentro del contenedor, ir a la carpeta montada (ya dentro de `/root`) y correr:

   ```bash
   make
   ```

   Esto va a instalar automáticamente las dependencias (`libncurses-dev`, `ncurses-term`) si no están, y compilar los tres binarios: `master`, `view` y `player`.

3. Para ejecutar una partida de prueba con configuración estándar (10x10, 200ms delay, 3 jugadores automáticos), usá:

   ```bash
   make run
   ```

   O bien se puede ejecutar directamente con parámetros a eleccion, por ejemplo:

   ```bash
   ./src/master -w 15 -h 10 -d 100 -t 8 -v ./src/view -p ./src/player ./src/player ./src/player
   ```
   ### 📥 Significado de los parámetros:

   - `-w 15`: ancho del tablero (mínimo 10)
   - `-h 10`: alto del tablero (mínimo 10)
   - `-d 100`: delay entre fotogramas, en milisegundos (100 ms = 0.1 segundos)
   - `-t 8`: timeout por inactividad (en segundos). Si no hay movimientos válidos durante este tiempo, el juego termina
   - `-s 1234`: *(opcional)* semilla para el generador de recompensas (por default usa `time(NULL)`)
   - `-v ./src/view`: ruta al binario de la vista (puede omitirse para jugar sin vista)
   - `-p jugador1 jugador2 ...`: **último parámetro obligatorio**. Lista de ejecutables de jugadores (entre 1 y 9). Cada uno es lanzado como un proceso hijo.

   ### 📌 Notas:
   - El orden de los jugadores determina su letra (A, B, C...).
   - Los jugadores reciben el tamaño del tablero como argumentos (`width height`).
   - El `master` los atiende con política **round-robin**.

## 🧹 Limpieza

Para borrar binarios y objetos compilados:

```bash
make clean
```

## ✅ Requisitos del entorno

- Este TP **debe** ejecutarse y compilarse dentro de la imagen oficial de la materia:  
  `agodio/itba-so-multi-platform:3.0`

- `make` ya se encarga de instalar lo necesario si usás el contenedor y tenés red (instala `libncurses-dev` y `ncurses-term` si faltan).

## 🕹️ Sobre el juego

- Cada jugador automático analiza el estado del tablero y elige un movimiento entre 8 posibles direcciones (0 a 7).
- El `master` atiende a los jugadores en **round-robin**.
- La `view` se sincroniza por semáforos y muestra la grilla con las recompensas, cuerpos y cabezas de los jugadores, sus estadísticas y posiciones.

> El juego termina cuando no quedan movimientos posibles o se alcanza el timeout.  
> La `view` muestra el estado final hasta que se presione una tecla.

---

> Proyecto desarrollado para la materia **Sistemas Operativos** — ITBA  
