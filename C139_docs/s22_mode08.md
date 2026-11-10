# System 22 — C139 en mode 0x08 (ridgera2, raverace) et 0x0C (ridgerac, ridgeracf)

Analyse statique (désassemblage capstone des régions `:maincpu` vidées par MAME, remises en big-endian).
Aucune exécution d'émulateur. Ce qui est lu dans le code est donné comme fait, avec l'adresse ;
ce qui est déduit est marqué **hypothèse**.

Conventions :
- Variables de travail relatives à A6 (A6 = 0x10008000) : `$181A(A6)` = 0x1000981A.
  Le CPU est big-endian : l'octet `$1104` est le poids **fort** du mot `$1104` (bits 8-15), l'octet `$1105` le poids faible.
- Registres C139 (base 0x20020000) : +0 STATUS, +2 MODE, +4 CONTROL, +6 START, +8 RXSIZE, +A TXSIZE, +C RXOFFSET, +E TXOFFSET.
  `TXSIZE.b` = octet 0x2002000B = poids faible de TXSIZE.
- RAM C139 : 0x20010000 (mot 0x0000) … 0x20013FFF. Zone TX = mots 0x000-0xFFF, zone RX = mots 0x1000-0x1FFF (0x20012000).
- Interruptions (registres système 0x40000000) :
  ridgera2 et raverace programment SCI = 0x36 (niveau 6, vecteur 30) et VBLANK = 0x35 (niveau 5, vecteur 29) ;
  ridgerac et ridgeracf programment SCI = 0x35 (niveau 5) et VBLANK = 0x34 (niveau 4). Acquittement SCI : écriture en 0x40000007.
- Outils (scratchpad `c139re\work\`) : `fulldis.py` (désassemblage complet → `<jeu>.lst`, annotations A6), `da.py`, `hx.py`, `xr.py` (références croisées),
  `strs.py`, `sim.py` (simulation à deux bornes du code de liaison, § 4.6).
  Attention : capstone n'affiche pas le facteur d'échelle de l'adressage `(d8,PC,Dn*2)` ; les tables indexées (0x1BFDC, 0x26D1A, 0x27670) le sont bien ×2.

---

## 1. Ce qui est commun à ridgera2 et raverace

Les deux jeux ont le même pilote de liaison, aux tailles de trame et aux délais près.

| | ridgera2 (RRS2) | raverace (RV2 Ver.B) |
|---|---|---|
| Init C139 | 0x1BDA8 (appelée au boot 0xCEA8) | 0x26B20 (boot 0x4370) |
| Handler IRQ SCI | 0x1BE4C | 0x26BBE |
| Ordonnanceur d'émission | 0x1BF38 → 0x1BF72 | 0x26C84 (sélection + écriture registres) |
| « Kick » si TXSIZE = 0 | 0x1BD9A (appelé seulement par l'I/O test : 0x7568, 0x2285C) | 0x26B12 (seulement par l'I/O test : 0x610A) |
| Envoi de démarrage | `jsr 0x1BF38` au boot (0xCDAE) | **aucun** |
| Analyse RX (dans l'IRQ) | 0x1BFEC | 0x26D2A |
| Construction trame propre | 0x1C588 | 0x26F86 |
| Trames supplémentaires (concurrents IA) | poste 0x1C734, écrit 0x1C76C | poste 0x274CA, écrit 0x275BE |
| Comptage / LINKCT (VBLANK) | 0x1C144 | 0x26EAE (le comptage proprement dit en 0x26F44) |
| Consommateur de l'anneau (VBLANK) | 0x1C60A | 0x27126 |
| Handler VBLANK | 0xD116 | 0x44FE |
| Longueur (TXSIZE) | 0x24 demi-mots (variable -$7756 = 0x22, +2) | 0x28 (variable $1038 = 0x26, +2) |
| Octets utiles | 0x22 | 0x26 |
| Délai « trame propre revenue » | 16 VBLANK ($1860) | 8 VBLANK ($1108) |
| Délai « compteur de trame figé » | 16 VBLANK ($181E[]) | 8 VBLANK ($110C[]) |

### 1.1 Format d'une trame (dans la RAM C139, un octet par demi-mot)

```
mot 0 .. L-3 : octets utiles (L-2 octets), octet de poids fort d'abord pour chaque mot 16 bits du paquet
mot L-2      : longueur L (0x24 ridgera2, 0x28 raverace)          <- écrit avant CONTROL=3
mot L-1      : somme 16 bits des octets utiles (pas la longueur)  <- écrit APRÈS CONTROL=3
réception    : le dernier demi-mot porte le bit 8 (fin de trame), mis par la puce
```
Paquet (en RAM de travail, recopié octet par octet) :
- ridgera2 (0x1C588) : 6 mots de `$189C + n*16` (enregistrement « lien » du CPU n), 10 mots de `$1400 + n*64` (état voiture), 0xFFFF → 0x22 octets.
- raverace (0x26F86) : 6 mots de `$11AC + n*16`, 12 mots de `$12AC + n*64`, 0xFFFF → 0x26 octets.

Enregistrement lien (6 mots) : mot 0 = numéro de CPU (l'octet 1 de la trame porte donc l'identifiant, lu avec `& 0xF`),
mot 1 = GRP, mot 2 = drapeaux/étape (raverace `$11A4/$11A5`), mot 3, octet 8 = drapeaux d'état (`$1808` / `$11A8`),
octet 9 = **compteur de trames** (`$1809 = $1FBF` / `$11A9 = $C4D`, incrémenté à chaque VBLANK), mot 5 (`$180A` / `$11AA`).

Vérification à la réception : `(somme - Σ mots 0..L-2 + L) & 0xFF == 0`, ce qui contrôle à la fois les données et la longueur.

### 1.2 Écritures de registres

Init (ridgera2 0x1BDA8 avec la table 0x1BD7A ; raverace 0x26B20 avec la table 0x26AF2, identiques) :
```
byte 0x40000019 = 1
regs[0..7] = 0000 000F 0000 0011 0004 0004 0000 0000   ; STATUS, MODE=0x0F (config), CONTROL, START=0x11, RXSIZE=4, TXSIZE=4, RXOFF=0, TXOFF=0
RAM C139[0..0x1FFF] = 0
regs[0..7] = 0000 000C 0000 0011 0000 0000 0000 1000   ; MODE=0x0C, START=0x11 (écrit APRÈS MODE), TXOFFSET=0x1000
(effacement d'une zone de travail : 0x10005800 / 0x10002000, 0x400 longs)
START = 0
compteurs d'erreur = 0 ; anneau RX ($19BE / $105E, 0x50 mots) = 0xFFFF ; banque courante = 0 ; longueur = 0x22 / 0x26
```
Au tout début du boot (avant le test RAM) : CONTROL = 0, START = 1 (ridgera2 0x246D8, raverace 0x2FA68).
Les routines d'erreur/redémarrage en 0x400/0x404/0x408 écrivent MODE = 0x0F (arrêt).

Émission (ordonnanceur, les deux jeux) :
```
TXOFFSET = off ; START = 1 ; TXSIZE = L ; MODE &= 8 ; START = 0
```
Construction d'une trame : `CONTROL = 1 ; mots 0..L-2 ; CONTROL = 3 ; mot L-1 (somme)`.
Masques sur MODE, et rien d'autre (jamais de OR, jamais d'écriture directe après l'init) :

| Où | Masque | Bits effacés |
|---|---|---|
| début du VBLANK (0xD11E ; 0x26EAE) | `&= 9` | 1, 2 |
| après avoir posté la trame propre (0x1C16E ; 0x26ECC) ou les trames IA (0x1C746 ; 0x274DC) | `&= 0xB` | 2 |
| ordonnanceur, après TXSIZE (0x1BF60 ; 0x26D08) | `&= 8` | 0, 1, 2 |
| sortie d'IRQ après RX / erreur / émission | `&= 9` | 1, 2 |
| sortie d'IRQ « rien à émettre » | `&= 0xD` | 1 seulement |

---

## 2. Ridge Racer 2 (ridgera2)

### 2.1 Variables

| Var | Rôle |
|---|---|
| `$1800` | CPU NO. (0-7, depuis l'option 0x1000685C & 7, routine 0x1C110) ; `$1810 = n*2`, `$1812 = n*16`, `$1814 = n*64` |
| `$1802` | GRP NO. ; `$1816` = ALONE ; `$1818` = LINKCT (noms de l'overlay 0x1E36C, chaînes 0x1E47C) |
| `$181A` | masque des emplacements à émettre (bit n = CPU n ; bits 8-15 = trames IA) |
| `$181C` | position du tourniquet de l'ordonnanceur |
| -$775A | TXOFFSET choisi ; -$7756 = 0x22 (longueur - 2) |
| `$199C[16]` | TXOFFSET pour réémettre la dernière trame reçue de n (= 0x1000 + début dans la zone RX) ; 0 pour la sienne |
| `$185E` | masque « vus » (bits 0-15) ; `$1860` = VBLANK depuis le retour de la trame propre |
| `$181E[16]` | VBLANK depuis le dernier changement du compteur de trame de n ; `$183F[n*2]` = dernière valeur vue |
| `$19BC` | banque courante de l'anneau (0, 0x20, 0x40, 0x60 octets) ; `$19BE` = 4 banques × 16 mots ; `$1A3E` = copie de la banque consommée |
| `$1A5E` | RXOFFSET lu à l'entrée de l'IRQ |
| `$2174` | masque des emplacements IA émis par cette borne ; `$215E` = premier emplacement IA (8 ou 12) ; `$1E60` = nombre-1 ; `$2172` = CPU maître des IA ; `$217C` = transfert en cours |
| `$4F4[16]` | +1 à chaque VBLANK (0xD1A4), -1 à chaque trame reçue de n : reste constant si exactement une trame de n par VBLANK (affiché par l'overlay) |
| -$7750 / -$774A / -$7748 / -$7752 / -$774E / -$774C | erreurs : STATUS bit 0 / IRQ d'émission avec TXSIZE ≠ 0 / IRQ sans cause / fin de trame introuvable / somme fausse / débordement de l'anneau |

### 2.2 Handler IRQ SCI (0x1BE4C, niveau 6)

```c
d3 = STATUS;  $1A5E = RXOFFSET;  bclr #5,-$77D5;  -$7746 = 0xFFFF;
if (d3 & 1) { -$7750++; goto ack_all; }            // erreur
if (d3 & 2) { rx_parse(); goto ack_all; }          // trame reçue (prioritaire sur le bit 2)
if (!(d3 & 4)) { -$7748++; goto ack_idle; }        // IRQ sans cause
// bit 2 : émetteur disponible
if (TXSIZE.b != 0) { -$774A++; goto ack_all; }     // l'émetteur n'a pas fini : anomalie
if ($181A == 0) goto ack_idle;                     // rien à émettre
send_next();                                       // 0x1BF38
START = 0; ack_sys(); MODE &= 9; rte;              // NB : STATUS n'est pas écrit
ack_all:  STATUS = 0; START = 0; ack_sys(); MODE &= 9; rte;
ack_idle: ack_sys(); MODE &= 0xD; rte;             // bit 2 de MODE laissé tel quel
```

### 2.3 Ordonnanceur (0x1BF38 / 0x1BF72)

```c
send_next() {
  d4 = $181C; d0 = $181A;
  for (i = 0; i < 16; i++, d4 = (d4+1) & 15)       // tourniquet
    if (d0 & (1 << d4)) {
      if (d4 == $1800)                       -$775A = 0;                       // trame propre : TX 0x000
      else if ($2174 && ($2174 & 1<<d4))     -$775A = tab_1BFDC[d4-8];         // IA : 0x40,0x80,..,0x200
      else                                   -$775A = $199C[d4];               // réémission : 0x1000+pos RX
      d0 &= ~(1 << d4); d4 = (d4+1) & 15; break;
    }
  -$7756 = 0x22; $181C = d4; $181A = d0;
  TXOFFSET = -$775A; START = 1; TXSIZE = 0x24; MODE &= 8; START = 0;  // même si rien n'a été choisi
}
```
Une seule trame par interruption ; la suivante part à l'interruption « émetteur disponible » suivante.
Au boot, `jsr 0x1BF38` (0xCDAE) avec `$181A = 0` émet 0x24 mots depuis TX 0 (encore nuls) : une trame factice
(somme fausse chez le voisin, comptée -$774E) qui sert à lancer la chaîne d'interruptions et fait passer MODE de 0x0C à 0x08.

### 2.4 Analyse RX (0x1BFEC, dans l'IRQ)

```c
rx_parse() {
  p = $1A5E & 0xFFF;
  for (k = 0; k < 0x21; k++) { p = (p-1) & 0xFFF; if (rx[p] & 0x100) goto found; }
  -$7752++; return;                                  // pas de fin de trame
found:
  $1A5E = p+1;                                       // juste après la trame
  s = (p - 0x23) & 0xFFF;                            // début (longueur fixe 0x24, le mot longueur n'est pas lu)
  if ((rx[p] - Σ rx[s..s+0x22] + 0x24) & 0xFF) { -$774E++; return; }
  id = rx[s+1] & 0xF;
  $185E |= 1 << id;  $4F4[id]--;
  // anneau : 4 banques, à partir de la banque courante (celle que le prochain VBLANK consommera)
  for (b = $19BC, k = 0; k < 4; k++, b = (b+0x20) & 0x60) if (ring[b][id] < 0) break;
  if (k == 4) { -$774C++; b = $19BC; }               // débordement : écrase l'entrée la plus ancienne
  ring[b][id] = $1A5E;                               // on garde la POSITION, pas les données
  $199C[id] = 0x1000 + s;
  if (id == $1800) { $1860 = 0; $199C[id] = 0; }     // trame propre revenue
  else if (!($2174 && ($2174 & 1<<id))) $181A |= 1 << id;   // à réémettre (sauf nos propres IA)
}
```
Une interruption ne traite que **la dernière** trame avant RXOFFSET : si deux trames arrivent avant que l'IRQ soit servie,
la première est perdue.

### 2.5 Consommateur (0x1C60A, appelé par le VBLANK 0xD142, donc 60 fois par seconde)

```c
copy = ring[$19BC]; ring[$19BC] = 0xFFFF…; $19BC = ($19BC + 0x20) & 0x60;   // une banque par VBLANK
for (n = 15; n >= 0; n--) if (copy[n] >= 0) {
  relit la trame dans la RAM RX à rebours depuis copy[n] (somme et longueur relues mais non vérifiées) ;
  id = mot 0 ; si 0xFFFF, == $1800 ou dans $2174 : ignorée ;
  sinon 6 mots -> $189C + id*16, 10 mots -> $1400 + id*64.
}
```
Donc : au plus 4 trames en attente par identifiant, une consommée par VBLANK. La trame est relue dans la RAM du C139
jusqu'à 4 VBLANK après sa réception : la zone RX (0x1000 mots, ~113 trames de 0x24) ne doit pas avoir fait le tour entre-temps.

### 2.6 VBLANK (0xD116) et comptage

```c
vblank() {                                // niveau 5, préempté par le SCI (niveau 6)
  0xD6C4(); MODE &= 9;
  link_tick();                            // 0x1C144
  0xD778();                               // IA : si $1800 != 0 && $1800 == $2172 -> $2174 = masque ; si !$217C -> 0x1C734
  0x1C60A();                              // consommateur
  0x1C45C();                              // applique les voitures reçues (si pas ALONE, etc.)
  compteurs ; $4F4[0..15]++ ; ack VBLANK (0x40000009)
}
link_tick() {                             // 0x1C144
  build_own_frame();                      // 0x1C588 : CONTROL=1, mots 0..0x22, CONTROL=3, somme
  $181A |= 1 << $1800;  MODE &= 0xB;      // poste la trame propre
  $1809 = $1FBF;                          // compteur de trame
  for (n = 15..0) {
    if ($183F[n] != rec[n].octet9) $181E[n] = 0;  $183F[n] = rec[n].octet9;
    if ($185E & 1<<n) if (++$181E[n] >= 16) $185E &= ~(1<<n);   // n figé 16 VBLANK : oublié
  }
  if (++$1860 >= 16) $185E &= ~(1 << $1800);                    // trame propre pas revenue depuis 16 VBLANK
  LINKCT = ($185E & 1<<$1800) ? popcount($185E & 0xFF) : 0;    // $1818 (0x1C1E0)
  $180C = plus haut n (0-7) vu, $180E = *16 ;
  0x1C370 (masques de groupe), 0x1C3F6 (efface enregistrements et anneau des n non vus), 0x1C2AE (recopie l'enregistrement propre)
}
```

### 2.7 Groupe, ALONE, IA : ce qui décide course liée ou solo

- Élection du groupe (0x1607A → 0x160E4) : si personne n'a le drapeau « hôte » (octet 8 bit 1), la borne le prend et
  choisit `GRP = 1 + max(GRP des enregistrements à drapeaux 4/5/6)` ; sinon elle reprend le GRP de l'hôte.
- Décision (0x164DC, quand le compte à rebours `$1894` vaut 8) :
  ```c
  if (LINKCT == 0) ALONE = 0xFFFF;                 // <- exige le retour de la trame propre
  else { ALONE = 0; if (nb d'enregistrements avec GRP == $1802 <= 1) ALONE = 0xFFFF; }
  ```
  **ridgera2 ne joue lié que si LINKCT ≠ 0**, c'est-à-dire si sa propre trame fait le tour de l'anneau en moins de 16 VBLANK.
  C'est pourquoi il restait solo avec l'émulation d'origine à ID identiques (aucun retour) et se lie avec le modèle WIP.
- IA (0x11E06) : en course liée à 2-4 bornes du même groupe, la borne de plus haut numéro (`$2176 → $2172`) émet aussi
  des concurrents IA dans les emplacements 8.. (ou 12.. si 8 est déjà vu ; aucun si 12 aussi), nombre = table 0x11F00.
  La borne CPU 0 ne peut pas être maître (test `$1800 != 0` en 0xD77E).

---

## 3. Rave Racer (raverace)

Même code ; seules les différences sont détaillées.

### 3.1 Variables

| Var | Rôle |
|---|---|
| `$11A0` | CPU NUMBER (0x10001060 & 7, routine 0x26E24 ; modifiable dans le menu, 0x2D6DC) ; `$1158/$115A/$115C` = n*2/16/64 |
| `$11A2` GRP, `$1100` ALONE (« ALN »), `$1102` LINKCT (« LNK ») | noms de l'overlay 0xACC2 / 0xAD8E (chaînes 0xADD4) |
| `$1034` | masque à émettre ; `$1036` tourniquet ; `$103A` TXOFFSET choisi ; `$1038` = 0x26 |
| `$103C[16]` | TXOFFSET de réémission |
| `$1104` | masque « vus » ; `$1108` VBLANK depuis le retour de la trame propre ; `$110C[]`, `$112D[n*2]` comme `$181E/$183F` |
| `$105C` banque, `$105E` 4×16 mots, `$10DE` copie, `$10FE` RXOFFSET à l'entrée d'IRQ | anneau |
| `$1162` masque IA émis par cette borne ; `$1168` premier emplacement IA (8 ou 12) ; `$4A94` = `$1168*64` ; `$4A96` = nombre-1 ; `$116A` mode « passation » | IA |
| `$1002` STATUS bit 0, `$1004` IRQ émission avec TXSIZE ≠ 0, `$1006` IRQ sans cause, `$1008` fin introuvable, `$100A` somme, `$100C` **débordement de l'anneau** | erreurs (IRQ) |
| `$1150` / `$1152` | **trames dont l'octet longueur ≠ 0x28 au moment de la consommation** / dernière valeur lue |
| `$114C` / `$114E` | trames dont le mot CPU a des bits 4-15 non nuls / dernière valeur |
| `$1012[16]` | décrémenté à chaque trame reçue de n (aucun incrément trouvé) |
| `$110A` | mis à 0xFFFF par chaque IRQ SCI (utilisé par l'I/O test) |

Dans l'I/O test (0x6150) : « C139 RA ERR » = `$1008`, « SUM ERR » = `$100A`, « FRAME ERR » = `$1002`, « CNT. OK » = mot 0x10002000.

### 3.2 Différences de code

- **Pas d'envoi de démarrage.** Hors I/O test, l'ordonnanceur 0x26C84 n'est appelé que par l'IRQ (0x26C06) : raverace
  n'émet **rien** tant qu'il ne reçoit pas d'interruption « émetteur disponible » (STATUS bit 2, TXSIZE.b = 0). Rien d'autre
  que l'effacement du bit 2 de MODE, juste après avoir posté une trame (0x26ECC, 0x274DC) ou au début du VBLANK (0x26EAE),
  ne précède ce besoin ; **hypothèse** : c'est cet effacement qui provoque l'interruption sur le matériel.
- RX (0x26D2A) : recherche de la fin sur 0x25 mots, trame 0x28, CPU en mot `s+1`, délais 8 au lieu de 16.
- Consommateur (0x27126) : en plus, `octet longueur == 0x28` sinon `$1150++` (`$1152` = valeur) ; `mot CPU & 0xFFF0 == 0` sinon `$114C++`.
- VBLANK (0x44FE) : `0xEFEE ; si -$7800 != 3 { 0x26EAE ; 0x273EE (IA) ; 0x27126 (consommateur) ; 0x2721A ; 0x470E }`.
  -$7800 = 3 n'est utilisé que par un sous-menu de test (0x2A082).
- 0x26EAE : `MODE &= 9 ; copie debug (si $CD6 == 1) ; construction (0x26F86) ; $1034 |= 1<<$11A0 ; MODE &= 0xB ; $11A9 = $C4D ;`
  péremption (8) ; 0x2708A (oublie les non vus) ; `if (++$1108 >= 8) bclr $11A0, $1105` ; LINKCT en 0x26F44 ; `$115E` = plus haut vu ; 0x27008 ; 0x2708A ; 0x270F2 (enregistrement propre).

### 3.3 Trames IA (« routine multi-trames »)

- Mise en place (0x184E2, au départ d'une course) : si ALONE ≠ 0 → `$1162 = 0` (rien n'est émis ; en démo ou en solo) ;
  sinon 0x185E0 :
  ```c
  maitre = plus haut CPU n (0-7) dont rec[n].GRP == $11A2 ;  $1164 = maitre;
  if (maitre != $11A0) { $1162 = $4A94 = $1168 = $4A96 = 0; return; }
  slot = 8; if ($1104 & 1<<8) { slot = 12; if ($1104 & 1<<12) { rien; return; } }
  $1168 = slot; $4A94 = slot*64; $4A96 = tab_186B4[nb membres du groupe]   // 3,3,3,2,1,0,0,0 = nombre-1
  $1162 = bits slot .. slot+$4A96 (0x18802) ; initialise les états IA
  ```
- Chaque VBLANK (0x273EE) : si `$1162 != 0` et `$116A == 0` → 0x274CA :
  0x275BE écrit les `$4A96+1` trames IA dans la zone TX aux demi-mots `tab_27670[k]/2` = 0x40, 0x80, 0xC0, 0x100
  (emplacements 8-11) ou 0x140 … 0x200 (12-15), chacune avec CONTROL 1 … 3 ; puis `$1034 |= $1162 ; MODE &= 0xB`.
  L'ordonnanceur les émet avec TXOFFSET = `tab_26D1A[d4-8]` (mêmes positions).
- Si `$116A != 0` (0x27400, passation) : plus d'émission IA ; on attend que chaque emplacement IA ne soit plus vu
  (bits 8-15 de `$1104`) pendant 8 VBLANK, puis on reprend. 0x27446 (qui arme `$116A`) n'a pas d'appel direct trouvé.
- En course liée à 2, le maître émet donc 1 + 4 trames par VBLANK, et l'autre borne doit réémettre ces 5 trames ;
  chaque borne émet ~6 trames et en reçoit ~6 par VBLANK (360 par seconde dans chaque sens).

### 3.4 ALONE, GRP et LINKCT dans raverace

- ALONE (`$1100`) est décidé d'après les enregistrements reçus (0xFCC0-0xFD38 : nombre d'enregistrements avec les
  drapeaux octet 8 bits 0/1 ; un seul participant → `$2344 = 1` → `ALONE = tab_FDA4[1] = 0xFFFF`), GRP repris de
  l'enregistrement « hôte » (octet 8 bit 6). **LINKCT n'intervient pas** dans cette décision.
- LINKCT n'est lu que par : l'affichage (0xACDC, 0xADAA), 0xC150 (si LINKCT ≠ 0, la phase d'animation `$2034` est prise sur
  le compteur de trame du plus haut CPU vu), 0xE724 (texte « link » si > 1), 0xF7AC (**attract synchronisé** : si LINKCT > 1,
  la borne met `$11A4` bit 0 et attend que chaque borne en attract ait le même bit dans son enregistrement, puis repart à
  l'étape max + 1), 0xFAFE (`$46F4` = plus haut CPU vu + 1 → choix des vues de l'attract en panorama, 0xF9EE).
- Conséquence : raverace peut faire une course liée **avec LINKCT = 0**, c'est-à-dire sans jamais revoir sa propre trame.
  C'est exactement la situation de l'émulation d'origine à ID identiques, la seule où il se lie dans les essais.

---

## 4. Question 2 : raverace avec le modèle WIP

### 4.1 Ce que raverace attend (lu dans le code)

1. Une interruption « émetteur disponible » (STATUS bit 2 avec TXSIZE.b = 0) chaque fois que l'émetteur est libre **et**
   que le jeu a quelque chose à émettre ; sans elle, raverace n'émet jamais (pas d'envoi de démarrage, cf. 3.2).
2. Après une écriture de TXSIZE, TXSIZE ≠ 0 tant que la trame n'est pas partie, puis 0. Une interruption bit 2 avec
   TXSIZE ≠ 0 est comptée comme anomalie (`$1004`).
3. Une interruption de réception par trame, servie avant l'arrivée de la suivante (seule la dernière trame avant
   RXOFFSET est analysée).
4. Au plus une trame par identifiant et par VBLANK en régime établi ; jusqu'à 4 d'avance sont absorbées, au-delà `$100C`
   compte et l'entrée la plus ancienne est écrasée (les données restent fraîches).
5. Les données d'une trame restent intactes dans la zone RX pendant au moins 4 VBLANK (`$1150` sinon).
6. Pour LINKCT ≠ 0 : sa propre trame revient au moins une fois tous les 8 VBLANK. Pour qu'une borne reste « vue » :
   son compteur de trame (octet 9) change au moins une fois tous les 8 VBLANK.
7. Débit : ~2 trames par VBLANK dans chaque sens en attract, ~6 en course liée avec IA.

### 4.2 Pourquoi l'émulation d'origine « marche » pour raverace

Avec l'émulation d'origine en mode 08 :
- l'IRQ n'est levée qu'à la réception, avec STATUS = 6 : le bit 1 est prioritaire, l'ordonnanceur n'est **jamais** appelé ;
- l'émission vient du « hack raverace » : à chaque CONTROL = 3, TXSIZE = (dernier mot écrit) + 2, puis envoi au tick suivant
  depuis TXOFFSET & 0xFFF ; TXOFFSET vaut encore 0x1000 (2ᵉ bloc d'init) → 0 → trame propre. C'est pour cela que le masque
  0x1FFF « cassait » raverace (§ 6 de C139.md) : il envoyait alors la zone RX ;
- à ID identiques, la puce ne relaie rien ; chaque borne ne reçoit que la trame du voisin, jamais la sienne → **LINKCT = 0
  en permanence**, mais ALONE (enregistrements) permet la course liée ;
- quand les IA sont actives, les trames IA sont écrites en 0x40.. avec leur propre CONTROL = 3 : le TXSIZE fabriqué couvre
  alors 0x000 jusqu'à la dernière trame IA (ex. 0x128 mots) et part en **une seule** trame ; le voisin n'analyse que les
  0x28 derniers mots (la dernière IA). La trame propre du maître ne passe que si le tick tombe entre sa construction et
  celle des IA (simulation : le voisin ne voit que l'emplacement 11).

Avec ID différents, la puce d'origine relaie : chaque borne revoit sa trame, LINKCT = 2… et raverace reste solo (essais du 2026-11-02).

### 4.3 Écarts entre le modèle WIP et ce que lit le code

| # | WIP | Effet |
|---|---|---|
| 1 | IRQ « émetteur libre » à **chaque** tick (800 Hz) tant que TXSIZE = 0 et STATUS ≠ 6 | ~730 IRQ inutiles par seconde (chemin `ack_idle`), sans effet sur la logique |
| 2 | émission au tick suivant l'écriture de TXSIZE, puis IRQ au tick d'après : 2 ticks par trame au minimum | débit ≤ 400 trames/s |
| 3 | `send_data` et l'IRQ d'émission sont bloqués tant que STATUS == 6 ; la réception passe d'abord à chaque tick | tant qu'il reste des trames à lire (file TCP non vide), la borne **n'émet plus du tout** |
| 4 | STATUS bit 2 reste à 1 après le premier acquittement (écriture de 0 → 4) | sans conséquence (le bit 1 est testé avant) |
| 5 | TXOFFSET 0x1000+pos : `(off + j) & 0x1FFF` | une trame réémise qui chevauche la fin de la zone RX lit la zone TX : ~1 réémission sur 100 a une somme fausse (`$100A`, simulation : 10 en 20 s, 0 avec un repli dans la moitié RX) |
| 6 | une seule action par tick | en course avec IA (360 trames/s dans chaque sens) le WIP plafonne à ~266 émissions/s : ~25 % des demandes sont fusionnées (perdues), sans débordement |

Aucun de ces écarts ne produit plus d'une trame par identifiant et par VBLANK : ils ne peuvent pas faire monter `$100C`.

### 4.4 Simulation (scratchpad `c139re\work\sim.py`)

Modèle des routines 0x26BBE, 0x26C84, 0x26D2A, 0x26EAE, 0x27126 (et variante ridgera2 : 0x24, délais 16) sur deux bornes,
contre trois puces : `wip` (namco_c139_wip.cpp), `orig` (ID identiques), `origrelay` (ID différents), `latch` (§ 7).
VBLANK atomique, réseau sans perte.

| Cas | Résultat |
|---|---|
| wip, attract (sans IA) | les deux bornes : vus = 0x0006, LINKCT = 2, aucun débordement |
| wip, course avec 4 IA | vus = 0x0F06, LINKCT = 2, ~44 trames/s par identifiant au lieu de 60, ~266 émissions/s par borne (saturation), quelques sommes fausses (écart 5) |
| orig | vus = l'autre seulement (0x0004 / 0x0002), **LINKCT = 0** |
| orig + 4 IA | la borne non maître ne voit que l'emplacement 11 (0x0800) |
| origrelay | LINKCT = 2 (comme wip) |
| latch | LINKCT = 2, 1 IRQ d'émission par trame, aucune erreur, même avec IA |
| wip / orig / latch, cab2 5 % plus lente | `$100C` ≈ 2,5 par seconde sur cab2, dans **les trois** modèles |
| latence de 100 ms par saut | aucun effet (seuls les trous comptent, pas le retard) |

### 4.5 Ce que cela dit de l'observation « `$100C` monte, `$1104` reste à 0 sur cab2 »

- Dans le code, `$1104 |= bit` (0x26D9C) précède `$100C++` (0x26DCC) dans le même chemin, et la péremption ne retire un bit
  qu'après 8 VBLANK sans changement. **`$100C` ne peut pas monter avec `$1104` nul en permanence.** À vérifier :
  regarder le **mot** 0x10009104 ; l'octet 0x10009104 seul (bits 8-15, emplacements IA) vaut 0 hors course.
- `$100C` qui monte régulièrement = un identifiant reçu plus vite qu'un par VBLANK de cab2. Causes possibles hors puce :
  cab2 plus lente que le temps réel (débogueur ouvert, machine chargée), ou rattrapage d'un arriéré TCP (cab2 a démarré
  sa liaison après cab1 : la file s'est remplie pendant le POST). Avec le WIP, pendant ce rattrapage, l'écart 3 empêche
  cab2 d'émettre (cab1 perd alors cab2 et le retour de sa propre trame). **Hypothèse** : raverace n'a pas de compteur
  « trames reçues − VBLANK » comme `$4F4[]` de ridgera2 ; il faut suivre `$100C` et la file de réception côté MAME
  (`m_fifo_rx.used()` / 0x200).
- `$1150` (longueurs fausses à la consommation) ne peut venir que d'une zone RX réécrite moins de 4 VBLANK après la
  réception : plus de ~100 trames de 0x28 en 4 VBLANK, ou des trames de taille différente. Si ce compteur monte, la puce
  écrit trop (arriéré traité à 800 trames/s : 13 trames par VBLANK) ou des trames trop longues.
- Le point commun des essais en échec (orig à ID différents, WIP) est **LINKCT = 2**, qui active chez raverace l'attract
  synchronisé (0xF7AC), l'attract en panorama (`$46F4`) et la phase d'animation reçue (0xC150). **Hypothèse** : l'échec
  vient de ces chemins ou de leur sensibilité au rythme des trames, pas de la signalisation de fin d'émission elle-même.
  Pour trancher : sur les deux bornes, relever `$1102`, `$1104` (mot), `$1100`, `$11A2`, `$100A`, `$100C`, `$1150`,
  `$114C`, `$1004`, et l'étape d'attract `$232E`/`$11A4-$11A5` ; tester aussi le WIP en forçant en continu `$1108 = 8`
  (triche MAME à chaque image : LINKCT reste à 0) pour voir si raverace se lie alors comme avec l'origine.

### 4.6 La fin d'émission dans le WIP

Ce que les deux jeux contrôlent est respecté : TXSIZE revient à 0 quand la trame est partie, l'IRQ arrive avec
STATUS bit 2. Les défauts sont ailleurs (écarts 2, 3, 5, 6) : émission retardée d'un tick, émission bloquée pendant les
réceptions, débit plafonné, repli de l'adresse. L'IRQ répétée à 800 Hz est tolérée par les deux jeux mais ne correspond
probablement pas au matériel (cf. § 7).

---

## 5. Question 3 : réglages, EEPROM et menu

EEPROM parallèle 8 Ko en 0x58000000 (HN58C65), un octet par adresse.

### 5.1 ridgera2
- Répertoire : bloc de 32 octets en 0x400 (signature « V146 » à +0x12) ; 16 octets = numéro de bloc de chaque
  enregistrement ; enregistrement r à `0x500 + 16 * rép[r]` (défaut `rép[r] = 0x10*r`, 8 emplacements de 0x20 par
  enregistrement pour l'usure), 15 mots + somme (`~Σ`) ; copie en RAM 0x10006800 + r*0x20 (0x1FEEC…0x2002E).
- GAME OPTIONS = enregistrement 2 à partir de +0x10 (RAM 0x10006850), défauts ROM 0x2009A :
  octets 0-4 GAME DIFFICULTY (NOVICE, INTERMEDIATE, ADVANCED, T.T., LINKABLE), 5-9 TIME EXTEND, 0xA SPEED (KM/H, MPH),
  0xB SOUND IN ATTRACT, **0xC CPU NO.** (0x1000685C, 0-7 affiché 1-8), **0xD CAR TYPE** (0x1000685D, couleur, envoyée dans la trame).
  En EEPROM (disposition par défaut) : CPU NO. = **0x71C**, CAR TYPE = 0x71D, somme de l'enregistrement en 0x71E-0x71F.
- **LINK** (dernière ligne de GAME OPTIONS, 0x217C4) : simple affichage du nombre de bornes vues
  (`popcount($185E & 0xFF)`, y compris soi-même seulement si sa trame revient). Le curseur s'arrête à l'option 0xD (CAR TYPE) :
  LINK n'est **pas réglable**. « LINKABLE » est la ligne de difficulté/temps du parcours de course liée.
  Changer CPU NO. dans le menu met à jour `$1800`, `$1810-$1814` immédiatement (0x21742) : c'est le réglage à changer
  pour que deux bornes aient des numéros différents.

### 5.2 raverace
- Répertoire : blocs de 0x40 en 0x000 + 0x40*k (signature « V154 » à +0x3A), un bit de sélection par enregistrement ;
  enregistrement r de 32 octets en `0x200 + r*0x40 + sel*0x20`, somme `~Σ` sur 30 octets en +0x1E ; copie en RAM 0x10001000 + r*0x20.
- GAME OPTIONS (1/2) = enregistrement 3 (RAM 0x10001060) : octet 0 **CPU NUMBER** (EEPROM **0x2C0** ou 0x2E0), octet 1 CAR TYPE (0x2C1 / 0x2E1),
  2-4 DIFFICULTY (LINKED PLAY, SOLO PLAY, TIME TRIALS), 5-0xC LAPS par parcours (RR SHORT, CITY, MOUNTAIN, MOUNTAIN EX × LINKED/SOLO).
- **LINK** (ligne 6 du menu, 0x2D798) : affiche ON si `popcount($1104 & 0xFF) != 0`, OFF sinon. Lecture seule.
- Overlay de debug : 0xAC98 / 0xAD8E (CPU, GRP, ALN, LNK).

---

## 6. Question 4 : ridgerac et ridgeracf

### 6.1 ridgerac (RR2 Ver.B)
- Même petit pilote que ridgeracf (init 0x14CDE, table 0x14CBE : 0x0F/START 1/RXSIZE 4/TXSIZE 4 puis MODE 0x0C, START 1, TXOFFSET 0x1000).
- Émission 0x14D56, appelée par le VBLANK (0x9FD8, niveau 4) **à chaque image** et par l'I/O test (0x5416, 0x1A704) :
  ```c
  if (len == 0 || TXSIZE.b != 0) return;
  START = 1; TXOFFSET = pos; pos = (pos + len + 2) & 0xFFF;    // anneau d'émission dans la zone TX
  CONTROL = 1; écrit len octets de 0x10006000, longueur, somme;  // pas de CONTROL = 3
  TXSIZE = len + 2; MODE &= 0xC; START = 0; len = 0x20;
  ```
- IRQ SCI niveau 5 (0x14DF0) : STATUS lu puis remis à 0, acquittement, bit 1 → copie de la trame en 0x10005800 et
  contrôle de somme (`-$7760` = SUM ERR), sinon compteur ; sortie `MODE &= 0xC`. Pas de traitement du bit 2.
- 0x10005800 n'est lu que par l'I/O test. **Aucune course liée** : la puce sert au test (« C139 RA ERR / SUM ERR »)
  et à l'émission d'une trame factice (0x22 demi-mots, données de 0x10006000) à chaque image.

### 6.2 ridgeracf (Full Scale, 3 cartes)
- Rôle de la carte : 0x50000001 bit 1 = 0 → centre (maître), bit 1 = 1 → côté (bit 0 : gauche/droite, angle `-$7554`).
- Le centre, à chaque VBLANK (0xA05E, niveau 4), appelle 0x15396 : 0x18820 remplit 0x10006000 (compteur d'image `$19A4`,
  position/orientation du joueur, états du jeu en quartets, caméra, 15 voitures × 7 mots), puis
  `TXOFFSET = pos (anneau) ; CONTROL = 1 ; 0xF0 octets ; longueur 0xF2 ; CONTROL = 3 ; somme ; START = 1 ; TXSIZE = 0xF2 ; MODE &= 0xC ; START = 0`.
- Toutes les cartes ont l'IRQ SCI (niveau 5, 0x15432) qui recopie la dernière trame en 0x10005800 (longueur lue dans la trame).
  Les côtés n'émettent jamais ; leur boucle (0xA89C) dépaquette 0x10005800 (0x18734) et rendent leur vue ;
  si le compteur reçu vaut 0, MODE = 0x0C (0x18744).
- **Hypothèse** : comme les côtés ne réémettent rien par logiciel, soit le C139 relaie seul en mode 0x0C (anneau
  centre → gauche → droite → centre), soit le câblage du Full Scale diffuse la sortie du centre aux deux côtés.
- Le « not working » de MAME vient surtout de l'absence des deux autres cartes (trois S22 complètes à faire tourner),
  pas d'un usage exotique du C139.

---

## 7. Ce que le C139 doit faire pour ces jeux

Proposition de sémantique (ce qui est marqué *déduit* ne se lit pas directement mais explique tous les masques ci-dessus ;
elle satisfait ridgera2 et raverace dans la simulation, modèle `latch`).

**RAM et trames**
- 0x2000 demi-mots de 9 bits. En émission, seuls les 8 bits bas de chaque mot partent ; la puce met le bit 8 sur le
  dernier demi-mot reçu (fin de trame).
- Adresse d'émission : TXOFFSET sur 13 bits ; une trame commencée en 0x1000+ (réémission depuis la zone RX) doit se replier
  **dans la moitié RX** : `adresse(j) = (TXOFFSET & 0x1000) | ((TXOFFSET + j) & 0xFFF)`.
- Réception : écriture à `0x1000 + (RXOFFSET & 0xFFF)`, RXOFFSET avance (12 bits), STATUS bit 1, interruption.

**Émission**
- L'écriture de TXSIZE ≠ 0 lance l'émission de TXSIZE demi-mots depuis TXOFFSET (mode 08 et 0C). CONTROL = 1 / 3
  encadre seulement l'écriture du paquet par le CPU (la somme est écrite après CONTROL = 3) ; START 1 → 0 encadre le
  chargement de TXOFFSET/TXSIZE. Ni l'un ni l'autre ne doit déclencher d'envoi.
- TXSIZE lit ≠ 0 pendant l'émission, 0 quand elle est finie (les jeux testent `TXSIZE.b`).
- Émission et réception sont indépendantes (liaison bidirectionnelle) : une réception en attente ne doit ni bloquer ni
  retarder l'émission.

**Interruptions** (*déduit*)
- STATUS bit 2 = « émetteur libre » : un état, à 1 quand rien n'est en cours ; écrire 0 dans STATUS ne l'efface pas
  (même convention que « écriture de 0 → 4 » de l'émulation actuelle). STATUS bits 0 et 1 s'effacent par écriture de 0.
- MODE bit 2 = verrou d'interruption d'émission : la puce lève l'IRQ « émetteur libre » quand l'émetteur est libre et que
  ce bit est à 0, et le met à 1 en la levant. Le CPU le remet à 0 pour réarmer :
  - après avoir lancé une trame (`&= 8`, `&= 9`) → IRQ à la fin de cette trame ;
  - après avoir posté une trame pendant que l'émetteur est libre (`&= 0xB`) → IRQ immédiate ;
  - dans le chemin « rien à émettre » le jeu laisse le bit à 1 (`&= 0xD`) → plus d'IRQ jusqu'au prochain réarmement.
  En mode 0x0C (ridgerac, ridgeracf) le bit reste à 1 (`&= 0xC`) : pas d'IRQ d'émission, conforme à leurs handlers.
  MODE vaut 0x0C après l'init : aucune IRQ d'émission avant le premier réarmement (ridgera2 : envoi de boot ; raverace : VBLANK).
- MODE bit 1 = même verrou pour la réception (effacé à chaque sortie d'IRQ). Une trame qui arrive pendant que le bit est à 1
  est écrite en RAM ; l'IRQ est levée quand le CPU efface le bit si STATUS bit 1 est encore à 1 (*déduit*). Le handler
  efface STATUS avant MODE : une trame arrivée pendant le handler est perdue, ce que le jeu tolère (il ne lit que la dernière).
- Bit 0 de MODE : effacé seulement juste après TXSIZE ; rôle inconnu (aucune lecture).
- Variante tolérée par les deux jeux (c'est le WIP) : IRQ « émetteur libre » répétée tant que l'émetteur est libre.

**Relais**
- Mode 08 : **pas de relais automatique** ; les jeux réémettent eux-mêmes les trames étrangères et s'arrêtent sur la leur.
  Un relais matériel ferait doubler chaque trame.
- Mode 0C (ridgeracf) : relais nécessaire si le câblage est un anneau (*hypothèse*, cf. 6.2).

**Rythme et débit**
- Une IRQ de réception par trame, servie avant que la suivante ne déplace RXOFFSET (pas de lots).
- Ne pas injecter plus d'une trame par identifiant et par VBLANK en régime établi ; ne pas traiter un arriéré à pleine
  vitesse (il déborde l'anneau de 4 et, au-delà de ~100 trames en 4 VBLANK, réécrit la zone RX avant sa consommation).
- Débit utile : ≥ 6 trames de 0x28 par VBLANK dans chaque sens (course liée raverace avec IA), soit ≥ 360 trames/s
  en émission **et** en réception simultanément. Un tick de 800 Hz qui fait une seule chose à la fois n'y suffit pas.

**Modifications minimales du WIP qui en découlent**
1. Émettre dès l'écriture de TXSIZE (ou dans le même tick), et lever « émetteur libre » à la fin de cette émission.
2. Ne plus conditionner `send_data` et l'IRQ d'émission à `STATUS != 6`.
3. Replier les adresses d'émission 0x1000+ dans la moitié RX.
4. (Option) IRQ « émetteur libre » une fois par fin d'émission ou par effacement du bit 2 de MODE, au lieu de chaque tick.
5. Tick plus rapide ou réception à l'arrivée des données, en gardant une IRQ par trame.
6. Pour raverace, vérifier en parallèle la piste LINKCT = 2 (§ 4.5) : c'est le régime que le matériel réel produit, et c'est
   aussi celui qui échoue dans les deux émulations où il apparaît.
