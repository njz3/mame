# C139 sur Super System 22 : dirtdash, tokyowar, alpinr2b, alpinerd, alpines, aquajet, propcycl, timecris, airco22b

Analyse statique (capstone M68K) des régions `:maincpu` vidées par MAME (mots de 32 bits retournés).
Super 22 : registres C139 en 0x420000 (+0 STATUS, +2 MODE, +4 CONTROL, +6 START, +8 RXSIZE, +A TXSIZE, +C RXOFFSET, +E TXOFFSET),
RAM en 0x410000 (0x2000 demi-mots ; TX = mots 0-0xFFF = 0x410000-0x411FFF, RX = mots 0x1000-0x1FFF = 0x412000-0x413FFF).
Contrôleur système en 0x700000 : +2 = niveau d'IRQ SCI (0 = coupée), +6 = acquittement SCI.
EEPROM 0x460000 (`umask32(0xff00ff00)`) : l'octet d'EEPROM *n* est à l'adresse 0x460000 + 2*n.

Conventions : `a6 = $E08000` dans tokyowar (ex. `-$5FFA(a6)` = 0xE02006). « H : » marque une hypothèse.

**Piège d'outil** : capstone n'affiche pas le facteur d'échelle de l'index quand le mot d'extension « bref » a un déplacement non nul.
`30317402` est affiché `move.w $2(a1, d7.w), d0` alors que c'est `move.w 2(a1,d7.w*4),d0` (bits 10-9 du mot d'extension = 10).
Il faut vérifier les octets dès qu'une table est indexée (ça concerne aussi les autres analyses faites avec `c139dis.py`).

## 1. Verdict par jeu

| Jeu | Link réel ? | Ce que fait le CPU principal avec le C139 | IRQ SCI |
|---|---|---|---|
| **tokyowar** | **oui, 2 à 4 bornes**, anneau avec **relais logiciel** | pilote complet 0x92F82-0x93820 ; une trame par VBLANK + relais dans l'IRQ | niveau 4 (0x4274), vecteur 0x70 → 0x932A0 |
| **alpinr2b** | **oui, 2 bornes** (LEFT/RIGHT), pas de relais | pilote 0x60FC-0x63BE (C compilé) ; une trame par image, appelée par la boucle principale (0x5D16/0x5E08/0x5E94 via `d7 = #$6270`) | niveau 4 (écrit par la routine d'émission 0x63B2), vecteur 0x70 → 0x60DC |
| dirtdash | **non** | `MODE = 0x0D` une seule fois (0x1384) ; routine de remise à zéro 0x2130 jamais appelée | **coupée** : `$700002 = 0` (0x12A8) ; handler niveau 2 (0x158A) = simple acquittement |
| alpines | non (code mort) | pilote copié d'alpinr2b (init 0x6312, émission 0x64B6) **jamais référencé** ; écran COMMUNICATION TEST (0x9EB2/0x9F3E) qui remplit le tampon sans jamais émettre | jamais programmée (seule la routine morte écrit `$700002`) |
| timecris | non | au boot (0x1135C) : init 0x128E8 (`STATUS 0, MODE 0x0F, CONTROL 0, START 3`, le reste à 0), puis test de la RAM sur 9 bits (0x1292A, motifs 0x1FF) | coupée (`$700002 = 0`, 0x12C7C) |
| alpinerd | non | test RAM 0x410000-0x413FFF au boot (0x8C26) ; aucun registre | niveau 2, handler 0x8CA2 = acquittement |
| aquajet | non | test RAM au boot (0x367A) ; aucun registre | niveau 2, handler 0x3CDC = acquittement |
| airco22b | non | test RAM au boot (0x519C/0x51C2) ; aucun registre | niveau 3, handler 0x5508 = acquittement |
| propcycl | non | aucun accès (les « hits » 0xC2BC/0xC306 sont des constantes de données) | niveau 3, handler 0xC1B8 = acquittement |

Chaînes : seuls tokyowar (« LINK », « ID NO. », « TEAM COLOR », « 1VS1 … 3VS1 BATTLE ») et alpinr2b (« LINK ON 1/2 », « LINK OFF », « LINK NG », « ID NO. », « COMMUNICATION TEST »)
ont des réglages de link. Dirt Dash n'a ni « LINK » ni option de cabine (GAME OPTIONS : difficulté, distances, STAGE SELECT, moteur, CABINET MOTION, son, vitesse).

## 2. Tokyo Wars (tokyowar, TW2 Ver.A)

### 2.1 Init (0x92F82, appelée une fois au boot depuis 0x4250 après effacement des 0x2000 mots de RAM C139 en 0x422E)
```
id = EEPROM[8..9] & 3              // movep.l $10(0x460000) >> 16 ; -$6000(a6) = id ; -$5FFC = id
-$5FEC = -$5FE8 = 0                // masques de « nœuds virtuels » (chars IA), voir 2.6
MODE     = 0x0F                    // 0x92FB2
CONTROL  = 0
START    = 1
RXSIZE   = 0xFF                    // jamais relu par le jeu
TXSIZE   = 0
RXOFFSET = 0x1000                  // début de la zone RX
TXOFFSET = 0
STATUS   = 0
MODE     = 0x0D                    // 0x92FEE
START    = 0
// puis remise à zéro des compteurs, file TX, historiques
```
Ensuite : `$700002 = 0x34` (IRQ SCI niveau 4), VBLANK niveau 2 (vecteur 0x68 → 0x49E0).

### 2.2 Format de trame (fixe : 0x46 = 70 demi-mots, un octet par demi-mot)
| mot | contenu |
|---|---|
| 0 | `0x81 + id` |
| 1 | compteur de sauts (0 à l'émission) |
| 2 | `0xA2` (marqueur de type) |
| 3 | `0x73 + id` |
| 4-0x43 | 64 octets d'état du char (`-$4E00(a6) + id*0x40` = 0xE03200 + id*0x40) |
| 0x44 | copie du compteur de sauts (0 à l'émission) |
| 0x45 | `0x100 \| somme8` : **bit 8 = fin de trame** ; somme8 = octet bas de (0x81+id) + 0xA2 + (0x73+id) + Σ données |

Source : construction en 0x930CA (trame propre au mot TX 0) et 0x931B4 (trames des nœuds virtuels 4-15, table 0x9322A :
nœud *n* lu depuis `-$4E00 + tbl[n].src` et écrit au mot TX `tbl[n].dst` = 0x46, 0x8C, … 0x348).

### 2.3 Émission propre, à chaque VBLANK (0x49E0 → `jsr 0x9362E` puis `jsr 0x93072`, sauf si DSP occupé `$C00000 bit 0`)
```
93072: -$5FFC = id ; -$5FEA = -$5FEC & 0xFFF0
       if (TXSIZE & 0xFF) != 0 && (TXOFFSET & 0x1000) == 0: return   // le C139 émet encore NOTRE trame : on saute ce VBLANK
       if -$5FF6 != 0xFFFF: -$5FF6++          // trames émises depuis le dernier retour de la nôtre
       build_own_frame(TX[0])                 // 2.2
       build_virtual_frames()                 // 0x931B4, pour chaque bit 4..15 de -$5FEA
       MODE = 0x0F                            // 0x9311E : section critique (H : bit 1 = IRQ C139 masquée)
       if (TXSIZE & 0xFF) != 0:               // occupé à relayer → tout mettre en file
           queue_push(0) ; queue_push(virtuels) ; MODE = 0x09 ; return          // 0x93190
       queue_push(virtuels)                   // 0x9326A
       if queue non vide:                     // il faudra l'IRQ de fin d'émission
           TXOFFSET = 0 ; START = 1 ; TXSIZE = 0x46 ; MODE = 0x0B ; START = 0 ; MODE &= 0x0D   // → 0x09
       else:
           TXOFFSET = 0 ; TXSIZE = 0x46 ; MODE = 0x0D                                          // pas d'IRQ de fin
```
La file logicielle : 32 entrées de TXOFFSET en `-$5FE0(a6)` (0xE02020), tête `-$5FE4`, queue `-$5FE2`.

### 2.4 Handler d'IRQ SCI (niveau 4, 0x932A0)
```
if $4302(a6) || $4300(a6): $700006 = 0 ; rte          // link suspendu : acquitte le contrôleur seulement
st = STATUS
if !(st & 2): goto tx_done                             // 0x932E8 : tout ce qui n'est pas « RX » est traité comme fin d'émission
-$5F9E++ ; STATUS = 0 ; $700006 = 0
// --- recherche de la trame : à rebours depuis RXOFFSET-1, au plus 0x46 mots, premier mot avec bit 8
p = (RXOFFSET & 0xFFF) ; repeat 0x46: p = (p-1) & 0xFFF ; if RX[p] & 0x100: found
if !found: err ; goto next_tx
s = p - 0x45                                           // début de trame (anneau de 0x1000 mots)
if (RX[s+2] & 0xFF) != 0xA2: err ; goto next_tx
if (RX[s+1] & 0xFF) != (RX[s+0x44] & 0xFF): err ; goto next_tx
hop = RX[s+1] + 1 ; if hop > 4: err ; goto next_tx      // TTL : au plus 4 sauts
RX[s+1] = RX[s+0x44] = hop                             // le jeu MODIFIE la trame en place dans la zone RX
n = RX[s] - 0x81 ; if n != RX[s+3] - 0x73 || n >= 16: err ; goto next_tx
if n == id:                                            // notre trame a fait le tour de l'anneau
    -$5FF6 = 0 ; seen |= 1<<n ; -$5FF8 = hop & 7       // hop = nombre de machines dans l'anneau
    goto next_tx                                       // retirée de l'anneau (pas de relais)
if -$5FEA & (1<<n): seen |= 1<<n ; goto next_tx        // un de nos nœuds virtuels : pas de relais
copy RX[s..s+0x44] (octets bas) vers le tampon double du nœud n (-$5E00 + n*0x8C [+0x46]), sum8 de ces 69 octets
RX[s+0x45] += 2 (octet bas)                            // somme corrigée des deux compteurs incrémentés
if (RX[s+0x45] & 0xFF) != sum8: -$5F9A++ ; err ; goto next_tx   // somme fausse : PAS relayée
if n >= 4 && hop >= 3: seen |= 1<<n ; goto next_tx     // nœud virtuel : relais limité à 2 sauts
seen |= 1<<n
// --- RELAIS LOGICIEL : on renvoie la trame telle qu'elle est dans la zone RX
off = 0x1000 + s                                       // TXOFFSET pointe dans la zone RX (bit 12)
if (TXSIZE & 0xFF) != 0:  queue_push(off) ; MODE = 0x09            // 0x934D8
elif queue non vide:      TXOFFSET = off ; START = 1 ; TXSIZE = 0x46 ; MODE = 0x09 ; START = 0
else:                     TXOFFSET = off ; TXSIZE = 0x46 ; MODE = 0x0D
rte
next_tx:  MODE = queue non vide ? 0x09 : 0x0D ; rte
tx_done:  // 0x935A6
-$5F9C++ ; $700006 = 0                                 // (STATUS n'est pas effacé ici)
if queue vide: -$5F96++ ; MODE = 0x0D ; rte
off = queue_pop()
if queue encore non vide: TXOFFSET = off ; START = 1 ; TXSIZE = 0x46 ; MODE = 0x09 ; START = 0
else:                     TXOFFSET = off ; TXSIZE = 0x46 ; MODE = 0x0D
rte
```

### 2.5 Comptage des machines et décision du jeu lié (0x9362E, à chaque VBLANK)
```
MODE = 0x0F
-$5FF2 = seen ; seen = 0 ; historique de 8 masques (-$5EB0) ; si les 8 sont égaux : -$5FF0 = masque stable
historique de 8 valeurs de -$5FF8 (-$5E90) ; si les 8 sont égales : -$5FFA = nombre stable de machines ; -$5FF8 = 0
-$5FF4 = (-$5FF6 < 8)            // « link vivant » : notre trame est revenue dans les 8 dernières émissions
copie des tampons doubles par nœud vers les structures du jeu ; MODE = queue ? 0x09 : 0x0D
```
- Le nombre de machines est **le compteur de sauts de notre propre trame quand elle revient** : chaque autre borne l'incrémente une fois,
  nous une fois → valeur = N. Seul (TX rebouclé sur RX) → 1. Pas de retour → 0.
- Le jeu copie `-$5FFA` et `-$5FF0` en 0xE1C2F2 / 0xE1C2F4 au début de la partie (0x1A734) et s'en sert pour former les équipes
  (0x1A8EA, 0x1CEB8 : 0 est traité comme 1). Modes 1VS1, 2VS1, 2VS2, 3VS1.
- **Max 4 bornes** : ID sur 2 bits, TTL de 4 sauts (avec 5 machines notre trame est rejetée au 5ᵉ saut → compte 0).
- Nœuds virtuels 4-15 : chars IA hébergés par une borne (table 0x93A80 par ID : 0x0071, 0x0382, 0x1C04, 0xE008 → 3 chars IA par borne quand `$1304(a6)` ≠ 0).
  Une borne peut donc émettre jusqu'à 4 trames par VBLANK, plus les relais : jusqu'à 16 trames de 70 mots par image sur chaque tronçon avec 4 bornes.

### 2.6 Réglages (GAME OPTIONS, chaînes 0x12A2C)
- **ID NO.** : 0-3, mot `$200C(a6)` (0xE0A00C) ↔ EEPROM octets **8-9** (mot gros-boutiste, l'ID est l'octet 9 & 3), sauvé par `movep.l` à 0x11700, relu à 0x1174C, 0x538C et 0x92F88.
- **LINK** : lecture seule, affiche `-$5FFA` (0x12D06) : 0 → « NG », 1 → « OFF », 2-4 → le chiffre.
- **TEAM COLOR** : GREEN/WHITE, `$201C(a6)` (0x12CF2 ; EEPROM 0x1008-0x1009 via `movep.l $2010(a0)`, 0x11718).
- Deux bornes ne doivent pas avoir le même ID : la trame de l'autre serait prise pour la sienne (comptée comme retour, jamais copiée).
- Overlay de debug non appelé en 0x94008 (DSW `$440001` bit 7) : ID, nombre, masques, compteurs RX/TX/erreurs.

### 2.7 Ce que tokyowar dit des registres
- **MODE est un champ de bits, pas un numéro de mode** : le jeu passe sans cesse entre 0x0F, 0x0B, 0x09 et 0x0D.
  - bit 2 (0x04) = 1 : **IRQ de fin d'émission masquée**. 0x09 seulement quand la file logicielle n'est pas vide (il faut l'IRQ pour la vider), 0x0D sinon.
  - bit 1 (0x02) = 1 : posé pendant les sections critiques (0x9311E, 0x9362E) puis effacé. H : masque global des IRQ du C139 (la réception continue).
  - bits 0 et 3 toujours à 1.
- **TXSIZE** : écrire un nombre de mots ≠ 0 lance l'émission (avec START = 0). Le jeu relit `TXSIZE & 0xFF` ≠ 0 comme « émetteur occupé » : il doit rester non nul pendant l'émission et revenir à 0 à la fin.
- **START** : 1 pendant la programmation (TXOFFSET/TXSIZE/MODE), puis 0. H : bit 0 = « retenir l'émission », pour ne pas manquer l'IRQ de fin d'émission. Le chemin 0x0D écrit TXSIZE avec START déjà à 0.
- **TXOFFSET** : adresse sur 13 bits ; 0x1000 + position RX pour relayer. Il est relu pendant une émission (bit 12 = « je relaie »).
  H : l'adresse doit boucler à l'intérieur de la moitié choisie (0x1FFF → 0x1000), sinon les relais de trames qui chevauchent la fin de l'anneau RX seraient faux.
- **RXOFFSET** : pointeur d'écriture RX (mot suivant), sur 12 bits dans la zone RX ; initialisé à 0x1000.
- **STATUS** : bit 1 = trame reçue ; tout autre motif d'IRQ = fin d'émission ; écrire 0 acquitte.
- **RXSIZE** : 0xFF écrit une fois, jamais relu. Il ne doit pas bloquer la réception : un compteur qui s'arrêterait à 0 couperait le link après 3 trames.

## 3. Alpine Racer 2 (alpinr2b, ARS2 Ver.B)

### 3.1 Init (0x60FC, appelée au boot en 0x5B90)
```
MODE = 0x00FF ; CONTROL = 0 ; START = 0 ; RXOFFSET = 0 ; RXSIZE = 0 ; TXOFFSET = 0 ; TXSIZE = 0
// pas d'écriture de STATUS ; MODE reste 0xFF (bit 1 à 1) jusqu'à la première émission
```

### 3.2 Émission, une fois par image (0x6270, appelée par la boucle principale, avant ou après la réception selon une bascule de phase)
```
$700002 = 0                                   // coupe l'IRQ SCI au contrôleur pendant la routine
buf = (toggle == 0) ? 0x200 : 0               // deux tampons TX (mots 0 et 0x200)
if n = payload_len ($E17E8C) != 0:            // 0x66 = 102 octets en jeu (0x2EDA6)
    TX[buf+0] = n + 2                         // longueur, non émise
    TX[buf+1] = 0x0A5                         // en-tête
    TX[buf+2 .. buf+1+n] = octets du payload ($E17D92)
    TX[buf+2+n] = 0x100 | (n + 1)             // fin de trame : bit 8 + distance en mots jusqu'à l'en-tête
    toggle ^= 0x200
    if (TXSIZE & 0xFF) == 0:                  // émetteur libre ; sinon trame perdue pour cette image
        TXOFFSET = toggle + 1 ; START = 1 ; TXSIZE = TX[toggle] & 0xFF (= n+2 = 0x68) ; START = 0 ; TX[toggle] = 0
copie des trames reçues (liste de 8 au plus) de la zone RX vers 8 cases de 0xFA octets en $E17E90
MODE &= 0xFFFD ; $700002 = 4
```
**Pas de relais** : TXOFFSET ne pointe jamais dans la zone RX.

### 3.3 Handler d'IRQ SCI (0x60DC → 0x616C)
```
st = STATUS ; STATUS = 0 ; MODE = 0xFFFD            // bit 2 toujours à 1 : jamais d'IRQ de fin d'émission
if st & 2:
    p = RXOFFSET & 0xFFF ; chercher à rebours (p inclus, 32 mots au plus) un mot avec bit 8
    if pas trouvé: $E18660++ ; goto end
    L = RX[p] & 0xFF ; RX[p] = 0                    // efface la marque de fin
    h = (p - L) & 0xFFF
    if (RX[h] & 0x1FF) != 0xA5: $E18664++ ; goto end
    ajoute (h+1, L) à la liste $E1B00C (8 entrées ; la plus ancienne sort si la liste est pleine)
end: STATUS = 0 ; MODE &= 0xFFFD ; $700006 = 0
```

### 3.4 Validation et état du link (0x2EDB4, une fois par image)
- Seule **la dernière trame reçue** de l'image est examinée (la case non vide la plus haute) ; aucune trame → compteur `$E1866A`,
  au bout de 60 images → type 0x51 (perte du link) ; sinon type 0x50.
- Le payload contient une étiquette de 32 bits en +0x10 : `0x173 << 20 | compteur_d'images << 4 | ID`, posée à l'émission (0x2EAD6).
  - étiquette == la dernière émise, ou `(étiquette & 0xF) == (mon ID & 0xF)` → **c'est ma propre trame** → état 7 ;
  - `étiquette >> 20 != 0x173` → état 9 ;
  - somme 16 bits en +0x62 (et +0x64 pour le type 0x40) fausse → compteurs d'erreur `$E1866C` / `$E18670` ;
  - sinon état 4 et les données de l'autre skieur sont prises.
- Affichage (0xFF7C, GAME OPTIONS) de `$E1867E` : 4 → « LINK ON 2 », 1 → « LINK OFF », 7 → « LINK ON 1 », 9 → « LINK NG ».
  La course liée teste l'état 4 (0x3B4CA). « LINK ON 1 » veut dire « je reçois mes propres trames », c'est-à-dire un anneau à une borne (TX rebouclé sur RX).
- Le premier mot du payload est un type de message (0x52, 0x50, 0x51, 0x40, 0x10, 0x11, 0x30) : poignée de main propre au jeu.

### 3.5 Réglages
- **ID NO.** : 0 = LEFT, 1 = RIGHT (bascule `eori.b #1`, 0xFCC0) : octet de réglage `$E186B8+0x32` = **octet 0x32 de l'EEPROM**
  (les 0x200 premiers octets d'EEPROM sont copiés en $E186B8 à 0x6500).
- **Deux bornes au maximum.** Elles doivent avoir des ID différents, sinon chacune prend la trame de l'autre pour la sienne (« LINK ON 1 » permanent).
- COMMUNICATION TEST (0xA36A) : affiche les 8 registres, la zone TX, les 8 cases RX et les compteurs d'erreur ; il émet des trames de longueur aléatoire (0x1E-0x9D octets).

## 4. Alpine Surfer (alpines, AF2 Ver.A) : même pilote, jamais branché
- Init 0x6312 et émission 0x64B6 identiques à alpinr2b, mais **aucune référence** à ces adresses (ni jsr/bsr, ni pointeur, ni constante).
  L'IRQ SCI n'est jamais programmée (seule la routine morte écrit `$700002`). Le COMMUNICATION TEST (table de tests 0x1ACD80 : 0x9EB2/0x9F3E)
  remplit le tampon (0xA19C) sans jamais émettre. H : reste de code d'alpinr2b ; une autre révision pourrait se lier.
- Intérêt : cette version utilise l'**IRQ de fin d'émission**, ce qui confirme les bits :
  - émission : `TXOFFSET = buf+1 ; START = 1 ; TXSIZE = n ; MODE &= 0xFFFB` (bit 2 à 0 : IRQ de fin autorisée) `; START = 0` ;
    si l'émetteur est occupé, la trame reste en attente (son mot de longueur reste non nul) ;
  - IRQ (0x635A) : `if STATUS & 4` (fin d'émission) : s'il y a une trame en attente → `TXOFFSET = buf+1 ; START = 1 ; TXSIZE = len ; MODE = 0xFFF9 ; START = 0`,
    sinon `MODE = 0xFFFD` (bit 2 à 1 : plus d'IRQ de fin) ; puis `if STATUS & 2` : réception comme alpinr2b.
  - STATUS bit 2 = fin d'émission ; MODE bit 2 = masque de l'IRQ de fin d'émission. C'est cohérent avec tokyowar et avec ridgera2 (C139.md § 4.1).

## 5. Dirt Dash (dirtdash, DT2 Ver.C) : pas de link dans le code
- Le seul accès effectif est `move.w #$0D, $420002` en 0x1384, au boot, après la copie de 0x1F0000 vers 0xE16000. C'est l'origine du « mode 0x0D » des notes MAME.
- 0x2130 (`lea $420000,a4` puis 8 écritures) n'a aucun appelant ni pointeur. Il contient une coquille : `38F8 000F` = `move.w $000F.w,(a4)+`
  au lieu de `#$0F` (MODE recevrait 0xF400).
- IRQ SCI coupée (`$700002 = 0`, 0x12A8). Le handler de niveau 2 (0x158A) ne fait qu'acquitter. Aucun TXSIZE, aucune lecture RX, aucun réglage de cabine ni de link.
- Les autres « hits » de c139_maincpu.asm (0x13EEA4 et suivants) sont des données (géométrie).
- H : les clones dirtdasha/dirtdashb/dirtdashj n'ont pas été vidés ; à vérifier, mais rien n'indique une version liable.

## 6. Pourquoi l'émulation actuelle (namco_c139_orig.cpp) donne « une seule machine »

**Dirt Dash** : rien à voir avec le C139. Le jeu n'émet rien (TXSIZE n'est jamais écrit, donc `send_data` ne part jamais),
ne lit rien et son IRQ SCI est coupée. Sur la vraie borne il court aussi seul. Aucune modification de l'émulation ne le fera se lier.

**Tokyo Wars** (LINK = 1 / « OFF », alors qu'il faudrait 2) :
1. Relais automatique dans `read_data` (`if (m_buffer[0] != m_linkid) send_frame()`) **en plus** du relais logiciel. Avec A et B :
   B renvoie à A la copie brute de F_A (hop 0) ; A la reconnaît comme sienne (`m_linkid`), la range quand même en RX et le jeu compte hop 0+1 = **1 machine**.
   Viennent ensuite les copies relayées par le jeu (hop 1, 2, 3, 4), que le C139 de A relaie à son tour. `-$5FF8` vaut successivement 1, 2, 3, 4 dans le même VBLANK :
   le filtre de 8 valeurs égales retient ce que donne la chronologie, 1 en pratique selon la note TODO de MAME. Le trafic est multiplié.
2. MODE traité comme une énumération : quand sa file logicielle n'est pas vide, le jeu écrit 0x0B puis 0x09. MAME bascule alors dans le comportement « mode 9 » :
   émission par recherche du bit 8 avec un masque de 12 bits, TXSIZE jamais remis à 0 (le jeu croit l'émetteur occupé pour toujours),
   bidouille du compteur de link. En MODE 0x0B ou 0x0F, MAME ne fait plus rien.
3. Pas d'IRQ de fin d'émission. MAME n'envoie une IRQ que sur réception (et STATUS = 4 après chaque acquittement) ; le handler du jeu voit toujours le bit 1
   et ne vide jamais sa file : les trames des chars IA et les relais retardés sont perdus.
4. `tx_mask = 0x1FFF` : un relais qui chevauche 0x1FFF repart dans la zone TX.
5. Instance seule : rien ne revient, donc LINK = « NG » au lieu de « OFF » (H : la vraie borne seule a son anneau refermé sur elle-même).

**Alpine Racer 2** (« LINK ON 1 » en attract et en test, cf. le TODO de MAME) : B relaie automatiquement F_A vers A, et A range sa propre trame en RX.
Comme le jeu n'examine que la dernière trame de l'image, il trouve souvent la sienne : état 7, et les données de l'autre sont ignorées pour cette image.
En partie, ça « marche à peu près ». Avec le même ID NO. dans les deux EEPROM, le link ne se fait jamais.

## 7. Ce que le C139 doit faire pour ces jeux

Modèle déduit de tokyowar, alpinr2b et alpines (et compatible avec ridgera2, C139.md § 4.1). « H » = non prouvé par le code.

1. **Pas de relais matériel, pas de filtrage par origine** dans ce fonctionnement (MODE 0x09/0x0B/0x0D/0x0F, 0xFD/0xFF/0xF9). Chaque mot émis
   par une borne arrive au RX de la suivante. C'est le logiciel qui relaie (tokyowar) ou qui n'en a pas besoin (alpinr2b, 2 bornes).
   Une borne seule : H, TX rebouclé sur son propre RX (tokyowar affiche alors « OFF », alpinr2b « LINK ON 1 »).
2. **MODE = bits** (4 bits relus tels qu'écrits ; les bits 4-15 de 0xFF/0xFFFD/0xFFF9 sont ignorés) :
   bit 2 = masque de l'IRQ de fin d'émission ; bit 1 = H : masque global des IRQ du C139 (posé pendant l'init et les sections critiques) ;
   bits 0 et 3 = 1 dans tous ces jeux (sens inconnu). Écrire 0x09/0x0B/0x0F ne doit **pas** changer la façon d'émettre ou de recevoir.
3. **Émission** : elle commence quand TXSIZE ≠ 0 et START = 0. Elle envoie TXSIZE mots de 9 bits depuis TXOFFSET (13 bits ; bit 12 = zone RX ;
   H : l'adresse boucle dans sa moitié de 4 K). Les mots partent tels quels : les jeux mettent eux-mêmes le bit 8 sur le dernier ;
   forcer le bit 8 sur le dernier mot et l'effacer ailleurs reste compatible. TXSIZE décompte et vaut 0 à la fin ; c'est son octet bas que les jeux testent.
   À la fin : STATUS |= 4, IRQ si MODE bit 2 = 0 (et H : bit 1 = 0).
4. **Réception** : chaque mot reçu (9 bits) est écrit en `0x1000 | (RXOFFSET & 0xFFF)`, puis RXOFFSET avance sur 12 bits dans la zone RX ;
   relu, il pointe sur le mot suivant. Sur un mot avec le bit 8 (fin de trame) : STATUS |= 2 et IRQ. Les jeux ne traitent que la dernière trame
   (ils remontent depuis RXOFFSET). Il faut donc livrer **une trame par IRQ** et attendre l'acquittement (STATUS = 0) avant d'écrire la suivante,
   comme le fait déjà la condition `STATUS != 6`, à réécrire en « bit 1 en attente ».
   tokyowar modifie la trame en place dans la zone RX avant de la relayer : la RAM RX doit rester lisible et inscriptible par le CPU sur 9 bits
   (timecris teste aussi ces 9 bits).
5. **STATUS** : bit 1 = trame reçue, bit 2 = émission terminée (événement mémorisé ; ne pas le laisser à 1 en permanence, sinon une IRQ de niveau
   tournerait en boucle dès que MODE bit 2 = 0) ; écrire 0 efface. tokyowar traite toute IRQ sans bit 1 comme une fin d'émission.
6. **RXSIZE / CONTROL** : écrits seulement à l'init (0xFF/0, 0/0). Ils ne doivent rien bloquer.
7. **Débit** (H) : tokyowar à 4 bornes peut faire passer jusqu'à environ 16 trames de 70 mots par image et par tronçon (trames propres, chars IA, relais).
   Une trame par tick de 800 Hz (environ 13 par image) ne suffit pas toujours : il faut un débit plus élevé, ou un temps calculé par mot.
8. **Configuration** : rien à faire pour dirtdash, timecris, alpinerd, alpines, aquajet, airco22b et propcycl, à part tolérer l'init et le test de RAM.
   Pour tokyowar, l'ID NO. (EEPROM 8-9, 0-3) doit différer d'une borne à l'autre ; pour alpinr2b, l'ID NO. (EEPROM 0x32 : 0 = LEFT, 1 = RIGHT) aussi.

Piste pour Final Lap (« UP TO 1 CAN PLAY », mode 0x0D, System 2), H non vérifiée : si Final Lap compte ses machines par le retour de sa propre trame,
comme tokyowar, le même relais automatique en double expliquerait le compte de 1.
