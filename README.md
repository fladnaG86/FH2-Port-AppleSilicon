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

This project lets **Azimuth 0.9.10**, Almost There Games' compatibility wrapper for the Xbox One
version of **Forza Horizon 2 (1.0.0.64)**, run on Apple Silicon Macs through **CrossOver** and
Apple's **Game Porting Toolkit (D3DMetal)**. Azimuth itself targets Windows and Linux; this repo
only adds layers *underneath* it, the way DXVK or vkd3d sit under a game:

- **`d3d12.dll`**: a D3D12 layer between Azimuth and D3DMetal (loaded through a Wine DLL override).
- **`fh2pad.dll`**: the bottle's provider of `Windows.Gaming.Input.Gamepad`, adding a keyboard.
- **FH2 Port.app**: a launcher that installs both into a CrossOver bottle and starts the game.

Developed and tested on a MacBook Pro M1 Max (32 GB), macOS 27.2, CrossOver on Wine 11 with
GPTK 4.0b3. Free roam, races, menus, audio, controllers and keyboard work.

### What the layer does

| Problem on D3DMetal | What the layer does |
|---|---|
| `ID3D12Device3::OpenExistingHeapFromAddress` is not implemented, and Azimuth places GPU heaps over the game's memory | Emulated: a CPU-visible heap mirrors that memory, synced in 64-byte blocks before each submit and after the GPU; queue signals are held until the data is visible |
| `WriteBufferImmediate` does nothing | Replayed on the CPU before the fence, or turned into a copy from an upload ring |
| Azimuth only starts on devices that report existing heaps and resource binding tier 3 | Both reported as supported (existing heaps are the emulation above) |
| Tiled resources do not alias | Reported as unsupported, so Azimuth takes another path |
| **Input layouts using `D3D12_APPEND_ALIGNED_ELEMENT` over several vertex buffers get wrong offsets.** In the world-prop layout `I_YAXIS` is fetched 28 bytes and `I_COLOUR` 56 bytes past their real position | Offsets are written out per slot before the layout reaches D3DMetal. This fixes the giant "phantom walls", black or upside-down trees, bent poles and misaligned barriers |

The input-layout bug has a standalone repro, `layout_test` in `src/d3d12/d3d12.c`, run by
`tools/fh2test.c`: with append-aligned offsets 17 of 17 instances read the wrong attributes, with
explicit offsets 0 of 17.

The game itself also needs one fix on macOS Wine: it reads the current fiber inline with
`mov reg, gs:[0x20]`, which does not reach the TEB there. The layer reroutes those 12 reads **in
memory**, after Azimuth has loaded the game; the game files on disk stay untouched.

### Install with the app

1. In CrossOver create a **Windows 10 64-bit** bottle, copy the Azimuth folder into its `drive_c`
   and run `azimuth.exe` once to pick your game folder.
2. Put Microsoft's **`d3dcompiler_47.dll`** in the Azimuth folder (Wine's one cannot compile
   Azimuth's GPU detile shader). It is not included here; one source is the `core/` folder of the
   Firefox 62 Windows 64-bit installer.
3. Open the DMG from the [Releases](../../releases), drag **FH2 Port** to Applications and open it.
   It is not notarized: the first time, click "Done", then System Settings › Privacy & Security ›
   **Open Anyway**.
4. Choose the bottle, the CrossOver installation that runs it, then the folder that contains
   `azimuth.exe`. The app installs the layer and starts the game; later a double-click is enough.
   Hold **Option** while opening it to choose again or to uninstall.

The app reads the bottle folder from CrossOver's own **BottleDir** preference, so a custom bottle
folder works without extra setup, and it lists every CrossOver in `/Applications`, newest D3DMetal
first.

The app also sets `antialiasing=off` (MSAA draws dashes in the sky on D3DMetal) and
`resolution=1440`, which supersamples and smooths the edges.

### Install by hand

```sh
brew install mingw-w64
make                                             # build/d3d12.dll, build/fh2pad.dll
BOTTLE=<bottle> AZDIR=<folder> scripts/install.sh --settings
BOTTLE=<bottle> AZDIR=<folder> scripts/run.sh    # MTL_HUD_ENABLED=1 for the Metal HUD
```

Both scripts take `BOTTLE_ROOT` (the folder holding the bottles, default CrossOver's standard one)
and `CX` (the CrossOver installation, default `/Applications/CrossOver.app/Contents/SharedSupport/CrossOver`).

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
  lost the immediate writes of command lists once more than 4096 lists had been created. Since
  that fix a 31-minute session ran clean; please report it if it still happens.
- MSAA must stay off. The intro movies play (no intro skip).

### Debugging

Logs are in `data/logs` of the Azimuth folder (`fh2port.log`, `fh2pad.log`, `azimuth.log`) and
`~/Library/Logs/FH2Port/launcher.log`. Switches: `FH2_COUNT=1` (per-PSO draw counters),
`FH2_TINT=1` (magenta twins of instanced PSOs for live bisection), `FH2_APPEND=0` (layouts passed
through unfixed), `FH2_FIBER=0`, `FH2_TILED=1`, `FH2_SKIP_EI=1`, `FH2_SKIP_VID=1`, `FH2_PATCH=…`,
`FH2_LABEL=<address>` (traces the values written to that address through record, execute, signal
and completion). Live commands go through `data/logs/fh2port.ctl` (`top`, `reset`, `dump N`,
`tint …`, `tintlist`, `tintset …`, `watch …`, `patch on|off`, `stacks` for the Windows-side call
stack of every thread, useful when the game hangs).

### License

[MIT](LICENSE) for the code in this repository.

---

## Italiano

Questo progetto fa girare **Azimuth 0.9.10**, il wrapper di compatibilità di Almost There Games per
la versione Xbox One di **Forza Horizon 2 (1.0.0.64)**, sui Mac Apple Silicon tramite **CrossOver**
e **Game Porting Toolkit (D3DMetal)** di Apple. Azimuth è pensato per Windows e Linux; questo repo
aggiunge solo dei layer *sotto* di lui, come fanno DXVK o vkd3d sotto un gioco:

- **`d3d12.dll`**: un layer D3D12 fra Azimuth e D3DMetal (caricato con un override DLL di Wine).
- **`fh2pad.dll`**: il provider di `Windows.Gaming.Input.Gamepad` della bottle, con la tastiera.
- **FH2 Port.app**: un launcher che installa entrambi in una bottle di CrossOver e avvia il gioco.

Sviluppato e provato su MacBook Pro M1 Max (32 GB), macOS 27.2, CrossOver su Wine 11 con GPTK
4.0b3. Mondo aperto, gare, menu, audio, controller e tastiera funzionano; con Azimuth a 60 fps e
1440 righe il gioco sta intorno ai 60 fps ([video](https://github.com/fladnaG86/FH2-Port-AppleSilicon/releases/tag/video)).

### Cosa fa il layer

| Problema su D3DMetal | Cosa fa il layer |
|---|---|
| `OpenExistingHeapFromAddress` non è implementato, e Azimuth mette gli heap GPU sopra la memoria del gioco | Emulato: un heap visibile dalla CPU fa da specchio a quella memoria, sincronizzato a blocchi di 64 byte prima di ogni invio e dopo la GPU; i signal della coda aspettano che i dati siano visibili |
| `WriteBufferImmediate` non fa nulla | Rieseguito sulla CPU prima del fence, oppure trasformato in una copia da un buffer di upload |
| Azimuth parte solo se il device dichiara existing heaps e resource binding tier 3 | Entrambi dichiarati supportati (gli existing heaps sono l'emulazione qui sopra) |
| Le tiled resource non fanno aliasing | Dichiarate non supportate, così Azimuth usa un'altra strada |
| **I layout con `D3D12_APPEND_ALIGNED_ELEMENT` su più vertex buffer ricevono offset sbagliati.** Nel layout degli oggetti del mondo `I_YAXIS` viene letto 28 byte e `I_COLOUR` 56 byte più avanti | Gli offset vengono scritti per ogni slot prima di passare il layout a D3DMetal. Questo corregge i "muri fantasma" giganti, gli alberi neri o capovolti, i pali storti e le sponde disallineate |

Il bug dei layout ha una riproduzione autonoma, `layout_test` in `src/d3d12/d3d12.c`, eseguita da
`tools/fh2test.c`: con offset append-aligned 17 istanze su 17 leggono attributi sbagliati, con
offset espliciti 0 su 17.

Anche il gioco ha bisogno di una correzione su Wine per macOS: legge la fiber corrente con
`mov reg, gs:[0x20]`, che lì non arriva al TEB. Il layer reindirizza quelle 12 letture **in
memoria**, dopo che Azimuth ha caricato il gioco; i file del gioco su disco restano intatti.

### Installazione con l'app

1. In CrossOver crea una bottle **Windows 10 64-bit**, copia la cartella di Azimuth nel suo
   `drive_c` e avvia `azimuth.exe` una volta per scegliere la cartella del gioco.
2. Metti **`d3dcompiler_47.dll`** di Microsoft nella cartella di Azimuth (quello di Wine non compila
   il suo shader di detile). Non è incluso; si trova per esempio nella cartella `core/`
   dell'installer Windows 64-bit di Firefox 62.
3. Apri la DMG dalle [Release](../../releases), trascina **FH2 Port** in Applicazioni e aprila.
   Non è notarizzata: la prima volta clicca "Fine", poi Impostazioni di Sistema › Privacy e
   sicurezza › **Apri comunque**.
4. Scegli la bottle, l'installazione di CrossOver che la esegue e poi la cartella che contiene
   `azimuth.exe`. L'app installa il layer e avvia il gioco; le volte dopo basta il doppio clic.
   Tieni premuto **Opzione** mentre la apri per scegliere di nuovo o per disinstallare.

L'app legge la cartella delle bottle dalla preferenza **BottleDir** di CrossOver, quindi una
cartella personalizzata funziona senza altri passaggi, ed elenca ogni CrossOver in `/Applications`
partendo dal D3DMetal più recente.

L'app imposta anche `antialiasing=off` (con l'MSAA il cielo ha dei trattini su D3DMetal) e
`resolution=1440`, che fa supersampling e ammorbidisce i bordi.

### Installazione a mano

Comandi nella sezione inglese: `make`, poi `scripts/install.sh --settings` e `scripts/run.sh` con
`BOTTLE` e `AZDIR` (e `BOTTLE_ROOT` / `CX` per una cartella o un'installazione di CrossOver
diverse). `install.sh --uninstall` annulla tutto; `tools/app/make_app.sh <versione>` costruisce app
e DMG.

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
  ne erano state create più di 4096. Dopo quella correzione una sessione di 31 minuti è andata
  liscia; se succede ancora, segnalalo.
- L'MSAA va lasciato spento. I filmati iniziali vengono riprodotti (niente salto dell'intro).

### Licenza

[MIT](LICENSE) per il codice di questo repository.
