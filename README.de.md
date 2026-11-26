Language: [English](https://github.com/briefnotion/vtmux/blob/main/README.md), [Español](https://github.com/briefnotion/vtmux/blob/main/README.es.md), [Français](https://github.com/briefnotion/vtmux/blob/main/README.fr.md), [Deutsch](https://github.com/briefnotion/vtmux/blob/main/README.de.md), [日本語](https://github.com/briefnotion/vtmux/blob/main/README.ja.md), [中文](https://github.com/briefnotion/vtmux/blob/main/README.zh.md), [Corporate](https://github.com/briefnotion/vtmux/blob/main/README.corporate.md), [Cajun](https://github.com/briefnotion/vtmux/blob/main/README.cajun.md), [CyberPunk](https://github.com/briefnotion/vtmux/blob/main/README.cyberpunk.md)

# vtmux

Ein Terminal-Multiplexer mit gleitenden und überblendenden Übergängen. Starte `vtmux` in einer Konsole und du siehst scheinbar dieselbe Konsole, mit einer zusätzlichen Zeile
am unteren Rand: der Steuerleiste. Alles funktioniert wie zuvor. Drücke `Ctrl-B`, und die Leiste ändert ihre Farbe: Du bist im Befehlsmodus, in dem du
Konsolen erstellen und schließen kannst, indem du nach einem Doppelpunkt einen Befehl eingibst, oder mit den Pfeiltasten links und rechts zwischen den Konsolen wechselst, und die
Bildschirme gleiten (oder blenden) ineinander über.

![vtmux](docs/demo_sheet.png)

*vtmux mit echten Programmen, die Leiste am unteren Rand. Oben: die Radarkarte von [vtweather](https://github.com/briefnotion/vtweather) in der
zweiten Konsole, bei gedrücktem `Ctrl-B` (die Leiste ist bernsteinfarben, Befehlsmodus). Unten links: die Gewitterszene von vtshow in der vierten Konsole.
Unten rechts: ein Chat-Programm mit chinesischem Text (生日快乐, „alles Gute zum Geburtstag“), zwei Spalten breit gezeichnet.*

Es baut auf [vtscreen](https://github.com/briefnotion/vtscreen) auf, derselben Zeichenbibliothek wie vtshow und vtweather, mit ihren
beibehaltenen Bildern: Eine ruhende Konsole kostet beim Zeichnen so gut wie nichts. Der darin enthaltene Terminal-Emulator (`src/emulator.*`) ist neu: ein Zustandsautomaten-Parser
und ein Bildschirm im Speicher, der gegen `tmux` und `pyte` getestet wird.

## Bauen und starten

Linux, `g++` mit C++17, `cmake` ab 3.16, `git`:

    git clone --recursive --shallow-submodules --depth 1 https://github.com/briefnotion/vtmux.git
    cd vtmux/build
    ./m
    ./vtmux

`./m` (im Verzeichnis `build`) baut vtmux oder baut es nach einer Änderung neu. Es holt auch das Submodul `vtscreen`, falls der Klon ohne es angelegt wurde,
und meldet, wenn `cmake` oder ein Compiler fehlt. `./m test` baut auch die Tests und führt sie aus, und `./m clean` entfernt das Gebaute.

`./install` (ebenfalls in `build`) führt `./m` aus und kopiert dann `vtmux` nach `~/.local/bin`, sodass du `vtmux` an jeder Eingabeaufforderung tippen kannst (oder nach `/usr/local/bin`,
wenn du es als root ausführst). `./install --system` legt es für alle nach `/usr/local/bin` (es fragt nach sudo), `./install DIR` legt es nach `DIR`, und
`./install --remove` entfernt es wieder. Liegt der Ordner nicht in deinem `PATH`, sagt es das und zeigt die Zeile, die du hinzufügen musst.

Dasselbe wie `./m` von Hand: `cmake -B build && cmake --build build`. `-DVTMUX_TESTS=OFF` überspringt das Bauen der Tests.

    vtmux [--transition slide|dip|dissolve|crossfade] [--transition-ms N] [--no-color] [--fps N] [-- command args ...]

Ohne Befehl führt die erste Konsole deine Shell (`$SHELL`) aus. Programme darin laufen mit `TERM=xterm-256color`.

## Tasten

Im Normalmodus geht jede Taste unverändert an das Programm in der Konsole (auch Ctrl-C und Ctrl-Z). Die einzige beobachtete Taste ist `Ctrl-B`.

| im Befehlsmodus (nach `Ctrl-B`) | bewirkt |
|---|---|
| `Left`, `Right` | die vorherige, die nächste Konsole, mit dem Übergang (ein Gleiten entlang der Konsolenreihe; von der letzten gleitet `Right` den ganzen Weg zurück zur ersten); man bleibt im Befehlsmodus, um weiterblättern zu können |
| `1` bis `9` | diese Konsole, und zurück in den Normalmodus |
| `c`, `n`, `p` | eine neue Konsole, die nächste, die vorherige (wie in tmux) |
| `Up`, `Down`, `PageUp`, `PageDown`, `Home` | im Verlauf der Konsole zurückblättern (2000 Zeilen); die Leiste zeigt, wie weit zurück. `End` kehrt zum Live-Bildschirm zurück |
| `:` | einen Befehl eintippen (Enter führt ihn aus, Esc bricht ab, Backspace korrigiert, Ctrl-U löscht) |
| `Esc`, `Enter` | zurück in den Normalmodus (und zum Live-Bildschirm). Tippen in die Konsole bewirkt dasselbe |
| `Ctrl-B` | sendet ein echtes Ctrl-B an das Programm und zurück in den Normalmodus |

## Gleiten

Die Konsolen liegen nebeneinander in einer Reihe, in der Reihenfolge der Leiste. Beim Wechseln gleitet die Ansicht an der Reihe entlang, sodass beide Bildschirme teilweise zu sehen sind
und ein Sprung über mehrere Konsolen die dazwischenliegenden vorbeiziehen lässt. Eine Taste, die während eines Gleitens gedrückt wird, startet das nächste von der Stelle aus, an der
die Ansicht gerade ist. Eine neue Konsole (`new`) kommt neben der angezeigten, rechts davon, in zwei Schritten hinein: zuerst gleitet die alte nach links hinaus und lässt leeren Raum zurück (die Leere, standardmäßig ein dunkles Blau), dann fällt die neue von oben herein. Auch das Schließen geht in zwei Schritten: Die geschlossene Konsole fällt unten hinaus, dann gleitet die links davon (oder die nächste, falls es die erste war) herein. Beide zusammen dauern anderthalbmal so lange wie ein Schritt. Die Leiste bleibt ruhig, und der
Cursor wird gezeichnet, sobald eine Konsole gelandet ist. Alles wird von der Uhr getaktet, sodass ein langsames Terminal Bilder überspringt und das Gleiten gleich lange dauert.

## Kopieren und Einfügen

- In einer gewöhnlichen Konsole (eine Auflistung, eine Shell) markierst du mit der Maus wie immer: Das erledigt dein Terminalprogramm, und vtmux steht nicht im Weg.
- Ein Programm, das die Maus selbst verwendet (micro, btop, vim mit `mouse=a`), bekommt stattdessen die Mausereignisse, sodass Ziehen im Programm markiert.
  Um mit der Markierung deines Terminalprogramms zu markieren, halte beim Ziehen `Shift` gedrückt (Windows Terminal, die meisten Linux-Terminals).
- Ein Programm, das selbst kopiert (micro mit `"clipboard": "terminal"` in den Einstellungen, vim und Plugins im tmux-Stil, `printf` aus einem Skript)
  sendet den Text in einer Escape-Sequenz (OSC 52) an das Terminal, und dein Terminalprogramm legt ihn in die Zwischenablage. vtmux leitet diese von
  der angezeigten Konsole weiter, sodass Ctrl-C in micro so funktioniert wie ohne vtmux. Eine Konsole im Hintergrund darf deine
  Zwischenablage nicht setzen. Nur Schreibzugriffe werden weitergeleitet: Ein Programm kann deine Zwischenablage nicht über vtmux lesen. `:set clipboard off` stoppt das alles (standardmäßig an).
  Dein Terminalprogramm hat dafür möglicherweise auch einen eigenen Schalter.
- Einfügen geht als Einfügen durch (geklammert, wenn das Programm es verlangt hat).

## Befehle (nach dem Doppelpunkt)

| Befehl | bewirkt |
|---|---|
| `new [name] [-- command ...]` | eine neue Konsole, die deine Shell (oder den Befehl) ausführt, und dorthin wechseln. `new "my name"` behält Leerzeichen |
| `close`, `kill` | diese Konsole schließen (ihr Programm erhält ein Hangup); beim Schließen der letzten wird vtmux beendet |
| `next`, `prev`, `goto N` | wechseln, mit dem Übergang |
| `rename NAME` | diese Konsole in der Leiste benennen |
| `list` | die Konsolen kurz in der Leiste anzeigen |
| `set transition slide\|dip\|dissolve\|crossfade` | wie der Wechsel aussieht: `slide` (Standard) lässt die Bildschirme nebeneinander entlanggleiten und die dazwischen vorbeiziehen; `dip` blendet den alten Bildschirm ins Dunkle ab und den neuen herauf; `dissolve` wechselt Zelle für Zelle; `crossfade` mischt die Farben wie vtshow. (`set fade` funktioniert weiterhin) |
| `set transition-ms N` | wie lange ein Schritt dauert, 0 bis 5000 (Standard 1000; 0 wechselt sofort). Ein Gleiten über mehrere Konsolen dauert etwas länger, bis zum Doppelten |
| `set void-color RRGGBB\|off` | die Farbe des leeren Raums, während eine Konsole geöffnet oder geschlossen wird (Standard `101220`; `off` lässt den eigenen Hintergrund des Terminals) |
| `set clipboard on\|off` | ob Programme deine Zwischenablage setzen dürfen (OSC 52, siehe Kopieren und Einfügen; Standard an) |
| `set fps on\|off` | die gemessene Bildrate in der Leiste anzeigen (Standard aus). `--fps N` legt die höchste Bildrate fest (Standard 60) |
| `quit` | alles schließen und beenden |
| `help` | eine Karte in der Bildschirmmitte (etwa zwei Drittel davon) mit allen Tasten und Befehlen. Die Konsole läuft drumherum weiter. `Up`, `Down`, `PageUp`, `PageDown`, `Home`, `End` scrollen sie, wenn der Bildschirm zu klein ist, um alles zu zeigen; `Enter`, `Space`, `Esc` oder `q` schließt sie |

Eine Konsole, deren Programm von selbst endet (du tippst `exit`), wird geschlossen. Wenn die letzte schließt, endet vtmux und dein Terminal wird so zurückgesetzt, wie es war.

## Wie es funktioniert

Siehe [DESIGN.md](DESIGN.md). Kurz gesagt: Jede Konsole ist ein Programm auf einem Pseudo-Terminal (`Session`) mit einem Lese-Thread, der einen
`Emulator` füttert; der UI-Thread malt nur die geänderten Zeilen in ein beibehaltenes vtscreen-`Picture` (`PanePainter`), mischt zwei Bilder
für eine Überblendung (`blend`), zeichnet die Leiste und zeigt das Ergebnis. Eingabebytes des Normalmodus gehen direkt an das Programm. Weil das echte Terminal
dann die Tasten so senden muss, wie das Programm sie erwartet, versetzt vtmux es in die Eingabemodi der angezeigten Konsole (Anwendungs-Cursortasten, geklammertes
Einfügen, Maus, Fokus) und nimmt es beim Beenden wieder heraus.

## Was noch nicht fertig ist

- Fett, kursiv, unterstrichen, durchgestrichen und blinkend werden als die üblichen Codes an dein Terminal gesendet, und breite Zeichen (Chinesisch, Emoji) belegen zwei
  Spalten. Wie sie aussehen, oder ob sie überhaupt erscheinen (Blinken und Kursiv oft nicht), liegt am Terminalprogramm. Kombinierende Zeichen werden
  verworfen, und ein Buchstabe, den das Terminal in anderer Breite zeichnet als die Tabelle des Emulators, kann seltsam aussehen.
- Kein Lösen und Wiederverbinden (es gibt keinen Server), keine geteilten Fenster, kein Kopiermodus (der Verlauf kann angesehen, aber nicht ausgewählt werden), keine Mausbefehle, keine Konfigurationsdatei.
- Vom Emulator nicht unterstützt: Sixel, Zeilen doppelter Breite und doppelter Höhe (`ESC # 3` bis `6`; sie werden ignoriert), kombinierende Zeichen (sie werden
  verworfen). Die Umschaltung 80/132 Spalten (DECCOLM) funktioniert innerhalb des Fensterbereichs: siehe unten.

## Tests

- `tests/run_tests.sh` baut jeden Unit-Test (`tests/*_tests.cpp`, jeder nennt seine Quellen in einer `// sources:`-Zeile) mit den strengen Warnoptionen
  und den Adress- und Undefined-Behaviour-Sanitizern und führt sie aus: den Parser, den Emulator, den Tastenparser, den Maler, die
  Überblendungen, die Sitzungen (echte Programme auf echten Pseudo-Terminals) und die App (die Modi, Befehle und Überblendungen, gesteuert durch Bytes und eine erfundene Uhr). `ctest --test-dir build` führt dieselben Tests aus dem CMake-Build aus.
- `tests/e2e/run.sh` führt das echte `vtmux` in einem Pseudo-Terminal mit echten Programmen darin aus (`less`, `vi`, `top`, `htop`, Ctrl-C und
  Ctrl-Z, Größenänderung, die Modusspiegelung, schnelles Wechseln, viel Ausgabe, CPU im Leerlauf) und liest den Bildschirm mit einem Terminal-Emulator zurück.
- `tests/conformance/run.sh` füttert Tausende erzeugte Escape-Sequenz-Ströme, handgeschriebene Bildschirme und Aufzeichnungen echter Programme an
  den Emulator (`tools/vtdump.cpp`), tmux und pyte und vergleicht die Bildschirme. `tests/vt100_tests.cpp`, `vt102_tests.cpp` und `vt52_tests.cpp` legen
  das Verhalten der DEC-Terminals fest. Was dabei gefunden wurde, steht in `tests/conformance/FINDINGS.md`.
- Um das echte Programm `vttest` auszuprobieren (`sudo apt install vttest`), führe es in vtmux aus. vttest schaltet den Bildschirm mit
  DECCOLM auf 80 (oder 132) Spalten, nachdem es das mit Modus 40 erlaubt hat, wie xterm es verlangt, und vtmux macht das innerhalb des Fensterbereichs: Der Bildschirm wird links 80 Spalten breit
  (132, wenn der Bereich so breit ist; ein schmalerer Bereich behält seine ganze Breite), der Rest des Bereichs bleibt leer, dem Programm wird die neue Größe mitgeteilt
  (SIGWINCH), und der Bildschirm wird gelöscht. Ein vollständiger Reset (`reset`, `ESC c`), das Abschalten von Modus 40 oder eine Größenänderung des Fensters bringt den ganzen Bereich zurück.
  Ohne Modus 40 löscht die Umschaltung nur den Bildschirm, sodass der Befehl `reset` nichts verengt. Die Zeilen doppelter Breite von vttest werden nicht
  doppelt gezeichnet (siehe oben).
- `tools/demo.py` erzeugt eine Reihe von Screenshots von vtmux aus einem Pseudo-Terminal; `tools/preview_transition.py` erzeugt die drei Überblendungen.

## Lizenz

GNU General Public License, Version 3 (siehe `LICENSE`). Die Bibliothek `vtscreen`, auf der es aufbaut, steht unter der MIT-Lizenz.
