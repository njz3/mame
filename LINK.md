# Jeux de course en link avec MAME njz3 — mode d'emploi et astuces

Ce document rassemble les réglages et les étapes pour faire courir ensemble plusieurs instances de MAME
(plusieurs bornes, ou plusieurs fenêtres sur un même PC). Il s'appuie sur les essais du 2026-11-02 au 2026-11-12
(exe 0.289 njz3 : release g8e81ca4, puis g6b9d67c pour Konami, puis `dev/mame3d` g754eb0adb02).
Détails sur la puce Namco : `C139.md` (en anglais : `C139_en.md`).

Les NVRAM prêtes à l'emploi (une par borne et par jeu) et la liste pour l'image (`liste-image.csv`) sont dans le
dossier de notes partagé des sessions : `link-tests-0289/` (README en français, mêmes informations que ci-dessous).

---

## 1. Principe

- Chaque instance ouvre **une connexion TCP entrante** et **une connexion sortante vers la borne suivante** :
  ```
  mame.exe <jeu> -comm_localhost <mon IP> -comm_localport <mon port> -comm_remotehost <IP suivante> -comm_remoteport <port suivant>
  ```
  Valeurs par défaut : `0.0.0.0:15112` en local, `127.0.0.1:15112` en distant. Une instance seule avec ces valeurs
  se connecte à elle-même : l'anneau est bouclé, le jeu reste jouable seul.
- Les bornes forment un **anneau** : cab1 → cab2 → cab3 → cab4 → cab1. À 2 bornes : cab1 → cab2 → cab1.
- **`-comm_framesync` n'est pas utilisé** : tous les essais se font sans.
- **Chaque borne a sa propre NVRAM** (ou ses propres lignes DIP dans le cfg) : c'est là que le jeu lit son numéro
  de borne, son rôle (maître ou esclave) et le nombre de machines. Sans cela, chaque borne court seule.
- L'ordre et l'écart de démarrage n'ont pas d'importance : tous les jeux se lient avec 0, 4 ou 15 s d'écart
  entre les bornes. La connexion se rétablit seule si une borne démarre en retard.

### Sur les bornes

`RunMAMELinkGame.cmd` appelle `set_cab_id.bat`, qui donne `CAB_ID` (`_link_cab1` ou `_link_cab2`),
puis `set_cab_network<CAB_ID>.bat` :

| Borne | LOCALIP | LOCALPORT | NEXTIP | NEXTPORT |
|---|---|---|---|---|
| cab1 | 192.168.1.4 | 15112 | 192.168.1.5 | 15113 |
| cab2 | 192.168.1.5 | 15113 | 192.168.1.4 | 15112 |

Le dossier d'écriture est `-writedir C:\Racecab\Saves\mame_link_cabN` (UWF). Au premier lancement, MAME lit les
NVRAM en place dans `nvram\<jeu>`, que Build-Config pose depuis `nvram_link_cabN`. Ensuite, il ne lit plus que
celles du writedir. **Pour changer la NVRAM d'une borne, il faut donc vider `Saves\mame_link_cabN\nvram\<jeu>`.**

Pour 3 ou 4 bornes, il faut aussi des scripts `set_cab_network_link_cab3/4.bat` qui ferment l'anneau
(cab2 → cab3, cab3 → cab4, cab4 → cab1).

Touches des bornes : pièce = vJoy 1, Test = vJoy 2, Service = vJoy 3, Start = vJoy 4.

### Sur un seul PC (essais)

Une adresse de boucle locale et un port par instance, un `-writedir` par instance (sinon les NVRAM se mélangent) :
```
mame.exe raverace -writedir cab1 -comm_localhost 127.0.0.4 -comm_localport 15112 -comm_remotehost 127.0.0.5 -comm_remoteport 15113
mame.exe raverace -writedir cab2 -comm_localhost 127.0.0.5 -comm_localport 15113 -comm_remotehost 127.0.0.4 -comm_remoteport 15112
```
Mettre la NVRAM de la borne N dans `cabN\nvram\<jeu>\` avant le premier lancement.
Pour 4 instances : 127.0.0.4 à .7, ports 15112 à 15115, la dernière pointe vers la première.

---

## 2. Réglages par jeu

### 2.1 Namco (C139) : tout est dans la NVRAM

Avec le nouveau C139 (depuis g8e81ca4), **aucun réglage de port n'est nécessaire** : les ports standard 15112/15113
suffisent pour tous les jeux. Il suffit de poser la NVRAM de la borne : rien à faire dans les menus.

| Jeu (set) | Bornes | Ce qui distingue les bornes |
|---|---|---|
| ridgera2 (Ridge Racer 2) | 2-4 | CPU NO. 1-4 et CAR TYPE 1-4 (EEPROM 0x71C/0x71D) |
| raverace (Rave Racer) | 2-4 | CPU NUMBER 1-4 et CAR TYPE 1-4 (EEPROM 0x2C0/0x2C1) |
| acedrive (Ace Driver) | 2-8 | PCB ID NUMBER 1-4 (EEPROM 0x111) |
| victlap (Ace Driver Victory Lap) | 2-8 | PCB ID NUMBER 1-4 (EEPROM 0x10C) |
| winrun91, winrungp (Winning Run '91, Suzuka GP) | 2-8 | CAR TYPE et CPU NO. (octets 8-9, somme en 0xC) |
| finallap, finalap2, finalap3 (Final Lap 1/2/3) | 2-8 | n° de PCB 0xFE01-0xFE04 (octets 0x10-0x11) |
| fourtrax (Four Trax) | 2-8 | GRID NO 0-3 (octet 8) |
| suzuka8h, suzuk8h2 (Suzuka 8 Hours 1/2) | 2-8 | GRID NO 0-3 (octet 9) |
| dirtfoxj (Dirt Fox) | 2-8 | CAR TYPE 1-4 (octet 0x47, somme en 0x41) |
| cybrcycc (Cyber Cycles) | 2-4 | ID NUMBER 0-3 (EEPROM 0x26) |
| adillor (Armadillo Racing) | 2-4 | PCB ID 0-3 (enregistrement à somme de contrôle, voir `C139.md` § 7) |
| alpinr2b (Alpine Racer 2) | 2 | ID NO. 0 LEFT / 1 RIGHT (EEPROM 0x32) |

Pour refaire un réglage à la main :
- **raverace** : Test (bascule). Volant à droite pour descendre, à gauche pour monter, accélérateur pour entrer,
  éditer ou valider. Aller dans GAME OPTIONS (2e ligne). Le curseur arrive sur NEXT PAGE ; 13 crans à gauche mènent à
  CPU NUMBER. Faire accélérateur, volant à droite (+1), accélérateur. 1 cran à droite mène à CAR TYPE : même manœuvre.
  Descendre jusqu'à EXIT, accélérateur, puis Test (bascule) pour sortir. La ligne sous le curseur clignote.
- **acedrive / victlap** : Test (bascule). Levier (Shift Up) pour descendre, accélérateur pour entrer.
  Aller dans GAME OPTIONS, sur PCB ID NUMBER. Accélérateur, Shift Up (1 → 2), accélérateur.
  Shift Up jusqu'à EXIT, accélérateur, puis Test.
- **winrun91 / winrungp** : Test (bascule) ouvre GAME OPTIONS 1. Volant pour changer de ligne, levier Up/Down pour
  changer la valeur. Mettre CAR TYPE A → B et CPU NO. 1 → 2, puis Test. La ligne « LINK OK n » ne fait qu'afficher.
- Les jeux sans tableau de menu ici (Final Lap, Four Trax, Suzuka, Dirt Fox, Cyber Cycles, Armadillo, Alpine Racer 2) :
  utiliser les NVRAM fournies.

### 2.2 Konami (K056230) : DIP MAME « Network ID »

midnrun (Midnight Run), windheat (Winding Heat), gticlub (GTI Club). Rien à régler dans le jeu : cab1 garde l'ID 1
par défaut, et les autres bornes reçoivent une ligne DIP dans la section `<input>` de leur cfg :

| Jeu | cab2 | cab3 | cab4 |
|---|---|---|---|
| midnrun, windheat | `<port tag=":IN3" type="DIPSWITCH" mask="12" defvalue="12" value="8" />` | value="4" | value="0" |
| gticlub | `<port tag=":IN3" type="DIPSWITCH" mask="3" defvalue="3" value="2" />` | value="1" | value="0" |

**gticlub** refait le réglage des commandes à chaque démarrage. Avec une EEPROM vierge et sans volant, il affiche
« DEVICE ERROR (STEERING WHEEL) – PUSH TEST SWITCH TO SKIP » au bout d'environ 70 s. Les EEPROM fournies sont déjà
passées. Sinon : un appui bref sur Test, puis Shift Down une fois (le menu boucle sur GAME MODE), puis Start.

### 2.3 Sega : menus de test ou DIP

| Jeu | Carte | Réglage | Où |
|---|---|---|---|
| **scrossa** (pas scross !) | s32comm | cab1 NETWORK / MASTER, cab2 NETWORK / SLAVE | menu test (EEPROM) + `scrossa.cfg` (entrées) |
| orunners (OutRunners) | s32comm | NETWORK ASSIGNMENTS → COMMUNICATION NETWORK : cab1 MASTER / ID 1, cab2 SLAVE / ID 2 | menu test |
| f1lap (F1 Super Lap) | s32comm | GAME ASSIGNMENTS → NETWORK ID NUMBER : cab1 « 1 MASTER », cab2 « 2 » | menu test, **puis relancer le jeu** |
| radr (Rad Rally) | s32comm | Game & system setting → Network ID number (changer avec Start) : cab1 « 1 Master », cab2 « 2 » | menu test |
| vr (Virtua Racing), vformula (Virtua Formula) | m1comm | GAME SYSTEM → LINK ID : cab1 MASTER, cab2 SLAVE | menu test, **sortir par EXIT puis YES** |
| pdriftl (Power Drift Link) | ybdcomm | Link_SW1 : Cabinet ID 1/2, Cabinet Count 2 sur les deux | DIP MAME (cfg) |
| smgp (Super Monaco GP) | xbdcomm | Machine ID 1/2, Number of Machines 2 | DIP MAME (cfg) |

Détails des menus :
- **scrossa** : il faut un **appui très bref** sur Test (≈ 40 ms). Un appui plus long sélectionne EXIT, où le curseur
  est par défaut, et le jeu redémarre au relâchement. C'est le comportement de la ROM, la vraie borne fait pareil.
  Service déplace le curseur, Test valide. Depuis EXIT, 12 fois Service mènent à COMMUNICATION SETTING ; faire Test.
  Dans ce menu : 1re ligne, Test (STANDALONE → NETWORK) ; 2e ligne, Test (MASTER ↔ SLAVE) ; EXIT, Test ;
  puis EXIT du menu principal.
- **orunners** : Service = Push SW1, Test = Push SW2 dans les menus.
- **vr / vformula** : bouton rouge pour choisir, vert pour entrer, bleu pour changer la valeur
  (NO LINK → LIVE → SLAVE → MASTER). Le vert ne sort que depuis la ligne EXIT. Il faut ensuite répondre **YES**
  à « SAVE THE CHANGE ? », sinon rien n'est gardé. Le menu de vformula est plus court que celui de vr :
  après LINK ID viennent RACE MODE, COUNTRY, ADVERTISE, DIFFICULTY, EXIT. Compter les crans, sinon le curseur
  repasse par le haut.
- **f1lap** : le nouvel ID n'est pris en compte qu'après une relance du jeu.

Lignes DIP Sega (section `<input>` du cfg de chaque borne) :
- **pdriftl** : sur toutes les bornes `<port tag=":ybdcomm:Link_SW1" type="DIPSWITCH" mask="240" defvalue="16" value="32" />`
  (nombre de bornes : 2 → 32, 3 → 48, 4 → 64). En plus, sur les bornes 2 à 4 :
  `<port tag=":ybdcomm:Link_SW1" type="DIPSWITCH" mask="15" defvalue="1" value="2" />` (value = n° de borne).
- **smgp** : sur toutes les bornes `<port tag=":mainpcb:IO1PORTD" type="DIPSWITCH" mask="56" defvalue="56" value="48" />`
  (nombre de machines : 2 → 48, 3 → 40, 4 → 32). En plus, sur les bornes 2 à 4 :
  `<port tag=":mainpcb:IO1PORTD" type="DIPSWITCH" mask="7" defvalue="7" value="6" />` (borne 2 → 6, 3 → 5, 4 → 4).

MAME n'a pas d'option de DIP en ligne de commande : ces lignes vont dans le cfg propre à chaque borne.

---

## 3. Pièges connus

- **scross ne se lie pas** : son menu COMMUNICATION SETTING est du code mort. Lancer **scrossa**
  (« World, linkable », déjà contenu dans scross.zip fusionné) avec `scrossa.cfg`. Sans section `<input>` dans le cfg,
  vJoy 1 n'est pas Coin 1 et le jeu reste à 0 crédit.
- **finallap, finalap2, fourtrax, dirtfoxj** : il manque `lh5762.6n` (CRC 90db1bf6) dans les zip du NAS.
  Le poser en fichier libre dans `MameRoms\<set>\lh5762.6n`, sans toucher au zip ; MAME le trouve à côté.
  Son contenu est le même que celui de `04544191.6r` de finalap3.zip.
- **smgp** : chaque joueur doit se qualifier (un tour en moins de 45 s). Un joueur qui rate la qualification
  revient à « ENTRIES WELCOME » : c'est normal, ce n'est pas un problème de link.
- **vr.zip** : ne jamais l'écraser (il contient epr-15111.12 et vformula fusionné).
- **Changer de NVRAM** sur une borne sous UWF : vider d'abord le sous-dossier du writedir (voir § 1).
- **Rave Racer à 4** : il est arrivé 2 fois sur 14 essais que deux ou trois bornes affichent la même POSITION
  (une fois avec d'autres MAME en cours sur le PC, une fois au coude à coude). Les 6 essais suivants sont cohérents.
  À surveiller, sans régression démontrée.
- **Ancien exe 919EFA6F (avant g8e81ca4)** : il demandait le port 15114 sur cab2 pour acedrive, victlap,
  winrun91 et winrungp, et ne liait ni ridgera2 ni Final Lap. Ce réglage n'est plus utile (il est sans effet s'il reste).

## 4. Jeux sans link

- **ridgerac** : Ridge Racer standard, pas de link dans le jeu.
- **ridgeracf** (Full Scale) : 3 écrans sur une seule machine, ROM de textures jamais dumpées, non jouable.
- **driveyes** (Driver's Eyes) : une seule machine à 3 écrans, les trois cartes sont reliées dans le pilote.
  Les trois écrans fonctionnent.
- **dirtdash** (Dirt Dash) : pas de code de link dans le jeu.

## 5. Vérifier qu'un link fonctionne

- **Écran d'attraction ou de sélection** :
  - ridgera2 : « Up to 4 can race at once! » ;
  - raverace : « PLAY LINKED GAMES… » ;
  - Final Lap : « UP TO 2/4 CAN RACE SIMULTANEOUSLY » ;
  - smgp : « ENTRIES WELCOME – LINK SYSTEM ».
- **En course** :
  - la position est donnée sur le nombre de bornes (1/2, 2/2…) ;
  - les voitures des autres joueurs apparaissent, dans leurs couleurs ;
  - vr et vformula affichent 1ST/2, 2ND/2.
- Une borne seule affiche au contraire 1/1, « UP TO 1 CAN PLAY » ou « One-player VS computer cars ».

## 6. Avec `dev/mame3d` (rendu HD)

- Le link fonctionne aussi avec `-hires3d 2` ou `4` et le rendu Vulkan (`-hires3d_renderer auto`). Testé sur les
  12 jeux à puce 3D (System 22, System 21, gticlub, midnrun, windheat), avec 0, 4 ou 15 s d'écart.
- `-verbose` doit afficher « hires3d drawn with Vulkan on … ». Vérifié en solo pour un jeu de chaque famille.
- `-output network` envoie au démarrage `mame_start = <jeu>` en broadcast UDP sur le port 8001.
  BackForceFeeder et le FFB Plugin njz3 l'écoutent tous les deux (port partagé).
- À la sortie, tous les jeux testés quittent MAME avec le code 0 : les 27 jeux en natif, les 12 jeux 3D en Vulkan.
