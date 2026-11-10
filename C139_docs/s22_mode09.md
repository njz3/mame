# C139 « mode 09 » sur System 22 / Super 22 : acedrive, victlap, cybrcomm, adillor, cybrcycc

Analyse statique des dumps `dumps/<jeu>/maincpu.bin` (capstone, mode 68020). Les adresses sont des offsets dans la ROM
programme remise en big-endian. Les variables de travail sont notées `-$xxxx(a6)` comme dans le code.
`TXRAM[n]` / `RXRAM[n]` = demi-mot n de la RAM du C139 (TX : mots 0x000-0xFFF, RX : mots 0x1000-0x1FFF).

Piège d'outil : capstone n'affiche pas le facteur d'échelle d'un index « brief » quand il y a un déplacement
(`move.w d2,$2(a2,d1.w)` à cybrcycc 0xB5E est en réalité `$2(a2,d1.w*4)`, extension 0x1402). Les lectures ci-dessous ont été
vérifiées sur les octets bruts chaque fois que l'échelle comptait.

---

## 0. Conclusions principales

1. **Le C139 ne relaie rien et n'a pas d'identifiant d'anneau.** Aucun des cinq jeux n'écrit d'ID dans la puce (les 8 registres
   sont chargés depuis des tables constantes). Ce sont **les jeux qui relaient** : à la réception, ils incrémentent un compteur
   de sauts dans la trame en RAM RX, corrigent la somme, puis réémettent la trame (TXOFFSET pointé dans la zone RX pour
   acedrive / victlap / cybrcomm / adillor, copie dans la zone TX pour cybrcycc). Chaque jeu retire de l'anneau ses propres
   trames et jette celles qui ont fait trop de sauts.
2. **Le relais du jeu passe par l'interruption « émetteur libre »** (STATUS bit 2), que MAME ne génère jamais. Dans MAME, le
   chemin d'émission des jeux est donc du code mort. C'est le relais automatique de MAME, piloté par l'ID dérivé de
   `remotehost:remoteport` et le compteur de l'octet 0x1FF, qui le remplace. D'où la dépendance aux ID.
3. **Le registre 1 (« MODE ») est un masque d'interruptions.** L'écran COMMUNICATION de cybrcycc nomme les huit registres :
   `IRQ MSK B-9 S-M R-C S-C R-AD S-AD`. Bit n de MSK = 1 masque la source n de IRQ (= STATUS) : bit 0 erreur, bit 1 trame
   reçue, bit 2 émetteur libre. « 9 » et « 0x0D » ne sont pas deux modes : les jeux passent de l'un à l'autre pour démasquer
   ou masquer l'interruption d'émission. Le code de cybrcomm et d'adillor impose que **la puce remette elle-même le bit 2
   de MSK à 1 quand elle lève l'interruption d'émission** (voir § 1.3).
4. **Nombre de bornes** : chaque jeu le déduit du compteur de sauts de sa propre trame revenue (famille A : `LINKCT = sauts − 2`
   = nombre d'*autres* bornes), ou du nombre de nœuds actifs (cybrcycc). La puce n'y contribue pas.
5. **ID identiques dans MAME** (acedrive, victlap) : aucune trame n'est relayée, la trame d'une borne ne lui revient jamais,
   donc LINKCT = 0 des deux côtés alors que chacune reçoit encore les données de l'autre (anneau de 2). Le salon démarre
   quand « voitures inscrites − 1 = LINKCT » : la première borne qui s'inscrit voit 1 voiture et part seule ; la seconde voit
   2 voitures (dont une déjà en course), attend le minuteur et affiche « ENTRY: 1 CAR ».
6. Maximum : acedrive 8, victlap 8, cybrcomm 2, adillor 4, cybrcycc 4 bornes.

---

## 1. Ce que les jeux disent des registres

### 1.1 Noms Namco des registres
cybrcycc, écran COMMUNICATION (chaînes vers 0x16F0, affichage 0x158E-0x1608 dans l'ordre des registres 0 à 7) :

| Reg | Adresse S22 / SS22 | Nom MAME | Étiquette Namco | Lecture proposée |
|---|---|---|---|---|
| 0 | 0x20020000 / 0x420000 | STATUS | **IRQ** | drapeaux d'interruption : b0 erreur, b1 trame reçue, b2 émetteur libre |
| 1 | +2 | MODE | **MSK** | masque d'interruptions (1 = masqué) |
| 2 | +4 | CONTROL | **B-9** | « bit 9 » ? toujours 0 dans ces jeux |
| 3 | +6 | START | **S-M** | « send mode » ? b0 = émission retenue (1) / libre (0) |
| 4 | +8 | RXSIZE | **R-C** | compteur réception ; jamais lu par ces jeux |
| 5 | +A | TXSIZE | **S-C** | compteur d'émission en mots, décompté jusqu'à 0 |
| 6 | +C | RXOFFSET | **R-AD** | adresse d'écriture RX (mot suivant le dernier reçu) |
| 7 | +E | TXOFFSET | **S-AD** | adresse du premier mot à émettre (13 bits, toute la RAM) |

Deuxième ligne de l'écran : `MY RIRQ SIRQ E-ER T-ER` = ID propre + 1, compteur d'IRQ de réception ($E0A628),
**compteur d'IRQ d'émission** ($E0A62A), erreurs « pas de bit de synchro » ($E0A62C), erreurs « mauvais en-tête » ($E0A62E).
Le jeu compte donc lui-même des interruptions d'émission.

### 1.2 Usage commun des registres
- **Initialisation** : deux blocs de 8 mots écrits dans l'ordre 0…7, avec MSK = 0x0F (tout masqué) pendant le premier ;
  RAM du C139 effacée entre les deux (0x2000 mots).
- **CONTROL (B-9)** : toujours 0. Ces jeux n'utilisent pas le mécanisme CONTROL 1/3 de ridgera2.
- **START (S-M)** : l'émission démarre quand S-C ≠ 0 et S-M bit 0 = 0.
  - acedrive et victlap ont START = 0 en permanence : l'écriture de TXSIZE lance l'émission. Leur gestionnaire réécrit START = 0 à chaque passage.
  - cybrcomm et adillor : START = 1, puis TXSIZE, puis START = 0. L'init laisse START = 3, émission retenue.
  - cybrcycc : START = 0 depuis l'init ; écrit TXSIZE = 0, puis TXOFFSET, puis TXSIZE = n.
- **Valeur START écrite pendant MSK = 0x0F** (`m_reg_f3` de MAME) : 3 pour acedrive, victlap, cybrcomm et adillor ; 0 pour cybrcycc ;
  pour comparaison, 0x11 pour ridgera2. Signification inconnue (débit ? sélection d'horloge 12/16 MHz ?).
  **Elle ne change pas l'unité de TXOFFSET** : acedrive (f3 = 3) et ridgera2 (f3 = 0x11) écrivent tous deux des indices de mots,
  y compris 0x1000+x pour pointer dans la zone RX. Le `tx_offset >>= 1` de MAME quand `f3 & 2` est un artefact.
- **TXOFFSET (S-AD)** : indice de mot sur 13 bits. Le jeu le réécrit avant **chaque** trame, depuis son gestionnaire
  « émetteur libre ». Aucun jeu ne compte sur un auto-incrément. Les « += 0x100 » et « += subOffset*2 » de MAME compensent
  seulement l'absence de cette interruption.
- **TXSIZE (S-C)** : nombre de mots. Il redescend à 0 en fin d'émission : cybrcycc lit `TXSIZE.b` et n'émet que s'il vaut 0 (0xB84) ;
  ridgera2 compte une erreur s'il n'est pas nul à la fin de l'émission. Exact pour cybrcomm, adillor et cybrcycc ;
  **longueur + 1** pour acedrive (0x3A pour 0x39 mots) et victlap (0x3B pour 0x3A).
- **RXOFFSET (R-AD)** : lu en entrée de gestionnaire.
  - cybrcycc lit le dernier mot reçu en `(RXOFFSET − 1) & 0xFFF` : RXOFFSET pointe donc sur le mot **suivant**.
  - La famille A balaie à rebours **à partir de** RXOFFSET, sur une fenêtre de longueur + 1 mots : le décalage d'un mot y entre.
  - Les jeux ne gardent que 12 bits : `(2·RXOFFSET & 0x1FFF) | 0x2000` en octets.
- **Bit 8 = bit de synchro, posé par le jeu** : `ori.w #$100` sur le dernier mot de la trame, la somme de contrôle
  (acedrive 0x29032, victlap 0x27F26, cybrcomm 0xF9BA, adillor 0x88F0) ou le mot de longueur (cybrcycc 0xD52).
  En réception, les jeux retrouvent la fin de trame en cherchant le bit 8 dans la RAM RX. La puce doit donc transporter et
  stocker 9 bits par mot. Elle n'a pas à poser ce bit elle-même.
- **RAM 9 bits** : les jeux rangent des variables dans des mots TX libres (TXRAM[0x10], [0x11], [0x12], [0x20], [0x21], [0x22])
  en coupant les masques de 16 bits en deux octets. C'est cohérent avec une RAM de 8K × 9 bits.

### 1.3 Interruptions : ce que le code impose
Fin de gestionnaire, dans chaque jeu :
- acedrive / victlap : `MSK = (pending ≠ 0) ? 9 : 0x0D` (0x29294 / 0x28188), après l'acquittement du contrôleur système.
- cybrcycc : `MSK = (file non vide) ? 9 : 0x0D` (0xBD8-0xBF8).
- cybrcomm / adillor : lecture-modification-écriture.
  - Réception : `MSK &= 9` (0xFC58 / 0x8BCA).
  - Émission : `MSK &= 9` s'il reste des trames (0xFA92 / 0x89DA), sinon `MSK &= 0x0D` (0xFABC / 0x8A04).
- Trame propre construite au VBLANK : `MSK &= 0x0B` (acedrive 0x29044/0x29050, victlap 0x27F38/0x27F44, cybrcomm 0xF9E2, adillor 0x8924).
  cybrcycc écrit MSK = 9 (0xD76).

Conséquences :
- **STATUS bit 2 = émetteur libre, en niveau.** Il n'est pas effacé par l'écriture de 0 dans STATUS : cybrcycc efface STATUS en entrée
  du chemin réception, puis compte sur une IRQ d'émission après `MSK = 9`.
  Il vaut 1 après reset : acedrive 0x28AF6 et victlap 0x279E0 l'attendent (10000 essais) avant l'init, sinon boucle infinie.
- **Une IRQ d'émission est demandée en démasquant le bit 2 de MSK pendant que l'émetteur est libre.** C'est ainsi que le VBLANK
  relance l'envoi après une période sans émission.
- **La puce doit remettre MSK bit 2 à 1 quand elle lève l'IRQ d'émission.** cybrcomm et adillor ne mettent jamais ce bit à 1
  eux-mêmes. En fin de file ils font `MSK &= 0x0D`, et au VBLANK `MSK &= 0x0B`. Si la puce ne touchait pas au bit 2, ces deux
  opérations écriraient la même valeur (9) :
  - soit les deux relancent une IRQ, et la fin de file tourne en boucle d'interruptions ;
  - soit aucune ne la relance, et les trames du VBLANK ne partent jamais.

  Le seul modèle cohérent : la levée de l'IRQ d'émission positionne MSK bit 2 ; `&= 0x0D` le conserve (rien à envoyer), `&= 9`
  et `&= 0x0B` le remettent à 0 (une nouvelle IRQ quand l'émetteur sera libre). Pour acedrive et cybrcycc, qui écrivent 9 ou
  0x0D explicitement, le comportement est le même.
- Le bit 1 de MSK est remis à 0 à **chaque** sortie de gestionnaire, chemin émission compris, alors qu'aucun jeu ne le met à 1.
  *Hypothèse* : la puce masque de la même façon la réception (MSK bit 1 = 1 à la levée de l'IRQ de réception).
  MSK bits 0 et 3 restent à 1 dans ces jeux, donc l'interruption d'erreur est masquée. ridgera2, qui a MSK = 8, traite l'erreur.
- Le gestionnaire teste le bit 1 (réception) **avant** le bit 2. Si les deux sont présents, il ne traite que la réception, puis
  redémasque l'émission s'il a des trames en attente, ce qui provoque une nouvelle IRQ.
- Acquittement du contrôleur système : S22 `clr.b $40000007` (acedrive 0x2929E, victlap 0x28192 ; cybrcomm écrit le mot `$40000007`),
  Super 22 : écriture en 0x700006 (cybrcycc 0xBD2, adillor 0x89D4/0x89FE/0x8BC4).
  Tous acquittent **avant** la dernière écriture de MSK : une IRQ relevée par cette écriture n'est pas perdue.

Conséquence pour les « modes » de MAME (hypothèse plus large) : 8, 9, 0x0C et 0x0D sont des masques.
- 0x0C = 8 avec l'émission masquée.
- 0x0D = 9 avec l'émission masquée.
- 0x0F = tout masqué (init, menus de test : acedrive 0x36E72 et 0x36F58, victlap 0x3DC4E et 0x3DD34).

Le `switch (m_reg[MODE])` de MAME est donc à revoir pour toutes les familles.

---

## 2. Famille A : acedrive, victlap, cybrcomm, adillor

Quatre implémentations du même protocole :
- chaque borne possède un ou plusieurs **slots** (emplacements de trame dans la RAM TX, indexés par numéro de voiture) ;
- une trame porte le numéro du slot, un octet magique, un **compteur de sauts**, la charge utile et une somme de contrôle sur 8 bits avec le bit 8 ;
- chaque récepteur valide la trame, incrémente les sauts et la somme en RAM RX, copie la charge dans la boîte aux lettres du slot, puis
  **réémet la trame directement depuis la zone RX** (TXOFFSET = 0x1000 + position) ;
- une borne retire ses propres trames et en déduit LINKCT (nombre d'autres bornes) = sauts − 2.

### 2.1 Paramètres

| | acedrive | victlap | cybrcomm | adillor |
|---|---|---|---|---|
| carte / base C139 | S22 0x2002xxxx / 0x2001xxxx | S22 | S22 | Super 22 0x42xxxx / 0x41xxxx |
| init | 0x28AF6 (appel 0x53A) | 0x279E0 (appels 0x576, 0x5CCE) | 0xF8A2 (appel 0x42A8) | 0x87D0 (appel 0x42FE) |
| table des registres | 0x28B22 | 0x27A0C | 0xF882 | 0x87B0 |
| bloc 1 (0…7) | 0,0F,0,3,0,0,1000,1000 | idem | idem | idem |
| bloc 2 (0…7) | 0,9,0,**0**,0,0,1000,1000 | idem | 0,9,0,**3**,0,0,1000,1000 | 0,9,0,**3**,0,0,1000,1000 |
| après init | MSK = 0x0D (0x5D8) | MSK = 0x0D (0x624) | MSK &= 9 après une attente (0xF914) | MSK &= 9 (0x8848) |
| IRQ SCI | niveau 6, 0x78 → 0xA46 → **0x2907E** | niveau 6, 0xAC6 → **0x27F72** | niveau 6, **0xFA0C** | niveau 4, **0x894E** |
| VBLANK → link | niveau 5, 0xA00 → 0x28BD6 | 0xA68 → 0x27AC0 | niveau 5 (0x4802) ou 4 (0x4A7A) selon la phase (0x40000004 = 0x35 / 0x34), tous deux → 0xFCAA puis 0xF92C | niveau 2, 0x4A00 → 0x855C puis 0x8860 |
| slots | 16, mot 0x800 + 0x80·s | 16, 0x800 + 0x80·s | 8, mot 0x800 + 0x100·s | 16, 0x800 + 0x80·s |
| trame (mots) | **0x39** | **0x3A** | **0xDE** | **0x4A** |
| TXSIZE écrit | **0x3A** | **0x3B** | 0xDE | 0x4A |
| w0 / w1 | slot / **0x8D** | slot / **0x9C** | slot / **0x94** | slot / **0xB0** |
| w2 | sauts (1 à l'émission) | idem | idem | idem |
| w3 | n° PCB de l'émetteur | idem | charge | charge |
| w4 | TXRAM[0x22] (toujours 0) | idem | charge | charge |
| charge | w5-w0x37 avec un octet de contrôle (somme8 ^ 0x5A) tous les 16 mots | w5-w0x38, idem | w3-w0xDC | w3-w0x48 |
| somme | w0x38 = somme8(w0..w0x37) \| 0x100 | w0x39 | w0xDD | w0x49 |
| sauts max | < 16 | < 16 | < 8 | < 16 |
| bornes max | 8 | 8 | 2 | 4 |

### 2.2 acedrive : pseudo-code commenté

```c
// ---- Boot : 0x524 efface la RAM C139 (0x20010000-0x20014000), puis jsr 0x28AF6
c139_init():                                  // 0x28AF6
    STATUS = 0; START = 0;
    for (i = 0; i <= 10000; i++)              // 0x28B06 : attend « émetteur libre »
        if (STATUS & 4) goto ok;
    for (;;) ;                                // 0x28B16 : blocage si la puce ne répond pas
ok: regs[0..7] = {0, 0x0F, 0, 3, 0, 0, 0x1000, 0x1000};  // 0x28B42, MSK = 0x0F, START = 3 → m_reg_f3 = 3
    RAM[0..0x1FFF] = 0;                       // 0x28B54
    regs[0..7] = {0, 9, 0, 0, 0, 0, 0x1000, 0x1000};      // 0x28B66
    txptr[0..15] = 0xFFFF;  txMask = 0xFFFF;  pending = 0; // -$72E0, -$7290, -$72E8
    ... ; clr.b $40000007;                    // acquittement SCI
// 0x5D8 : MSK = 0x0D (émission masquée), puis 0x5E6 : contrôleur système, SCI niveau 6 (0x36 en 0x40000002)

// ---- À chaque VBLANK (60 Hz) : 0xA00 → jsr 0x28BD6
link_vblank():                                // 0x28BD6
    if (ringTimer) ringTimer--;               // -$717A, rechargé à 15 au retour de la trame propre
    ownMask = (ownMask & 0xFF00) | bit(pcbId);   // -$72EE ; pcbId = -$73FC ; octet haut = slots 8-15 pris dynamiquement
    LINKCT  = ringTimer ? (TXRAM[0x10] & 0xFF) : 0;   // -$73F8 : nombre d'AUTRES bornes de l'anneau
    TXRAM[0x21] = ownMask; TXRAM[0x20] = ownMask >> 8;
    seenMask = TXRAM[0x11] << 8 | (TXRAM[0x12] & 0xFF);  // -$72F0
    for (s = 0; s < 16; s++)                  // table des voitures -$4000(a6) + 0x100·s
        if (ownMask & bit(s))  build_payload(s); // 0x28C4A : w5..w0x37 du slot s, octets de contrôle ^0x5A
        else if (seenMask & bit(s)) copy_mailbox(s); // 0x28E1C : copie protégée par compteur
                                              // (octet haut de w0 du slot, comparé avant et après)
    conflicts = OR(histo[-$71DE]);  ...       // 0x28F7C : slots 8-15 réclamés par une borne de n° supérieur
    if (++vbl & 1) TXRAM[0x11..0x12] = OR(historique des réceptions -$72B0[16], décalé) ;  // 0x28FA6
    txMask = TXRAM[0x20] << 8 | TXRAM[0x21].b;  // -$7290
    for (s = 15; s >= 0; s--) if (txMask & bit(s)) {   // 0x28FE0
        txptr[s] = 0x1000 + 0x100*s;          // octets ; TXOFFSET = mot 0x800 + 0x80·s
        w = &RAM[0x800 + 0x80*s];
        w[0] = s; w[1] = 0x8D; w[2] = 1;  w[3] = pcbId; w[4] = TXRAM[0x22];
        sum = 0; for (i = 0; i < 0x38; i++) { w[i] &= 0xFF; sum += w[i]; }
        w[0x38] = (sum & 0xFF) | 0x100;       // bit de synchro sur le dernier mot
        pending |= bit(s);
        MSK &= 0x0B;                          // 0x29044 : demande une IRQ « émetteur libre »
    }
    MSK &= 0x0B;                              // 0x29050
    update_cars();                            // 0x292B4

// ---- IRQ SCI : 0x2907E (niveau 6)
sci_irq():
    irqCount++;                               // -$7178
    st = STATUS;
    if (st & 2) goto rx;
    if (st & 4) {                             // émetteur libre : envoie le slot suivant, tourniquet
        for (n = 0, r = rr & 15; n < 16; n++, r = (r + 1) & 15)
            if (pending & bit(r)) {
                pending &= ~bit(r);
                TXOFFSET = txptr[r] >> 1;     // indice de mot : slot TX (0x800+) OU trame en zone RX (0x1000+)
                TXSIZE   = 0x3A;              // 0x290C8 : trame de 0x39 mots + 1
                break;
            }
        rr = r + 1;                           // -$72C0
    }
    START = 0;                                // 0x290E0
    goto out;                                 // pas d'écriture de STATUS sur ce chemin
rx: p = (2*RXOFFSET & 0x1FFF) | 0x2000;       // octets dans 0x2001xxxx, zone RX
    for (n = 0; n <= 0x39; n++, p -= 2)       // 0x2910A : remonte jusqu'à 0x3A mots pour trouver le bit 8
        if (RAM16(p) & 0x100) goto found;
    goto err;
found:
    start = p - 0x38*2;  frameptr = start;    // -$72BC
    slot = w0;  sum = w0;                     // -$72B9
    if (w1 != 0x8D) goto err;                 // 0x2914C
    hops = w2 + 1;  w2 = hops;                // 0x29160 : réécrit en RAM RX
    if (hops >= 16) goto err;
    if (txMask & bit(slot)) {                 // l'un de NOS slots : jamais relayé
        if (w3 == pcbId) {                    // notre trame a fait le tour
            TXRAM[0x10] = max(hops - 2, 0);   // 0x291AA : LINKCT = N - 1
            ringTimer = 15;
        } else if (w3 > pcbId)                // une borne de n° supérieur réclame le même slot
            conflicts |= bit(slot);
        goto ack;
    }
    check w3, w4, puis 0x33 mots w5..w0x37 (somme ^ 0x5A tous les 16) → sinon err
    if ((sum8(w0..w0x37) & 0xFF) != w0x38.b) goto err;   // somme calculée avec les sauts d'ORIGINE
    w0x38 = ((sum + 1) & 0xFF) | 0x100;       // 0x29230 : corrige la somme pour les sauts + 1
    RAM[0x805 + 0x80*slot ...] = charge;      // 0x29240 : boîte aux lettres = slot TX non émis
    RAM8[0x20011000 + 0x100*slot] += 1;       // 0x2925E : compteur de cohérence (octet haut de w0)
    rcv[slot] = 0xFF;                         // -$7170
    txptr[slot] = frameptr;                   // 0x29270 : RELAIS depuis la zone RX
    pending |= bit(slot);  seenHist |= bit(slot);
    goto ack;
err: errCount++;                              // -$7174
ack: STATUS = 0;                              // 0x2928C
out: clr.b $40000007;                         // acquittement SCI
     MSK = pending ? 9 : 0x0D;                // 0x292A8
     rte;
```

Overlay de débogage (0x289F8, appelant non localisé) : il affiche `LNK CNT` = -$73F8, `LNK ERR` = -$717A (minuteur), `LNK SENBIT` = -$72EE et `LNK RECV` = -$72B0.

**Salon (entry)** :
- 0x4D38 : minuteur = 0x4B0 VBLANK (20 s).
- Boucle 0x4DE8-0x4E7C :
  - phase propre = 1 (-$63E0), groupe = le plus petit vu (0x29B5C) ;
  - `n = voitures inscrites` (0x29B90 : entrées 0-7 de la table des voitures avec état $10 ≠ 0 et phase $4E ≠ 0) ;
  - **départ si `n − 1 == LINKCT` (0x4E6C)**, sinon dès que le minuteur expire.

  Une borne qui se croit seule (LINKCT = 0) part donc dès qu'elle est seule inscrite. Une borne qui voit déjà une autre voiture
  (en course) alors que LINKCT = 0 attend le minuteur.
- Le nombre affiché derrière « ENTRY : n CAR » (chaîne 0x9793) vient probablement de -$63D0 (0x2995A : voitures de même phase et de même groupe).
  *Hypothèse* : la routine d'affichage n'a pas été trouvée.
- Maître du temps : 0x1FA2, le plus petit n° de slot vu en dessous du sien sert de référence.

### 2.3 victlap
Même code qu'acedrive, à quelques constantes près :
- init 0x279E0, table 0x27A0C ; MSK = 0x0D en 0x624, SCI niveau 6 en 0x642 ;
- VBLANK 0x27AC0 ; construction des trames 0x27EC0-0x27F44 ;
- IRQ 0x27F72 :
  - réception en 0x27FE0, balayage de 0x3B mots ;
  - début = sync − 0x39 ; w1 = 0x9C (0x28040) ; sauts en 0x28054 ;
  - trame propre et LINKCT en 0x28064-0x280AA ;
  - charge 0x34 mots, contrôle de la somme et correction en 0x2810A-0x28124 ;
  - relais en 0x28160-0x28176, acquittement en 0x28180, MSK en 0x2819C.
- TXSIZE = 0x3B en 0x27FBC ;
- salon en 0x408A : `compte(0x28A98) − 1 == LINKCT`.

### 2.4 cybrcomm
Même principe, autre programmeur ; adillor reprend ce code presque à l'identique.

```c
c139_init():                                  // 0xF8A2
    $40000019 = 1;
    regs = {0,0x0F,0,3,0,0,0x1000,0x1000};    // 0xF8B6
    RAM = 0;
    regs = {0,9,0,3,0,0,0x1000,0x1000};       // 0xF8D6 : START = 3, émission retenue
    txptr[0..7] = 0xFFFF; txMask = 0xFFFF; pending = 0;   // $1001B9C6, $1001BA0E, $1001B9C4
    attente de 0x7000 boucles; MSK &= 9; acquittement SCI ($40000007)    // 0xF914

vblank (0x4A7A au niveau 4, 0x4802 au niveau 5) : jsr 0xFCAA ; jsr 0xF92C
0xFCAA :  ownMask = bit(cpuNo) | extra(-$7D56);  TXRAM[0x21] = ownMask; TXRAM[0x20] = ownMask >> 8;
          seen = TXRAM[0x11] << 8 | TXRAM[0x12].b;  nSeen = popcount(seen) (-$7D72);
          LINKCT = TXRAM[0x10] & 0xFF (-$7D74);   // persistant : aucun minuteur
          for (s = 0; s < 8; s++) : slot propre → charge en w3.. (0xFD54) ; slot vu → copie de la trame RX mise en file,
                                    somme revérifiée (0xFE32-0xFECA)
0xF92C :  tous les 16 VBLANK : TXRAM[0x12] = vus.b ; TXRAM[0x11] = vus >> 8 ; vus = 0
          txMask = TXRAM[0x20] << 8 | TXRAM[0x21].b;
          pour chaque slot s de txMask : txptr[s] = 0x1000 + 0x200*s ; w0 = s ; w1 = 0x94 ; w2 = 1 ;
                                         w0..w0xDC &= 0xFF ; w0xDD = somme8 | 0x100 ; pending |= bit(s)
          if (pending) MSK &= 0x0B;           // 0xF9E2

sci_irq():                                    // 0xFA0C
    st = STATUS;
    if (st & 2) goto rx;
    if (!(st & 4)) { ack; MSK &= 0x0D; rte; }
    pour 8 slots en tourniquet : si pending & bit(r) {
        pending &= ~bit(r);
        TXOFFSET = txptr[r] >> 1;  START = 1;  TXSIZE = 0xDE;   // 0xFA56-0xFA64
        if (pending == 0) { START = 0; ack; MSK &= 0x0D; rte; }  // dernière trame : l'émission reste masquée
        START = 0; ack; MSK &= 9; rte;                            // il en reste : redemande une IRQ
    }
    ack; MSK &= 0x0D; rte;
rx: rxIrq++;  remonte jusqu'à 0xDF mots depuis RXOFFSET pour trouver le bit 8 (0xFAEC) ; sinon → ack
    début = sync − 0xDD ; slot = w0 ;  w1 != 0x94 → drop
    sauts = w2 + 1 ; w2 = sauts ; sauts >= 8 → drop
    if (txMask & bit(slot)) { -$7D76 = 0; TXRAM[0x10] = max(sauts-2, 0); goto drop; }   // 0xFB6C-0xFB88
    met en file (pointeur, somme partielle ; double tampon par slot en -$7D00) ;
    somme w3..w0xDC ; (w0xDD + 1) != somme → drop ; w0xDD = (w0xDD + 1) | 0x100 ;        // 0xFBF6-0xFC1E
    txptr[slot] = début ; pending |= bit ; vus |= bit ;                                  // 0xFC20-0xFC44
ack: STATUS = 0; acquittement SCI; MSK &= 9; rte;        // 0xFC4A
drop: vus &= ~bit(slot); pending &= ~bit(slot); goto ack;  // 0xFC66
```

Note : le retour de la trame propre efface aussi le bit `pending` de ce slot (chemin drop).
Si une trame propre plus récente attendait encore d'être émise, elle est perdue. C'est sans conséquence quand l'aller-retour prend moins d'un VBLANK.

### 2.5 adillor
Copie de cybrcomm, avec ces différences :
- 16 slots de 0x80 mots ;
- trame 0x4A mots, w1 = 0xB0, TXSIZE = 0x4A ;
- seuil de sauts 16 ;
- IRQ en 0x894E :
  - émission 0x896E-0x8A10 ;
  - réception 0x8A12 ;
  - « vu » -$7B40 marqué pour **toute** trame d'en-tête valide (0x8AA6), y compris la sienne ;
  - trame propre en 0x8AB8-0x8AE2 ;
  - somme en 0x8AF2-0x8B16 ;
  - relais en 0x8B92-0x8BB6 ;
- VBLANK : 0x855C (consommateur) puis 0x8860 (émission).

État du lien :
- -$7B74 = LINKCT (TXRAM[0x10]) ;
- -$7B72 = nombre de bits 0-3 de -$7B40 (bornes vues, remis à 0 tous les 16 VBLANK) ;
- écran de test « LINK ON » (0xB04E-0xB08E) : lié si `LINKCT + 1 == vus`, « NG » sinon.

---

## 3. Famille B : cybrcycc (Super 22, autre protocole)

```c
c139_init():                                  // 0xC06 (appel 0x738)
    STATUS=0; MSK=0x0F; CONTROL=0; START=0 (→ m_reg_f3 = 0); TXSIZE=0; RXSIZE=0; TXOFFSET=0x1000; RXOFFSET=0x1000;
    RAM C139 = 0 (0x3498) ;  TXRAM[0..0x3E7] = 0x1FF ;  file TX $E0A606[4] = {taille, offset} = 0 ; rr = 0 ; file RX = 0
    STATUS = 0; MSK = 9;
// contrôleur système : SCI niveau 4 (0x700002 = 4, en 0x822) → vecteur 0x310 → bsr 0xA5C ; rte

frame_out():                                  // 0xD00-0xD7E, boucle principale (0x8E2 → 0xCB4)
    if (!(n = payloadLen)) goto out;          // $E08402, fixé par le type de message (0xE36-0x1012)
    buf ^= 0x100;                             // $E08404 : double tampon TX, mot 0 ou 0x100
    w = &RAM[buf];  w[0] = 0x5A;  w[1] = ownId;          // ownId = $E08400 (0-3) ; sauts = 0
    w[2..n+1] = octets de charge (en-tête de 10 octets : compteur, état, type $E0A65C, session $E0A65E, ..., somme négative)
    len = n + 2;  w[len] = len | 0x100;       // le dernier mot porte la synchro ET la longueur
    queue[ownId] = { taille = len + 1, offset = buf };
out: MSK = 9;                                 // 0xD76

sci_irq():                                    // 0xA5C
    st = STATUS;
    if (st & 2) {                             // réception
        STATUS = 0;  rxIrq++;                 // effacé AVANT de lire RXOFFSET
        q = (RXOFFSET - 1) & 0xFFF;           // dernier mot reçu
        cherche le bit 8 dans RX[q], RX[q-1] … RX[q-7] ; sinon errNoSync++ ; goto out
        len = RX[q] & 0xFF;  s = (q - len) & 0xFFF;          // la longueur est dans le mot de synchro
        if ((RX[s] & 0x1FF) != 0x5A) { errMagic++; goto out; }
        h = (RX[s+1] & 0xFF) + 0x10;  RX[s+1] = h;           // sauts dans les bits 4-7 de w1
        if (h >= 0x50) goto out;              // au plus 4 sauts
        met s+1 en file pour l'analyseur (16 entrées $E0A634/$E0A636)
        node = h & 0xF;  if (node > 3) goto out;
        if (node == ownId) goto out;          // trame propre : retirée, non relayée
        copie RX[s..s+len] → TX mot 0x800 + 0x200*node ;  RX[q] = 0 (efface la synchro dans la RAM RX)
        queue[node] = { len + 1, 0x800 + 0x200*node };       // relais par COPIE dans la zone TX
    } else {
        STATUS = 0;
        if (!(st & 4)) goto out;
        txIrq++;                              // « SIRQ »
        if (TXSIZE.b != 0) goto out;          // émetteur occupé : S-C décompte jusqu'à 0
        r = premier queue[] non vide en tourniquet sur 4 ; sinon goto out
        TXSIZE = 0;  TXOFFSET = queue[r].offset;  TXSIZE = queue[r].taille;  queue[r].taille = 0;
    }
out: clr.b $700006;                           // acquittement SCI
     MSK = (une file non vide) ? 9 : 0x0D;
     rts → rte
```

Analyse et numérotation :
- **Analyseur** (0xCB4) : pour chaque entrée de la file RX, recopie les octets bas depuis w1 jusqu'au mot de synchro.
- **Messages** (0x1048-0x124A) :
  - octet 0 = nœud (bits 0-1) + sauts (bits 4-5) ;
  - en-tête de 10 octets vérifié par somme nulle ;
  - le type ($4 de la fiche du nœud, à $E0A668 + 0x100·nœud) aiguille sur 0x10BC.
- **Le type 0x0D sert à numéroter** (0x11BC-0x1228). C'est la diffusion des réglages de course, et elle est acceptée même si
  l'ID de l'émetteur vaut le sien (0x108C). Si sa session (octet 5) diffère de $E0A65E :
  - **ownId = sauts & 3** (0x11DA-0x11E6) : la position dans l'anneau mesurée depuis l'émetteur, lui-même à 0 ;
  - copie dans $E0AFAE, l'image EEPROM 0x26 ;
  - adopte les réglages $E0AFB4..$E0AFBB.

  La numérotation suppose donc que **chaque nœud qui relaie incrémente les sauts**, ce que fait le gestionnaire du jeu.
- Nombre de nœuds actifs (0x124C-0x1282) : $E0AF28 = fiches d'état ≠ 0 ; $E0AF2A = fiches en état 0x0D.

---

## 4. Position, nombre de bornes, réglages

| Jeu | Réglage (menu de test) | Stockage | Valeurs | Bornes max |
|---|---|---|---|---|
| acedrive | PCB ID NUMBER (chaîne 0x2F39E) | EEPROM 0x58000000, enregistrement de 32 octets en **0x100** (marque 0x141, somme en 0x11F). **Octet 0x111** → $418B → -$73FC (0x2D222) | 0-7 (8 voitures dans la table : 0x29AD2, 0x29B90 ; 8 couleurs d'équipe) | **8** |
| victlap | PCB ID NUMBER (0x2E77E) | enregistrement 0x100 (marque 0x156), **octet 0x10C, bits 7-5** → $414A (0x2C44E) → -$73FC (0x2C64C) | 0-7 (« 2 … 8 PLAYERS ») | **8** |
| cybrcomm | CPU NO. (0x8428) | EEPROM 0x02-0x11 → $2A00(a6) (0x6492), **octet 0x00A** → $2A10, `& 1` (0x8574) → -$7EBE (0x816A) | 0-1 | **2** |
| adillor | PCB ID (0xAF5B) | bloc -$6600(a6) lu depuis l'enregistrement 0 (0x93C4, 0x9554 ; copie de secours au n° 0x10 en 0x93E8). **Octet 7 = EEPROM 0x007** → -$7E80 (0x944E) | 0-3, affiché + 1 (0xAD5A) | **4** |
| cybrcycc | ID NUMBER (0x7077) | image EEPROM en $E0AF88 (octet n en 0x460000 + 2n). **Octet 0x026** → $E08400 (0x155DC) ; écrit par 0x6F04 | 0-3 ; **réassigné par le lien** (type 0x0D, § 3) | **4** |

Dans la famille A, la position est le numéro de PCB : c'est le slot, le bit dans les masques et w3. Si deux bornes ont le même numéro,
chacune prend le slot de l'autre pour le sien. Le nombre de bornes vient du compteur de sauts de la trame revenue.
Dans cybrcycc, la position est réglée puis recalculée d'après la distance au maître.

Le n° de PCB d'acedrive ou de victlap n'est jamais réécrit par le lien. Le retour de cab2 au n° 1 noté dans C139.md § 5
ne vient donc pas de ce code ; cybrcycc, lui, réécrit son ID.

---

## 5. Comportement avec l'émulation actuelle de MAME

### 5.1 Ce qui se passe réellement (acedrive, victlap, cybrcomm, adillor)
1. `m_irq_cb` n'est appelé que dans `read_data()` : **aucune IRQ d'émission**.
   - Le chemin émission des gestionnaires ne s'exécute jamais.
   - `pending` ne se vide jamais, et MSK reste à 9 après le premier VBLANK.
   - TXOFFSET garde sa valeur d'init (0x1000), aux corrections près.
   - Le jeu ne relaie donc rien.
2. Envoi « mode 9 » de MAME :
   - à chaque tick, si `m_txblock == 0` : `tx_offset = TXOFFSET >> 1` (car `f3 & 2`) = mot 0x800, le slot 0 ;
   - `find_sync_bit` cherche le premier bit 8 :
     - le slot propre, quand c'est le 0 ;
     - sinon le bit 8 du compteur de cohérence d'une boîte aux lettres : `+= 0x100`, ce qui avance d'un slot ;
     - sinon un bloc qui commence sur une autre boîte aux lettres, dont le récepteur garde les 0x39 derniers mots ;
   - `m_txblock` est remis à 0 par chaque écriture de MSK = 9. Les gestionnaires en écrivent après chaque réception, et le VBLANK aussi.
3. La puce MAME relaie toute trame d'ID différent. À la trame de retour chez son émetteur, elle remplace les sauts (octet 7) par le compteur
   0x1FF (`f3 & 2`) : `sauts = N`, le jeu en fait N + 1, donc LINKCT = N − 1, la bonne valeur.
   Ce chemin de retour ne vérifie pas la somme, ce qui explique que la valeur modifiée passe.
4. Le basculement 9 / 0x0D (`switch` de MAME) n'a pas d'effet : une fois `pending` non nul, MSK reste à 9.

### 5.2 ID identiques
- La puce ne relaie plus rien. Une trame reçue est prise pour la sienne : TXSIZE = 0 et octet 7 = 1, sans effet.
- Dans un anneau de 2, chaque borne reçoit quand même directement les trames de l'autre. Les voitures sont donc échangées.
- La trame propre d'une borne ne lui revient jamais :
  - le minuteur d'anneau (-$717A, 15 VBLANK) expire ;
  - **LINKCT = 0 des deux côtés** (« LINK COUNT » = 0) ;
  - cybrcomm et adillor : TXRAM[0x10] n'est jamais écrit, LINKCT = 0 ; adillor affiche « NG », car vus = 2 et LINKCT + 1 = 1.
- Salon, avec la règle « départ si inscrits − 1 == LINKCT » :
  1. la borne A s'inscrit la première, 1 inscrit, `0 == 0` : **elle part tout de suite, seule** ;
  2. B s'inscrit et compte 2 voitures, car A est en course avec $4E ≠ 0 : `1 ≠ 0`, B **attend le minuteur (jusqu'à 20 s)** ;
  3. pendant ce temps, B n'a qu'une voiture dans la phase d'inscription : **« ENTRY: 1 CAR »**.

  C'est exactement le symptôme observé.

### 5.3 Les 2 échecs sur 9 avec des ID distincts
Pistes, à vérifier dans l'ordre :
1. **Même numéro de PCB sur les deux bornes.** C'est le cas si l'EEPROM 0x111 de cab2 est revenue à 1 (piège noté dans C139.md § 5).
   - Les deux bornes prennent le slot 0.
   - La trame de l'autre arrive sur l'un de « nos » slots avec `w3 == pcbId` : elle est comptée comme trame propre revenue, avec sauts = 1
     (puce qui relaie) ou 2. TXRAM[0x10] alterne entre 0 et 1 selon la trame reçue en dernier.
   - Les données de l'autre voiture sont ignorées : 1 seul inscrit.
   - Selon la valeur de LINKCT au moment du test, départ solo ou attente sur « ENTRY: 1 CAR ».
   - Vérification : relire 0x111 (acedrive) ou 0x10C (victlap) après un échec.
2. Effets de l'envoi improvisé de MAME : blocs de 0xB9 mots, trames rejetées quand `find_sync_bit` démarre sur une boîte aux lettres.
   En temps normal ce sont des pertes isolées, sans effet sur LINKCT sauf 15 VBLANK de suite.
3. Décision du salon sur une valeur instantanée : si LINKCT est à 0 au moment où l'on s'inscrit (lien pas encore établi,
   `m_linktimer` ou sockets TCP), la borne part seule.

### 5.4 Prévisions pour les jeux non testés
- **cybrcomm, adillor** : avec des ID identiques, LINKCT = 0. Avec des ID distincts, comportement comme acedrive.
- **cybrcycc**, f3 = 0 :
  - le compteur 0x1FF et `>>1` sont inactifs, mais le chemin émission est aussi mort ;
  - MAME envoie toujours depuis TXOFFSET = 0x1000 & 0xFFF = mot 0, le tampon 0. **Les messages construits dans le tampon 0x100,
    soit un sur deux, ne partent jamais** et le tampon 0 est renvoyé deux fois ;
  - à 2 bornes, cela devrait marcher dans les deux réglages d'ID, la trame propre n'étant pas nécessaire ;
  - à 3 ou 4 bornes : avec des ID identiques, pas de relais ; avec des ID distincts, la puce relaie la trame **d'origine** et non la copie
    dont le jeu a incrémenté les sauts. Toutes les bornes non maîtres se numérotent alors 1 (type 0x0D).

---

## 6. Différences entre les cinq jeux

| | acedrive | victlap | cybrcomm | adillor | cybrcycc |
|---|---|---|---|---|---|
| Famille | A | A (copie d'acedrive) | A (variante) | A (copie de cybrcomm) | B |
| START (S-M) en marche | 0 | 0 | 3, puis 1/0 autour de chaque trame | idem cybrcomm | 0 |
| TXSIZE | longueur + 1 | longueur + 1 | longueur | longueur | longueur (= len + 1) |
| Source du relais | RX (TXOFFSET 0x1000+) | RX | RX | RX | copie en TX 0x800 + 0x200·n |
| Trame propre reconnue par | slot dans le masque **et** w3 = n° PCB | idem | slot dans le masque | slot dans le masque | nœud (w1 & 3) = ownId |
| Sauts | w2, limite 16 | w2, 16 | w2, 8 | w2, 16 | w1 bits 4-7, limite 5 |
| Nombre de bornes | sauts − 2, minuteur de 15 VBLANK | idem | sauts − 2, persistant | sauts − 2 et nombre vu (bits 0-3) | nœuds actifs |
| Fenêtre de recherche RX | depuis RXOFFSET, 0x3A mots | 0x3B | 0xDF | 0x4B | RXOFFSET − 1 … − 8 |
| Fin de gestionnaire | MSK = 9 / 0x0D | idem | MSK &= 9 / 0x0D | idem | MSK = 9 / 0x0D |
| Acquittement SCI | 0x40000007 | 0x40000007 | 0x40000007 | 0x700006 | 0x700006 |
| Données propres | slots 0-7 (+ 8-15 dynamiques) | idem | slot CPU NO. (+ extra -$7D56) | slot PCB ID (+ extra -$7B58) | ID 0-3 |

---

## 7. Ce que le C139 doit faire pour ces jeux

Modèle minimal qui satisfait le code des cinq jeux. Les points marqués *(H)* sont des hypothèses compatibles avec ce code.

**RAM**
- 0x2000 mots de 9 bits, lisibles et inscriptibles comme de la RAM ordinaire : les jeux y rangent des variables (TXRAM[0x10-0x22]).
- Garder 16 bits ne gêne pas, mais **seuls les bits 0-8 circulent sur le lien**.
- Pas d'effet de bord sur les écritures en RAM (le `m_txsize = offset` de MAME est à supprimer).

**Émission**
- Elle démarre quand TXSIZE ≠ 0 **et** START bit 0 = 0, sur l'écriture de TXSIZE ou le passage de START à 0.
- Elle envoie TXSIZE mots depuis TXOFFSET, sur 13 bits :
  - adresse du mot i : `(TXOFFSET & 0x1000) | ((TXOFFSET + i) & 0xFFF)`, c'est-à-dire un repli à l'intérieur de la même moitié. Une trame relayée peut chevaucher la fin de l'anneau RX ; avec 0x39 mots, cela arrive environ une fois sur 72.
  - **les 9 bits** de chaque mot partent tels quels : pas de bit 8 ajouté ni retiré, pas d'en-tête propre à la puce ;
  - envoyer exactement TXSIZE mots convient : pour acedrive et victlap, le mot en trop vaut toujours 0 et il ne crée jamais de faux bit de synchro ;
  - s'arrêter après le premier mot qui a le bit 8 convient aussi *(H : comportement réel inconnu)*.
- TXSIZE redescend à 0 en fin d'émission. TXOFFSET peut avancer comme un compteur d'adresse ; aucun jeu ne s'en sert.
- **Ne jamais relayer automatiquement, et ne marquer aucune trame d'un ID** : ni `m_linkid`, ni compteur 0x1FF, ni réécriture de l'octet 7.
- En fin d'émission : STATUS bit 2 = 1. Si MSK bit 2 = 0, lever l'IRQ et **mettre MSK bit 2 à 1**.

**Réception**
- Chaque trame reçue (une émission distante) s'écrit mot à mot en `RX[0x1000 + (RXOFFSET & 0xFFF)]`, puis RXOFFSET avance.
  En fin de trame, RXOFFSET pointe sur le mot suivant.
- Après la trame : STATUS bit 1 = 1, IRQ, MSK bit 1 = 1 *(H)*.
- **Une trame à la fois** : ne pas écrire la suivante en RAM tant que STATUS bit 1 et MSK bit 1 *(H)* ne sont pas revenus à 0.
  Les jeux n'analysent que la dernière trame avant RXOFFSET ; cybrcycc efface STATUS avant de lire RXOFFSET et ne remet MSK à 0 qu'en sortie.

**STATUS (IRQ)**
- bit 2 = émetteur libre, en niveau (TXSIZE = 0 et rien en cours) ; il vaut 1 après reset, ce qu'exigent acedrive 0x28AF6 et victlap 0x279E0 ;
- bit 1 = trame reçue, mémorisé ;
- bit 0 = erreur ;
- écrire 0 efface les bits 0-1 sans toucher au bit 2.

**MSK (« MODE »)**
- Bit n = 1 masque la source n. La puce met à 1 le bit de la source qu'elle signale, au moins pour le bit 2.
- Les jeux remettent ces bits à 0 pour obtenir l'interruption suivante.
- À chaque écriture de MSK, réévaluer : émetteur libre et bit 2 = 0 ⇒ IRQ, bit 2 = 1 ; trame en attente et bit 1 = 0 ⇒ la livrer.
- **Ne pas traiter 0x0D comme un mode distinct**, ni 0x0C, ni 8 / 9 comme des modes. MSK = 0x0F : tout masqué.

**Ligne d'IRQ vers le contrôleur système**
- Une impulsion `ASSERT` par événement suffit : le contrôleur la mémorise jusqu'à son acquittement.
- Les jeux acquittent avant d'écrire MSK. Une IRQ perdue pendant le gestionnaire est donc relevée par la réévaluation sur écriture de MSK.

**CONTROL, START en configuration, RXSIZE** : sans effet pour ces jeux. `m_reg_f3` ne doit commander ni l'unité de TXOFFSET ni un quelconque relais.

**Rythme**
- Duplex intégral : émission et réception indépendantes, sans le « pas d'envoi si STATUS == 6 » de MAME.
- Charge par borne et par VBLANK, avec N bornes :
  - famille A : N trames émises (la sienne et N − 1 relais) et N reçues ;
  - acedrive à 8 bornes : 8 × 0x3A mots ≈ 28 000 mots/s par sens.
- Un tick de 800 Hz qui ne traite qu'une trame par tick et par sens suffit tout juste.
- Il vaut mieux finir l'émission au tick suivant (ou après une durée proportionnelle à la taille) qu'instantanément pendant le gestionnaire.
- Délais de garde côté jeu :
  - acedrive et victlap : la trame propre doit revenir en moins de 15 VBLANK ;
  - famille A : un slot reste « vu » environ 32 VBLANK (acedrive) ou 16 VBLANK (cybrcomm, adillor) après sa dernière trame.

**Tests à faire avec ce modèle**
1. acedrive et victlap avec des **ID identiques**, puis des numéros de PCB différents. Attendu : LINKCT = 1 et départ commun.
2. Trois instances d'acedrive : LINKCT = 2 sur chaque borne, et les trois voitures visibles.
3. cybrcomm (CPU NO. 0 et 1) et adillor (« LINK ON » dans le test d'E/S) : liaison.
4. cybrcycc à 2, puis 3 bornes : numérotation 0, 1, 2 par le type 0x0D, et « SIRQ » qui augmente sur l'écran COMMUNICATION.

---

## 8. Hypothèses et incertitudes
- Le sens de MSK bit 1, qui serait mis à 1 par la puce à l'IRQ de réception, n'est pas démontré. Les jeux le remettent toujours à 0, ce qui ne permet pas de trancher.
- Pour acedrive et victlap, on ignore si la puce envoie le mot en trop (TXSIZE = longueur + 1) ou s'arrête au bit 8. Les deux conviennent.
- Le moment de l'IRQ de réception sur le vrai matériel (au mot à bit 8 ou en fin de rafale) n'est pas connu. Le modèle « une trame = une émission » suffit.
- La signification de START = 3 / 0 / 0x11 écrit sous MSK = 0x0F est inconnue (débit ? horloge ?).
- « B-9 » (CONTROL) pourrait commander le 9e bit, ce qui expliquerait CONTROL 1/3 dans ridgera2. Ces jeux ne l'utilisent pas.
- Le compteur affiché par « ENTRY : n CAR » n'a pas été localisé : -$63D0 est probable.
- L'origine des 2 échecs sur 9 reste à établir (§ 5.3). Le doublon de numéro de PCB est la piste la plus simple à vérifier.
