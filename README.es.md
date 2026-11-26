Language: [English](https://github.com/briefnotion/vtmux/blob/main/README.md), [Español](https://github.com/briefnotion/vtmux/blob/main/README.es.md), [Français](https://github.com/briefnotion/vtmux/blob/main/README.fr.md), [Deutsch](https://github.com/briefnotion/vtmux/blob/main/README.de.md), [日本語](https://github.com/briefnotion/vtmux/blob/main/README.ja.md), [中文](https://github.com/briefnotion/vtmux/blob/main/README.zh.md), [Corporate](https://github.com/briefnotion/vtmux/blob/main/README.corporate.md), [Cajun](https://github.com/briefnotion/vtmux/blob/main/README.cajun.md), [CyberPunk](https://github.com/briefnotion/vtmux/blob/main/README.cyberpunk.md)

# vtmux

Un multiplexor de terminal con transiciones deslizantes y con fundidos. Ejecuta `vtmux` en una consola y verás lo que parece la misma consola, con una fila extra
en la parte inferior: la barra de control. Todo funciona como antes. Pulsa `Ctrl-B` y la barra cambia de color: estás en el modo de comandos, donde puedes
crear y cerrar consolas escribiendo un comando después de dos puntos, o pasar de una consola a otra con las flechas izquierda y derecha, y las
pantallas se deslizan (o se funden) unas con otras.

![vtmux](docs/demo_sheet.png)

*vtmux ejecutando programas reales, con su barra a lo largo de la parte inferior. Arriba: el mapa de radar de [vtweather](https://github.com/briefnotion/vtweather) en la
segunda consola, con `Ctrl-B` pulsado (la barra es ámbar: modo de comandos). Abajo a la izquierda: la escena de la tormenta de vtshow, en la cuarta consola.
Abajo a la derecha: un programa de chat que muestra texto chino (生日快乐, "feliz cumpleaños"), dibujado a dos columnas de ancho.*

Está construido sobre [vtscreen](https://github.com/briefnotion/vtscreen), la misma biblioteca de dibujo que usan vtshow y vtweather, con sus
imágenes retenidas: una consola inactiva casi no cuesta nada de dibujar. El emulador de terminal que lleva dentro (`src/emulator.*`) es nuevo: un analizador
de máquina de estados y una pantalla en memoria que se prueba contra `tmux` y `pyte`.

## Compilar y ejecutar

Linux, `g++` con C++17, `cmake` 3.16 o posterior, `git`:

    git clone --recursive --shallow-submodules --depth 1 https://github.com/briefnotion/vtmux.git
    cd vtmux/build
    ./m
    ./vtmux

`./m` (en el directorio `build`) compila vtmux, o lo recompila después de un cambio. También descarga el submódulo `vtscreen` si el clon se hizo
sin él, y avisa si falta `cmake` o un compilador. `./m test` compila también las pruebas y las ejecuta, y `./m clean` borra lo compilado.

`./install` (también en `build`) ejecuta `./m` y luego copia `vtmux` a `~/.local/bin`, para que puedas escribir `vtmux` en cualquier terminal (o a `/usr/local/bin`
si lo ejecutas como root). `./install --system` lo pone en `/usr/local/bin` para todos (pide sudo), `./install DIR` lo pone en `DIR`, y
`./install --remove` lo quita de nuevo. Si la carpeta no está en tu `PATH`, lo dice y muestra la línea que hay que añadir.

Lo mismo que `./m`, a mano: `cmake -B build && cmake --build build`. `-DVTMUX_TESTS=OFF` omite la compilación de las pruebas.

    vtmux [--transition slide|dip|dissolve|crossfade] [--transition-ms N] [--no-color] [--fps N] [-- command args ...]

Sin comando, la primera consola ejecuta tu shell (`$SHELL`). Los programas de dentro se ejecutan con `TERM=xterm-256color`.

## Teclas

En el modo normal, cada tecla va al programa de la consola, sin tocar (también Ctrl-C y Ctrl-Z). La única tecla vigilada es `Ctrl-B`.

| en el modo de comandos (después de `Ctrl-B`) | hace |
|---|---|
| `Left`, `Right` | la consola anterior, la siguiente, con la transición (un deslizamiento a lo largo de la fila de consolas; desde la última, `Right` desliza todo el camino de vuelta a la primera); sigue en el modo de comandos para que puedas seguir pasando |
| `1` a `9` | esa consola, y vuelve al modo normal |
| `c`, `n`, `p` | una consola nueva, la siguiente, la anterior (como en tmux) |
| `Up`, `Down`, `PageUp`, `PageDown`, `Home` | mira atrás en el historial de la consola (2000 líneas); la barra muestra hasta dónde has retrocedido. `End` vuelve a la pantalla en vivo |
| `:` | escribe un comando (Enter lo ejecuta, Esc lo abandona, Backspace edita, Ctrl-U borra) |
| `Esc`, `Enter` | vuelve al modo normal (y a la pantalla en vivo). Escribir en la consola hace lo mismo |
| `Ctrl-B` | envía un Ctrl-B de verdad al programa, y vuelve al modo normal |

## Deslizamiento

Las consolas están una al lado de otra en una fila, en el orden de la barra. Al cambiar, la vista se desliza a lo largo de la fila, de modo que las dos pantallas se ven en parte
y un salto sobre varias consolas muestra pasar las que quedan en medio. Una tecla pulsada mientras un deslizamiento está en marcha inicia el siguiente desde donde está
la vista en ese momento. Una consola nueva (`new`) entra junto a la que se muestra, a su derecha, en dos pasos: primero la anterior se desliza hacia la izquierda, dejando espacio vacío (el vacío, de un azul oscuro por defecto), y luego la nueva cae desde arriba. Cerrar también son dos pasos: la consola cerrada cae por abajo, y luego se desliza la que está a su izquierda (o la siguiente, si era la primera). Entre los dos pasos tardan una vez y media lo que tarda un paso. La barra se queda quieta, y el
cursor se dibuja cuando una consola ha aterrizado. Todo se mide con el reloj, así que un terminal lento se salta fotogramas y el deslizamiento dura lo mismo.

## Copiar y pegar

- En una consola normal (un listado, un shell) selecciona con el ratón como siempre: lo hace tu programa de terminal, y vtmux no se interpone.
- Un programa que usa el ratón por sí mismo (micro, btop, vim con `mouse=a`) recibe los eventos del ratón, así que arrastrar selecciona dentro del programa.
  Para seleccionar con la selección de tu programa de terminal, mantén pulsado `Shift` mientras arrastras (Windows Terminal, la mayoría de los terminales de Linux).
- Un programa que copia por sí mismo (micro con `"clipboard": "terminal"` en su configuración, vim y los complementos de estilo tmux, `printf` desde un script)
  envía el texto al terminal en una secuencia de escape (OSC 52), y tu programa de terminal lo pone en el portapapeles. vtmux las reenvía desde
  la consola que se muestra, así que Ctrl-C en micro funciona igual que sin vtmux. Una consola en segundo plano no puede cambiar tu
  portapapeles. Solo se reenvían las escrituras: un programa no puede leer tu portapapeles a través de vtmux. `:set clipboard off` lo detiene todo (activado por defecto).
  Tu programa de terminal puede tener también su propio interruptor para esto.
- Pegar pasa como un pegado (con paréntesis, si el programa lo pidió).

## Comandos (después de los dos puntos)

| comando | hace |
|---|---|
| `new [name] [-- command ...]` | una consola nueva que ejecuta tu shell (o el comando), y va a ella. `new "my name"` conserva los espacios |
| `close`, `kill` | cierra esta consola (su programa recibe un hangup); al cerrar la última se sale de vtmux |
| `next`, `prev`, `goto N` | cambia de consola, con la transición |
| `rename NAME` | pone nombre a esta consola en la barra |
| `list` | muestra las consolas en la barra durante un momento |
| `set transition slide\|dip\|dissolve\|crossfade` | cómo se ve el cambio: `slide` (el predeterminado) desliza las pantallas una junto a otra, pasando las del medio; `dip` funde la pantalla anterior a oscuro y sube la nueva; `dissolve` cambia celda a celda; `crossfade` mezcla los colores como hace vtshow. (`set fade` sigue funcionando) |
| `set transition-ms N` | cuánto dura un paso, de 0 a 5000 (por defecto 1000; 0 cambia al instante). Un deslizamiento sobre varias consolas tarda algo más, hasta el doble |
| `set void-color RRGGBB\|off` | el color del espacio vacío mientras una consola se abre o se cierra (por defecto `101220`; `off` deja el fondo del propio terminal) |
| `set clipboard on\|off` | si los programas pueden cambiar tu portapapeles (OSC 52, véase Copiar y pegar; activado por defecto) |
| `set fps on\|off` | muestra en la barra la tasa de fotogramas medida (desactivado por defecto). `--fps N` fija el máximo de fotogramas por segundo (por defecto 60) |
| `quit` | cierra todo y sale |
| `help` | una tarjeta en el centro de la pantalla (unos dos tercios de ella) con todas las teclas y comandos. La consola sigue funcionando alrededor. `Up`, `Down`, `PageUp`, `PageDown`, `Home`, `End` la desplazan cuando la pantalla es demasiado pequeña para mostrarlo todo; `Enter`, `Space`, `Esc` o `q` la cierran |

Una consola cuyo programa termina por sí solo (escribes `exit`) se cierra. Cuando se cierra la última, vtmux termina y tu terminal queda como estaba.

## Cómo funciona

Véase [DESIGN.md](DESIGN.md). En resumen: cada consola es un programa en un pseudoterminal (`Session`) con un hilo lector que alimenta un
`Emulator`; el hilo de la interfaz pinta solo las filas que cambiaron en una `Picture` retenida de vtscreen (`PanePainter`), mezcla dos imágenes
para un fundido (`blend`), dibuja la barra y muestra el resultado. Los bytes de entrada del modo normal van directamente al programa. Como el terminal real
debe enviar entonces las teclas como el programa las espera, vtmux lo pone en los modos de entrada de la consola mostrada (teclas de cursor de aplicación, pegado con
paréntesis, ratón, foco) y lo saca de ellos al salir.

## Lo que aún no está hecho

- Negrita, cursiva, subrayado, tachado y parpadeo se envían a tu terminal como los códigos habituales, y los caracteres anchos (chino, emoji) ocupan dos
  columnas. Cómo se ven, o si se ven siquiera (el parpadeo y la cursiva a menudo no), depende del programa de terminal. Los signos combinantes se
  descartan, y una letra que el terminal dibuja con un ancho distinto al de la tabla del emulador puede verse rara.
- Sin separar y volver a conectar (no hay servidor), sin paneles divididos, sin modo de copia (el historial se puede mirar, no seleccionar), sin comandos de ratón, sin archivo de configuración.
- No admitido por el emulador: sixel, líneas de doble ancho y de doble alto (`ESC # 3` a `6`; se ignoran), signos combinantes (se
  descartan). El cambio de 80/132 columnas (DECCOLM) funciona dentro del panel: véase más abajo.

## Pruebas

- `tests/run_tests.sh` compila cada prueba unitaria (`tests/*_tests.cpp`, cada una nombra sus fuentes en una línea `// sources:`) con las opciones estrictas de
  advertencias y los sanitizadores de direcciones y de comportamiento indefinido, y las ejecuta: el analizador, el emulador, el analizador de teclas, el pintor, los
  fundidos, las sesiones (programas reales en pseudoterminales reales) y la aplicación (los modos, comandos y fundidos, dirigidos por bytes y un reloj inventado). `ctest --test-dir build` ejecuta las mismas pruebas desde la compilación con CMake.
- `tests/e2e/run.sh` ejecuta el `vtmux` real en un pseudoterminal con programas reales dentro (`less`, `vi`, `top`, `htop`, Ctrl-C y
  Ctrl-Z, cambio de tamaño, el reflejo de modos, cambios rápidos, salida masiva, CPU en reposo) y lee la pantalla de vuelta con un emulador de terminal.
- `tests/conformance/run.sh` alimenta miles de flujos de secuencias de escape generados, pantallas escritas a mano y capturas de programas reales al
  emulador (`tools/vtdump.cpp`), a tmux y a pyte, y compara las pantallas. `tests/vt100_tests.cpp`, `vt102_tests.cpp` y `vt52_tests.cpp` fijan
  el comportamiento de los terminales DEC. Lo que encontró está en `tests/conformance/FINDINGS.md`.
- Para probar el programa real `vttest` (`sudo apt install vttest`) ejecútalo dentro de vtmux. vttest cambia la pantalla a 80 (o 132) columnas con
  DECCOLM después de permitirlo con el modo 40, como exige xterm, y vtmux lo hace dentro del panel: la pantalla pasa a tener 80 columnas de ancho a la izquierda
  (132 si el panel es tan ancho; un panel más estrecho conserva todo su ancho), el resto del panel queda en blanco, al programa se le informa del nuevo tamaño
  (SIGWINCH), y la pantalla se borra. Un reinicio completo (`reset`, `ESC c`), desactivar el modo 40, o cambiar el tamaño de la ventana devuelve todo el panel.
  Sin el modo 40 el cambio solo borra la pantalla, así que el comando `reset` no estrecha nada. Las líneas de doble ancho de vttest no se
  dibujan dobles (véase arriba).
- `tools/demo.py` hace un conjunto de capturas de pantalla de vtmux desde un pseudoterminal; `tools/preview_transition.py` genera los tres fundidos.

## Licencia

Licencia Pública General de GNU, versión 3 (véase `LICENSE`). La biblioteca `vtscreen` sobre la que se construye tiene licencia MIT.
