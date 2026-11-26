Language: [English](https://github.com/briefnotion/vtmux/blob/main/README.md), [Español](https://github.com/briefnotion/vtmux/blob/main/README.es.md), [Français](https://github.com/briefnotion/vtmux/blob/main/README.fr.md), [Deutsch](https://github.com/briefnotion/vtmux/blob/main/README.de.md), [日本語](https://github.com/briefnotion/vtmux/blob/main/README.ja.md), [中文](https://github.com/briefnotion/vtmux/blob/main/README.zh.md), [Corporate](https://github.com/briefnotion/vtmux/blob/main/README.corporate.md), [Cajun](https://github.com/briefnotion/vtmux/blob/main/README.cajun.md), [CyberPunk](https://github.com/briefnotion/vtmux/blob/main/README.cyberpunk.md)

# vtmux

Un multiplexeur de terminal avec des transitions par glissement et par fondu. Lancez `vtmux` dans une console et vous obtenez ce qui ressemble à la même console, avec une ligne
supplémentaire en bas : la barre de contrôle. Tout fonctionne comme avant. Appuyez sur `Ctrl-B` et la barre change de couleur : vous êtes en mode commande, où vous pouvez
créer et fermer des consoles en tapant une commande après deux-points, ou passer d'une console à l'autre avec les flèches gauche et droite, et les
écrans glissent (ou se fondent) les uns dans les autres.

![vtmux](docs/demo_sheet.png)

*vtmux faisant tourner de vrais programmes, avec sa barre tout en bas. En haut : la carte radar de [vtweather](https://github.com/briefnotion/vtweather) dans la
deuxième console, avec `Ctrl-B` enfoncé (la barre est ambre : mode commande). En bas à gauche : la scène d'orage de vtshow, dans la quatrième console.
En bas à droite : un programme de discussion affichant du texte chinois (生日快乐, « joyeux anniversaire »), dessiné sur deux colonnes de large.*

Il est construit sur [vtscreen](https://github.com/briefnotion/vtscreen), la même bibliothèque de dessin que vtshow et vtweather, avec ses
images conservées : une console inactive ne coûte presque rien à dessiner. L'émulateur de terminal intégré (`src/emulator.*`) est nouveau : un analyseur à
machine à états et un écran en mémoire, testé face à `tmux` et `pyte`.

## Compiler et lancer

Linux, `g++` avec C++17, `cmake` 3.16 ou plus récent, `git` :

    git clone --recursive --shallow-submodules --depth 1 https://github.com/briefnotion/vtmux.git
    cd vtmux/build
    ./m
    ./vtmux

`./m` (dans le répertoire `build`) compile vtmux, ou le recompile après une modification. Il récupère aussi le sous-module `vtscreen` si le clone a été fait
sans lui, et vous prévient s'il manque `cmake` ou un compilateur. `./m test` compile aussi les tests et les exécute, et `./m clean` supprime ce qui a été compilé.

`./install` (également dans `build`) exécute `./m` puis copie `vtmux` dans `~/.local/bin`, pour que vous puissiez taper `vtmux` à n'importe quelle invite (ou dans `/usr/local/bin`
si vous le lancez en root). `./install --system` le place dans `/usr/local/bin` pour tout le monde (il demande sudo), `./install DIR` le place dans `DIR`, et
`./install --remove` le retire. Si le dossier n'est pas dans votre `PATH`, il le dit et montre la ligne à ajouter.

La même chose que `./m`, à la main : `cmake -B build && cmake --build build`. `-DVTMUX_TESTS=OFF` évite de compiler les tests.

    vtmux [--transition slide|dip|dissolve|crossfade] [--transition-ms N] [--no-color] [--fps N] [-- command args ...]

Sans commande, la première console lance votre shell (`$SHELL`). Les programmes à l'intérieur tournent avec `TERM=xterm-256color`.

## Touches

En mode normal, chaque touche va au programme de la console, sans être modifiée (Ctrl-C et Ctrl-Z aussi). La seule touche surveillée est `Ctrl-B`.

| en mode commande (après `Ctrl-B`) | fait |
|---|---|
| `Left`, `Right` | la console précédente, la suivante, avec la transition (un glissement le long de la rangée de consoles ; depuis la dernière, `Right` glisse tout le chemin du retour jusqu'à la première) ; on reste en mode commande pour pouvoir continuer à feuilleter |
| `1` à `9` | cette console, et retour au mode normal |
| `c`, `n`, `p` | une nouvelle console, la suivante, la précédente (comme dans tmux) |
| `Up`, `Down`, `PageUp`, `PageDown`, `Home` | remonter dans l'historique de la console (2000 lignes) ; la barre montre jusqu'où l'on est remonté. `End` revient à l'écran en direct |
| `:` | taper une commande (Enter l'exécute, Esc renonce, Backspace corrige, Ctrl-U efface) |
| `Esc`, `Enter` | retour au mode normal (et à l'écran en direct). Taper dans la console fait la même chose |
| `Ctrl-B` | envoie un vrai Ctrl-B au programme, et retour au mode normal |

## Glissement

Les consoles sont côte à côte dans une rangée, dans l'ordre de la barre. Changer fait glisser la vue le long de la rangée, de sorte que les deux écrans sont visibles en partie
et qu'un saut par-dessus plusieurs consoles montre défiler celles du milieu. Une touche pressée pendant qu'un glissement est en cours lance le suivant à partir de l'endroit où
se trouve la vue à ce moment. Une nouvelle console (`new`) s'insère à côté de celle qui est affichée, à sa droite, en deux temps : d'abord l'ancienne glisse vers la gauche en laissant un espace vide (le vide, d'un bleu sombre par défaut), puis la nouvelle tombe du haut. La fermeture se fait aussi en deux temps : la console fermée tombe par le bas, puis celle qui est à sa gauche (ou la suivante, si c'était la première) glisse à sa place. L'ensemble dure une fois et demie ce que dure un temps. La barre reste immobile, et le
curseur n'est dessiné qu'une fois la console posée. Tout est cadencé par l'horloge : un terminal lent saute des images et le glissement dure le même temps.

## Copier-coller

- Dans une console ordinaire (une liste, un shell), sélectionnez à la souris comme d'habitude : c'est votre programme de terminal qui le fait, et vtmux ne gêne pas.
- Un programme qui utilise lui-même la souris (micro, btop, vim avec `mouse=a`) reçoit les événements de la souris à la place, donc le glissé sélectionne à l'intérieur du programme.
  Pour sélectionner avec la sélection de votre programme de terminal, maintenez `Shift` en faisant glisser (Windows Terminal, la plupart des terminaux Linux).
- Un programme qui copie par lui-même (micro avec `"clipboard": "terminal"` dans ses réglages, vim et les extensions de type tmux, `printf` depuis un script)
  envoie le texte au terminal dans une séquence d'échappement (OSC 52), et votre programme de terminal le place dans le presse-papiers. vtmux les transmet depuis
  la console affichée, donc Ctrl-C dans micro fonctionne comme sans vtmux. Une console en arrière-plan ne peut pas modifier votre
  presse-papiers. Seules les écritures sont transmises : un programme ne peut pas lire votre presse-papiers à travers vtmux. `:set clipboard off` arrête tout (activé par défaut).
  Votre programme de terminal peut aussi avoir son propre réglage pour cela.
- Le collage passe comme un collage (avec délimiteurs, si le programme l'a demandé).

## Commandes (après les deux-points)

| commande | fait |
|---|---|
| `new [name] [-- command ...]` | une nouvelle console lançant votre shell (ou la commande), et on y va. `new "my name"` garde les espaces |
| `close`, `kill` | ferme cette console (son programme reçoit un hangup) ; fermer la dernière quitte vtmux |
| `next`, `prev`, `goto N` | changer de console, avec la transition |
| `rename NAME` | donne un nom à cette console dans la barre |
| `list` | affiche les consoles dans la barre un moment |
| `set transition slide\|dip\|dissolve\|crossfade` | l'aspect du changement : `slide` (par défaut) fait glisser les écrans côte à côte en faisant défiler ceux du milieu ; `dip` fond l'ancien écran vers le noir puis fait remonter le nouveau ; `dissolve` change cellule par cellule ; `crossfade` mélange les couleurs comme le fait vtshow. (`set fade` fonctionne toujours) |
| `set transition-ms N` | la durée d'un temps, de 0 à 5000 (1000 par défaut ; 0 change instantanément). Un glissement par-dessus plusieurs consoles dure un peu plus, jusqu'au double |
| `set void-color RRGGBB\|off` | la couleur de l'espace vide pendant l'ouverture ou la fermeture d'une console (`101220` par défaut ; `off` laisse le fond propre au terminal) |
| `set clipboard on\|off` | si les programmes peuvent modifier votre presse-papiers (OSC 52, voir Copier-coller ; activé par défaut) |
| `set fps on\|off` | affiche dans la barre la fréquence d'images mesurée (désactivé par défaut). `--fps N` fixe le maximum d'images par seconde (60 par défaut) |
| `quit` | ferme tout et quitte |
| `help` | une carte au milieu de l'écran (environ les deux tiers) avec toutes les touches et commandes. La console continue de tourner autour. `Up`, `Down`, `PageUp`, `PageDown`, `Home`, `End` la font défiler quand l'écran est trop petit pour tout montrer ; `Enter`, `Space`, `Esc` ou `q` la ferment |

Une console dont le programme se termine de lui-même (vous tapez `exit`) se ferme. Quand la dernière se ferme, vtmux se termine et votre terminal est remis comme il était.

## Comment ça marche

Voir [DESIGN.md](DESIGN.md). En bref : chaque console est un programme sur un pseudo-terminal (`Session`) avec un thread de lecture qui alimente un
`Emulator` ; le thread d'interface ne peint dans une `Picture` conservée de vtscreen (`PanePainter`) que les lignes qui ont changé, mélange deux images
pour un fondu (`blend`), dessine la barre et affiche le résultat. Les octets d'entrée du mode normal vont directement au programme. Comme le vrai terminal
doit alors envoyer les touches comme le programme les attend, vtmux le met dans les modes d'entrée de la console affichée (touches de curseur d'application, collage
avec délimiteurs, souris, focus) et l'en sort en quittant.

## Ce qui n'est pas encore fait

- Gras, italique, souligné, barré et clignotant sont envoyés à votre terminal comme les codes habituels, et les caractères larges (chinois, emoji) occupent deux
  colonnes. Leur aspect, ou même leur affichage (le clignotement et l'italique souvent non), dépend du programme de terminal. Les signes combinants sont
  ignorés, et une lettre que le terminal dessine avec une largeur différente de celle de la table de l'émulateur peut paraître étrange.
- Pas de détachement ni de rattachement (il n'y a pas de serveur), pas de panneaux divisés, pas de mode copie (l'historique peut être consulté, pas sélectionné), pas de commandes à la souris, pas de fichier de configuration.
- Non pris en charge par l'émulateur : sixel, lignes en double largeur et en double hauteur (`ESC # 3` à `6` ; elles sont ignorées), signes combinants (ils sont
  ignorés). Le basculement 80/132 colonnes (DECCOLM) fonctionne à l'intérieur du panneau : voir plus bas.

## Tests

- `tests/run_tests.sh` compile chaque test unitaire (`tests/*_tests.cpp`, chacun nomme ses sources sur une ligne `// sources:`) avec les options strictes
  d'avertissement et les assainisseurs d'adresses et de comportement indéfini, et les exécute : l'analyseur, l'émulateur, l'analyseur de touches, le peintre, les
  fondus, les sessions (de vrais programmes sur de vrais pseudo-terminaux) et l'application (les modes, les commandes et les fondus, pilotés par des octets et une horloge fictive). `ctest --test-dir build` exécute les mêmes tests depuis la compilation CMake.
- `tests/e2e/run.sh` lance le vrai `vtmux` dans un pseudo-terminal avec de vrais programmes à l'intérieur (`less`, `vi`, `top`, `htop`, Ctrl-C et
  Ctrl-Z, redimensionnement, la recopie des modes, changements rapides, sortie massive, CPU au repos) et relit l'écran avec un émulateur de terminal.
- `tests/conformance/run.sh` envoie des milliers de flux de séquences d'échappement générés, d'écrans écrits à la main et de captures de vrais programmes à
  l'émulateur (`tools/vtdump.cpp`), à tmux et à pyte, et compare les écrans. `tests/vt100_tests.cpp`, `vt102_tests.cpp` et `vt52_tests.cpp` figent
  le comportement des terminaux DEC. Ce qu'il a trouvé est dans `tests/conformance/FINDINGS.md`.
- Pour essayer le vrai programme `vttest` (`sudo apt install vttest`), lancez-le dans vtmux. vttest bascule l'écran sur 80 (ou 132) colonnes avec
  DECCOLM après l'avoir autorisé avec le mode 40, comme xterm l'exige, et vtmux le fait à l'intérieur du panneau : l'écran devient large de 80 colonnes à gauche
  (132 si le panneau est aussi large ; un panneau plus étroit garde toute sa largeur), le reste du panneau reste vide, le programme est informé de la nouvelle taille
  (SIGWINCH), et l'écran s'efface. Une réinitialisation complète (`reset`, `ESC c`), la désactivation du mode 40, ou le redimensionnement de la fenêtre rend tout le panneau.
  Sans le mode 40, le basculement ne fait qu'effacer l'écran, donc la commande `reset` ne rétrécit rien. Les lignes en double largeur de vttest ne sont pas
  dessinées en double (voir plus haut).
- `tools/demo.py` produit un ensemble de captures d'écran de vtmux depuis un pseudo-terminal ; `tools/preview_transition.py` produit les trois fondus.

## Licence

Licence publique générale GNU, version 3 (voir `LICENSE`). La bibliothèque `vtscreen` sur laquelle il s'appuie est sous licence MIT.
