# C139 sur Namco System 2 : Final Lap 1/2/3, Four Trax, Suzuka 8 Hours 1/2, Dirt Fox (+ classement des autres jeux)

Analyse statique (capstone M68K, `python -I`) des régions vidées par MAME (`dumps/<jeu>/maincpu.bin`, `slave.bin`, octets permutés deux à deux).
Aucune exécution d'émulateur. « **H :** » marque une hypothèse ; le reste est lu dans le code.

Plan mémoire System 2 (les deux 68000 voient la même chose) :
- registres C139 en 0x4A0000 : +0 STATUS, +2 MODE, +4 CONTROL, +6 START, +8 RXSIZE, +A TXSIZE, +C RXOFFSET, +E TXOFFSET ;
- RAM C139 en 0x480000-0x483FFF = 0x2000 demi-mots. Adresse puce = (adresse hôte − 0x480000) / 2. TX = mots 0x0000-0x0FFF, RX = mots 0x1000-0x1FFF (0x482000) ;
- C148 (contrôleur d'IRQ de chaque CPU) : 0x1CC000 = niveau de l'IRQ SCI, 0x1CE000 = niveau VBLANK, écriture en 0x1DC000 = acquittement SCI.
  Dans MAME, `sci_int_w` déclenche les deux C148 ; seul le CPU qui a programmé un niveau SCI non nul la prend ;
- EEPROM 0x180000 (`umask16(0x00ff)`) : l'octet d'EEPROM *n* est en 0x180000 + 2n, et à l'offset *n* du fichier NVRAM de MAME.

Conventions : `a6 = 0x100000` dans tous ces jeux (`$2012(a6)` = 0x102012). « wN » = mot N d'une trame.
Tous les accès indexés vérifiés ont un facteur d'échelle 1 (68000), le piège capstone de `super22.md` ne s'applique pas ici.

## 1. Verdict

| Jeu | Protocole | CPU / IRQ SCI | Trame | Qui relaie | Identité de la borne | Max |
|---|---|---|---|---|---|---|
| finallap | « Final Lap », MODE 0x0D, pas de config 0x0F | maître, niveau 2, handler 0x8A46 | 0x14 mots | **le jeu** (TXOFFSET = 0x1000 + position RX) | n° de PCB 16 bits en EEPROM 0x10-0x11 | 8 |
| finalap2 | idem | maître, niveau 2, 0xCA9C | 0x16 mots | le jeu | « PCB NO » (EEPROM 0x10-0x11) | 8 |
| finalap3 | idem | maître, niveau 2, 0xD60A | 0x16 mots | le jeu | « PCB NO » | 8 |
| fourtrax | « à créneaux », MODE 0x0F/START 3 puis 0x09 ↔ 0x0D | maître, niveau 2, 0x4D34 | 0x25 mots, type 0x41 | le jeu (file d'émission) | « GRID NO » (EEPROM octet 8) | 8 joueurs (16 créneaux) |
| suzuka8h | idem | maître, niveau 2, 0x5B26 | 0x25 mots, type 0x66 | le jeu | « GRID NO » (octet 9) ; DIP « Status Monitor » | 8 |
| suzuk8h2 | idem | maître, niveau 2, 0x681C | 0x25 mots, type 0x82 | le jeu | « GRID NO » (octet 9) | 8 |
| dirtfoxj | idem (variante) | **esclave**, niveau 5, 0x1B28 | 0x20 mots, type 0x42 | le jeu | « CAR TYPE » 1-8 (GAME OPTIONS) | 8 |

Les quatre protocoles du System 2 (y compris kyukaidk et sws, § 5) ont trois points communs :
1. le relais est fait **par le logiciel**, avec un compteur de sauts dans la trame ;
2. le jeu reconnaît lui-même ses trames revenues (par n° de PCB ou par créneau) et les retire de l'anneau ;
3. le nombre de machines est **le compteur de sauts de sa propre trame quand elle revient**.

Le C139 ne doit donc ni filtrer, ni relayer, ni modifier les trames.

## 2. Final Lap (finallap), mode 0x0D

### 2.1 Démarrage et init (0x8F34, appelée en 0x1156)
```
// boot 0x1130-0x1188
init_c139();                       // 0x8F34
C148.VBLANK_level = 1;             // 0x116E, vecteur 0x64 -> 0x254A
C148.SCI_level    = 2;             // 0x1176, vecteur 0x68 -> 0x8A46 (l'esclave ne programme pas de niveau SCI)

void init_c139()                   // 0x8F34
{
    STATUS = 0; START = 0;
    for (n = 10000; n >= 0; n--)          // 0x8F44 : attend « émetteur libre »
        if (STATUS & 4) goto ok;
    print("SCI ERROR"); for (;;);         // 0x8F54-0x8F64 : blocage
ok:
    *(u32*)&RXSIZE = 0;                   // clr.l 0x4A0008 : RXSIZE = TXSIZE = 0
    *(u32*)&RXOFFSET = 0;                 // clr.l 0x4A000C : RXOFFSET = TXOFFSET = 0 (bit 12 à 0)
    START = 0;
    rx_prev = 0;                          // $2004
    MODE = 0x0D;                          // 0x8F86 ; jamais de MODE 0x0F, jamais de CONTROL
    my_id = EEPROM[0x10] << 8 | EEPROM[0x11];   // 0x8F8E -> $7008
    phase = 1; $7052..$7058 = 0;          // $7050 = phase de jeu (envoyée en w2)
    clear car table $104000 (32 x 0x80);  // 0 = moi, 1-7 = autres bornes, 8-15 = voitures IA
}
```

### 2.2 Trame (0x14 demi-mots, un octet par mot)
| Mot | Contenu (émission 0x8D0E) | Réception (0x8B80) |
|---|---|---|
| w0, w1 | n° de PCB de l'émetteur (octet haut, octet bas) | `movep.w 1(a2)` → d3, comparé à $7008 |
| w2 | phase de jeu $7050 (≠ 0 ; **0 = trame de voiture IA**) | $58 |
| w3 | $7052 (IA : index du créneau × 0x20) | $5A |
| w4, w5 | messages ponctuels $7054/$7056 (remis à 0 après l'envoi) | $5C/$5E (+ $7058 si w4 ≠ 0) |
| w6-w8 | position sur 24 bits (`$4(a0) + $11C(a6)`) | $50 |
| w9-w10, w11-w12 | `$2(a0)`, `$12(a0)` (16 bits chacun) | $54, $56 |
| w13 | drapeaux : bit 0 en course, bit 2 = `$4E(a0)` | `(a1)`, $4E |
| w14-w16 | `$40`, `$16`, `$1A` | idem |
| w17 | **type de voiture** `$14(a0)` (0-3 ; 4 = IA, H : livrée) | $14 |
| w18 | **compteur de sauts**, 0 au départ | voir 2.4 |
| w19 | somme des octets bas de w0-w18 & 0xFF, **+ 0x100** (bit 8 = fin de trame) | comparée sur 8 bits |

### 2.3 Émission, depuis le VBLANK (0x254A → 0x8D0E, 60 Hz)
```
void link_tx()                          // 0x8D0E
{
    build own frame at TX word 0;       // 0x8D16-0x8DBE, w18 = 0
    for (;;) {                          // 0x8DC4
        while (!(STATUS & 4)) ;         // attente active, interruptions autorisées
        sr |= 0x0F00;                   // masque IPL 7
        if (STATUS & 4) break;          // re-test interruptions masquées
        restore sr;
    }
    w19 = checksum(w0..w18) | 0x100;
    TXOFFSET = 0x0000;                  // 0x8DFC
    TXSIZE   = 0x14;                    // 0x8E02 : déclenche l'émission (START reste à 0)
    restore sr;
    off = 0x14;
    for each car k in $104400..$1047FF (8 voitures IA)      // 0x8E14
        if (car.active > 0 && car.$4C > 0) {               // IA calculée par cette borne
            build frame at TX word off: w0-w1 = my_id, w2 = 0, w3 = (k*0x80+0x4400)>>2 & 0xFF, ...;
            same wait/mask; TXOFFSET = off; TXSIZE = 0x14; off += 0x14;
        }
}
```
Jusqu'à 9 trames par image, écrites l'une derrière l'autre en TX (0x00, 0x14, 0x28…). Le jeu ne regarde que STATUS bit 2.
Il compte donc sur la puce pour **mettre STATUS bit 2 à 0 tant que la trame précédente n'est pas partie**. Sinon il écrase TXOFFSET/TXSIZE.

### 2.4 Interruption SCI = réception + relais (0x8A46)
```
isr_sci()                                        // 0x8A46, niveau 2
{
    pos  = RXOFFSET;  cnt = RXSIZE;  RXSIZE = 0; // lus puis RXSIZE remis à 0 tout de suite
    pos &= 0xFFF;
    if (pos > 0xF00) { RXOFFSET = 0; pos = 0; }  // le jeu gère lui-même le tampon RX (linéaire, 0-0xF13)
    rx_next = pos;                               // $2000
    rx_frame(cnt);                               // 0x8ADE
    C148.ack_sci();                              // clr 0x1DC000
    STATUS = 0;
    MODE   = 0x0D;                               // dernière écriture du handler (réarme, voir § 5)
}

rx_frame(cnt)                                    // 0x8ADE
{
    if ((cnt & 0xFF) != 0xEC) goto drop;         // 0xEC = -0x14 : RXSIZE est un décompteur, UNE trame par IRQ
    f = &RAM[0x1000 + rx_prev];                  // trame reçue = de l'ancien pointeur ($2004) au nouveau
    if ((sum8(f[0..18])) != (f[19] & 0xFF)) goto drop;
    id = f[0] << 8 | f[1];
    if (id == my_id) {                           // 0x8CCA : MA trame a fait le tour
        if (f[2] & 0xFF) {                       // trame principale (pas une IA)
            me.$58..$5E = f[2..5];
            link_hops = f[18] & 0xFF;            // $2012 = nombre d'AUTRES machines
            me.$46++;                            // « LINK ON »
        }
        goto done;                               // pas relayée : retirée de l'anneau
    }
    if ((f[18] & 0xFF) > 8) goto drop;           // 0x8B2E : 9 sauts maximum
    f[18]++;                                     // compteur de sauts, modifié DANS le tampon RX
    if (STATUS & 4) {                            // 0x8B3C
        f[19] = sum8(f[0..18]) | 0x100;          // somme recalculée, bit 8 remis
        TXOFFSET = 0x1000 + rx_prev;             // ÉMET DEPUIS LA ZONE RX
        TXSIZE   = 0x14;
        rx_prev  = rx_next;
        store(f);                                // 0x8B80
    } else {                                     // 0x8A96 : traite d'abord, puis attend l'émetteur
        store(f);
        while (!(STATUS & 4)) ;                  // attente active DANS l'IRQ
        f[19] = ...; TXOFFSET = 0x1000 + rx_prev; TXSIZE = 0x14;
        rx_prev = rx_next;
    }
    return;
drop:
done:
    rx_prev = rx_next;                           // 0x8CBA/0x8CC2/0x8D06 : resynchronisation
}

store(f)                                         // 0x8B80
{
    if (f[2] == 0) {                             // 0x8C4E : voiture IA d'une autre borne
        c = $104400 + ((f[3] & 0xE0) << 2);  c.$4A = id;  c.$4C = 0xFFFF;  ... c.$46++;
    } else {
        s = slot of id in $104080..$104380 (7 slots), else first free slot (s.$4A = id), else return;
        copy w2..w17 into s;  s.$46++;
    }
}
```
- Une interruption = une trame. RXSIZE est remis à 0 par le jeu et doit valoir −0x14 (octet bas 0xEC) à l'interruption suivante.
  Si la puce empile deux trames avant que l'IRQ soit servie, les deux sont perdues (0xD8 ≠ 0xEC).
- Le handler ne lit jamais STATUS pour savoir pourquoi il a été appelé : toute IRQ est traitée comme une réception.
  Une IRQ sans trame lit RXSIZE = 0 et ne fait que resynchroniser (`rx_prev = RXOFFSET`). C'est sans danger, sauf au milieu d'une réception.

### 2.5 Comptage des machines et vivacité
- `$2012` = compteur de sauts de ma trame quand elle revient = **N − 1**. Ce n'est mis à jour que dans ce cas, jamais remis à 0 ensuite.
- Attract « UP TO n CAN RACE SIMULTANEOUSLY » (chaîne 0x84CC) : en 0x8450, le caractère 6 est remplacé par la glyphe `0x60 + $2012`, ce qui affiche N (H : glyphe 0x60 = « 1 »).
- Mode test (0x1466) : « LINK OFF » si `me.$46` (= $4046) n'a pas bougé, sinon « LINK ON n » avec n = `0x81 + ($2012 & 0xF)` = N.
  Le même codage 0x81 sert aux réglages de pièces, où la valeur 0 s'affiche « 1 ».
- Créneaux distants (0x892A, à chaque VBLANK) : un créneau qui reçoit des trames repart avec un compteur de 10.
  Après 10 images sans trame (environ 167 ms), il est libéré.
- Logique de salon (0x26CC-0x27B8) : `if ($2012 != 0)` → regarde la phase `$58` des autres (4 ou 5 = course en cours, on peut rejoindre).
  `$2012 > 4` allonge le délai d'inscription (0x270A : $7010 = 0x4EC au lieu de 0x3C0).

### 2.6 Final Lap 2 / Final Lap 3 (même code, adresses décalées)
| | finalap2 | finalap3 |
|---|---|---|
| init | 0xD008 (appel 0x1164) | 0xDB76 (appel 0x1164) |
| IRQ SCI (niveau 2) | 0xCA9C | 0xD60A |
| émission (VBLANK) | 0xCDAA (appel 0x43C0) ; vivacité 0xC980 | 0xD918 (appel 0x42D6) |
| trame | **0x16 mots** : w0-w17 comme FL1, w18 = `($72&3)<<2 \| $68`, w19 = $7052, **w20 = sauts**, w21 = somme \| 0x100 | idem |
| test RXSIZE | `== 0xEA` (−0x16) | idem |
| LINK ON n (mode test) | 0x1B92 : glyphe 0xDD1 + $2012 | 0x1C84 |
| UP TO n (attract) | 0xC582 (gros chiffres 0x3770 + 2·$2012) ; 0x30CA : 0xDD0 + $2012 + 1 | 0x170BA : 0x60 + $2012 |

Init identique à FL1 : STATUS 0, START 0, attente de STATUS bit 2, RXSIZE/TXSIZE/RXOFFSET/TXOFFSET = 0, START 0, MODE 0x0D, my_id lu en EEPROM 0x10-0x11.

### 2.7 Identité, type de voiture, nombre maximal
- **Le réseau identifie la borne par le n° de PCB de 16 bits en EEPROM 0x10-0x11**, pas par le type de voiture.
  - Final Lap 1 (0x1730) : si l'octet 6 de l'EEPROM ≠ 0x57, l'EEPROM est réinitialisée (table 0x17CC) et le n° est tiré au hasard.
    Le tirage vaut `(u16)($44301A × $44301E)`, recommencé tant que le résultat est nul (registres 6 et 7 du C116, H : un compteur sur le vrai matériel).
    Le n° s'affiche en hexadécimal dans l'écran de test (0x12BC-0x12CC). FL1 n'a aucun moyen de le changer en mode test.
  - Final Lap 2 (0x1434) / 3 (0x1450) : même tirage si la signature (octet 6 = 0x57/0x88/0xA1) est absente. Le n° s'affiche « PCB NO » dans GAME OPTIONS (FL2 0x1A9E).
    Il est re-tiré (`rand` 0xD960/0xE498 + compteur d'images) en FL2 0x1CB8-0x1CEE et FL3 0x1DB2-0x1E0A.
    H : il faut une combinaison cachée sur l'écran qui affiche « H.OK / T.OK » : `$8(a6) == 0x10` (volant), `$0(a6) == 0x18` (accélérateur) et un front sur le bit 4 de `$460000`.
- **Type de voiture** (connecteurs, ports MAME MCUB bit 1 et MCUH bit 7, lus dans $7114/$7116) : il ne sert qu'au véhicule.
  `$14` (0x2F4A-0x2F70) = combinaison des deux bits (0-3), ou une valeur aléatoire si « Automatic Car Select ».
  Il est envoyé en w17 et sélectionne la palette (0x6844). Deux bornes peuvent avoir la même voiture.
- **Maximum 8 bornes** : 7 créneaux distants ($104080-$104380) plus soi. Une 8e borne distante est relayée mais pas affichée (0x8BB0 `rts`).
  Le relais s'arrête à 9 sauts (w18 ≤ 8 avant incrément).

### 2.8 Pourquoi Final Lap ne voit qu'une machine sous l'émulation actuelle
1. **Cause principale : les deux instances MAME ont le même n° de PCB.**
   - FL1 : à partir d'une NVRAM vierge (tout à 0xFF), le n° vient du produit des registres C116 6 et 7.
     MAME les renvoie constants (`namcos2_v.cpp`, `c116_r` : `if (offset > 0x180b) return 0xff; // fix for finallap boot`).
     Le n° est donc le même dans toutes les instances : 0x0001 si l'octet haut non câblé se lit 0xFF, 0xFE01 s'il se lit 0.
     Une NVRAM copiée d'une borne à l'autre donne le même résultat.
   - FL2/FL3 : les jeux de MAME chargent une NVRAM par défaut depuis la ROM (`ROM_REGION(0x2000, "nvram")`, `namcos2.cpp` l. 3017 et 3194). Le PCB NO est donc identique partout.
   - Conséquence (0x8B20) : B prend chaque trame de A pour la sienne revenue. Il ne la relaie pas, il range son compteur de sauts (0) dans $2012 et il incrémente $4046.
     Résultat sur les deux bornes : « LINK ON 1 » en test, « UP TO 1 » en attract, et l'autre voiture n'entre jamais dans la table.
     Ça se produit quels que soient les ID d'anneau, comme dans les essais.
   - Vérification proposée : FL1 affiche le même n° hexadécimal en test sur les deux bornes, et « LINK ON 1 » plutôt que « LINK OFF ».
2. Même avec des n° distincts, le mode 0x0D émulé casse le protocole :
   - **relais automatique** des trames dont l'ID d'anneau diffère : le doublon garde le compteur de sauts. La trame de A lui revient avec 0 saut, puis avec 1, puis repart.
     Les copies ping-pongent jusqu'à 9 sauts et $2012 oscille ;
   - **émission différée** : la trame ne part qu'au tick suivant (800 Hz), avec les registres du moment. STATUS bit 2 reste toujours à 1 (écrire 0 donne 4).
     Le VBLANK enchaîne jusqu'à 9 couples TXOFFSET/TXSIZE et le relais de l'IRQ en ajoute. Seul le dernier part : la trame principale est perdue dès qu'une IA suit, et des relais sont écrasés ;
   - `send_data` ne fait rien tant que STATUS = 6. Sous trafic continu, une trame reçue à chaque tick bloque l'émission.
3. Ce qui est juste dans l'émulation actuelle :
   - RXSIZE décrémenté (le jeu exige −0x14/−0x16 sur l'octet bas) ;
   - TXOFFSET dans la zone RX : c'est le mécanisme de relais du jeu, et le code l'accepte déjà (`tx_mask = 0x1fff` en 0x0D). Le commentaire « not supported » est périmé.
   Le relais de la puce, en revanche, doit disparaître.

## 3. Four Trax, Suzuka 8 Hours, Suzuka 8 Hours 2, Dirt Fox : protocole « à créneaux », mode 0x09/0x0D

Même code de base, avec les adresses suivantes :

| | fourtrax | suzuka8h | suzuk8h2 | dirtfoxj |
|---|---|---|---|---|
| CPU du pilote | maître | maître | maître | **esclave** (le maître publie/lit les masques) |
| init | 0x498A (appel 0x13FA) | 0x5736 (0x180A) | 0x642C (0x119C) | 0x19EE (0x9EE), tables 0x19CE |
| routine périodique | 0x4A6A, VBLANK 0x33E0 | 0x5816, VBLANK 0x39EC | 0x650C, VBLANK 0x3F42 | 0x1A50, boucle principale 0xA20 |
| IRQ SCI | niveau 2, 0x4D34 | niveau 2, 0x5B26 | niveau 2, 0x681C | niveau 5 (table C148 0x490), 0x4E6 → 0x1B28 |
| type (w1) | 0x41 | 0x66 | 0x82 | 0x42 |
| longueur de trame | 0x25 | 0x25 | 0x25 | 0x20 |
| TXSIZE écrit | **0x2C** | 0x25 | 0x25 | **0x3F**, entre START = 1 et START = 0 |
| réglage du créneau | GRID NO = EEPROM octet 8 → $102004 (table 0x2EB4) | GRID NO = octet 9 | GRID NO = octet 9 | CAR TYPE (1-8, couleur), $1005D4 |

### 3.1 Init (Four Trax 0x498A ; les autres à l'identique)
```
STATUS = 0; START = 0; $2282 = 10;
wait up to 10000 polls for STATUS & 4, else "SCI ERROR" + hang;   // 0x49A2
regs[0..7] = { 0x0000, 0x000F, 0x0000, 0x0003, 0x0000, 0x0000, 0x1000, 0x1000 };
     // STATUS, MODE = 0x0F, CONTROL = 0, START = 3 (valeur de config), RXSIZE, TXSIZE, RXOFFSET = TXOFFSET = 0x1000
clear RAM 0x480000-0x483FFF (0x2000 mots);
regs[0..7] = { 0x0000, 0x0009, 0x0000, 0x0000, 0x0000, 0x0000, 0x1000, 0x1000 };     // MODE = 0x09
reloc[0..15] = 0xFFFF ($2120) ; mine = 0xFFFF ($2170) ; pending = 0 ($2118) ; C148.ack_sci();
```
- La valeur de config est **START = 3** pour les quatre jeux (Dirt Fox a la même table).
- RXOFFSET et TXOFFSET valent 0x1000 : ce sont des adresses de mot sur 13 bits. Le jeu ne réécrit plus jamais RXOFFSET ni RXSIZE : le tampon RX est circulaire et géré par la puce.

### 3.2 Trame et zone d'échange dans la RAM du C139
- Créneaux 0-15. Le créneau *s* a sa zone TX en mots `0x800 + s·0x80`, à l'adresse hôte `0x481000 + s·0x100`.
- La RAM C139 sert aussi de **boîte aux lettres entre les deux CPU** (Dirt Fox) et entre le VBLANK et l'IRQ :
  - TX mot 0x10 = N − 1 (écrit par l'IRQ, lu dans $2008) ;
  - TX mots 0x11-0x12 = masque des créneaux vus ;
  - TX mots 0x20-0x21 = masque des créneaux à moi ;
  - TX mots 0x22-0x23 = recopiés en w3-w4.
- Trame : w0 = créneau, w1 = type, **w2 = compteur de sauts (1 au départ)**, w3-w4 = TX mots 0x22-0x23, données (FT/S8H : w5-w0x23, DF : w3-w0x1E).
  Le dernier mot (FT/S8H w0x24, DF w0x1F) = somme des octets bas des mots précédents & 0xFF **| 0x100** (fin de trame).

### 3.3 Routine périodique (Four Trax 0x4A6A, une fois par VBLANK)
```
if ($2282) $2282--;                                  // vivacité du link (15 images, cf. 3.4)
mine = (mine & 0xFF00) | (1 << grid_no);             // $2112 ; octet haut = autres créneaux à moi (H : IA)
TX[0x20] = mine >> 8; TX[0x21] = mine & 0xFF;
seen = TX[0x11] << 8 | TX[0x12] & 0xFF;              // $2110
others_minus_1 = TX[0x10] & 0xFF;                    // $2008 = N - 1
for s in 0..15:
    if (mine & bit(s)) write my car data into TX slot s (w5..);       // 0x4ADA
    else if (seen & bit(s)) read car data of slot s from its TX copy;  // 0x4B80, relue si modifiée entre-temps
every 2nd call: TX[0x11..0x12] = OR of the last 16 $2150 snapshots;   // créneaux vus sur environ 32 images
for s in 15..0 with bit(s) in (TX[0x20]<<8 | TX[0x21]):               // 0x4CA6
    f = TX slot s; f[0] = s; f[1] = TYPE; f[2] = 1; f[3..4] = TX[0x22..0x23];
    f[last] = sum8(f[0..last-1]) | 0x100;
    reloc[s] = byte offset 0x1000 + s*0x100;  pending |= bit(s);
    MODE &= 0x0B;                                    // 0x4D04 : retire le bit 2 = demande d'IRQ « émetteur libre »
```

### 3.4 Interruption SCI (Four Trax 0x4D34)
```
st = STATUS;
if (st & 2) {                                         // ---- réception (prioritaire)
    p = (RXOFFSET*2 & 0x1FFF) | 0x2000;               // pointeur RX courant (adresse de mot × 2)
    for (k = 0; k < 0x26; k++, p -= 2)                // DF : 0x21
        if (RAM16[p] & 0x100) goto found;             // cherche EN ARRIÈRE le mot de fin (bit 8)
    goto out;
found:
    p -= 2*(LEN-1);  start = p;                       // $2144 (LEN = 0x25, DF 0x20)
    s = w0;  if (w1 != TYPE) goto out;
    w2 += 1 (sauf moniteur S8H, cf. 3.6) ; écrit dans le tampon RX ;  if (w2 >= 0x10) goto out;
    if (mine & bit(s)) {                              // MA trame a fait le tour
        TX[0x10] = max(w2 - 2, 0);                    // = N - 1 (N incréments, dont le mien)
        $2282 = 15;  goto out;                        // « LINK ON »
    }
    copy data words to tmp; last = last + 1 | 0x100 (écrit dans le tampon RX) ;
    if (sum8 != last) goto out;                       // somme corrigée de l'incrément
    copy data into TX slot s (w5.., DF w3..) ; TX slot s, octet haut du mot 0 += 1 (« nouvelle trame ») ;
    reloc[s] = start;                                 // RELAIS : on renverra la trame depuis la zone RX
    pending |= bit(s);  seen_now |= bit(s);
} else if (st & 4) {                                  // ---- émetteur libre
    for (i = 0; i < 16; i++, rr = (rr+1) & 15)        // tourniquet $2140
        if (pending & bit(rr)) {
            pending &= ~bit(rr);
            TXOFFSET = reloc[rr] >> 1;                // 0x800+s*0x80 (ma trame) ou 0x1000+pos (relais)
            TXSIZE   = 0x2C;                          // S8H/S8H2 0x25 ; DF : START=1 ; TXSIZE=0x3F ; START=0
            break;                                    // UNE trame par IRQ
        }
    START = 0;
}
out:
STATUS = 0;                                           // FT/S8H ; DF ne l'écrit que dans la branche RX
C148.ack_sci();
MODE = 0x09 | (pending ? 0 : 4);                      // FT/S8H. DF : MODE &= 9 si reste/RX, MODE &= 0x0D sinon
```
- L'émission est **pilotée par interruption**. Quand le jeu a quelque chose à envoyer, il met MODE bit 2 à 0.
  La puce doit alors lever une IRQ avec STATUS bit 2 (« émetteur libre »). Le handler envoie une trame, puis remet MODE à 9 s'il en reste.
  Quand la file est vide, il met MODE à 0x0D, ce qui coupe cette IRQ.
- Dirt Fox ne met **jamais** MODE bit 2 à 1 lui-même :
  - routine périodique : `MODE &= 0x0B` ;
  - IRQ, s'il reste des trames ou après une réception : `&= 9` ;
  - IRQ, quand la file est vide : `&= 0x0D` (laisse le bit 2 tel quel).
  **H forte** : quand la puce lève l'IRQ « émetteur libre », elle met elle-même MODE bit 2 à 1, et le logiciel l'efface pour la réarmer.
  Sans ça, Dirt Fox serait soit noyé d'IRQ, soit bloqué (aucun front quand il efface un bit déjà à 0).
  Même logique pour le bit 1 et l'IRQ de réception (FL1 réécrit 0x0D, kyukaidk fait `bclr #1, MODE`, ridgera2 `&= 9 / &= 0xD`).
- La réception ne traite que **la trame la plus récente** : la recherche part de RXOFFSET et remonte.
  Si deux trames sont arrivées avant l'IRQ, la plus ancienne est perdue : il faut livrer une trame par IRQ.
- TXSIZE 0x2C (FT) et 0x3F (DF) dépassent la longueur de trame (0x25, 0x20). **H :** en mode 9 bits, la puce s'arrête au mot qui porte le bit 8.
  Si elle envoyait tout, les mots en trop seraient des zéros (fin de zone TX) ou la suite du tampon RX, sans bit 8. La recherche à rebours (fenêtre LEN + 1) les tolère : les deux modèles marchent.

### 3.5 Comptage des machines
- N − 1 = `w2 − 2` de ma trame revenue → TX mot 0x10 → `$2008` (FT 0x4AB8, S8H 0x5884, DF maître 0x8A16 → $10A91C).
- Four Trax, attract « UP TO n CAN RACE SIMULTANEOUSLY » (chaîne 0x1176) : en 0x122E, le caractère 6 = `'1' + $2008` = N.
  Mode test : « LINK » en 0x178A, chiffre `0x291 + $2008`, ON/OFF selon `$2282` (ma trame revenue dans les 15 dernières images).
- Suzuka 8h/8h2 : même chose (« UP TO ? CAN RACE », 0x13C0).
  Avec le DIP « System is Status Monitor » ($460027 bit 6 → $40D410 = 1, 0x2E54), la machine ne joue pas : créneau 0, aucun créneau à elle.
  Elle **relaie sans incrémenter** les sauts (`seq d5 ; sub.b d5,d0`, 0x5C02) et compte `popcount(seen & 0xFF) − 1` (0x582E).
- Dirt Fox : le maître lit aussi le nombre de créneaux vus, `popcount(TX[0x11..0x12])` → $10AA24 (0x89E0).

### 3.6 Réglages, nombre maximal
- **Créneau = GRID NO** (FT octet 8, S8H/S8H2 octet 9 de l'EEPROM ; valeur par défaut **0** dans les tables 0x2E9E (FT), 0x35EA (S8H), 0x34A4 (S8H2)).
  Dirt Fox : **CAR TYPE** dans GAME OPTIONS (1-8 « … 7 GREEN, 8 BLUE », variable `a6+0x194` = $1005D4, `& 7` en 0x899E), dans l'EEPROM à somme de contrôle (signature « DART »).
  Dirt Fox a une NVRAM par défaut en ROM dans MAME (`namcos2.cpp` l. 2561).
- Deux bornes avec le même créneau se prennent chacune les trames de l'autre pour les siennes. Il faut **un GRID NO / CAR TYPE différent par borne**.
- Les masques de joueurs tiennent sur 8 bits (octet bas). Le moniteur S8H ne compte que l'octet bas et DF fait `& 7`. **8 bornes joueurs**, plus les créneaux 8-15 (H : véhicules IA).
  Une trame meurt à 16 sauts.
- Le type de véhicule (« CAR TYPE » de FT/S8H) est un réglage à part. Pour FT/S8H, H : c'est une livrée, comme dans Final Lap.

### 3.7 Remarques sur l'émulation actuelle (mode 0x09)
- `m_reg_f3 & 2` (START = 3 en config) divise TXOFFSET par 2. C'est contraire au code : TXOFFSET = `reloc >> 1` est déjà une adresse de mot, 0x800 + s·0x80 ou 0x1000 + position RX.
- L'émulation ne lève jamais l'IRQ « émetteur libre » : le jeu n'entre donc jamais dans sa branche d'émission.
  Le lien tient (H) par une suite de coïncidences :
  - le tick envoie tout seul depuis 0x1000 >> 1 = 0x800, donc la zone du créneau 0 ;
  - `find_sync_bit` y cherche le premier bit 8, ce qui attrape aussi la trame d'un autre créneau si la zone 0 est vide ;
  - la recherche à rebours du récepteur ne garde que la fin, donc le début en trop est ignoré ;
  - la puce relaie avec son propre compteur `m_buffer[0x1ff]`, réinjecté en w2.
  Les relais du jeu, eux, ne partent jamais.

## 4. Ce que ces jeux disent de la puce (registres)
Sources : code ci-dessus, plus le test « COMMUNICATION TEST » (HSCI/LSCI) de la bibliothèque de test System 2 (Dirt Fox maître 0xE67A/0xE6D0, identique dans une vingtaine de jeux, § 5).
Ce test fait un bouclage TX → RX par scrutation : il écrit n mots en TX, RXSIZE = TXSIZE = n, attend TXSIZE.b = 0, puis vérifie trois choses :
- `RXOFFSET − 0x1000 == TXOFFSET` ;
- `RXSIZE.b == 0` ;
- les données reçues (& 0x1FF).

| Registre | Comportement attendu |
|---|---|
| STATUS | bit 2 = **émetteur libre**, lu en direct : 1 au reset (sinon « SCI ERROR »), 0 pendant une émission. bit 1 = trame reçue (cause d'IRQ, testée en premier par FT/S8H/DF). bit 0 = erreur (kyukaidk la compte). Écrire 0, ou `bclr`, efface les bits 0-1 ; le bit 2 n'est pas modifiable. |
| MODE | bit 3 toujours à 1. bit 0 à 1 dans tous les jeux System 2 qui se lient (bit 8 = fin de trame fourni par le jeu). **bit 1** = IRQ RX verrouillée/masquée. **bit 2** = IRQ « émetteur libre » verrouillée/masquée. 0x0F = tout masqué (init) ; 0x0D = RX seule (Final Lap, kyukaidk) ; 0x09 = RX + TX. **H :** la puce met le bit à 1 quand elle lève l'IRQ correspondante, le handler l'efface en dernier (cf. 3.4). |
| START | jamais utilisé par FL (toujours 0). FT/S8H : 0 après TXSIZE. DF, kyukaidk, ridgera2 : 1 avant TXOFFSET/TXSIZE, 0 après. **H :** bit 0 = « gel » ; l'émission part quand TXSIZE ≠ 0 et START bit 0 = 0. Valeur de config (écrite sous MODE 0x0F) = 3 (FT/S8H/DF), 0x11 (ridgera2). **H :** bit 1 = débit (le test alterne START 0/2 entre « HSCI » et « LSCI » ; Metal Hawk et Dirt Fox ont des chaînes « 1 MBPS / 2 MBPS »). |
| RXSIZE | **décompteur**, −1 par mot reçu (FL le remet à 0 et attend −0x14 ; le test le charge à n et attend 0). |
| TXSIZE | longueur à émettre, **décompte jusqu'à 0** quand la trame part (le test et sws attendent TXSIZE.b = 0). L'écriture déclenche l'émission (avec START bit 0 = 0). |
| RXOFFSET | pointeur RX de 13 bits, adresse de mot (0x1000 = début de la zone RX), post-incrémenté. L'écriture se fait en `0x1000 \| (RXOFFSET & 0xFFF)`, avec rebouclage sur 12 bits. FL écrit 0 et masque `& 0xFFF` ; FT/DF écrivent 0x1000 et le lisent sur 13 bits. |
| TXOFFSET | adresse de mot sur **13 bits dans toute la RAM** : la zone RX sert au relais (0x1000 + position). Post-incrémenté pendant l'émission (le test le relit). Jamais d'adressage en octets dans ces jeux. |
| CONTROL | jamais écrit par FL, toujours 0 dans FT/S8H/DF : le bit 8 est écrit directement par le CPU (`+0x100`, `ori #$100`). **H** (sws, ridgera2) : avec CONTROL = 3, les écritures CPU posent le bit 8 (« mot de fin ») ; avec CONTROL = 1, elles l'effacent. |
| RAM | 0x2000 × 9 bits ; le bit 8 se lit et s'écrit depuis l'hôte. Les bits 9-15 ne servent pas (FT fait `addq.b #1` sur l'octet haut d'un mot TX qui n'est jamais émis). |

## 5. Autres jeux System 2

Vingt jeux contiennent la même routine de **test de communication en bouclage** (HSCI/LSCI, test RXSIZE/TXSIZE/pointeurs/données, aucune IRQ) dans leur mode test.
Elle est aussi sur le maître de kyukaidk (0x1F872), sws (0x2C29C), sws92 (0x31404) et sws93 (0x3409C), qui ont par ailleurs un vrai link (voir plus bas).
Pour les seize jeux suivants, le C139 ne sert à rien en jeu :
- assault (0xCEB8) ;
- bubbletr (0x32EA, chaînes « SCI ERROR ») ;
- burnforc (0x2E9C) ;
- cosmogng (0x2284) ;
- dsaber (0x31A4) ;
- finehour (0x2924) ;
- gollygho (0x32D8) ;
- marvland (0x303A) ;
- mirninja (0x1EEAA) ;
- ordyne (0x21CEA) ;
- phelios (0x2710E) ;
- rthun2 (0x15BFA sur le maître, copie en 0x15BFE dans l'esclave) ;
- sgunner (0x235D2) ;
- sgunner2 (0x38DCE) ;
- valkyrie (0x3E2C6) ;
- Dirt Fox (0xE67A, maître).

Les autres « accès » relevés par `c139scan.py` dans ces jeux sont des constantes de données (`ori.l #$48xxxx`, `btst …$480400`).

- **assault, burnforc, cosmogng, dsaber, finehour, gollygho, marvland, mirninja, ordyne, phelios, rthun2, sgunner, sgunner2, valkyrie, bubbletr** : pas de link. Seulement le COMMUNICATION TEST de l'écran de test, qui demande un bouclage TX → RX.
  Sans bouclage, MAME doit afficher une erreur ; c'est sans conséquence.
- **luckywld** : aucun accès au C139 (le seul relevé, 0x22098, est une donnée). Pas de link.
- **metlhawk** : aucun accès aux registres. La RAM C139 n'apparaît que dans la table du test RAM (0x1CBA).
  Les chaînes « SCI OK », « 1 MBPS / 2 MBPS », « LSCI FLAME ERROR », « LSCI NOT CONNECT » (0xA80-0xBC0) existent, mais aucun code trouvé ne pilote la puce.
  H : link prévu mais pas utilisé, ou piloté par un adressage que le balayage n'a pas vu.
- **kyukaidk** (Kyuukai Douchuuki) : **vrai link**, piloté par l'esclave.
  - Init 0x13AE : registres = {0, 0x0F, 0, 0, 0, 0, 0x1000, 0}, SCI niveau 4, puis `bclr #1, MODE` → 0x0D.
  - IRQ 0x16C2 : `bclr #1, STATUS`, acquittement C148, `bclr #1, MODE`, `bclr #0, STATUS` (erreurs).
  - Trames de longueur variable ; le dernier mot (bit 8) = n° de machine 0-14 (0xF = diffusion) et un type.
  - Lecture **en avant** depuis un pointeur logiciel jusqu'à RXOFFSET.
  - Relais logiciel avec compteur de sauts (0x18BC : TXOFFSET = 0x1000 | position, limite 0xF), START bit 0 levé autour de TXOFFSET/TXSIZE.
  - Usage : H, jeu entre bornes reliées. Il confirme le modèle MODE 0x0D / bit 1 de Final Lap.
- **sws, sws92, sws93** (Super World Stadium) : **vrai link** sur l'esclave (0x254-0x874, même code dans les trois).
  - MODE 0xFC (0x0C + bits hauts), CONTROL 1/3.
  - Trames fixes de 0x48 mots, dans 16 zones de 0x80 mots en TX.
  - Avant chaque émission, le jeu attend `TXSIZE.b == 0`.
  - Relais logiciel : copie dans la zone TX du créneau, puis réémission.
  - N° de machine 0-3 (réglage), ID tiré du produit des registres C116 (0x5B6, même piège que Final Lap).
  - H : jusqu'à 4 bornes. Le mot de fin (bit 8) est posé avec CONTROL = 3 (0x728 : `CONTROL = 3 ; clr.w $8E(a0)`).

## 6. Ce que le C139 doit faire pour ces jeux
1. **Ne rien relayer, ne rien filtrer, ne rien modifier.**
   - Chaque trame reçue va dans la RAM RX, y compris les trames de la machine elle-même qui ont fait le tour.
   - Le jeu relaie ce qu'il veut en pointant TXOFFSET dans la zone RX.
   - L'« ID d'anneau » (`m_linkid`), le relais automatique, `m_buffer[0x1ff]` et la réécriture de w2 en mode 9 sont à supprimer pour ces modes.
   - H, à vérifier sur les jeux S22 en mode 09 (acedrive, victlap) : s'ils relaient eux-mêmes comme Four Trax, la même règle vaut pour eux.
2. **Émission**
   - Elle part quand TXSIZE ≠ 0 et START bit 0 = 0, évalué aux écritures de TXSIZE et de START.
   - La puce prend les mots `RAM[(TXOFFSET + i) & 0x1FFF]` sur 9 bits, tels quels, sans ajouter de bit 8.
   - Elle s'arrête après TXSIZE mots, ou (H) au premier mot qui porte le bit 8 quand MODE bit 0 = 1.
   - En fin d'émission : TXOFFSET += n, TXSIZE = 0.
   - Il faut **capturer les données à l'écriture** (émission instantanée) : avec l'émission différée, les trames sont écrasées (§ 2.8).
     Si l'émission prend du temps, STATUS bit 2 doit être à 0 pendant ce temps.
3. **Réception**
   - Un paquet = une trame. Chaque mot va en `RAM[0x1000 | (RXOFFSET & 0xFFF)]` ; ensuite RXOFFSET++ (bit 12 conservé, 12 bits bas qui rebouclent) et RXSIZE−−.
   - Ensuite STATUS |= 2 et IRQ RX si MODE bit 1 = 0 (H : la puce met alors MODE bit 1 à 1).
   - **Une seule trame par IRQ.** Les trames suivantes restent en file réseau, sans être perdues, jusqu'au réarmement par le jeu.
   - Le réarmement est l'écriture de MODE avec le bit 1 à 0 : c'est la dernière écriture de tous les handlers.
     Il ne faut pas se fier à `STATUS = 0` : Four Trax l'écrit **avant** l'acquittement C148, une IRQ levée à ce moment-là serait effacée.
   - Final Lap exige que RXSIZE vaille exactement −longueur (octet bas) à chaque IRQ.
   - Optionnel (H) : effacer le bit 8 du mot en RXOFFSET après chaque trame, pour qu'un mot de fin périmé ne soit pas pris par la recherche à rebours de FT/S8H/DF. Ce mot ne fait partie d'aucune trame lue par les jeux étudiés.
4. **IRQ « émetteur libre »**
   - Elle est levée (STATUS bit 2) quand l'émetteur est libre et MODE bit 2 = 0, donc dès que le jeu efface ce bit.
   - H : la puce met alors MODE bit 2 à 1, pour éviter la tempête d'IRQ de Dirt Fox.
   - Jamais en MODE 0x0D : le handler de Final Lap prend toute IRQ pour une réception.
5. **STATUS**
   - Bit 2 vrai au reset et chaque fois que l'émetteur est libre (attente « SCI ERROR » à l'init).
   - Écrire efface les bits 0-1 sans toucher au bit 2.
6. **Pas d'adressage en octets** pour START = 3 : TXOFFSET et RXOFFSET sont des adresses de mot.
   MODE 0x0F = « tout masqué » (on y écrit la config) ; ce n'est pas un mode d'émission.
7. **Côté MAME, régler les bornes**
   - Final Lap 1/2/3 : un n° de PCB **différent et non nul** par instance, octets 0x10-0x11 du fichier NVRAM.
     FL2/FL3 peuvent aussi le re-tirer en test ; FL1 ne peut pas.
     Autre piste côté pilote : faire varier les registres C116 6/7 d'une instance à l'autre. Mais MAME est déterministe, deux instances identiques tireraient le même n°.
   - Four Trax / Suzuka : GRID NO distincts (GAME OPTIONS, octet 8 ou 9 du fichier NVRAM).
   - Dirt Fox : CAR TYPE distincts.
   - Maximum 8 bornes dans tous ces jeux.
8. **Débit** (H) : 1 ou 2 Mbit/s selon les chaînes. Les jeux n'ont besoin d'aucune temporisation réaliste, à condition qu'aucune trame ne soit perdue ni écrasée.

## 7. Points ouverts
- Rôle exact de START = 3 en config, et des bits hauts de MODE (sws 0xFC, alpinr2b 0xFD).
- En mode 9 bits, l'émission s'arrête-t-elle au bit 8 (TXSIZE 0x2C/0x3F) ? Sans effet sur ces jeux, mais à trancher sur une vraie carte.
- La puce met-elle MODE bit 1/bit 2 à 1 quand elle lève l'IRQ ? Dirt Fox en a besoin ; à vérifier en lançant Dirt Fox en link avec le modèle proposé.
- Valeur de l'octet haut non câblé de `c116_r` (0xFF ou 0), qui donne l'ID 0x0001 ou 0xFE01 de Final Lap 1 (sans conséquence : il est identique partout).
- Contenu des NVRAM par défaut de finalap2/finalap3/dirtfoxj (PCB NO / CAR TYPE), non vidées ici.
