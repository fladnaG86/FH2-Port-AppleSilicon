# FH2 Port for Apple Silicon

[![A race on a MacBook Pro M1 Max with the Metal HUD (click for the video)](media/race.jpg)](https://github.com/fladnaG86/FH2-Port-AppleSilicon/releases/tag/video)

*MacBook Pro M1 Max, Azimuth set to 60 fps and 1440 lines: about 60 fps, 11–13 ms of GPU per frame
in races (Metal HUD, top right). [Watch the video](https://github.com/fladnaG86/FH2-Port-AppleSilicon/releases/tag/video).*

**[English](#english) · [Italiano](#italiano)**

> Not affiliated with, endorsed by or connected to Microsoft, Xbox Game Studios, Playground Games,
> Turn 10 Studios, Almost There Games, CodeWeavers or Apple. This repository contains **no game
> files, no Azimuth files and no keys**, and nothing in it modifies Azimuth. You need your own
> legitimate copy of Forza Horizon 2 for Xbox One and Azimuth from its
> [official page](https://almost-there-games.itch.io/azimuth). Almost There Games does not support
> setups with third-party layers: **do not send them reports about this one.**

---

## English

This project lets **Azimuth 1.0.2**, Almost There Games' compatibility wrapper for the Xbox One
version of **Forza Horizon 2 (1.0.0.64)**, run on Apple Silicon Macs through **CrossOver** and
Apple's **Game Porting Toolkit (D3DMetal)**. Azimuth itself targets Windows and Linux; this repo
only adds layers *underneath* it, the way DXVK or vkd3d sit under a game:

- **`d3d12.dll`**: a D3D12 layer between Azimuth and D3DMetal (loaded through a Wine DLL override).
- **`fh2pad.dll`**: the bottle's provider of `Windows.Gaming.Input.Gamepad`, adding a keyboard.
- **FH2 Port.app**: a launcher that installs both into a CrossOver bottle and starts the game.

Developed and tested on a MacBook Pro M1 Max (32 GB), macOS 27.2, CrossOver on Wine 11 with
GPTK 4.0b3 and Azimuth 1.0.2 (0.9.10 starts with this layer too, but 0.1.7 was played on 1.0.2 only).
Free roam, races, menus, audio, controllers and keyboard work.

### What's new

- **0.1.8**: with anti-aliasing on, the **car customisation, purchase and upgrade screens no longer
  stop the game** (issue #15). The showroom asks for 8x MSAA targets; Apple GPUs take at most 4x,
  so D3DMetal offers no 8x and Azimuth stopped with "the host takes no 8 samples of format 10". The
  layer now creates those targets and their pipelines with 4 samples (`FH2_MSAA8=0` turns this
  off). MSAA still draws dashes in the sky, so anti-aliasing off stays the recommended setting.
  - The fiber fix finds its sites at run time and also knows **Forza Horizon 2 Presents Fast &
    Furious, Forza Horizon 3, Forza Motorsport 5 and 6** (PR #3 by @danielgpinheiro; only Forza
    Horizon 2 tested here).
- **0.1.7**: **Azimuth 1.0.x** works (it froze after a few minutes, issue #14), and the **stutter
  near buildings is gone**.
  - Azimuth 1.0 drives two GPU queues at once. The layer held every queue's signals with one
    fence and one worker thread, so each queue could end up waiting for the other; each queue
    now has its own.
  - The stutter: near busy places Azimuth reserves memory with `VirtualAlloc2`, asking for an
    address between 4 GB and 1 TB. Wine looks for room from the bottom of that range, trying every
    64 KB with `mach_vm_map`, and right above 4 GB macOS has Wine, Rosetta, D3DMetal and Metal
    mapped: thousands of failed tries, 40–170 ms per call, 2–7 s of every 10 s. The layer now picks
    a free address in the upper half of the range and asks for exactly that one: under 1 ms.
  - `FlushProcessWriteBuffers` no longer reads every thread's registers: one page protection change
    (macOS `mprotect`, called directly) drains the other cores' write buffers. ~17 µs instead of
    ~1.8 ms, and it cannot hang in `thread_get_state` (seen once with a single caller).
- **0.1.6**: about **half the stutter** in busy places. Wine on macOS runs threads
  of normal Windows priority below native apps (Mach priority 26 instead of 31), and Azimuth's
  worker and many game threads sit there; the layer now raises them to the native level.
- **0.1.5**: fixes the **random freeze** (no Present, Azimuth's watchdog reporting its worker stuck)
  seen after anything from 1 to 30 minutes, in the intro movie or in game. Wine on macOS implements
  `FlushProcessWriteBuffers` by reading every thread's registers; under Rosetta two threads doing
  that at once wait on each other forever. The layer now lets one call through at a time.
- **0.1.4**: the launcher also finds **CrossOver Preview**. The install steps no longer tell you to
  start `azimuth.exe` from CrossOver first: without the layer it stops with heap / DX12 errors
  before it asks for the game folder. Open FH2 Port instead; Azimuth then asks for the folder.
- **0.1.3**: bottles in a custom Bottles folder (for example on an external SSD) are found, and
  **Other bottle folder…** lets you pick any bottle by hand.

### What the layer does

| Problem on D3DMetal | What the layer does |
|---|---|
| `ID3D12Device3::OpenExistingHeapFromAddress` is not implemented, and Azimuth places GPU heaps over the game's memory | Emulated: a CPU-visible heap mirrors that memory, synced in 64-byte blocks before each submit and after the GPU; queue signals are held until the data is visible |
| `WriteBufferImmediate` does nothing | Replayed on the CPU before the fence, or turned into a copy from an upload ring |
| Queue signals held until the data is visible (row above) must stay in each queue's order; Azimuth 1.0 uses two queues | Each queue has its own fence, list and worker thread |
| Azimuth only starts on devices that report existing heaps and resource binding tier 3 | Both reported as supported (existing heaps are the emulation above) |
| Tiled resources do not alias | Reported as unsupported, so Azimuth takes another path |
| `FlushProcessWriteBuffers` (Wine on macOS reads every thread's registers) deadlocks under Rosetta when two threads call it at once, and costs ~1.8 ms a call | ntdll's `NtFlushProcessWriteBuffers` is hooked in memory: one caller at a time, and a page protection change (macOS `mprotect`) instead of the register reads |
| Wine looks for an address that fits Azimuth's `VirtualAlloc2` requirements (4 GB–1 TB) by trying every 64 KB from the bottom, through macOS's own mappings: up to 170 ms a call | ntdll's `NtAllocateVirtualMemoryEx` is hooked: the layer picks a free address in the upper half of the range and asks for it; Wine's search is the fallback |
| Threads of normal Windows priority get Mach priority 26, below native apps' 31 | Every 2 s, threads below above-normal are raised to above-normal (Mach 31) |
| **Input layouts using `D3D12_APPEND_ALIGNED_ELEMENT` over several vertex buffers get wrong offsets.** In the world-prop layout `I_YAXIS` is fetched 28 bytes and `I_COLOUR` 56 bytes past their real position | Offsets are written out per slot before the layout reaches D3DMetal. This fixes the giant "phantom walls", black or upside-down trees, bent poles and misaligned barriers |

The input-layout bug has a standalone repro, `layout_test` in `src/d3d12/d3d12.c`, run by
`tools/fh2test.c`: with append-aligned offsets 17 of 17 instances read the wrong attributes, with
explicit offsets 0 of 17.

The game itself also needs one fix on macOS Wine: it reads the current fiber inline with
`mov reg, gs:[0x20]`, which does not reach the TEB there. The layer reroutes those reads **in
memory**, after Azimuth has loaded the game; the game files on disk stay untouched.

It is not only Forza Horizon 2: the same read is in **Forza Horizon 2 Presents Fast & Furious**
(`Spire_1.0.0.6`), **Forza Horizon 3** (`Opus_1.0.125.0`), **Forza Motorsport 5**
(`Forza_1.0.0.20`, two hundred of them) and **Forza Motorsport 6** (`Monument_1.0.0.44`), and each
build puts them at different addresses with different registers. So the layer knows a title only by
its PE timestamp and image size, and finds the sites itself: it scans the game image's executable
sections for `65 REX.W 8b modrm(SIB) 25 20 00 00 00`, patches what it finds into a jump to a cave in
that section's `int3` padding (one cave per register), and patches the rest as more of the image
arrives. In the log: `game image found at … (Forza Horizon 3 1.0.125.0)` and
`fiber fix: 12 inline fiber reads rerouted … (…)`. An unidentified build is left alone, and a new
title costs two numbers in `known_images[]` — its timestamp and image size.

### Install with the app

1. In CrossOver (or CrossOver Preview) create a **Windows 10 64-bit** bottle and copy the Azimuth
   folder into its `drive_c` (the folder name is up to you). From the Terminal, with a bottle
   named `fh2` and Azimuth unzipped in Downloads:
   ```sh
   ditto ~/Downloads/azimuth-windows "$HOME/Library/Application Support/CrossOver/Bottles/fh2/drive_c/Azimuth"
   ```
   Do not start `azimuth.exe` from CrossOver: without the layer it stops at D3D12 checks
   (heap / DX12 errors) before it asks for the game folder.
2. Put Microsoft's **`d3dcompiler_47.dll`** in the Azimuth folder (Wine's one cannot compile
   Azimuth's GPU detile shader). It is not included here; one source is the `core/` folder of the
   Firefox 62 Windows 64-bit installer.
3. Open the DMG from the [Releases](../../releases), drag **FH2 Port** to Applications and open it.
   It is not notarized: the first time, click "Done", then System Settings › Privacy & Security ›
   **Open Anyway**.
4. Choose the bottle, then the folder that contains `azimuth.exe`. The app installs the layer and
   starts Azimuth, which asks for your game folder the first time (open the app once more after
   that so it applies its settings); later a double-click is enough. The game folder must be a real
   folder: Azimuth refuses symbolic links (copy it, or make an APFS clone with `cp -c -R`). Wine
   shows your home folder as a drive letter (often **Y:**), which is where to look for a game kept
   under your home. Hold **Option** while opening it to choose
   again or to uninstall. Bottles in a custom Bottles folder (for example on an external disk)
   are found too; otherwise pick **Other bottle folder…** and choose the bottle's folder.

The app also sets `antialiasing=off` (MSAA draws dashes in the sky on D3DMetal) and
`resolution=1440`, which supersamples and smooths the edges.

### Updating Azimuth without losing your saves

Saves live in each Azimuth folder (`data/titles/1A96545B/saves`). Close the game first, then
either:

- unzip the new Azimuth **over** the old folder: it keeps `data/titles`, `data/azimuth.ini` (which
  Azimuth converts itself), `d3dcompiler_47.dll` and the layer. `ditto` merges folders and deletes
  nothing; in the Finder choose **Merge** for `data`, never **Replace**, which would delete the
  saves:
  ```sh
  ditto ~/Downloads/azimuth-windows "$HOME/Library/Application Support/CrossOver/Bottles/<bottle>/drive_c/<folder>"
  ```
- or install it in a **new folder or a new bottle** (steps above) and copy `data/titles` from the
  old folder before the first start:
  ```sh
  B="$HOME/Library/Application Support/CrossOver/Bottles"
  ditto "$B/<old bottle>/drive_c/<old folder>/data/titles" "$B/<new bottle>/drive_c/<new folder>/data/titles"
  ```

Then open FH2 Port with **Option** held and choose the new folder if it changed; the app installs
the layer there. Updating FH2 Port itself never touches the saves. Copying `data/titles` somewhere
else first is a cheap backup.

### Install by hand

```sh
brew install mingw-w64
make                                             # build/d3d12.dll, build/fh2pad.dll
BOTTLE=<bottle> AZDIR=<folder> scripts/install.sh --settings
BOTTLE=<bottle> AZDIR=<folder> scripts/run.sh    # MTL_HUD_ENABLED=1 for the Metal HUD
```

`install.sh` copies the two DLLs next to `azimuth.exe`, adds per-application DLL overrides for
`azimuth.exe` (`d3d12` native-then-builtin, `d3dcompiler_47` and the VC++ 2012 runtime native),
registers `fh2pad.dll` for `Windows.Gaming.Input.Gamepad`, and sets WineBus `DisableHidraw=1` so a
DualSense shows up as a gamepad. `install.sh --uninstall` undoes it. `tools/app/make_app.sh <version>`
builds the app and the DMG.

### Keyboard

| Key | Pad |
|---|---|
| W / S | throttle (RT) / brake (LT) |
| A / D | steering (eased: full lock in ~150 ms, back to centre in ~80 ms) |
| I J K L | right stick (look around) |
| Enter / Space | A |
| Esc / Backspace | B |
| F / R | X / Y (rewind) |
| Tab / C | Menu / View |
| Q / E | LB / RB |
| H / V | left / right stick click |
| Arrows | D-pad |

Keys count only while the game window has focus; keyboard and controllers work together.
`FH2_KEYBOARD=0` hands out Wine's gamepads unchanged.

### Known issues

- **Hang / GPU reset crash**: the game can stop waiting for a value the GPU should have written
  (Azimuth's "the GPU passed the work that writes the value … and the value is not there", or its
  watchdog reporting its worker stuck). Before 0.1.1 this happened after 8–20 minutes: the layer
  lost the immediate writes of command lists once more than 4096 lists had been created. The
  remaining freezes were the `FlushProcessWriteBuffers` deadlock fixed in 0.1.5; please report it
  if it still happens, with `azimuth.log` and `fh2port.log`.
- **Stutter** in busy places (the buildings around Place Masséna in Nice, for example): fixed in
  0.1.7, it was Wine's address search (see What's new). Please report it if you still see it, with
  `fh2port.log`.
- If the picture stops on **"Press RT to accelerate"** in the intro while the music goes on, the
  game is waiting for RT: check that the controller is on, or click the game window and hold W.
- **MSAA** (anti-aliasing 2x/4x in Azimuth's settings) draws dashes in the sky on D3DMetal: keep
  it off and use a render height above your screen's instead ("supersampled"). Since 0.1.8 it no
  longer stops the game in the car screens. The intro movies play (no intro skip).

### Debugging

Logs are in `data/logs` of the Azimuth folder (`fh2port.log`, `fh2pad.log`, `azimuth.log`) and
`~/Library/Logs/FH2Port/launcher.log`. Switches: `FH2_COUNT=1` (per-PSO draw counters),
`FH2_TINT=1` (magenta twins of instanced PSOs for live bisection), `FH2_APPEND=0` (layouts passed
through unfixed), `FH2_FIBER=0`, `FH2_TILED=1`, `FH2_SKIP_EI=1`, `FH2_SKIP_VID=1`, `FH2_PATCH=…`,
`FH2_LABEL=<address>` (traces the values written to that address through record, execute, signal
and completion), `FH2_FLUSH=real|0|nolock|skip` (Wine's real flush, serialized as in 0.1.5–0.1.6 /
flush fix off / timed without the lock / replaced by a local fence, diagnostic only), `FH2_PLACE=0`
(leave address placement to Wine), `FH2_VMSTATS=12` (every 10 s: virtual memory calls, their cost,
shapes and callers), `FH2_QUIET=1` (no periodic log lines, control file not polled),
`FH2_PRIO=<n>` (Windows priority threads are raised to; default 1, `0` leaves them alone),
`FH2_MSAA8=0` (8x MSAA targets passed to D3DMetal as they are, which stops the car screens).
Calls into D3DMetal slower than 4 ms are logged as `slow:`. Live commands go through `data/logs/fh2port.ctl` (`top`, `reset`, `dump N`,
`tint …`, `tintlist`, `tintset …`, `watch …`, `patch on|off`, `stacks` for the Windows-side call
stack of every thread, useful when the game hangs).

### License

[MIT](LICENSE) for the code in this repository.

---

## Italiano

Questo progetto fa girare **Azimuth 1.0.2**, il wrapper di compatibilità di Almost There Games per
la versione Xbox One di **Forza Horizon 2 (1.0.0.64)**, sui Mac Apple Silicon tramite **CrossOver**
e **Game Porting Toolkit (D3DMetal)** di Apple. Azimuth è pensato per Windows e Linux; questo repo
aggiunge solo dei layer *sotto* di lui, come fanno DXVK o vkd3d sotto un gioco:

- **`d3d12.dll`**: un layer D3D12 fra Azimuth e D3DMetal (caricato con un override DLL di Wine).
- **`fh2pad.dll`**: il provider di `Windows.Gaming.Input.Gamepad` della bottle, con la tastiera.
- **FH2 Port.app**: un launcher che installa entrambi in una bottle di CrossOver e avvia il gioco.

Sviluppato e provato su MacBook Pro M1 Max (32 GB), macOS 27.2, CrossOver su Wine 11 con GPTK
4.0b3 e Azimuth 1.0.2 (anche la 0.9.10 parte con questo layer, ma la 0.1.7 è stata giocata solo sulla
1.0.2). Mondo aperto, gare, menu, audio, controller e tastiera funzionano; con Azimuth a 60 fps e
1440 righe il gioco sta intorno ai 60 fps ([video](https://github.com/fladnaG86/FH2-Port-AppleSilicon/releases/tag/video)).

### Novità

- **0.1.8**: con l'anti-aliasing attivo, **le schermate di personalizzazione, acquisto e
  potenziamento auto non fermano più il gioco** (issue #15). Lo showroom chiede target MSAA 8x; le
  GPU Apple arrivano a 4x, quindi D3DMetal non offre l'8x e Azimuth si fermava con "the host takes
  no 8 samples of format 10". Ora il layer crea quei target e le loro pipeline a 4 campioni
  (`FH2_MSAA8=0` lo disattiva). L'MSAA disegna ancora trattini nel cielo, quindi resta consigliato
  tenere l'anti-aliasing spento.
  - La correzione delle fiber trova i punti da sola e riconosce anche **Forza Horizon 2 Presents
    Fast & Furious, Forza Horizon 3, Forza Motorsport 5 e 6** (PR #3 di @danielgpinheiro). **Avvio
    di Forza Motorsport 6 fixato**: con questo layer parte su M1 Max con Azimuth 1.0.2 (prima
    crashava sullo splash leggendo `0x8ff`). Qui sono stati provati Forza Horizon 2 e Forza
    Motorsport 6.
- **0.1.7**: **Azimuth 1.0.x** funziona (si bloccava dopo pochi minuti, issue #14) e **gli scatti
  vicino ai palazzi sono spariti**.
  - Azimuth 1.0 usa due code della GPU insieme. Il layer tratteneva i signal di tutte le code con un
    solo fence e un solo thread, e una coda poteva finire ad aspettare l'altra; ora ogni coda ha i suoi.
  - Gli scatti: nei punti affollati Azimuth riserva memoria con `VirtualAlloc2` chiedendo un
    indirizzo fra 4 GB e 1 TB. Wine cerca spazio dal fondo dell'intervallo provando ogni 64 KB con
    `mach_vm_map`, e subito sopra i 4 GB macOS ha Wine, Rosetta, D3DMetal e Metal: migliaia di
    tentativi a vuoto, 40–170 ms a chiamata, 2–7 s ogni 10 s. Ora il layer sceglie un indirizzo
    libero nella metà alta dell'intervallo e chiede proprio quello: meno di 1 ms.
  - `FlushProcessWriteBuffers` non legge più i registri di ogni thread: un cambio di protezione di
    una pagina (`mprotect` di macOS, chiamata diretta) svuota i buffer di scrittura degli altri core.
    ~17 µs invece di ~1,8 ms, e non può bloccarsi in `thread_get_state` (visto una volta con un solo chiamante).
- **0.1.6**: circa **metà degli scatti** nei punti affollati. Wine su macOS
  fa girare i thread a priorità normale di Windows sotto le app native (priorità Mach 26 invece di
  31), e lì stanno il worker di Azimuth e molti thread del gioco; ora il layer li porta al livello nativo.
- **0.1.5**: corregge il **blocco casuale** (nessun Present, il watchdog di Azimuth segnala il
  worker fermo) che arrivava dopo 1–30 minuti, nel filmato iniziale o in gioco. Wine su macOS
  implementa `FlushProcessWriteBuffers` leggendo i registri di ogni thread; sotto Rosetta due thread
  che lo fanno insieme si aspettano a vicenda per sempre. Ora il layer fa passare una chiamata alla volta.
- **0.1.4**: il launcher trova anche **CrossOver Preview**. I passi di installazione non dicono più
  di avviare prima `azimuth.exe` da CrossOver: senza il layer si ferma con errori heap / DX12 prima
  di chiedere la cartella del gioco. Apri invece FH2 Port; poi Azimuth chiede la cartella.
- **0.1.3**: trova le bottle in una cartella Bottles personalizzata (per esempio su un SSD esterno),
  e **Other bottle folder…** permette di scegliere a mano qualsiasi bottle.

### Cosa fa il layer

| Problema su D3DMetal | Cosa fa il layer |
|---|---|
| `OpenExistingHeapFromAddress` non è implementato, e Azimuth mette gli heap GPU sopra la memoria del gioco | Emulato: un heap visibile dalla CPU fa da specchio a quella memoria, sincronizzato a blocchi di 64 byte prima di ogni invio e dopo la GPU; i signal della coda aspettano che i dati siano visibili |
| `WriteBufferImmediate` non fa nulla | Rieseguito sulla CPU prima del fence, oppure trasformato in una copia da un buffer di upload |
| I signal trattenuti finché i dati sono visibili (riga sopra) devono seguire l'ordine di ogni coda; Azimuth 1.0 usa due code | Ogni coda ha fence, lista e thread propri |
| Azimuth parte solo se il device dichiara existing heaps e resource binding tier 3 | Entrambi dichiarati supportati (gli existing heaps sono l'emulazione qui sopra) |
| Le tiled resource non fanno aliasing | Dichiarate non supportate, così Azimuth usa un'altra strada |
| `FlushProcessWriteBuffers` (Wine su macOS legge i registri di ogni thread) si blocca sotto Rosetta se due thread la chiamano insieme, e costa ~1,8 ms a chiamata | `NtFlushProcessWriteBuffers` di ntdll è agganciata in memoria: un chiamante alla volta, e un cambio di protezione di una pagina (`mprotect` di macOS) al posto della lettura dei registri |
| Wine cerca un indirizzo che rispetti i requisiti di `VirtualAlloc2` di Azimuth (4 GB–1 TB) provando ogni 64 KB dal fondo, attraverso la memoria di macOS: fino a 170 ms a chiamata | `NtAllocateVirtualMemoryEx` di ntdll è agganciata: il layer sceglie un indirizzo libero nella metà alta dell'intervallo e chiede quello; la ricerca di Wine resta come ripiego |
| I thread a priorità normale di Windows ricevono priorità Mach 26, sotto il 31 delle app native | Ogni 2 s i thread sotto above-normal vengono portati ad above-normal (Mach 31) |
| **I layout con `D3D12_APPEND_ALIGNED_ELEMENT` su più vertex buffer ricevono offset sbagliati.** Nel layout degli oggetti del mondo `I_YAXIS` viene letto 28 byte e `I_COLOUR` 56 byte più avanti | Gli offset vengono scritti per ogni slot prima di passare il layout a D3DMetal. Questo corregge i "muri fantasma" giganti, gli alberi neri o capovolti, i pali storti e le sponde disallineate |

Il bug dei layout ha una riproduzione autonoma, `layout_test` in `src/d3d12/d3d12.c`, eseguita da
`tools/fh2test.c`: con offset append-aligned 17 istanze su 17 leggono attributi sbagliati, con
offset espliciti 0 su 17.

Anche il gioco ha bisogno di una correzione su Wine per macOS: legge la fiber corrente con
`mov reg, gs:[0x20]`, che lì non arriva al TEB. Il layer reindirizza quelle letture **in
memoria**, dopo che Azimuth ha caricato il gioco; i file del gioco su disco restano intatti.

Non è solo Forza Horizon 2: la stessa lettura è in **Forza Horizon 2 Presents Fast & Furious**
(`Spire_1.0.0.6`), **Forza Horizon 3** (`Opus_1.0.125.0`), **Forza Motorsport 5**
(`Forza_1.0.0.20`, duecento letture) e **Forza Motorsport 6** (`Monument_1.0.0.44`), e ogni build le
mette ad indirizzi e in registri diversi. Il layer riconosce quindi un titolo solo dal timestamp PE e
dalla dimensione dell'immagine, e trova i punti da solo: scansiona le sezioni eseguibili
dell'immagine del gioco cercando `65 REX.W 8b modrm(SIB) 25 20 00 00 00`, trasforma quelli che trova
in un salto a una cave nel padding `int3` della sezione (una cave per registro) e sistema il resto
man mano che l'immagine arriva. Nel log: `game image found at … (Forza Horizon 3 1.0.125.0)` e
`fiber fix: 12 inline fiber reads rerouted … (…)`. Una build non riconosciuta resta intatta, e un
titolo nuovo costa due numeri in `known_images[]`: il suo timestamp e la dimensione dell'immagine.

### Installazione con l'app

1. In CrossOver (o CrossOver Preview) crea una bottle **Windows 10 64-bit** e copia la cartella di
   Azimuth nel suo `drive_c` (il nome della cartella è libero). Dal Terminale, con una bottle
   chiamata `fh2` e Azimuth scompattato in Download:
   ```sh
   ditto ~/Downloads/azimuth-windows "$HOME/Library/Application Support/CrossOver/Bottles/fh2/drive_c/Azimuth"
   ```
   Non avviare `azimuth.exe` da CrossOver: senza il layer si ferma ai controlli D3D12 (errori
   heap / DX12) prima di chiedere la cartella del gioco.
2. Metti **`d3dcompiler_47.dll`** di Microsoft nella cartella di Azimuth (quello di Wine non compila
   il suo shader di detile). Non è incluso; si trova per esempio nella cartella `core/`
   dell'installer Windows 64-bit di Firefox 62.
3. Apri la DMG dalle [Release](../../releases), trascina **FH2 Port** in Applicazioni e aprila.
   Non è notarizzata: la prima volta clicca "Fine", poi Impostazioni di Sistema › Privacy e
   sicurezza › **Apri comunque**.
4. Scegli la bottle e poi la cartella che contiene `azimuth.exe`. L'app installa il layer e avvia
   Azimuth, che la prima volta chiede la cartella del gioco (dopo riapri l'app una volta perché
   applichi le sue impostazioni); le volte dopo basta il doppio clic. La cartella del gioco deve
   essere una cartella vera: Azimuth rifiuta i link simbolici (copiala, oppure fai un clone APFS con
   `cp -c -R`). Wine mostra la tua cartella home come un'unità (spesso **Y:**): è lì che trovi un
   gioco tenuto nella home. Tieni premuto **Opzione** mentre la apri per
   scegliere di nuovo o per disinstallare. Trova anche le bottle in una cartella Bottles
   personalizzata (per esempio su un disco esterno); altrimenti scegli **Other bottle folder…** e
   indica la cartella della bottle.

L'app imposta anche `antialiasing=off` (con l'MSAA il cielo ha dei trattini su D3DMetal) e
`resolution=1440`, che fa supersampling e ammorbidisce i bordi.

### Aggiornare Azimuth senza perdere i salvataggi

I salvataggi stanno in ogni cartella di Azimuth (`data/titles/1A96545B/saves`). Chiudi prima il
gioco, poi:

- scompatta il nuovo Azimuth **sopra** la cartella vecchia: restano `data/titles`,
  `data/azimuth.ini` (che Azimuth converte da solo), `d3dcompiler_47.dll` e il layer. `ditto` unisce
  le cartelle e non cancella niente; nel Finder scegli **Unisci** per `data`, mai **Sostituisci**,
  che cancellerebbe i salvataggi:
  ```sh
  ditto ~/Downloads/azimuth-windows "$HOME/Library/Application Support/CrossOver/Bottles/<bottle>/drive_c/<cartella>"
  ```
- oppure installalo in una **cartella o bottle nuova** (passi sopra) e copia `data/titles` dalla
  cartella vecchia prima del primo avvio:
  ```sh
  B="$HOME/Library/Application Support/CrossOver/Bottles"
  ditto "$B/<bottle vecchia>/drive_c/<cartella vecchia>/data/titles" "$B/<bottle nuova>/drive_c/<cartella nuova>/data/titles"
  ```

Poi apri FH2 Port tenendo premuto **Opzione** e scegli la cartella nuova se è cambiata: l'app ci
installa il layer. Aggiornare FH2 Port non tocca mai i salvataggi. Copiare prima `data/titles` da
un'altra parte è un backup che costa poco.

### Installazione a mano

Comandi nella sezione inglese: `make`, poi `scripts/install.sh --settings` e `scripts/run.sh` con
`BOTTLE` e `AZDIR`. `install.sh --uninstall` annulla tutto; `tools/app/make_app.sh <versione>`
costruisce app e DMG.

### Tastiera

| Tasto | Pad |
|---|---|
| W / S | acceleratore (RT) / freno (LT) |
| A / D | sterzo (graduale: fine corsa in ~150 ms, ritorno al centro in ~80 ms) |
| I J K L | stick destro (guardarsi intorno) |
| Invio / Spazio | A |
| Esc / Backspace | B |
| F / R | X / Y (rewind) |
| Tab / C | Menu / View |
| Q / E | LB / RB |
| H / V | click stick sinistro / destro |
| Frecce | D-pad |

I tasti contano solo quando la finestra del gioco ha il focus; tastiera e controller funzionano
insieme. `FH2_KEYBOARD=0` lascia i gamepad di Wine come sono.

### Problemi noti

- **Blocco / crash con reset della GPU**: il gioco può fermarsi in attesa di un valore che la GPU
  avrebbe dovuto scrivere ("the GPU passed the work that writes the value … and the value is not
  there" di Azimuth, oppure il suo watchdog che segnala il worker bloccato). Prima della 0.1.1
  succedeva dopo 8–20 minuti: il layer perdeva le scritture immediate delle command list dopo che
  ne erano state create più di 4096. I blocchi rimasti erano lo stallo di
  `FlushProcessWriteBuffers` corretto nella 0.1.5; se succede ancora, segnalalo con `azimuth.log` e
  `fh2port.log`.
- **Scatti** nei punti affollati (per esempio i palazzi intorno a Place Masséna a Nizza): corretti
  nella 0.1.7, erano la ricerca di indirizzi di Wine (vedi Novità). Se li vedi ancora segnalalo con
  `fh2port.log`.
- Se nell'intro l'immagine si ferma su **"Premi RT per accelerare"** mentre la musica continua, il
  gioco aspetta RT: controlla che il controller sia acceso, oppure clicca la finestra del gioco e tieni premuto W.
- **MSAA** (anti-aliasing 2x/4x nelle impostazioni di Azimuth): su D3DMetal disegna trattini nel
  cielo. Tienilo spento e usa invece un'altezza di rendering superiore a quella dello schermo
  ("supersampled"). Dalla 0.1.8 non ferma più il gioco nelle schermate auto. I filmati iniziali
  vengono riprodotti (niente salto dell'intro).

### Licenza

[MIT](LICENSE) per il codice di questo repository.
