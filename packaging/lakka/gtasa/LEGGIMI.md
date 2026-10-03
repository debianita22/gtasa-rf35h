# GTA: San Andreas sulla XiFan RF35H (Lakka)

La versione Android 2.11.311 del gioco, eseguita direttamente sulla console:
il binario originale `libGame.so` viene caricato e corretto con le patch di
gtasa_nx (il port per Switch), su Wayland/sway, Mesa (Panfrost) e OpenAL.
L'immagine di Lakka contiene solo il lanciatore: **i file del gioco vengono
dalla tua copia**.

## Cosa serve

La versione **2.11.311 per arm64-v8a** (Google Play), cioè:

- l'APK: su un telefono Android dove il gioco è installato,
  `adb shell pm path com.rockstargames.gtasa` ne mostra il percorso
  (`base.apk`, a volte con accanto `split_config.arm64_v8a.apk`: copiali tutti);
- l'OBB: `Android/obb/com.rockstargames.gtasa/main.*.obb` (ed eventuale
  `patch.*.obb`).

Un'altra versione non va: le patch sono scritte per gli indirizzi della
2.11.311 e il lanciatore lo verifica prima di avviare.

## Installazione

1. Copia APK e OBB nella cartella `gtasa` delle ROM: dalla rete è la
   condivisione **ROMs** di Lakka (`\\LAKKA\ROMs\gtasa`), sulla console
   `/storage/roms/gtasa`. Vanno bene anche dentro `ROMs/gtasa/import`.
2. In RetroArch: **Core senza contenuto > GTA: San Andreas**.
3. La prima volta il lanciatore installa il gioco (alcuni minuti, servono
   circa 3 GB liberi): l'avanzamento compare nelle notifiche di RetroArch.
   Se nel frattempo RetroArch si riavvia o chiudi il core, l'installazione
   continua: riaprendo il core se ne rivede l'avanzamento (non ne parte una
   seconda). APK e OBB finiscono in `ROMs/gtasa/import/originali`: quando il
   gioco funziona puoi cancellarli.
4. Poi, a ogni avvio: due secondi di attesa (per aprire il **Menu rapido**
   e cambiare le opzioni), RetroArch si chiude, parte il gioco; uscendo dal
   gioco RetroArch si riapre.

## Comandi

| Tasto della console | Nel gioco |
|---|---|
| in basso / a destra / a sinistra / in alto | CROCE / CERCHIO / QUADRATO / TRIANGOLO |
| L1, R1, L2, R2 | come sul pad PlayStation (L2/R2 analogici) |
| stick, L3, R3, croce direzionale | come sul pad |
| START | pausa |
| SELECT | mappa |
| SELECT + START per un secondo | esce dal gioco (anche: Pausa > Esci) |

Se i tasti in basso e a destra risultano invertiti: opzione **Scambia A e B**.
La tastiera per i trucchi del port Switch (L3+R3) qui non c'è.

## Opzioni (Menu rapido > Opzioni)

- **Fotogrammi al secondo**: 30 (predefinito, la metà esatta dei 60 Hz del
  pannello) o 60.
- **Risoluzione di rendering**: 100% = 640x480; sotto, il gioco disegna meno
  pixel e il compositor ingrandisce l'immagine (utile se la GPU è al limite).
- **Governor performance**: CPU, GPU e memoria al massimo sempre (predefinito),
  solo durante il caricamento, o mai.
- **Scambia A e B**, **Contatore FPS**, **Statistiche** (`stats.log`).

Le altre impostazioni sono in `ROMs/gtasa/gtasa_nx.cfg`, creato al primo
avvio e commentato riga per riga (tra queste: `vsync`, `glthread`,
`cpu_turbo`, `stick_deadzone`, le correzioni di gtasa_nx).

## Impostazioni grafiche del gioco

Distanza visiva, effetti, ombre e riflessi si regolano nelle opzioni del
gioco e restano salvati in `ROMs/gtasa/gta_sa.set`.

Al primo avvio il gioco sceglie da solo in base alla GPU, e su questa
partirebbe dal massimo: effetti alti, ombre in tempo reale, distanza 100,
riflessi delle auto al livello 3 (ridisegna la scena in una texture ogni
fotogramma). Il port lo fa partire invece con il profilo che il gioco usa per
le GPU deboli, più distanza visiva a metà:

| Impostazione | Primo avvio |
|---|---|
| Effetti visivi | bassi |
| Ombre | no |
| Distanza visiva | 50 |
| Riflessi delle auto | statici (livello 1) |

Da lì puoi alzarle nel menu del gioco e guardare gli fps (opzione
**Contatore FPS**). I valori di partenza sono le righe `game_*` di
`gtasa_nx.cfg`; valgono di nuovo solo se cancelli `gta_sa.set` o reimposti
le opzioni dal menu.

La **Risoluzione** del menu del gioco a 640x480 non fa niente: il gioco la
applica solo agli schermi più larghi di 640 pixel. Per far disegnare meno
pixel alla GPU c'è l'opzione **Risoluzione di rendering** del Menu rapido.

`texture_lod_bias 0` (predefinito) toglie dagli shader del gioco il bias che
fa leggere texture più dettagliate del necessario: meno banda e meno
sfarfallio a 640x480; `1` torna all'aspetto originale, un po' più nitido.

## Salvataggi e file

Tutto in `ROMs/gtasa`: salvataggi, impostazioni, `gtasa.log`. Per una copia
di sicurezza basta copiare la cartella dalla rete.

Spazio: dopo la prima partita, da SSH, `rf35h-gtasa slim` elenca le varianti
delle texture (dxt, pvr, etc, unc) che il gioco non usa su questa GPU;
`rf35h-gtasa slim --yes` le cancella (centinaia di MB).

## Se qualcosa va storto

Il motivo compare in RetroArch al lancio successivo (START per riprovare) e
resta in `ROMs/gtasa/last-error.txt`; i dettagli in `gtasa.log`
(`gtasa-import.log` per l'installazione, `launcher.log` per l'avvio).

Se un'installazione non riesce ma il gioco è già installato (per esempio un
`patch.*.obb` rimasto da solo nella cartella), START avvia il gioco già
installato; l'installazione viene ritentata alla prossima apertura del core,
finché gli archivi restano in `ROMs/gtasa`.

| Messaggio | Cosa fare |
|---|---|
| l'APK è a 32 bit (armeabi-v7a) | serve l'APK arm64-v8a |
| manca l'OBB | copia `main.*.obb` accanto all'APK |
| spazio insufficiente | libera spazio sulla scheda |
| non è la build 2.11.311 | serve esattamente la 2.11.311 |
| il gioco è uscito con codice N | `gtasa.log`: le ultime righe dicono dove |
| il gioco parte ma i tasti non rispondono | in `gtasa.log` la riga `pad: joystick ... (GUID ...)`: aggiungi una mappatura SDL con quel GUID in `ROMs/gtasa/gamecontrollerdb.txt` |

Per misurare le prestazioni: opzione **Statistiche**, poi `stats.log` (una
riga ogni 5 secondi con fps, tempi, CPU per thread, GPU, RAM, frequenze,
temperatura).

Il tempo di gioco finisce nel registro di RetroArch (la stessa cartella dei
log di runtime impostata in RetroArch), misurato dall'avvio del sistema:
non lo falsa l'orologio che si aggiorna quando arriva la rete.

## Licenze

Lanciatore e loader: MIT (gtasa_nx di fgsfds, Andy Nguyen e altri; port
RF35H). Il gioco appartiene a Rockstar Games e non è incluso.
