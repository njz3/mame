# Namco System 21 : comment les jeux pilotent le C139

Désassemblage statique (capstone, régions vidées par MAME, octets 68000 remis en big-endian). Outils dans `s21tools/`
du répertoire de travail : `d.py` (désassembler une plage), `xref.py` (références à une adresse), `absrange.py`
(accès absolus à une plage d'adresses), `vec.py` (vecteurs), `strs.py` (chaînes), `lin.py` (désassemblage linéaire).
Toutes les adresses sont celles du CPU indiqué ; `$xxxxxx` = adresse 68000.
Ce qui est marqué **hypothèse** n'est pas prouvé par le code.

Rappel du câblage MAME (les trois pilotes S21) :
- C139 : registres en `$B80000` (STATUS +0, MODE +2, CONTROL +4, START +6, RXSIZE +8, TXSIZE +A, RXOFFSET +C, TXOFFSET +E),
  RAM en `$B00000` (0x2000 demi-mots ; TX = mots 0x000-0xFFF = `$B00000-$B01FFF`, RX = mots 0x1000-0x1FFF = `$B02000-$B03FFF`).
  La RAM est vue par tous les 68000 (common_map).
- L'IRQ du C139 va aux C148 de chaque CPU (`sci_int_w`). Chaque CPU choisit son niveau SCI en écrivant `$1CC000` ;
  niveau 0 = pas d'interruption. L'acquittement est l'écriture de `$1DC000`. **C'est donc le CPU qui met un niveau SCI ≠ 0 qui gère le link.**
- NVRAM 8 Ko en `$180000-$183FFF`, octet bas seulement (umask 0x00FF) : octet n du fichier nvram = adresse `$180000 + 2n + 1`.

## 0. Résumé par jeu

| Jeu | Usage du C139 | CPU du link (niveau SCI) | Mode programmé | START en mode 0x0F |
|---|---|---|---|---|
| winrun (1988, Ver.09) | **test seulement** (« SCI » du SWITCH TEST, boucle) | maître, niveau 1 (= niveau VBL !) ; esclave inactif | 0x0F pendant le test, sinon 0 (reset) | 0 |
| winrungp (Suzuka GP) | **link entre bornes**, anneau, jusqu'à 8 bornes | esclave, niveau 4 (`L4 = $1574`) | 0x09 | 3 |
| winrun91 | **link entre bornes**, même code que winrungp, trame différente | esclave, niveau 4 (`L4 = $1528`) | 0x09 | 3 |
| cybsled (CY2) | **link entre bornes**, même protocole, 2 bornes (LEFT / RIGHT) | esclave, niveau 4 (`L4 = $37D8`) | 0x09 | 3 |
| driveyes | **link interne** entre les 3 cartes (centre → côtés), diffusion à sens unique | esclave de chaque carte, niveau 4 (`L4 = $3DF0`) | 0x0D (le centre passe à 0x09 tout seul, voir § 3) | 1 |
| aircomb (AC2 US) | puce initialisée, IRQ installée, **code de link mort** (jamais appelé) | maître, niveau 4 (`L4 = $125A`) | 0x0D | 0 |
| starblad | **init seulement** (mode 0x0F) ; routine RX jamais appelée | aucun (SCI niveau 0 sur les deux CPU) | 0x0F | 0 |
| solvalou | **aucun accès** au C139 | aucun (SCI niveau 0) | — | — |

Points essentiels :
1. Les trois jeux à link (winrungp, winrun91, cybsled) utilisent **le même protocole en anneau, et c'est le jeu qui relaie** :
   il pointe TXOFFSET dans la zone RX pour réémettre la trame reçue. Le C139 ne doit **pas** relayer.
2. Les registres MODE bits 1 et 2 servent de verrous d'interruption RX et TX : la puce les met à 1 quand elle lève l'IRQ, le jeu les remet à 0 pour réarmer. Le code ne marche qu'avec ce fonctionnement (§ 2.4).
3. TXOFFSET est une **adresse de mot sur 13 bits** (bit 12 = zone RX), pas une adresse d'octet.
4. Driver's Eyes : le centre émet, les côtés ne font que recevoir. Aucune carte ne relaie par logiciel.

---

## 1. Winning Run (winrun, 1988) : test seulement

- GAME OPTIONS (chaîne GPU `$45BB`) : COIN1, COIN2, TIME RANK, SPEED RANK, LAP EASY, LAP TECHNICAL, FREE PLAY, MOTION, HISCORE CLEAR.
  Pas de CPU NO., pas de LINK.
- L'esclave ne fait rien : boucle `$1074-$107E` (watchdog), tous les vecteurs d'IRQ en `$1080` (rte).
  Le code C139 présent en `$28E0` de la ROM esclave n'est pas atteignable.
- Le maître met le niveau SCI à **1** (`$1086`), le **même que la VBL** (`$108E`), et le handler L1 (`$1928`) est celui de la VBL
  (il n'acquitte que la VBL, `$1DE000`). Une IRQ du C139 relancerait donc la boucle de jeu, et l'IRQ SCI resterait sans
  acquittement, d'où une boucle d'interruptions. **Le C139 ne doit jamais lever d'IRQ ici.** En jeu, MODE vaut 0 (valeur de reset),
  car le jeu n'écrit dans la puce que dans le test.
- Test « SCI » du SWITCH TEST, maître : `$2016` (entrée), `$2064` (à chaque image), résultat dans `$600430` (1 = OK).
  Même code que winrungp `$29DE` / `$2A2C` (voir § 2.8).

## 2. Winning Run Suzuka GP, Winning Run '91, Cyber Sled : anneau en mode 9

### 2.1 Adresses

| | winrungp (esclave) | winrun91 (esclave) | cybsled (esclave) |
|---|---|---|---|
| Niveaux C148 de l'esclave | CPU 1, EX 1, POS 1, **SCI 4**, VBL 1 (`$1038-$1058`) | idem | SCI 4, VBL 6 (`$1058/$1060`) ; la VBL passe parfois au niveau 5 (`$1BB4`) |
| Init C139 (table) | `$142E` (table `$140E`), appelé en `$1074` | `$13E2` (table `$13C2`), appelé en `$1074` | `$3692` (table `$3672`), `jsr` en `$10C8/$1166` |
| Handler VBL | L1 `$110A` → `$1158`, `$149C`, `$17BC` | L1 `$110A` → `$1156`, `$1450`, `$1770` | L6 `$1682` (ou L5 `$1CFA`) → `$3188`, `$3700`, `$3A20` |
| Mise en file (MODE &= 0xB) | `$149C` (`$154A`) | `$1450` (`$14FE`) | `$3700` (`$37AE`) |
| Handler SCI | `$1574` | `$1528` | `$37D8` |
| Type de trame (mot 1) | **0x21** | **0x99** | **0x99** |
| Longueur de trame | **0x2A** mots | **0x24** mots | **0x5E** mots |
| TXSIZE écrit | 0x3F | 0x3F | 0x5E |
| N° de borne lu dans | `$900220` (maître : NVRAM `$180012`, `$105E`) | `$900228` (maître : `$180012 & 7`, `$105E`) | `$900228` (maître : `$180024 & 1`, `$1186`/`$1F40`) |
| Nombre de machines → maître | `$90020A` | `$900208` | `$900208` |
| Compteur « LINK vivant » | `$600B10` (mis à 0 au retour de sa trame) | `$600734` | `$90020A` |
| Variables de travail | `$108004-$10805C` | `$10C004-$10C05C` | `$10D14E-$10D1A6` |

Le code est le même ; seuls changent les constantes du tableau et l'emplacement des variables. On l'a vérifié par un diff
normalisé de winrungp `$149C-$17BC` contre winrun91 `$1450-$1770` : il diffère seulement par 0x21/0x99, 0x28/0x22, 0x2A/0x24 et 0x25/0x1F.
Cyber Sled reprend le même code avec 0x99, 0x5C et 0x59.
winrungp et winrun91 refusent donc mutuellement leurs trames. winrun91 et cybsled partagent le type 0x99, mais leurs longueurs diffèrent.
Dans la suite, les adresses sont celles de **winrungp**.

### 2.2 La RAM du C139 sert aussi de boîte aux lettres

Comme tous les CPU voient la RAM du C139, l'esclave y range des variables (valeurs ≤ 0xFF par demi-mot) :
- mot TX 0x10 (`$B00020`) : nombre d'**autres** machines dans l'anneau (relu par l'esclave en `$11DA` → `$90020A`) ;
- mots TX 0x11/0x12 (`$B00022/24`) : masque des n° reçus pendant la dernière fenêtre de 16 VBL (octet haut / octet bas) ;
- mots TX 0x20/0x21 (`$B00040/42`) : masque des n° émis par cette machine (octet haut / octet bas) ;
- mots TX **0x800 + id×0x80** (`$B01000 + id×0x100`), id = 0..15 : un emplacement de trame par n°.
  Pour son propre n°, c'est la trame à émettre. Pour un autre n°, c'est la dernière charge reçue, et le mot 0 sert de compteur de réceptions.

La RAM doit donc se comporter comme de la RAM ordinaire (au moins 9 bits relus tels quels), dans les deux zones.
Le jeu écrit aussi dans la zone RX (§ 2.5).

### 2.3 Initialisation (esclave `$142E`)

```
C148: SCI level = 4 (slave only; master $10AE and GPU write 0)
C139 regs <- table $140E, block 1:          // STATUS MODE CONTROL START RXSIZE TXSIZE RXOFFSET TXOFFSET
    0x0000 0x000F 0x0000 0x0003 0x0000 0x0000 0x1000 0x1000      // mode 0x0F, START = 3 ("config" value 3)
C139 RAM[0..0x1FFF] = 0                      // whole TX + RX RAM
C139 regs <- block 2:
    0x0000 0x0009 0x0000 0x0000 0x0000 0x0000 0x1000 0x1000      // MODE = 9, START = 0, RXOFFSET = TXOFFSET = 0x1000
txsrc[0..15] = 0xFFFF ; ownmask_copy = 0xFFFF ; pending = 0
SCI ack ($1DC000)                            // clears any IRQ raised during init
```
winrun91 (`$13C2`) et cybsled (`$3672`) ont exactement la même table.
RXOFFSET = 0x1000 vaut le pointeur RX 0 (le jeu masque, § 2.5). Le jeu n'écrit plus jamais RXOFFSET ensuite.

### 2.4 Émission

**Mise en file à chaque VBL** (`$149C`) :
```
if ((++vblcnt & 0xF) == 0) {                 // presence window = 16 VBL
    TX[0x12] = rxmask & 0xFF ; TX[0x11] = rxmask >> 8 ; rxmask = 0
}
own = TX[0x20] << 8 | TX[0x21]               // written by $1158: (1 << CPU_NO) | $90020E (drone ids)
for id in 0..15 where own & (1 << id):
    txsrc[id] = 0x1000 + id*0x100            // byte offset -> TXOFFSET 0x800 + id*0x80 (TX half)
    slot = &RAM[0x800 + id*0x80]             // payload words 3.. filled by $1158 (34 words)
    slot[0] = id ; slot[1] = 0x21 ; slot[2] = 1          // header: sender id, frame type, hop count = 1
    sum = 0
    for k in 0..0x28: slot[k] &= 0xFF ; sum += slot[k]    // clears bit 8 everywhere
    slot[0x29] = (sum & 0xFF) | 0x100                     // checksum + END-OF-FRAME (bit 8)
    pending |= 1 << id
if (pending) MODE &= 0x0B                    // clear MODE bit 2 = re-arm the TX-ready interrupt
```

**Handler SCI** (`$1574`), côté TX :
```
st = STATUS
if (st & 2) goto rx                          // receive has priority (§ 2.5)
if (!(st & 4)) { SCI ack ; MODE &= 0x0D ; rte }          // spurious
for 16 tries, slot = rr (round robin $108030):
    if (pending & (1 << slot)) {
        pending &= ~(1 << slot)
        TXOFFSET = txsrc[slot] >> 1          // WORD address, 13 bits: 0x800+id*0x80 (own) or 0x1000+k (relay from RX ring)
        START   = 1
        TXSIZE  = 0x3F                       // > frame length 0x2A : the frame ends on the bit-8 word
        if (pending) { rr = slot+1 ; START = 0 ; SCI ack ; MODE &= 0x09 ; rte }   // more to send: re-arm TX irq
        START = 0 ; rr = slot+1 ; SCI ack ; MODE &= 0x0D ; rte                  // last one: leave TX irq disarmed
    }
    rr++
SCI ack ; MODE &= 0x0D ; rte                 // nothing to send
```
- La branche TX n'écrit **jamais** STATUS. Seule la branche RX écrit 0.
- `&= 0x0D` (dernière trame ou rien à émettre) et `&= 0x09` (il en reste) ne diffèrent que par le bit 2.
  La mise en file VBL fait `&= 0x0B`, qui ne touche que le bit 2. Si MODE se relisait toujours 9, ces trois écritures seraient identiques.
  On aurait alors soit une IRQ d'émetteur libre sans fin, soit une émission qui ne repart jamais. **Il faut donc que la puce
  mette le bit 2 de MODE à 1 quand elle lève l'IRQ TX, et qu'elle la relève quand le jeu le remet à 0 alors que l'émetteur est libre.**
  Le bit 1 joue le même rôle pour RX. Ces conclusions viennent du code. Le comportement électrique exact reste une **hypothèse**.
- La seule façon de démarrer une émission est l'IRQ TX (STATUS bit 2). Aucun autre code n'écrit TXOFFSET, START ou TXSIZE.
  Après une période sans rien à émettre, c'est le `MODE &= 0x0B` de la VBL qui doit déclencher l'IRQ.
- Fin de trame : TXSIZE (0x3F) dépasse la trame (0x2A). Le jeu force à 0 le bit 8 de tous les mots, sauf le dernier.
  La trame s'arrête donc au premier mot dont le bit 8 vaut 1, ce mot compris (c'est le « sync bit »).
  Cyber Sled écrit TXSIZE = 0x5E, soit exactement la longueur. Les deux lectures concordent alors.

### 2.5 Réception, validation, relais (handler `$1574`, branche `$1630`)

```
rx:
p = (RXOFFSET * 2) & 0x1FFF                  // byte offset inside the RX half ($B02000 | p)
for (n = 0; n < 0x2B; n++, p = (p - 2) & 0x1FFF)        // scan back at most LEN+1 words
    if (RXRAM[p] & 0x100) goto found          // last word of the latest complete frame
goto done                                     // nothing found: ignore
found:
start = (p - 0x29*2) & 0x1FFF                 // frame = 0x2A words ending at p (wraps in the RX half)
id  = RX[start+0] & 0xFF ;  sum  = id
typ = RX[start+1] ;         sum += typ ; if (typ.b != 0x21) goto reject
hop = RX[start+2].b + 1 ;   RX[start+2] = hop ; sum += hop      // hop count incremented IN PLACE
if (hop >= 0x10) goto reject                  // circulated too long
if (ownmask & (1 << id)) {                    // our own frame is back: the ring is closed
    $600B10 = 0                               // "link alive" counter
    TX[0x10] = (hop >= 2) ? hop - 2 : 0       // number of OTHER machines (hop = N+1 for N machines)
    goto reject                               // the originator removes its frame from the ring
}
tmp[0..0x25] = RX[start+3 .. start+0x28] ; sum += each
cs = RX[start+0x29] + 1 ; RX[start+0x29] = cs | 0x100           // checksum +1 to match hop+1
if (sum.b != cs.b) goto reject
RAM[0x800 + id*0x80 + 3 ..] = tmp ; RAM[0x800 + id*0x80] += 1   // store payload, bump reception counter
txsrc[id] = 0x2000 | start                    // relay source = the frame in the RX ring (TXOFFSET bit 12 = 1)
pending |= 1 << id ; rxmask |= 1 << id        // the game will re-send it (relay) on the next TX interrupt
done:
STATUS = 0 ; SCI ack ; MODE &= 0x09 ; rte     // clears bits 1 and 2: re-arm RX and TX interrupts
reject:
rxmask &= ~(1 << id) ; pending &= ~(1 << id) ; goto done
```
Ce que cela impose à la puce :
- **RXOFFSET est le pointeur d'écriture RX**, en mots ; le jeu n'en prend que les 12 bits bas. La recherche couvre exactement
  LEN+1 positions à partir de RXOFFSET : la position RXOFFSET elle-même, puis LEN mots en arrière. Elle trouve la fin de la dernière trame complète même si une trame est en cours de réception.
  Cela colle avec « RXOFFSET = prochaine case libre » ; la lecture « dernier mot écrit » marcherait aussi.
- Les trames doivent arriver **contiguës et sans mot ajouté**, en gardant le **bit 8** du dernier mot.
  Le jeu localise le début de la trame à `fin − (LEN−1)`.
- Une interruption ne traite **qu'une trame**, la plus récente. Si deux trames se terminent avant que le handler tourne, la plus ancienne est perdue.
- Le **relais est fait par le jeu**. L'émission suivante part de `txsrc[id] >> 1 = 0x1000 + k`, une adresse de la zone RX,
  et prend la trame telle que le jeu l'a modifiée en place (hop + 1, somme + 1). Le C139 doit lire la RAM au moment où il émet.
  Une trame RX peut chevaucher la fin de la zone (0x1FFF → 0x1000), puisque 0x1000 n'est pas multiple de 0x2A.
  Le pointeur d'émission doit donc reboucler à l'intérieur de sa moitié, bit 12 conservé (**hypothèse** nécessaire).
- L'émetteur retire ses propres trames (masque `TX[0x20..0x21]`). Le compteur hop ≥ 16 sert de garde-fou.

### 2.6 Comptage des machines, affichage « LINK OK n »

- Une trame qui a fait le tour de N machines revient avec hop = 1 (départ) + (N−1) relais + 1 (incrément du handler) = N + 1.
  Donc `TX[0x10] = N − 1`, recopié dans `$90020A` (`$11E2`).
- GAME OPTIONS 1 de winrungp (chaîne GPU `$6188`, menu maître `$3332`/`$3372`/`$3480`, affichage GPU `$61F2`, table de pointeurs `$62FC`) :
  ligne LINK (y = 0x14) :
  - « OK » si `$200B10` (= `$600B10`) < 8, sinon « NG » (`$6BEE`). L'esclave incrémente ce compteur une fois par image
    (boucle principale `$1088`, plafonné à 8) et le remet à 0 quand sa trame revient.
    Donc OK = sa propre trame est revenue dans les 8 dernières images (≈ 133 ms).
  - puis le nombre `$20014A + 1` = `$90020A + 1` = **N, nombre de machines dans l'anneau, elle comprise** (`$6A1E`, décalage +1).
  Une borne seule, anneau ouvert, affiche « NG 1 ». Une borne bouclée sur elle-même afficherait « OK 1 ».
  LINK n'est pas un réglage : c'est un affichage d'état.
- winrun91 : même chose (pointeurs GPU `$1D90`) ; OK/NG d'après `$200734` (`$1D38` → `$2CD6`), nombre `$2007A0` + 1 (`$1D62`).
- Cyber Sled : GAME OPTIONS de l'esclave, ligne LINK (`$1E166`) : « OK » si `$10D14E` ≠ 0, c'est-à-dire si l'esclave a reçu
  au moins un n° étranger dans la fenêtre de 16 VBL précédente ; sinon « ERROR ». Le nombre N−1 passe par `$900208`.
  Le maître teste `$900208 == 1` en `$49D2`, ce qui veut dire « lié à l'autre borne ».
- Présence : `TX[0x11..0x12]` (n° entendus en 16 VBL) est relu à chaque VBL (`$1158` → `$108004`) pour savoir quelles voitures sont présentes.

### 2.7 Rythme

- Tout passe par l'esclave à la VBL (60 Hz). Chaque machine met en file sa ou ses trames (son n°, plus les n° 8-15 des voitures
  pilotées par l'ordinateur, voir § 2.9), puis émet une trame par IRQ « émetteur libre ».
  Les relais sont mis en file dès la réception et partent à l'IRQ TX suivante, à la suite.
  Dans un anneau de N machines, chacune émet donc environ N trames par VBL (N×0x2A mots pour winrungp, N×0x5E pour cybsled).
- Les trames partent dos à dos. Le récepteur doit pouvoir passer son handler (≈ 0,1-0,2 ms à 12,288 MHz) entre deux fins de trame (§ 2.5).
- Délais côté jeu : retour de sa propre trame dans les 8 images ; fenêtre de présence de 16 VBL. Aucun délai d'attente au niveau de la puce.
- Cyber Sled : VBL au niveau 6, au-dessus du SCI (4). Le handler VBL peut interrompre le handler SCI, jamais l'inverse.

### 2.8 Test « SCI » du SWITCH TEST (maître, winrungp `$29DE`/`$2A2C`, winrun `$2016`/`$2064`)

Table des pages de test : `$295C` (entrées) et `$299C` (à chaque image), page 3. Le résultat va dans `$600A70`, affiché par le GPU (chaîne `$64C5`, « SCI »).
```
enter:  START = 1 ; RXSIZE = TXSIZE = 0 ; STATUS = 0 ; MODE = 0x0F ; CONTROL = 0 ; START = 0   // START=0 in mode 0x0F
        RXSIZE = TXSIZE = 0 ; RXOFFSET = TXOFFSET = 0 ; TX[k] = k*0x25 (k = 0..15)
frame:  STATUS = 0 ; ok = 0 ; n = RXSIZE ; RXSIZE = 0 ; RXOFFSET = 0
        if (((-n) & 0xFF) == 0x10) ok = 1    // 16 words received since the previous frame: RXSIZE COUNTS DOWN
        TXOFFSET = 0 ; TXSIZE = 0x10          // send 16 words (no bit 8, START = 0): the TXSIZE write starts it
```
- En mode 0x0F, la puce émet donc TXSIZE mots dès l'écriture de TXSIZE, START valant 0.
  RXSIZE perd 1 par mot reçu. Le test vaut OK si 16 mots sont arrivés depuis l'image précédente.
- Aucune IRQ (SCI niveau 0 sur le maître ; en mode 0x0F les bits 1 et 2 de MODE valent 1).
  Le test passe si la machine en amont émet aussi 16 mots par image, ou avec une boucle TX → RX.
  Une borne seule en anneau ouvert affiche NG.

### 2.9 Réglages, n° de borne, nombre maximal de bornes

- **winrungp** (maître `$105E`, page GAME OPTIONS 1 `$3332`, sauvegarde `$3480`). Octets NVRAM :
  [0] = 0x4D, [1] = 0x01, [2] COIN1 COIN, [3] COIN1 CREDIT, [4] COIN2 COIN, [5] COIN2 CREDIT, [6] LAP, [7] FREE PLAY,
  [8] **CAR TYPE** (0-3 : A RED, B WHITE/RED, C YELLOW/BLUE, D BLUE ; la couleur part dans la trame, `$1530`), [9] **CPU NO.** (0-7, affiché 1-8),
  [0xA] MOTION, [0xB] réglage de GAME OPTIONS 2, [0xC] = somme des octets 0..0xB modulo 256 (vérifiée en `$2B26`).
  Le maître copie CPU NO. dans `$900220` ; l'esclave le prend `& 0xF`.
- **winrun91** : même disposition. [0] = 0x63, [1] = 0x00 (`$1DE4`), CPU NO. = [9] (`$105E`, `& 7` → `$900228`), somme en [0xC].
- **cybsled** : [0] = 0x7D, [1] = 0x03, **POSITION** = bit 0 de l'octet [0x12] (`$180024`, lu en `$1186` et `$1F40` ; menu esclave `$1E138` :
  « 1 LEFT » / « 2 RIGHT », via `$901B50`), somme des octets 0..0x15 en [0x16] (`$21AA`).
- **Nombre maximal de bornes** :
  - winrungp et winrun91 : **8** (CPU NO. sur 3 bits). Le protocole a 16 n° : les n° 0-7 sont les bornes, les n° 8-15 les voitures pilotées par l'ordinateur.
    Seule la borne de plus petit n° actif les simule (`$B880-$BA06` du maître, masques `$B0DC` = 0x0100…0xFF00 dans `$90020E`).
    Leur nombre dépend de N (`$B0EC` : 4, 4, 4, 3, 3, 3, 2, 2 pour N = 1..8). Le garde-fou hop < 16 permet au plus 14 machines.
  - Cyber Sled : **2** (LEFT = n° 0, RIGHT = n° 1). La borne RIGHT émet aussi le n° 8 (table du maître `$18E8`, `$268C`).
  - Les n° de borne doivent être **différents**. Avec deux fois le même n°, chaque borne prend la trame de l'autre pour la sienne et la jette.

## 3. Driver's Eyes : lien interne entre les trois cartes

Les trois cartes ont des ROM identiques ; l'écran vient des DSW 3-4, lus par le MCU en `$A00026`, valeur inversée.
`$A00026 & 0xC` vaut 0 pour le **centre**, 4 pour la gauche, 8 ou 0xC pour la droite. Le résultat est rangé en `$900104` (0 = centre), en `$3CE8` de l'esclave.
Le link tourne sur l'**esclave** de chaque carte : niveau SCI 4 (`$1050`), handler `$3DF0`, et IRQ CPU niveau 5 (`$3F0A`) envoyée par le maître.

**Init** (`$3C92`, table `$3C72`) :
```
block 1: STATUS 0, MODE 0x0F, CONTROL 0, START 1, RXSIZE 4, TXSIZE 4, RXOFFSET 0, TXOFFSET 0      // "config" START = 1
RAM[0..0x1FFF] = 0
block 2: STATUS 0, MODE 0x0D, CONTROL 0, START 1, RXSIZE 0, TXSIZE 0, RXOFFSET 0, TXOFFSET 0x1000
START = 1 (again) ; txoff = 0 ; srcoff = 0 ; chunks = 0 ; SCI ack
```
Les trois cartes programment **0x0D**. Ensuite :
- le centre fait `MODE &= 9` à chaque émission, donc MODE vaut **0x09** après la première ;
- les côtés n'écrivent plus que `MODE &= 0xD`, donc restent en **0x0D**.

C'est l'origine de la note MAME « 09 centre / 0D côtés ». START reste à 1 en permanence sur les côtés.

**Préparation** (IRQ CPU niveau 5, `$3F0A`, une fois par demande du maître) :
4 morceaux de 0x80 octets en `$102000/$102080/$102100/$102180`.
- En-tête : numéro de morceau (1..4), n° de séquence, 2 octets libres, total = 4 (morceau 1 seulement).
- Données : `$900200`, `$900240`, `$908040…`, `$908240…`.

Ensuite `chunks = 4` et `srcoff = 0`.

**Émission (centre seulement)** `$3D2A`. Appelée par la boucle principale (`$10EE`, `$19EE`, `$1A5E`, à chaque tour) et par le handler SCI :
```
if (chunks == 0) return
if (TXSIZE.lowbyte != 0) return              // transmitter still busy  <- TXSIZE must read 0 when done
START = 1
a = $B00000 + txoff ; TXOFFSET = txoff >> 1 ; txoff = (txoff + 0x100) & 0x1FFF   // TX half used as 32 x 0x80-word ring
CONTROL = 1
copy 0x80 source bytes, one per halfword (movep, low byte; high byte stays 0) ; cs16 = sum of 16-bit words
words 0x7C/0x7D = cs16 high/low bytes ; CONTROL = 0 ; word 0x7F = 0xFFFF      // END marker (bit 8)
chunks-- ; TXSIZE = 0x80 ; MODE &= 9 (clear bits 1,2: re-arm) ; START = 0 (go) ; $900120 = 1
```
**Handler SCI** `$3DF0` :
```
st = STATUS ; STATUS = 0 ; SCI ack
if (side) { parse(st) ; MODE &= 0x0D }       // re-arm RX only (TX stays disarmed: sides never transmit)
else      { busy-wait 0x200 dbra (~0.42 ms) ; tx_chunk() }       // centre: next chunk after a TX interrupt
```
**Réception (côtés)** `$3E3A` :
```
if (!(st & 2)) return
p = (RXOFFSET & 0xFFF)*2 + 2                 // starts at word RXOFFSET+1, scans back with no limit
while (!(RX[p] & 0x100)) p -= 2               // last word of the latest frame (0x1FF)
copy the 0x80 words of the frame backwards (low bytes) to $103780-$1037FF
err = 0
if (!(word before the frame & 0x100)) err |= 1      // frames must be contiguous (previous end marker)
if (cs8 != (1 + sum of bytes 3,7,...,0x7F)) err |= 2 // = low byte of cs16 at word 0x7D
if (seq != last_seq) { if (chunk != 1) err |= 4 ; last_seq = seq ; total = header[6..7] }
if (chunk > total) err |= 8
header[4..5] = err ; $900108 = err ; copy 0x80 bytes to $901000 + (chunk-1)*0x80
```
Le maître d'une carte de côté tire son état de ces données ; son code de côté commence en `$13EA`.
Dans l'un des états que le maître lui donne, l'esclave affiche « RECEIVE ERROR » (routine `$1382`, chaîne `$13AA` de l'esclave).
C'est ce qu'on voit quand rien n'arrive du centre.

Ce que la puce doit faire pour Driver's Eyes :
1. **Centre, émission** de trames de TXSIZE = 0x80 mots à partir de TXOFFSET (ring de 32 tampons dans la zone TX).
   L'émission démarre quand START passe de 1 à 0, TXSIZE ≠ 0. **TXSIZE doit se relire différent de 0 pendant l'émission et revenir à 0 à la fin**.
   Sinon le centre reste bloqué après le premier morceau, car la boucle principale interroge `tst.b $B8000B`.
   L'IRQ « émetteur libre » à la fin (MODE bit 2 remis à 0 juste avant) accélère les morceaux 2 à 4. Elle n'est pas indispensable.
2. **Côtés, réception** : écrire la trame à la suite dans le ring RX (RXOFFSET avance de 0x80 par trame), bit 8 du dernier mot conservé,
   puis IRQ RX (STATUS bit 1, MODE bit 1 remis à 0 par le handler), **une IRQ par trame**.
   Il faut laisser le temps du handler entre deux trames. Sur le vrai matériel, le centre attend environ 0,42 ms après chaque IRQ TX.
3. **Les deux côtés doivent recevoir chaque trame du centre.** Aucune carte ne relaie par logiciel.
   Les côtés n'émettent jamais : le seul appel à `$3D2A` qui ne dépend pas du rôle (`$19EE`) a lieu au démarrage, sans aucun morceau en file.
   Le centre ignore ce qu'il reçoit (il n'analyse pas). L'anneau actuel de MAME (`sci_de_hack` : gauche → centre → droite → gauche)
   ne fonctionne que parce que l'émulation actuelle relaie dans la puce.
   - **Hypothèse A** : dans la borne, la sortie TX du centre attaque les entrées RX des deux côtés (diffusion).
   - **Hypothèse B** : un C139 dont START bit 0 = 1 (émetteur bloqué) répète ce qu'il reçoit. Les côtés gardent START = 1 en permanence ;
     winrungp et RR2 ne gardent START = 1 que quelques instructions.
   Aucune preuve dans le code. Le plus simple et le plus sûr dans MAME : relier directement, à l'intérieur de la machine,
   l'émetteur du centre aux récepteurs des deux côtés, sans TCP.
4. Le centre a MODE = 9 après sa première émission, donc l'IRQ RX est armée. S'il reçoit quelque chose (renvoi par l'anneau),
   son handler attend 0,42 ms puis appelle `$3D2A`. C'est sans danger, mais inutile.

Nombre de cartes : 3, fixe. Il n'y a pas de link entre bornes.

## 4. Air Combat (aircomb, AC2 US) : code de link mort

- Maître : C148 SCI au niveau 4 (`$119A`), handler `$125A`.
  Il recopie les 8 registres en `$1001BE`, puis fait STATUS = 0, MODE = 0x0D, RXSIZE = 0, RXOFFSET = 0, `$100108 = 1`, ack.
- Init : `$19BE` (appelé en `$114A`) écrit les 8 registres en mode 0x0F, tout à 0 (START = 0). Puis MODE = 0x0D en `$11B8` (`$145A` dans l'autre chemin).
- `$19E6` (émission d'une trame de 0x1F mots : CONTROL 1, 0x1E mots, CONTROL 3, un dernier mot, TXOFFSET 0, TXSIZE 0x1F) et `$1AB8`/`$1AE4`
  (réception d'après RXOFFSET & 0xFF, somme des mots) ne sont **appelées nulle part**. On a cherché les `bsr`/`jsr`, les adresses absolues,
  les références relatives au PC et les mots bruts dans la ROM programme ; la ROM de données n'a pas été vérifiée.
  Le drapeau `$100108`, mis à 1 par le handler, n'est lu que dans ce code mort. L'état du link `$100006` n'est écrit que là, et jamais lu.
  Le rôle (`$7E32(a6) & 3`, une entrée) est calculé, mais personne ne s'en sert.
- Esclave : niveau SCI 0 (`$69BE`). Mêmes routines mortes (`$11F0`, `$1218` : trame de 4 mots = 2 données, somme, CONTROL 3, 0xFF ; `$126E`), handler `$6A7C`.
- Le « LINKNO. » du menu de l'esclave (`$DEF9`) appartient à l'OBJECT TEST : c'est le lien des sprites, pas le link entre bornes.
- La puce n'a qu'à rester silencieuse. Si une trame arrive, le handler rembobine RXOFFSET et ne fait rien d'autre.

## 5. Starblade, Solvalou

- starblad, maître : `$46C8` (appelé en `$4730`) écrit la table `$46DC` :
  STATUS 0, MODE 0x0F, CONTROL 1, START 0, RXSIZE 0xFF, TXSIZE 0, RXOFFSET 0, TXOFFSET 0x1000.
  La routine `$46EC` (test de STATUS bit 1, puis MODE 0, MODE 0x0D, RXOFFSET 0) n'est référencée nulle part.
  Niveau SCI 0 sur les deux CPU (maître `$4778`, esclave `$1FD2E`). Pas de link.
- solvalou : aucun accès aux registres ni à la RAM du C139 ; les rares correspondances sont des données. Niveau SCI 0 (maître `$4B6`, esclave `$246A`).

---

## 6. Ce que le C139 doit faire pour ces jeux

Modèle minimal qui fait tourner tout le code ci-dessus. Les points 1, 3 et 5 sont imposés par le code. Le reste est la lecture la plus simple, marquée **hypothèse** quand elle va au-delà.

**Registres**
1. **MODE.**
   - Les jeux écrivent 0x09, 0x0D ou 0x0F, puis seulement des `&=` (0x09, 0x0B, 0x0D).
   - **Bit 1 = verrou de l'IRQ RX, bit 2 = verrou de l'IRQ TX.** La puce met le bit à 1 en levant l'IRQ correspondante.
     Quand le jeu remet le bit à 0 et que la condition est vraie (émetteur libre, ou trame reçue en attente), la puce relève aussitôt l'IRQ.
     Ce sont des drapeaux internes à la puce, pas des bits de mode : 0x0D = « 9 avec TX verrouillé » (Driver's Eyes le prouve, 0x0D & 9 = 9),
     et 0x0F = « tout verrouillé ».
   - **Hypothèse** : bit 3 = puce active. MODE = 0 au reset, et dans ce cas aucune IRQ.
     C'est obligatoire pour winrun : son IRQ SCI est au niveau de la VBL et jamais acquittée.
   - Pas d'IRQ en mode 0x0F.
2. **STATUS.**
   - Bit 1 = trame reçue, bit 2 = émetteur libre (IRQ TX).
   - L'écriture de 0 efface. winrungp ne l'efface que dans sa branche RX.
3. **TXOFFSET** : adresse de **mot** sur 13 bits ; bit 12 = zone RX (relais). Il ne faut ni le diviser par 2 ni chercher le bit de synchro plus loin.
   Le pointeur d'émission reboucle à l'intérieur de sa moitié de 4 K mots (**hypothèse** nécessaire aux relais qui chevauchent 0x1FFF).
4. **START** bit 0 = émetteur bloqué. Une émission démarre quand TXSIZE ≠ 0 et que START bit 0 vaut 0, c'est-à-dire :
   - à l'écriture de TXSIZE si START = 0 (test SCI, Air Combat) ;
   - ou quand START passe de 1 à 0 (winrungp, cybsled, Driver's Eyes, RR2).

   La valeur écrite en mode 0x0F (3, 1 ou 0) n'a besoin d'aucun effet durable pour les jeux S21.
   Les comportements que MAME rattache à `m_reg_f3 & 2` sont faux ici : TXOFFSET en octets, remplacement du compteur de liens.
5. **TXSIZE** : nombre maximal de mots. La trame s'arrête **après le premier mot dont le bit 8 vaut 1, ou après TXSIZE mots**, au premier des deux.
   Cette règle couvre winrungp (bit 8 au mot 0x29, TXSIZE 0x3F), cybsled (0x5D / 0x5E), Driver's Eyes (0x7F / 0x80),
   le test SCI (pas de bit 8, 16 mots) et RR2 (0x24).
   TXSIZE se relit ≠ 0 tant que l'émission est en attente ou en cours, puis **revient à 0 à la fin** (Driver's Eyes, RR2).
   À ce moment, l'IRQ TX part si le bit 2 de MODE vaut 0.
6. **RXOFFSET** : pointeur d'écriture RX sur 12 bits. Chaque mot reçu va en `RAM[0x1000 + (RXOFFSET & 0xFFF)]`, puis RXOFFSET augmente de 1.
   Il reboucle dans les 4 K mots. La valeur initiale est celle écrite par le jeu (0x1000 ou 0).
7. **RXSIZE** perd 1 par mot reçu, sur 12 bits (test SCI). Aucun jeu S21 ne le lit en mode de jeu.
8. **CONTROL** : sans effet nécessaire en S21. Driver's Eyes met 1 pendant le remplissage, puis 0.
9. **RAM** : RAM ordinaire de 9 bits pour l'hôte, dans les deux zones (boîte aux lettres TX, correctifs dans la zone RX).
   Les mots relus doivent avoir des bits 9-15 propres, car le jeu teste `& 0x100`.

**Ligne et trames**
10. On émet des mots de 9 bits : 8 bits de données, plus le bit 8 sur le mot de fin.
    On reçoit les mêmes mots, écrits **à la suite** sans en-tête ni bourrage, bit 8 conservé.
    Fin de trame = mot reçu avec le bit 8 : STATUS |= 2, puis IRQ RX si le bit 1 de MODE vaut 0, sinon en attente jusqu'au réarmement.
11. **Pas de relais dans la puce**, pas de numéro d'anneau ni de compteur de sauts ajouté à la trame.
    winrungp, winrun91 et cybsled relaient eux-mêmes (TXOFFSET en zone RX), comptent les sauts (mot 2) et retirent leurs propres trames.
    L'émulation actuelle relaie et réécrit le mot 2 (`m_buffer[7] = m_buffer[0x1ff]`). Cela fonctionne par hasard, au prix de beaucoup de trames en double.
12. **Rythme** :
    - Livrer au plus **une trame par IRQ RX**. Garder les suivantes dans une file jusqu'à ce que le jeu remette le bit 1 de MODE à 0,
      ou au moins jusqu'à ce qu'il efface STATUS. Les handlers ne traitent que la trame la plus récente.
      Le vrai matériel en perdrait ; cette file est plus sûre et sans effet de bord.
    - Annoncer « émetteur libre » après un délai proportionnel à la longueur, par exemple 5-10 µs par mot (**hypothèse** sur le débit).
      On peut aussi le faire au tick suivant.
    - Il faut tenir environ N trames par VBL et par machine (8 × 0x2A mots pour winrungp à 8 bornes).
13. **Transport** : anneau simple, l'émetteur de chaque instance relié au récepteur de la suivante.
    Le jeu se charge de l'identité : CPU NO. ou POSITION différents dans la NVRAM.
14. **Driver's Eyes** : diffuser chaque trame du centre aux **deux** côtés, par exemple en reliant les trois C139 dans le pilote, sans socket.
    À défaut, garder l'anneau et faire répéter les trames par les cartes qui ne transmettent jamais (START bit 0 = 1) (**hypothèse B**).
    Il faut aussi que TXSIZE revienne à 0 en fin d'émission.
15. **winrun, Air Combat, Starblade, Solvalou** : aucune IRQ hors mode actif. Pour winrun, la seule demande est le test SCI en mode 0x0F
    (émission de 16 mots à l'écriture de TXSIZE, RXSIZE qui décompte, sans IRQ).
