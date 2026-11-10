# C139 "mode 09" on System 22 / Super 22: acedrive, victlap, cybrcomm, adillor, cybrcycc

Static analysis of the dumps `dumps/<game>/maincpu.bin` (capstone, 68020 mode). Addresses are offsets in the program ROM
put back in big-endian order. Working variables are written `-$xxxx(a6)` as in the code.
`TXRAM[n]` / `RXRAM[n]` = half-word n of the C139's RAM (TX: words 0x000-0xFFF, RX: words 0x1000-0x1FFF).

Tool pitfall: capstone does not show the scale factor of a "brief" index when there is a displacement
(`move.w d2,$2(a2,d1.w)` at cybrcycc 0xB5E is actually `$2(a2,d1.w*4)`, extension 0x1402). The readings below were
checked against the raw bytes whenever the scale mattered.

---

## 0. Main conclusions

1. **The C139 relays nothing and has no ring identifier.** None of the five games writes an ID into the chip (the 8 registers
   are loaded from constant tables). **The games do the relaying**: on reception, they increment a hop counter
   in the frame in RX RAM, fix the checksum, then transmit the frame again (TXOFFSET pointed into the RX area for
   acedrive / victlap / cybrcomm / adillor, a copy in the TX area for cybrcycc). Each game removes its own
   frames from the ring and discards those that have made too many hops.
2. **The game's relay goes through the "transmitter free" interrupt** (STATUS bit 2), which MAME never generates. In MAME, the
   games' transmit path is therefore dead code. MAME's automatic relay, driven by the ID derived from
   `remotehost:remoteport` and the counter in byte 0x1FF, replaces it. Hence the dependency on the IDs.
3. **Register 1 ("MODE") is an interrupt mask.** cybrcycc's COMMUNICATION screen names the eight registers:
   `IRQ MSK B-9 S-M R-C S-C R-AD S-AD`. Bit n of MSK = 1 masks source n of IRQ (= STATUS): bit 0 error, bit 1 frame
   received, bit 2 transmitter free. "9" and "0x0D" are not two modes: the games switch from one to the other to unmask
   or mask the transmit interrupt. The code of cybrcomm and adillor requires that **the chip itself set bit 2
   of MSK back to 1 when it raises the transmit interrupt** (see § 1.3).
4. **Number of cabinets**: each game deduces it from the hop counter of its own frame when it comes back (family A: `LINKCT = hops − 2`
   = number of *other* cabinets), or from the number of active nodes (cybrcycc). The chip plays no part in it.
5. **Identical IDs in MAME** (acedrive, victlap): no frame is relayed, a cabinet's frame never comes back to it,
   so LINKCT = 0 on both sides even though each one still receives the other's data (ring of 2). The lobby launches the race
   when "registered cars − 1 = LINKCT": the first cabinet to register sees 1 car and starts alone; the second sees
   2 cars (one of them already racing), waits for the timer and displays "ENTRY: 1 CAR".
6. Maximum: acedrive 8, victlap 8, cybrcomm 2, adillor 4, cybrcycc 4 cabinets.

---

## 1. What the games say about the registers

### 1.1 Namco register names
cybrcycc, COMMUNICATION screen (strings around 0x16F0, display at 0x158E-0x1608 in register order 0 to 7):

| Reg | S22 / SS22 address | MAME name | Namco label | Proposed interpretation |
|---|---|---|---|---|
| 0 | 0x20020000 / 0x420000 | STATUS | **IRQ** | interrupt flags: b0 error, b1 frame received, b2 transmitter free |
| 1 | +2 | MODE | **MSK** | interrupt mask (1 = masked) |
| 2 | +4 | CONTROL | **B-9** | "bit 9"? always 0 in these games |
| 3 | +6 | START | **S-M** | "send mode"? b0 = transmission held (1) / free (0) |
| 4 | +8 | RXSIZE | **R-C** | receive counter; never read by these games |
| 5 | +A | TXSIZE | **S-C** | transmit counter in words, counted down to 0 |
| 6 | +C | RXOFFSET | **R-AD** | RX write address (word following the last one received) |
| 7 | +E | TXOFFSET | **S-AD** | address of the first word to transmit (13 bits, the whole RAM) |

Second line of the screen: `MY RIRQ SIRQ E-ER T-ER` = own ID + 1, receive IRQ counter ($E0A628),
**transmit IRQ counter** ($E0A62A), "no sync bit" errors ($E0A62C), "bad header" errors ($E0A62E).
So the game itself counts transmit interrupts.

### 1.2 Common use of the registers
- **Initialisation**: two blocks of 8 words written in order 0…7, with MSK = 0x0F (everything masked) during the first one;
  C139 RAM cleared between the two (0x2000 words).
- **CONTROL (B-9)**: always 0. These games do not use ridgera2's CONTROL 1/3 mechanism.
- **START (S-M)**: transmission starts when S-C ≠ 0 and S-M bit 0 = 0.
  - acedrive and victlap keep START = 0 permanently: writing TXSIZE starts the transmission. Their handler writes START = 0 again on every pass.
  - cybrcomm and adillor: START = 1, then TXSIZE, then START = 0. The init leaves START = 3, transmission held.
  - cybrcycc: START = 0 from the init on; writes TXSIZE = 0, then TXOFFSET, then TXSIZE = n.
- **START value written while MSK = 0x0F** (MAME's `m_reg_f3`): 3 for acedrive, victlap, cybrcomm and adillor; 0 for cybrcycc;
  for comparison, 0x11 for ridgera2. Meaning unknown (rate? 12/16 MHz clock selection?).
  **It does not change the unit of TXOFFSET**: acedrive (f3 = 3) and ridgera2 (f3 = 0x11) both write word indices,
  including 0x1000+x to point into the RX area. MAME's `tx_offset >>= 1` when `f3 & 2` is an artefact.
- **TXOFFSET (S-AD)**: 13-bit word index. The game rewrites it before **each** frame, from its "transmitter free"
  handler. No game relies on an auto-increment. MAME's "+= 0x100" and "+= subOffset*2" only make up
  for the missing interrupt.
- **TXSIZE (S-C)**: number of words. It goes back down to 0 at the end of the transmission: cybrcycc reads `TXSIZE.b` and only transmits if it is 0 (0xB84);
  ridgera2 counts an error if it is not zero at the end of the transmission. Exact for cybrcomm, adillor and cybrcycc;
  **length + 1** for acedrive (0x3A for 0x39 words) and victlap (0x3B for 0x3A).
- **RXOFFSET (R-AD)**: read on entry to the handler.
  - cybrcycc reads the last word received at `(RXOFFSET − 1) & 0xFFF`: RXOFFSET therefore points to the **next** word.
  - Family A scans backwards **starting from** RXOFFSET, over a window of length + 1 words: the one-word offset fits within it.
  - The games only keep 12 bits: `(2·RXOFFSET & 0x1FFF) | 0x2000` in bytes.
- **Bit 8 = sync bit, set by the game**: `ori.w #$100` on the last word of the frame, the checksum
  (acedrive 0x29032, victlap 0x27F26, cybrcomm 0xF9BA, adillor 0x88F0) or the length word (cybrcycc 0xD52).
  On reception, the games find the end of the frame by searching for bit 8 in RX RAM. The chip must therefore carry and
  store 9 bits per word. It does not have to set this bit itself.
- **9-bit RAM**: the games store variables in free TX words (TXRAM[0x10], [0x11], [0x12], [0x20], [0x21], [0x22])
  by splitting 16-bit masks into two bytes. This is consistent with an 8K × 9-bit RAM.

### 1.3 Interrupts: what the code requires
End of handler, in each game:
- acedrive / victlap: `MSK = (pending ≠ 0) ? 9 : 0x0D` (0x29294 / 0x28188), after acknowledging the system controller.
- cybrcycc: `MSK = (queue not empty) ? 9 : 0x0D` (0xBD8-0xBF8).
- cybrcomm / adillor: read-modify-write.
  - Reception: `MSK &= 9` (0xFC58 / 0x8BCA).
  - Transmission: `MSK &= 9` if frames remain (0xFA92 / 0x89DA), otherwise `MSK &= 0x0D` (0xFABC / 0x8A04).
- Own frame built at VBLANK: `MSK &= 0x0B` (acedrive 0x29044/0x29050, victlap 0x27F38/0x27F44, cybrcomm 0xF9E2, adillor 0x8924).
  cybrcycc writes MSK = 9 (0xD76).

Consequences:
- **STATUS bit 2 = transmitter free, as a level.** It is not cleared by writing 0 to STATUS: cybrcycc clears STATUS on entry
  to the receive path, then relies on a transmit IRQ after `MSK = 9`.
  It is 1 after reset: acedrive 0x28AF6 and victlap 0x279E0 wait for it (10000 tries) before the init, otherwise infinite loop.
- **A transmit IRQ is requested by unmasking bit 2 of MSK while the transmitter is free.** This is how the VBLANK
  restarts sending after a period without transmission.
- **The chip must set MSK bit 2 back to 1 when it raises the transmit IRQ.** cybrcomm and adillor never set this bit to 1
  themselves. At the end of the queue they do `MSK &= 0x0D`, and at VBLANK `MSK &= 0x0B`. If the chip did not touch bit 2, these two
  operations would write the same value (9):
  - either both restart an IRQ, and the end of the queue turns into an interrupt loop;
  - or neither restarts it, and the VBLANK frames never go out.

  The only consistent model: raising the transmit IRQ sets MSK bit 2; `&= 0x0D` keeps it (nothing to send), `&= 9`
  and `&= 0x0B` set it back to 0 (a new IRQ when the transmitter is free). For acedrive and cybrcycc, which write 9 or
  0x0D explicitly, the behaviour is the same.
- MSK bit 1 is set back to 0 on **every** handler exit, transmit path included, although no game sets it to 1.
  *Hypothesis*: the chip masks reception in the same way (MSK bit 1 = 1 when the receive IRQ is raised).
  MSK bits 0 and 3 stay at 1 in these games, so the error interrupt is masked. ridgera2, which has MSK = 8, handles the error.
- The handler tests bit 1 (reception) **before** bit 2. If both are present, it only handles the reception, then
  unmasks transmission again if it has frames pending, which causes a new IRQ.
- System controller acknowledge: S22 `clr.b $40000007` (acedrive 0x2929E, victlap 0x28192; cybrcomm writes the word `$40000007`),
  Super 22: write to 0x700006 (cybrcycc 0xBD2, adillor 0x89D4/0x89FE/0x8BC4).
  All of them acknowledge **before** the last write to MSK: an IRQ raised again by this write is not lost.

Consequence for MAME's "modes" (broader hypothesis): 8, 9, 0x0C and 0x0D are masks.
- 0x0C = 8 with transmission masked.
- 0x0D = 9 with transmission masked.
- 0x0F = everything masked (init, test menus: acedrive 0x36E72 and 0x36F58, victlap 0x3DC4E and 0x3DD34).

MAME's `switch (m_reg[MODE])` therefore needs to be revisited for all families.

---

## 2. Family A: acedrive, victlap, cybrcomm, adillor

Four implementations of the same protocol:
- each cabinet owns one or more **slots** (frame slots in TX RAM, indexed by car number);
- a frame carries the slot number, a magic byte, a **hop counter**, the payload and an 8-bit checksum with bit 8;
- each receiver validates the frame, increments the hops and the checksum in RX RAM, copies the payload into the slot's mailbox, then
  **transmits the frame again directly from the RX area** (TXOFFSET = 0x1000 + position);
- a cabinet removes its own frames and deduces LINKCT (number of other cabinets) = hops − 2 from them.

### 2.1 Parameters

| | acedrive | victlap | cybrcomm | adillor |
|---|---|---|---|---|
| board / C139 base | S22 0x2002xxxx / 0x2001xxxx | S22 | S22 | Super 22 0x42xxxx / 0x41xxxx |
| init | 0x28AF6 (call at 0x53A) | 0x279E0 (calls at 0x576, 0x5CCE) | 0xF8A2 (call at 0x42A8) | 0x87D0 (call at 0x42FE) |
| register table | 0x28B22 | 0x27A0C | 0xF882 | 0x87B0 |
| block 1 (0…7) | 0,0F,0,3,0,0,1000,1000 | same | same | same |
| block 2 (0…7) | 0,9,0,**0**,0,0,1000,1000 | same | 0,9,0,**3**,0,0,1000,1000 | 0,9,0,**3**,0,0,1000,1000 |
| after init | MSK = 0x0D (0x5D8) | MSK = 0x0D (0x624) | MSK &= 9 after a wait (0xF914) | MSK &= 9 (0x8848) |
| SCI IRQ | level 6, 0x78 → 0xA46 → **0x2907E** | level 6, 0xAC6 → **0x27F72** | level 6, **0xFA0C** | level 4, **0x894E** |
| VBLANK → link | level 5, 0xA00 → 0x28BD6 | 0xA68 → 0x27AC0 | level 5 (0x4802) or 4 (0x4A7A) depending on the phase (0x40000004 = 0x35 / 0x34), both → 0xFCAA then 0xF92C | level 2, 0x4A00 → 0x855C then 0x8860 |
| slots | 16, word 0x800 + 0x80·s | 16, 0x800 + 0x80·s | 8, word 0x800 + 0x100·s | 16, 0x800 + 0x80·s |
| frame (words) | **0x39** | **0x3A** | **0xDE** | **0x4A** |
| TXSIZE written | **0x3A** | **0x3B** | 0xDE | 0x4A |
| w0 / w1 | slot / **0x8D** | slot / **0x9C** | slot / **0x94** | slot / **0xB0** |
| w2 | hops (1 on transmission) | same | same | same |
| w3 | sender's PCB number | same | payload | payload |
| w4 | TXRAM[0x22] (always 0) | same | payload | payload |
| payload | w5-w0x37 with a check byte (sum8 ^ 0x5A) every 16 words | w5-w0x38, same | w3-w0xDC | w3-w0x48 |
| checksum | w0x38 = sum8(w0..w0x37) \| 0x100 | w0x39 | w0xDD | w0x49 |
| max hops | < 16 | < 16 | < 8 | < 16 |
| max cabinets | 8 | 8 | 2 | 4 |

### 2.2 acedrive: commented pseudo-code

```c
// ---- Boot: 0x524 clears the C139 RAM (0x20010000-0x20014000), then jsr 0x28AF6
c139_init():                                  // 0x28AF6
    STATUS = 0; START = 0;
    for (i = 0; i <= 10000; i++)              // 0x28B06: waits for "transmitter free"
        if (STATUS & 4) goto ok;
    for (;;) ;                                // 0x28B16: hangs if the chip does not respond
ok: regs[0..7] = {0, 0x0F, 0, 3, 0, 0, 0x1000, 0x1000};  // 0x28B42, MSK = 0x0F, START = 3 → m_reg_f3 = 3
    RAM[0..0x1FFF] = 0;                       // 0x28B54
    regs[0..7] = {0, 9, 0, 0, 0, 0, 0x1000, 0x1000};      // 0x28B66
    txptr[0..15] = 0xFFFF;  txMask = 0xFFFF;  pending = 0; // -$72E0, -$7290, -$72E8
    ... ; clr.b $40000007;                    // SCI acknowledge
// 0x5D8: MSK = 0x0D (transmission masked), then 0x5E6: system controller, SCI level 6 (0x36 at 0x40000002)

// ---- On every VBLANK (60 Hz): 0xA00 → jsr 0x28BD6
link_vblank():                                // 0x28BD6
    if (ringTimer) ringTimer--;               // -$717A, reloaded to 15 when the own frame comes back
    ownMask = (ownMask & 0xFF00) | bit(pcbId);   // -$72EE; pcbId = -$73FC; high byte = slots 8-15 taken dynamically
    LINKCT  = ringTimer ? (TXRAM[0x10] & 0xFF) : 0;   // -$73F8: number of OTHER cabinets in the ring
    TXRAM[0x21] = ownMask; TXRAM[0x20] = ownMask >> 8;
    seenMask = TXRAM[0x11] << 8 | (TXRAM[0x12] & 0xFF);  // -$72F0
    for (s = 0; s < 16; s++)                  // car table -$4000(a6) + 0x100·s
        if (ownMask & bit(s))  build_payload(s); // 0x28C4A: w5..w0x37 of slot s, check bytes ^0x5A
        else if (seenMask & bit(s)) copy_mailbox(s); // 0x28E1C: copy protected by a counter
                                              // (high byte of the slot's w0, compared before and after)
    conflicts = OR(histo[-$71DE]);  ...       // 0x28F7C: slots 8-15 claimed by a cabinet with a higher number
    if (++vbl & 1) TXRAM[0x11..0x12] = OR(receive history -$72B0[16], shifted);  // 0x28FA6
    txMask = TXRAM[0x20] << 8 | TXRAM[0x21].b;  // -$7290
    for (s = 15; s >= 0; s--) if (txMask & bit(s)) {   // 0x28FE0
        txptr[s] = 0x1000 + 0x100*s;          // bytes; TXOFFSET = word 0x800 + 0x80·s
        w = &RAM[0x800 + 0x80*s];
        w[0] = s; w[1] = 0x8D; w[2] = 1;  w[3] = pcbId; w[4] = TXRAM[0x22];
        sum = 0; for (i = 0; i < 0x38; i++) { w[i] &= 0xFF; sum += w[i]; }
        w[0x38] = (sum & 0xFF) | 0x100;       // sync bit on the last word
        pending |= bit(s);
        MSK &= 0x0B;                          // 0x29044: requests a "transmitter free" IRQ
    }
    MSK &= 0x0B;                              // 0x29050
    update_cars();                            // 0x292B4

// ---- SCI IRQ: 0x2907E (level 6)
sci_irq():
    irqCount++;                               // -$7178
    st = STATUS;
    if (st & 2) goto rx;
    if (st & 4) {                             // transmitter free: sends the next slot, round-robin
        for (n = 0, r = rr & 15; n < 16; n++, r = (r + 1) & 15)
            if (pending & bit(r)) {
                pending &= ~bit(r);
                TXOFFSET = txptr[r] >> 1;     // word index: TX slot (0x800+) OR frame in the RX area (0x1000+)
                TXSIZE   = 0x3A;              // 0x290C8: frame of 0x39 words + 1
                break;
            }
        rr = r + 1;                           // -$72C0
    }
    START = 0;                                // 0x290E0
    goto out;                                 // no write to STATUS on this path
rx: p = (2*RXOFFSET & 0x1FFF) | 0x2000;       // bytes in 0x2001xxxx, RX area
    for (n = 0; n <= 0x39; n++, p -= 2)       // 0x2910A: goes back up to 0x3A words to find bit 8
        if (RAM16(p) & 0x100) goto found;
    goto err;
found:
    start = p - 0x38*2;  frameptr = start;    // -$72BC
    slot = w0;  sum = w0;                     // -$72B9
    if (w1 != 0x8D) goto err;                 // 0x2914C
    hops = w2 + 1;  w2 = hops;                // 0x29160: written back to RX RAM
    if (hops >= 16) goto err;
    if (txMask & bit(slot)) {                 // one of OUR slots: never relayed
        if (w3 == pcbId) {                    // our frame went all the way round the ring
            TXRAM[0x10] = max(hops - 2, 0);   // 0x291AA: LINKCT = N - 1
            ringTimer = 15;
        } else if (w3 > pcbId)                // a cabinet with a higher number claims the same slot
            conflicts |= bit(slot);
        goto ack;
    }
    check w3, w4, then 0x33 words w5..w0x37 (sum ^ 0x5A every 16) → otherwise err
    if ((sum8(w0..w0x37) & 0xFF) != w0x38.b) goto err;   // checksum computed with the ORIGINAL hops
    w0x38 = ((sum + 1) & 0xFF) | 0x100;       // 0x29230: fixes the checksum for hops + 1
    RAM[0x805 + 0x80*slot ...] = payload;     // 0x29240: mailbox = TX slot that is not transmitted
    RAM8[0x20011000 + 0x100*slot] += 1;       // 0x2925E: consistency counter (high byte of w0)
    rcv[slot] = 0xFF;                         // -$7170
    txptr[slot] = frameptr;                   // 0x29270: RELAY from the RX area
    pending |= bit(slot);  seenHist |= bit(slot);
    goto ack;
err: errCount++;                              // -$7174
ack: STATUS = 0;                              // 0x2928C
out: clr.b $40000007;                         // SCI acknowledge
     MSK = pending ? 9 : 0x0D;                // 0x292A8
     rte;
```

Debug overlay (0x289F8, caller not located): it displays `LNK CNT` = -$73F8, `LNK ERR` = -$717A (timer), `LNK SENBIT` = -$72EE and `LNK RECV` = -$72B0.

**Lobby (entry)**:
- 0x4D38: timer = 0x4B0 VBLANKs (20 s).
- Loop 0x4DE8-0x4E7C:
  - own phase = 1 (-$63E0), group = the smallest one seen (0x29B5C);
  - `n = registered cars` (0x29B90: entries 0-7 of the car table with state $10 ≠ 0 and phase $4E ≠ 0);
  - **start if `n − 1 == LINKCT` (0x4E6C)**, otherwise as soon as the timer expires.

  A cabinet that believes it is alone (LINKCT = 0) therefore starts as soon as it is the only one registered. A cabinet that already sees another car
  (racing) while LINKCT = 0 waits for the timer.
- The number displayed after "ENTRY : n CAR" (string 0x9793) probably comes from -$63D0 (0x2995A: cars in the same phase and the same group).
  *Hypothesis*: the display routine was not found.
- Time master: 0x1FA2, the smallest slot number seen below its own serves as the reference.

### 2.3 victlap
Same code as acedrive, apart from a few constants:
- init 0x279E0, table 0x27A0C; MSK = 0x0D at 0x624, SCI level 6 at 0x642;
- VBLANK 0x27AC0; frame building 0x27EC0-0x27F44;
- IRQ 0x27F72:
  - reception at 0x27FE0, scan of 0x3B words;
  - start = sync − 0x39; w1 = 0x9C (0x28040); hops at 0x28054;
  - own frame and LINKCT at 0x28064-0x280AA;
  - payload of 0x34 words, checksum check and fix at 0x2810A-0x28124;
  - relay at 0x28160-0x28176, acknowledge at 0x28180, MSK at 0x2819C.
- TXSIZE = 0x3B at 0x27FBC;
- lobby at 0x408A: `count(0x28A98) − 1 == LINKCT`.

### 2.4 cybrcomm
Same principle, different programmer; adillor reuses this code almost unchanged.

```c
c139_init():                                  // 0xF8A2
    $40000019 = 1;
    regs = {0,0x0F,0,3,0,0,0x1000,0x1000};    // 0xF8B6
    RAM = 0;
    regs = {0,9,0,3,0,0,0x1000,0x1000};       // 0xF8D6: START = 3, transmission held
    txptr[0..7] = 0xFFFF; txMask = 0xFFFF; pending = 0;   // $1001B9C6, $1001BA0E, $1001B9C4
    wait of 0x7000 loops; MSK &= 9; SCI acknowledge ($40000007)    // 0xF914

vblank (0x4A7A at level 4, 0x4802 at level 5): jsr 0xFCAA; jsr 0xF92C
0xFCAA:   ownMask = bit(cpuNo) | extra(-$7D56);  TXRAM[0x21] = ownMask; TXRAM[0x20] = ownMask >> 8;
          seen = TXRAM[0x11] << 8 | TXRAM[0x12].b;  nSeen = popcount(seen) (-$7D72);
          LINKCT = TXRAM[0x10] & 0xFF (-$7D74);   // persistent: no timer
          for (s = 0; s < 8; s++): own slot → payload at w3.. (0xFD54); seen slot → copy of the queued RX frame,
                                   checksum checked again (0xFE32-0xFECA)
0xF92C:   every 16 VBLANKs: TXRAM[0x12] = seen_bits.b; TXRAM[0x11] = seen_bits >> 8; seen_bits = 0
          txMask = TXRAM[0x20] << 8 | TXRAM[0x21].b;
          for each slot s in txMask: txptr[s] = 0x1000 + 0x200*s; w0 = s; w1 = 0x94; w2 = 1;
                                     w0..w0xDC &= 0xFF; w0xDD = sum8 | 0x100; pending |= bit(s)
          if (pending) MSK &= 0x0B;           // 0xF9E2

sci_irq():                                    // 0xFA0C
    st = STATUS;
    if (st & 2) goto rx;
    if (!(st & 4)) { ack; MSK &= 0x0D; rte; }
    for 8 slots, round-robin: if pending & bit(r) {
        pending &= ~bit(r);
        TXOFFSET = txptr[r] >> 1;  START = 1;  TXSIZE = 0xDE;   // 0xFA56-0xFA64
        if (pending == 0) { START = 0; ack; MSK &= 0x0D; rte; }  // last frame: transmission stays masked
        START = 0; ack; MSK &= 9; rte;                            // frames remain: requests another IRQ
    }
    ack; MSK &= 0x0D; rte;
rx: rxIrq++;  goes back up to 0xDF words from RXOFFSET to find bit 8 (0xFAEC); otherwise → ack
    start = sync − 0xDD; slot = w0;  w1 != 0x94 → drop
    hops = w2 + 1; w2 = hops; hops >= 8 → drop
    if (txMask & bit(slot)) { -$7D76 = 0; TXRAM[0x10] = max(hops-2, 0); goto drop; }    // 0xFB6C-0xFB88
    queue it (pointer, partial sum; double buffer per slot at -$7D00);
    sum w3..w0xDC; (w0xDD + 1) != sum → drop; w0xDD = (w0xDD + 1) | 0x100;               // 0xFBF6-0xFC1E
    txptr[slot] = start; pending |= bit; seen_bits |= bit;                                     // 0xFC20-0xFC44
ack: STATUS = 0; SCI acknowledge; MSK &= 9; rte;         // 0xFC4A
drop: seen_bits &= ~bit(slot); pending &= ~bit(slot); goto ack;  // 0xFC66
```

Note: the return of the own frame also clears that slot's `pending` bit (drop path).
If a more recent own frame was still waiting to be transmitted, it is lost. This has no consequence when the round trip takes less than one VBLANK.

### 2.5 adillor
Copy of cybrcomm, with these differences:
- 16 slots of 0x80 words;
- frame of 0x4A words, w1 = 0xB0, TXSIZE = 0x4A;
- hop threshold 16;
- IRQ at 0x894E:
  - transmission 0x896E-0x8A10;
  - reception 0x8A12;
  - "seen" -$7B40 marked for **any** frame with a valid header (0x8AA6), including its own;
  - own frame at 0x8AB8-0x8AE2;
  - checksum at 0x8AF2-0x8B16;
  - relay at 0x8B92-0x8BB6;
- VBLANK: 0x855C (consumer) then 0x8860 (transmission).

Link state:
- -$7B74 = LINKCT (TXRAM[0x10]);
- -$7B72 = number of bits 0-3 set in -$7B40 (cabinets seen, reset to 0 every 16 VBLANKs);
- "LINK ON" test screen (0xB04E-0xB08E): linked if `LINKCT + 1 == seen_bits`, "NG" otherwise.

---

## 3. Family B: cybrcycc (Super 22, different protocol)

```c
c139_init():                                  // 0xC06 (call at 0x738)
    STATUS=0; MSK=0x0F; CONTROL=0; START=0 (→ m_reg_f3 = 0); TXSIZE=0; RXSIZE=0; TXOFFSET=0x1000; RXOFFSET=0x1000;
    C139 RAM = 0 (0x3498);  TXRAM[0..0x3E7] = 0x1FF;  TX queue $E0A606[4] = {size, offset} = 0; rr = 0; RX queue = 0
    STATUS = 0; MSK = 9;
// system controller: SCI level 4 (0x700002 = 4, at 0x822) → vector 0x310 → bsr 0xA5C; rte

frame_out():                                  // 0xD00-0xD7E, main loop (0x8E2 → 0xCB4)
    if (!(n = payloadLen)) goto out;          // $E08402, set by the message type (0xE36-0x1012)
    buf ^= 0x100;                             // $E08404: TX double buffer, word 0 or 0x100
    w = &RAM[buf];  w[0] = 0x5A;  w[1] = ownId;          // ownId = $E08400 (0-3); hops = 0
    w[2..n+1] = payload bytes (10-byte header: counter, state, type $E0A65C, session $E0A65E, ..., negative checksum)
    len = n + 2;  w[len] = len | 0x100;       // the last word carries the sync AND the length
    queue[ownId] = { size = len + 1, offset = buf };
out: MSK = 9;                                 // 0xD76

sci_irq():                                    // 0xA5C
    st = STATUS;
    if (st & 2) {                             // reception
        STATUS = 0;  rxIrq++;                 // cleared BEFORE reading RXOFFSET
        q = (RXOFFSET - 1) & 0xFFF;           // last word received
        search for bit 8 in RX[q], RX[q-1] … RX[q-7]; otherwise errNoSync++; goto out
        len = RX[q] & 0xFF;  s = (q - len) & 0xFFF;          // the length is in the sync word
        if ((RX[s] & 0x1FF) != 0x5A) { errMagic++; goto out; }
        h = (RX[s+1] & 0xFF) + 0x10;  RX[s+1] = h;           // hops in bits 4-7 of w1
        if (h >= 0x50) goto out;              // at most 4 hops
        queue s+1 for the parser (16 entries $E0A634/$E0A636)
        node = h & 0xF;  if (node > 3) goto out;
        if (node == ownId) goto out;          // own frame: removed, not relayed
        copy RX[s..s+len] → TX word 0x800 + 0x200*node;  RX[q] = 0 (clears the sync in RX RAM)
        queue[node] = { len + 1, 0x800 + 0x200*node };       // relay by COPY into the TX area
    } else {
        STATUS = 0;
        if (!(st & 4)) goto out;
        txIrq++;                              // "SIRQ"
        if (TXSIZE.b != 0) goto out;          // transmitter busy: S-C counts down to 0
        r = first non-empty queue[], round-robin over 4; otherwise goto out
        TXSIZE = 0;  TXOFFSET = queue[r].offset;  TXSIZE = queue[r].size;  queue[r].size = 0;
    }
out: clr.b $700006;                           // SCI acknowledge
     MSK = (any queue not empty) ? 9 : 0x0D;
     rts → rte
```

Parsing and numbering:
- **Parser** (0xCB4): for each entry of the RX queue, copies the low bytes from w1 up to the sync word.
- **Messages** (0x1048-0x124A):
  - byte 0 = node (bits 0-1) + hops (bits 4-5);
  - 10-byte header checked by a zero sum;
  - the type ($4 of the node's record, at $E0A668 + 0x100·node) dispatches at 0x10BC.
- **Type 0x0D is used for numbering** (0x11BC-0x1228). It is the broadcast of the race settings, and it is accepted even if
  the sender's ID equals its own (0x108C). If its session (byte 5) differs from $E0A65E:
  - **ownId = hops & 3** (0x11DA-0x11E6): the position in the ring measured from the sender, which is itself at 0;
  - copy into $E0AFAE, the EEPROM image 0x26;
  - adopts the settings $E0AFB4..$E0AFBB.

  The numbering therefore assumes that **each node that relays increments the hops**, which is what the game's handler does.
- Number of active nodes (0x124C-0x1282): $E0AF28 = state records ≠ 0; $E0AF2A = records in state 0x0D.

---

## 4. Position, number of cabinets, settings

| Game | Setting (test menu) | Storage | Values | Max cabinets |
|---|---|---|---|---|
| acedrive | PCB ID NUMBER (string 0x2F39E) | EEPROM 0x58000000, 32-byte record at **0x100** (marker 0x141, checksum at 0x11F). **Byte 0x111** → $418B → -$73FC (0x2D222) | 0-7 (8 cars in the table: 0x29AD2, 0x29B90; 8 team colours) | **8** |
| victlap | PCB ID NUMBER (0x2E77E) | record 0x100 (marker 0x156), **byte 0x10C, bits 7-5** → $414A (0x2C44E) → -$73FC (0x2C64C) | 0-7 ("2 … 8 PLAYERS") | **8** |
| cybrcomm | CPU NO. (0x8428) | EEPROM 0x02-0x11 → $2A00(a6) (0x6492), **byte 0x00A** → $2A10, `& 1` (0x8574) → -$7EBE (0x816A) | 0-1 | **2** |
| adillor | PCB ID (0xAF5B) | block -$6600(a6) read from record 0 (0x93C4, 0x9554; backup copy in record 0x10, at 0x93E8). **Byte 7 = EEPROM 0x007** → -$7E80 (0x944E) | 0-3, displayed + 1 (0xAD5A) | **4** |
| cybrcycc | ID NUMBER (0x7077) | EEPROM image at $E0AF88 (byte n at 0x460000 + 2n). **Byte 0x026** → $E08400 (0x155DC); written by 0x6F04 | 0-3; **reassigned by the link** (type 0x0D, § 3) | **4** |

In family A, the position is the PCB number: it is the slot, the bit in the masks and w3. If two cabinets have the same number,
each one takes the other's slot for its own. The number of cabinets comes from the hop counter of the returning frame.
In cybrcycc, the position is set and then recomputed from the distance to the master.

The PCB number of acedrive or victlap is never rewritten by the link. The reversion of cab2 to no. 1 noted in C139.md § 5
therefore does not come from this code; cybrcycc, on the other hand, does rewrite its ID.

---

## 5. Behaviour with MAME's current emulation

### 5.1 What actually happens (acedrive, victlap, cybrcomm, adillor)
1. `m_irq_cb` is only called in `read_data()`: **no transmit IRQ**.
   - The handlers' transmit path never runs.
   - `pending` never empties, and MSK stays at 9 after the first VBLANK.
   - TXOFFSET keeps its init value (0x1000), apart from the corrections.
   - So the game relays nothing.
2. MAME's "mode 9" sending:
   - on every tick, if `m_txblock == 0`: `tx_offset = TXOFFSET >> 1` (because of `f3 & 2`) = word 0x800, slot 0;
   - `find_sync_bit` looks for the first bit 8:
     - the own slot, when it is slot 0;
     - otherwise bit 8 of a mailbox's consistency counter: `+= 0x100`, which moves forward one slot;
     - otherwise a block that starts on another mailbox, of which the receiver keeps the last 0x39 words;
   - `m_txblock` is reset to 0 by every write of MSK = 9. The handlers write one after every reception, and so does the VBLANK.
3. The MAME chip relays any frame with a different ID. On the frame coming back to its sender, it replaces the hops (byte 7) with the
   0x1FF counter (`f3 & 2`): `hops = N`, the game turns it into N + 1, so LINKCT = N − 1, the right value.
   This return path does not check the checksum, which explains why the modified value gets through.
4. The 9 / 0x0D switching (MAME's `switch`) has no effect: once `pending` is non-zero, MSK stays at 9.

### 5.2 Identical IDs
- The chip no longer relays anything. A received frame is taken for its own: TXSIZE = 0 and byte 7 = 1, with no effect.
- In a ring of 2, each cabinet still receives the other's frames directly. So the cars are exchanged.
- A cabinet's own frame never comes back to it:
  - the ring timer (-$717A, 15 VBLANKs) expires;
  - **LINKCT = 0 on both sides** ("LINK COUNT" = 0);
  - cybrcomm and adillor: TXRAM[0x10] is never written, LINKCT = 0; adillor displays "NG", since seen_bits = 2 and LINKCT + 1 = 1.
- Lobby, with the rule "start if registered − 1 == LINKCT":
  1. cabinet A registers first, 1 registered, `0 == 0`: **it starts right away, alone**;
  2. B registers and counts 2 cars, since A is racing with $4E ≠ 0: `1 ≠ 0`, B **waits for the timer (up to 20 s)**;
  3. meanwhile, B has only one car in the registration phase: **"ENTRY: 1 CAR"**.

  This is exactly the observed symptom.

### 5.3 The 2 failures out of 9 with distinct IDs
Leads, to be checked in this order:
1. **Same PCB number on both cabinets.** This is the case if cab2's EEPROM 0x111 has gone back to 1 (pitfall noted in C139.md § 5).
   - Both cabinets take slot 0.
   - The other's frame arrives on one of "our" slots with `w3 == pcbId`: it is counted as an own frame coming back, with hops = 1
     (chip that relays) or 2. TXRAM[0x10] alternates between 0 and 1 depending on the frame received last.
   - The other car's data is ignored: only 1 registered.
   - Depending on the value of LINKCT at the time of the test, solo start or wait on "ENTRY: 1 CAR".
   - Check: read 0x111 (acedrive) or 0x10C (victlap) again after a failure.
2. Effects of MAME's improvised sending: blocks of 0xB9 words, frames rejected when `find_sync_bit` starts on a mailbox.
   Normally these are isolated losses, with no effect on LINKCT unless 15 VBLANKs in a row.
3. Lobby decision based on an instantaneous value: if LINKCT is at 0 at the moment of registering (link not yet established,
   `m_linktimer` or TCP sockets), the cabinet starts alone.

### 5.4 Predictions for the untested games
- **cybrcomm, adillor**: with identical IDs, LINKCT = 0. With distinct IDs, behaviour like acedrive.
- **cybrcycc**, f3 = 0:
  - the 0x1FF counter and `>>1` are inactive, but the transmit path is dead too;
  - MAME always sends from TXOFFSET = 0x1000 & 0xFFF = word 0, buffer 0. **The messages built in buffer 0x100,
    i.e. one in two, never go out** and buffer 0 is sent twice;
  - with 2 cabinets, this should work with both ID settings, since the own frame is not needed;
  - with 3 or 4 cabinets: with identical IDs, no relay; with distinct IDs, the chip relays the **original** frame and not the copy
    whose hops the game incremented. All the non-master cabinets then number themselves 1 (type 0x0D).

---

## 6. Differences between the five games

| | acedrive | victlap | cybrcomm | adillor | cybrcycc |
|---|---|---|---|---|---|
| Family | A | A (copy of acedrive) | A (variant) | A (copy of cybrcomm) | B |
| START (S-M) in operation | 0 | 0 | 3, then 1/0 around each frame | same as cybrcomm | 0 |
| TXSIZE | length + 1 | length + 1 | length | length | length (= len + 1) |
| Relay source | RX (TXOFFSET 0x1000+) | RX | RX | RX | copy in TX 0x800 + 0x200·n |
| Own frame recognised by | slot in the mask **and** w3 = PCB number | same | slot in the mask | slot in the mask | node (w1 & 3) = ownId |
| Hops | w2, limit 16 | w2, 16 | w2, 8 | w2, 16 | w1 bits 4-7, limit 5 |
| Number of cabinets | hops − 2, 15-VBLANK timer | same | hops − 2, persistent | hops − 2 and number seen (bits 0-3) | active nodes |
| RX search window | from RXOFFSET, 0x3A words | 0x3B | 0xDF | 0x4B | RXOFFSET − 1 … − 8 |
| End of handler | MSK = 9 / 0x0D | same | MSK &= 9 / 0x0D | same | MSK = 9 / 0x0D |
| SCI acknowledge | 0x40000007 | 0x40000007 | 0x40000007 | 0x700006 | 0x700006 |
| Own data | slots 0-7 (+ 8-15 dynamic) | same | CPU NO. slot (+ extra -$7D56) | PCB ID slot (+ extra -$7B58) | ID 0-3 |

---

## 7. What the C139 must do for these games

Minimal model that satisfies the code of the five games. Points marked *(H)* are hypotheses compatible with this code.

**RAM**
- 0x2000 9-bit words, readable and writable like ordinary RAM: the games store variables in it (TXRAM[0x10-0x22]).
- Keeping 16 bits does no harm, but **only bits 0-8 travel over the link**.
- No side effect on RAM writes (MAME's `m_txsize = offset` must be removed).

**Transmission**
- It starts when TXSIZE ≠ 0 **and** START bit 0 = 0, on the write to TXSIZE or when START goes to 0.
- It sends TXSIZE words from TXOFFSET, over 13 bits:
  - address of word i: `(TXOFFSET & 0x1000) | ((TXOFFSET + i) & 0xFFF)`, i.e. a wrap within the same half. A relayed frame can straddle the end of the RX ring; with 0x39 words, this happens about once in 72.
  - **all 9 bits** of each word go out as they are: no bit 8 added or removed, no chip-specific header;
  - sending exactly TXSIZE words is fine: for acedrive and victlap, the extra word is always 0 and it never creates a false sync bit;
  - stopping after the first word that has bit 8 is fine too *(H: real behaviour unknown)*.
- TXSIZE goes back down to 0 at the end of the transmission. TXOFFSET may advance like an address counter; no game uses it.
- **Never relay automatically, and do not tag any frame with an ID**: no `m_linkid`, no 0x1FF counter, no rewriting of byte 7.
- At the end of the transmission: STATUS bit 2 = 1. If MSK bit 2 = 0, raise the IRQ and **set MSK bit 2 to 1**.

**Reception**
- Each received frame (one remote transmission) is written word by word to `RX[0x1000 + (RXOFFSET & 0xFFF)]`, then RXOFFSET advances.
  At the end of the frame, RXOFFSET points to the next word.
- After the frame: STATUS bit 1 = 1, IRQ, MSK bit 1 = 1 *(H)*.
- **One frame at a time**: do not write the next one to RAM until STATUS bit 1 and MSK bit 1 *(H)* are back at 0.
  The games only parse the last frame before RXOFFSET; cybrcycc clears STATUS before reading RXOFFSET and only sets MSK back to 0 on exit.

**STATUS (IRQ)**
- bit 2 = transmitter free, as a level (TXSIZE = 0 and nothing in progress); it is 1 after reset, which acedrive 0x28AF6 and victlap 0x279E0 require;
- bit 1 = frame received, latched;
- bit 0 = error;
- writing 0 clears bits 0-1 without touching bit 2.

**MSK ("MODE")**
- Bit n = 1 masks source n. The chip sets to 1 the bit of the source it signals, at least for bit 2.
- The games set these bits back to 0 to get the next interrupt.
- On every write to MSK, re-evaluate: transmitter free and bit 2 = 0 ⇒ IRQ, bit 2 = 1; frame waiting and bit 1 = 0 ⇒ deliver it.
- **Do not treat 0x0D as a separate mode**, nor 0x0C, nor 8 / 9 as modes. MSK = 0x0F: everything masked.

**IRQ line to the system controller**
- One `ASSERT` pulse per event is enough: the controller latches it until it is acknowledged.
- The games acknowledge before writing MSK. An IRQ lost during the handler is therefore raised again by the re-evaluation on the MSK write.

**CONTROL, START during configuration, RXSIZE**: no effect for these games. `m_reg_f3` must control neither the unit of TXOFFSET nor any kind of relay.

**Timing**
- Full duplex: transmission and reception independent of each other, without MAME's "no sending if STATUS == 6".
- Load per cabinet and per VBLANK, with N cabinets:
  - family A: N frames transmitted (its own and N − 1 relays) and N received;
  - acedrive with 8 cabinets: 8 × 0x3A words ≈ 28,000 words/s per direction.
- An 800 Hz tick that only handles one frame per tick and per direction is only just enough.
- It is better to finish the transmission on the next tick (or after a time proportional to the size) than instantly during the handler.
- Timeouts on the game side:
  - acedrive and victlap: the own frame must come back within 15 VBLANKs;
  - family A: a slot stays "seen" for about 32 VBLANKs (acedrive) or 16 VBLANKs (cybrcomm, adillor) after its last frame.

**Tests to run with this model**
1. acedrive and victlap with **identical IDs**, then different PCB numbers. Expected: LINKCT = 1 and a common start.
2. Three instances of acedrive: LINKCT = 2 on each cabinet, and all three cars visible.
3. cybrcomm (CPU NO. 0 and 1) and adillor ("LINK ON" in the I/O test): link established.
4. cybrcycc with 2, then 3 cabinets: numbering 0, 1, 2 through type 0x0D, and "SIRQ" increasing on the COMMUNICATION screen.

---

## 8. Hypotheses and uncertainties
- The meaning of MSK bit 1, which would be set to 1 by the chip on the receive IRQ, is not proven. The games always set it back to 0, which does not settle the question.
- For acedrive and victlap, it is not known whether the chip sends the extra word (TXSIZE = length + 1) or stops at bit 8. Both are fine.
- The timing of the receive IRQ on the real hardware (at the word with bit 8 or at the end of the burst) is not known. The "one frame = one transmission" model is enough.
- The meaning of START = 3 / 0 / 0x11 written under MSK = 0x0F is unknown (rate? clock?).
- "B-9" (CONTROL) could control the 9th bit, which would explain CONTROL 1/3 in ridgera2. These games do not use it.
- The counter displayed by "ENTRY : n CAR" has not been located: -$63D0 is likely.
- The origin of the 2 failures out of 9 remains to be established (§ 5.3). The duplicate PCB number is the simplest lead to check.
