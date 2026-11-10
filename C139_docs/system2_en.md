# C139 on Namco System 2: Final Lap 1/2/3, Four Trax, Suzuka 8 Hours 1/2, Dirt Fox (+ classification of the other games)

Static analysis (capstone M68K, `python -I`) of the regions dumped by MAME (`dumps/<game>/maincpu.bin`, `slave.bin`, bytes swapped in pairs).
No emulator was run. "**H:**" marks a hypothesis; the rest is read from the code.

System 2 memory map (both 68000s see the same thing):
- C139 registers at 0x4A0000: +0 STATUS, +2 MODE, +4 CONTROL, +6 START, +8 RXSIZE, +A TXSIZE, +C RXOFFSET, +E TXOFFSET;
- C139 RAM at 0x480000-0x483FFF = 0x2000 half-words. Chip address = (host address − 0x480000) / 2. TX = words 0x0000-0x0FFF, RX = words 0x1000-0x1FFF (0x482000);
- C148 (IRQ controller of each CPU): 0x1CC000 = SCI IRQ level, 0x1CE000 = VBLANK level, write to 0x1DC000 = SCI acknowledge.
  In MAME, `sci_int_w` triggers both C148s; only the CPU that programmed a non-zero SCI level takes it;
- EEPROM 0x180000 (`umask16(0x00ff)`): EEPROM byte *n* is at 0x180000 + 2n, and at offset *n* of MAME's NVRAM file.

Conventions: `a6 = 0x100000` in all these games (`$2012(a6)` = 0x102012). "wN" = word N of a frame.
All the indexed accesses checked have a scale factor of 1 (68000), so the capstone pitfall of `super22.md` does not apply here.

## 1. Verdict

| Game | Protocol | CPU / SCI IRQ | Frame | Who relays | Cabinet identity | Max |
|---|---|---|---|---|---|---|
| finallap | "Final Lap", MODE 0x0D, no 0x0F config | master, level 2, handler 0x8A46 | 0x14 words | **the game** (TXOFFSET = 0x1000 + RX position) | 16-bit PCB number in EEPROM 0x10-0x11 | 8 |
| finalap2 | same | master, level 2, 0xCA9C | 0x16 words | the game | "PCB NO" (EEPROM 0x10-0x11) | 8 |
| finalap3 | same | master, level 2, 0xD60A | 0x16 words | the game | "PCB NO" | 8 |
| fourtrax | "slot-based", MODE 0x0F/START 3 then 0x09 ↔ 0x0D | master, level 2, 0x4D34 | 0x25 words, type 0x41 | the game (transmit queue) | "GRID NO" (EEPROM byte 8) | 8 players (16 slots) |
| suzuka8h | same | master, level 2, 0x5B26 | 0x25 words, type 0x66 | the game | "GRID NO" (byte 9); "Status Monitor" DIP | 8 |
| suzuk8h2 | same | master, level 2, 0x681C | 0x25 words, type 0x82 | the game | "GRID NO" (byte 9) | 8 |
| dirtfoxj | same (variant) | **slave**, level 5, 0x1B28 | 0x20 words, type 0x42 | the game | "CAR TYPE" 1-8 (GAME OPTIONS) | 8 |

The four System 2 protocols (including kyukaidk and sws, § 5) have three things in common:
1. relaying is done **by the software**, with a hop counter in the frame;
2. the game itself recognises its own frames when they come back (by PCB number or by slot) and removes them from the ring;
3. the number of machines is **the hop counter of its own frame when it comes back**.

The C139 must therefore neither filter, nor relay, nor modify the frames.

## 2. Final Lap (finallap), mode 0x0D

### 2.1 Boot and init (0x8F34, called at 0x1156)
```
// boot 0x1130-0x1188
init_c139();                       // 0x8F34
C148.VBLANK_level = 1;             // 0x116E, vector 0x64 -> 0x254A
C148.SCI_level    = 2;             // 0x1176, vector 0x68 -> 0x8A46 (the slave does not program an SCI level)

void init_c139()                   // 0x8F34
{
    STATUS = 0; START = 0;
    for (n = 10000; n >= 0; n--)          // 0x8F44: waits for "transmitter free"
        if (STATUS & 4) goto ok;
    print("SCI ERROR"); for (;;);         // 0x8F54-0x8F64: hang
ok:
    *(u32*)&RXSIZE = 0;                   // clr.l 0x4A0008: RXSIZE = TXSIZE = 0
    *(u32*)&RXOFFSET = 0;                 // clr.l 0x4A000C: RXOFFSET = TXOFFSET = 0 (bit 12 = 0)
    START = 0;
    rx_prev = 0;                          // $2004
    MODE = 0x0D;                          // 0x8F86; never MODE 0x0F, never CONTROL
    my_id = EEPROM[0x10] << 8 | EEPROM[0x11];   // 0x8F8E -> $7008
    phase = 1; $7052..$7058 = 0;          // $7050 = game phase (sent in w2)
    clear car table $104000 (32 x 0x80);  // 0 = me, 1-7 = other cabinets, 8-15 = AI cars
}
```

### 2.2 Frame (0x14 half-words, one byte per word)
| Word | Contents (transmission 0x8D0E) | Reception (0x8B80) |
|---|---|---|
| w0, w1 | sender's PCB number (high byte, low byte) | `movep.w 1(a2)` → d3, compared with $7008 |
| w2 | game phase $7050 (≠ 0; **0 = AI car frame**) | $58 |
| w3 | $7052 (AI: slot index × 0x20) | $5A |
| w4, w5 | one-off messages $7054/$7056 (reset to 0 after sending) | $5C/$5E (+ $7058 if w4 ≠ 0) |
| w6-w8 | 24-bit position (`$4(a0) + $11C(a6)`) | $50 |
| w9-w10, w11-w12 | `$2(a0)`, `$12(a0)` (16 bits each) | $54, $56 |
| w13 | flags: bit 0 racing, bit 2 = `$4E(a0)` | `(a1)`, $4E |
| w14-w16 | `$40`, `$16`, `$1A` | same |
| w17 | **car type** `$14(a0)` (0-3; 4 = AI, H: livery) | $14 |
| w18 | **hop counter**, 0 when sent | see 2.4 |
| w19 | sum of the low bytes of w0-w18 & 0xFF, **+ 0x100** (bit 8 = end of frame) | compared on 8 bits |

### 2.3 Transmission, from the VBLANK (0x254A → 0x8D0E, 60 Hz)
```
void link_tx()                          // 0x8D0E
{
    build own frame at TX word 0;       // 0x8D16-0x8DBE, w18 = 0
    for (;;) {                          // 0x8DC4
        while (!(STATUS & 4)) ;         // busy wait, interrupts enabled
        sr |= 0x0F00;                   // IPL 7 mask
        if (STATUS & 4) break;          // re-test with interrupts masked
        restore sr;
    }
    w19 = checksum(w0..w18) | 0x100;
    TXOFFSET = 0x0000;                  // 0x8DFC
    TXSIZE   = 0x14;                    // 0x8E02: triggers the transmission (START stays at 0)
    restore sr;
    off = 0x14;
    for each car k in $104400..$1047FF (8 AI cars)          // 0x8E14
        if (car.active > 0 && car.$4C > 0) {               // AI computed by this cabinet
            build frame at TX word off: w0-w1 = my_id, w2 = 0, w3 = (k*0x80+0x4400)>>2 & 0xFF, ...;
            same wait/mask; TXOFFSET = off; TXSIZE = 0x14; off += 0x14;
        }
}
```
Up to 9 frames per video frame, written one after another in TX (0x00, 0x14, 0x28…). The game only looks at STATUS bit 2.
It therefore relies on the chip to **keep STATUS bit 2 at 0 until the previous frame has gone out**. Otherwise it overwrites TXOFFSET/TXSIZE.

### 2.4 SCI interrupt = reception + relay (0x8A46)
```
isr_sci()                                        // 0x8A46, level 2
{
    pos  = RXOFFSET;  cnt = RXSIZE;  RXSIZE = 0; // read, then RXSIZE reset to 0 straight away
    pos &= 0xFFF;
    if (pos > 0xF00) { RXOFFSET = 0; pos = 0; }  // the game manages the RX buffer itself (linear, 0-0xF13)
    rx_next = pos;                               // $2000
    rx_frame(cnt);                               // 0x8ADE
    C148.ack_sci();                              // clr 0x1DC000
    STATUS = 0;
    MODE   = 0x0D;                               // last write of the handler (re-arms, see § 5)
}

rx_frame(cnt)                                    // 0x8ADE
{
    if ((cnt & 0xFF) != 0xEC) goto drop;         // 0xEC = -0x14: RXSIZE is a down-counter, ONE frame per IRQ
    f = &RAM[0x1000 + rx_prev];                  // received frame = from the old pointer ($2004) to the new one
    if ((sum8(f[0..18])) != (f[19] & 0xFF)) goto drop;
    id = f[0] << 8 | f[1];
    if (id == my_id) {                           // 0x8CCA: MY frame has gone round the ring
        if (f[2] & 0xFF) {                       // main frame (not an AI)
            me.$58..$5E = f[2..5];
            link_hops = f[18] & 0xFF;            // $2012 = number of OTHER machines
            me.$46++;                            // "LINK ON"
        }
        goto done;                               // not relayed: removed from the ring
    }
    if ((f[18] & 0xFF) > 8) goto drop;           // 0x8B2E: 9 hops maximum
    f[18]++;                                     // hop counter, modified IN the RX buffer
    if (STATUS & 4) {                            // 0x8B3C
        f[19] = sum8(f[0..18]) | 0x100;          // checksum recomputed, bit 8 set again
        TXOFFSET = 0x1000 + rx_prev;             // TRANSMITS FROM THE RX AREA
        TXSIZE   = 0x14;
        rx_prev  = rx_next;
        store(f);                                // 0x8B80
    } else {                                     // 0x8A96: processes first, then waits for the transmitter
        store(f);
        while (!(STATUS & 4)) ;                  // busy wait INSIDE the IRQ
        f[19] = ...; TXOFFSET = 0x1000 + rx_prev; TXSIZE = 0x14;
        rx_prev = rx_next;
    }
    return;
drop:
done:
    rx_prev = rx_next;                           // 0x8CBA/0x8CC2/0x8D06: resynchronisation
}

store(f)                                         // 0x8B80
{
    if (f[2] == 0) {                             // 0x8C4E: AI car of another cabinet
        c = $104400 + ((f[3] & 0xE0) << 2);  c.$4A = id;  c.$4C = 0xFFFF;  ... c.$46++;
    } else {
        s = slot of id in $104080..$104380 (7 slots), else first free slot (s.$4A = id), else return;
        copy w2..w17 into s;  s.$46++;
    }
}
```
- One interrupt = one frame. RXSIZE is reset to 0 by the game and must be −0x14 (low byte 0xEC) at the next interrupt.
  If the chip stacks up two frames before the IRQ is serviced, both are lost (0xD8 ≠ 0xEC).
- The handler never reads STATUS to find out why it was called: every IRQ is treated as a reception.
  An IRQ without a frame reads RXSIZE = 0 and only resynchronises (`rx_prev = RXOFFSET`). This is harmless, except in the middle of a reception.

### 2.5 Machine count and liveness
- `$2012` = hop counter of my frame when it comes back = **N − 1**. It is only updated in that case, and never reset to 0 afterwards.
- Attract "UP TO n CAN RACE SIMULTANEOUSLY" (string 0x84CC): at 0x8450, character 6 is replaced by glyph `0x60 + $2012`, which displays N (H: glyph 0x60 = "1").
- Test mode (0x1466): "LINK OFF" if `me.$46` (= $4046) has not changed, otherwise "LINK ON n" with n = `0x81 + ($2012 & 0xF)` = N.
  The same 0x81 encoding is used for the coin settings, where the value 0 is displayed as "1".
- Remote slots (0x892A, at every VBLANK): a slot that receives frames restarts with a counter of 10.
  After 10 video frames without a frame (about 167 ms), it is freed.
- Lobby logic (0x26CC-0x27B8): `if ($2012 != 0)` → looks at the others' phase `$58` (4 or 5 = race in progress, can be joined).
  `$2012 > 4` lengthens the entry delay (0x270A: $7010 = 0x4EC instead of 0x3C0).

### 2.6 Final Lap 2 / Final Lap 3 (same code, shifted addresses)
| | finalap2 | finalap3 |
|---|---|---|
| init | 0xD008 (call at 0x1164) | 0xDB76 (call at 0x1164) |
| SCI IRQ (level 2) | 0xCA9C | 0xD60A |
| transmission (VBLANK) | 0xCDAA (call at 0x43C0); liveness 0xC980 | 0xD918 (call at 0x42D6) |
| frame | **0x16 words**: w0-w17 as in FL1, w18 = `($72&3)<<2 \| $68`, w19 = $7052, **w20 = hops**, w21 = checksum \| 0x100 | same |
| RXSIZE test | `== 0xEA` (−0x16) | same |
| LINK ON n (test mode) | 0x1B92: glyph 0xDD1 + $2012 | 0x1C84 |
| UP TO n (attract) | 0xC582 (large digits 0x3770 + 2·$2012); 0x30CA: 0xDD0 + $2012 + 1 | 0x170BA: 0x60 + $2012 |

Init identical to FL1: STATUS 0, START 0, wait for STATUS bit 2, RXSIZE/TXSIZE/RXOFFSET/TXOFFSET = 0, START 0, MODE 0x0D, my_id read from EEPROM 0x10-0x11.

### 2.7 Identity, car type, maximum number
- **The network identifies the cabinet by the 16-bit PCB number in EEPROM 0x10-0x11**, not by the car type.
  - Final Lap 1 (0x1730): if EEPROM byte 6 ≠ 0x57, the EEPROM is reinitialised (table 0x17CC) and the number is picked at random.
    The draw is `(u16)($44301A × $44301E)`, repeated as long as the result is zero (C116 registers 6 and 7, H: a counter on the real hardware).
    The number is shown in hexadecimal on the test screen (0x12BC-0x12CC). FL1 has no way to change it in test mode.
  - Final Lap 2 (0x1434) / 3 (0x1450): same draw if the signature (byte 6 = 0x57/0x88/0xA1) is missing. The number is shown as "PCB NO" in GAME OPTIONS (FL2 0x1A9E).
    It is re-drawn (`rand` 0xD960/0xE498 + video frame counter) in FL2 0x1CB8-0x1CEE and FL3 0x1DB2-0x1E0A.
    H: this takes a hidden combination on the screen that shows "H.OK / T.OK": `$8(a6) == 0x10` (steering wheel), `$0(a6) == 0x18` (accelerator) and an edge on bit 4 of `$460000`.
- **Car type** (connectors, MAME ports MCUB bit 1 and MCUH bit 7, read into $7114/$7116): it is only used for the vehicle.
  `$14` (0x2F4A-0x2F70) = combination of the two bits (0-3), or a random value if "Automatic Car Select".
  It is sent in w17 and selects the palette (0x6844). Two cabinets can have the same car.
- **Maximum 8 cabinets**: 7 remote slots ($104080-$104380) plus oneself. An 8th remote cabinet is relayed but not displayed (0x8BB0 `rts`).
  Relaying stops at 9 hops (w18 ≤ 8 before the increment).

### 2.8 Why Final Lap only sees one machine under the current emulation
1. **Main cause: the two MAME instances have the same PCB number.**
   - FL1: from a blank NVRAM (all 0xFF), the number comes from the product of C116 registers 6 and 7.
     MAME returns constant values for them (`namcos2_v.cpp`, `c116_r`: `if (offset > 0x180b) return 0xff; // fix for finallap boot`).
     The number is therefore the same in every instance: 0x0001 if the unwired high byte reads as 0xFF, 0xFE01 if it reads as 0.
     An NVRAM copied from one cabinet to another gives the same result.
   - FL2/FL3: the MAME sets load a default NVRAM from ROM (`ROM_REGION(0x2000, "nvram")`, `namcos2.cpp` l. 3017 and 3194). The PCB NO is therefore identical everywhere.
   - Consequence (0x8B20): B takes each of A's frames for its own one coming back. It does not relay it, it stores its hop counter (0) in $2012 and it increments $4046.
     Result on both cabinets: "LINK ON 1" in test, "UP TO 1" in attract, and the other car never enters the table.
     This happens whatever the ring IDs, as seen in the trials.
   - Proposed check: FL1 shows the same hexadecimal number in test on both cabinets, and "LINK ON 1" rather than "LINK OFF".
2. Even with distinct numbers, the emulated mode 0x0D breaks the protocol:
   - **automatic relay** of the frames whose ring ID differs: the duplicate keeps the hop counter. A's frame comes back to it with 0 hops, then with 1, then goes out again.
     The copies ping-pong up to 9 hops and $2012 oscillates;
   - **deferred transmission**: the frame only goes out at the next tick (800 Hz), with the registers as they are at that moment. STATUS bit 2 always stays at 1 (writing 0 gives 4).
     The VBLANK chains up to 9 TXOFFSET/TXSIZE pairs and the IRQ relay adds more. Only the last one goes out: the main frame is lost as soon as an AI follows, and relays are overwritten;
   - `send_data` does nothing as long as STATUS = 6. Under continuous traffic, a frame received at every tick blocks transmission.
3. What is right in the current emulation:
   - RXSIZE decremented (the game requires −0x14/−0x16 on the low byte);
   - TXOFFSET in the RX area: this is the game's relay mechanism, and the code already accepts it (`tx_mask = 0x1fff` in 0x0D). The "not supported" comment is out of date.
   The chip's relay, on the other hand, must go.

## 3. Four Trax, Suzuka 8 Hours, Suzuka 8 Hours 2, Dirt Fox: "slot-based" protocol, mode 0x09/0x0D

Same base code, with the following addresses:

| | fourtrax | suzuka8h | suzuk8h2 | dirtfoxj |
|---|---|---|---|---|
| link driver CPU | master | master | master | **slave** (the master publishes/reads the masks) |
| init | 0x498A (call at 0x13FA) | 0x5736 (0x180A) | 0x642C (0x119C) | 0x19EE (0x9EE), tables 0x19CE |
| periodic routine | 0x4A6A, VBLANK 0x33E0 | 0x5816, VBLANK 0x39EC | 0x650C, VBLANK 0x3F42 | 0x1A50, main loop 0xA20 |
| SCI IRQ | level 2, 0x4D34 | level 2, 0x5B26 | level 2, 0x681C | level 5 (C148 table 0x490), 0x4E6 → 0x1B28 |
| type (w1) | 0x41 | 0x66 | 0x82 | 0x42 |
| frame length | 0x25 | 0x25 | 0x25 | 0x20 |
| TXSIZE written | **0x2C** | 0x25 | 0x25 | **0x3F**, between START = 1 and START = 0 |
| slot setting | GRID NO = EEPROM byte 8 → $102004 (table 0x2EB4) | GRID NO = byte 9 | GRID NO = byte 9 | CAR TYPE (1-8, colour), $1005D4 |

### 3.1 Init (Four Trax 0x498A; the others are identical)
```
STATUS = 0; START = 0; $2282 = 10;
wait up to 10000 polls for STATUS & 4, else "SCI ERROR" + hang;   // 0x49A2
regs[0..7] = { 0x0000, 0x000F, 0x0000, 0x0003, 0x0000, 0x0000, 0x1000, 0x1000 };
     // STATUS, MODE = 0x0F, CONTROL = 0, START = 3 (config value), RXSIZE, TXSIZE, RXOFFSET = TXOFFSET = 0x1000
clear RAM 0x480000-0x483FFF (0x2000 words);
regs[0..7] = { 0x0000, 0x0009, 0x0000, 0x0000, 0x0000, 0x0000, 0x1000, 0x1000 };     // MODE = 0x09
reloc[0..15] = 0xFFFF ($2120); mine = 0xFFFF ($2170); pending = 0 ($2118); C148.ack_sci();
```
- The config value is **START = 3** for all four games (Dirt Fox has the same table).
- RXOFFSET and TXOFFSET are 0x1000: these are 13-bit word addresses. The game never writes RXOFFSET or RXSIZE again: the RX buffer is circular and managed by the chip.

### 3.2 Frame and exchange area in the C139 RAM
- Slots 0-15. Slot *s* has its TX area at words `0x800 + s·0x80`, at host address `0x481000 + s·0x100`.
- The C139 RAM also serves as a **mailbox between the two CPUs** (Dirt Fox) and between the VBLANK and the IRQ:
  - TX word 0x10 = N − 1 (written by the IRQ, read into $2008);
  - TX words 0x11-0x12 = mask of the slots seen;
  - TX words 0x20-0x21 = mask of my slots;
  - TX words 0x22-0x23 = copied into w3-w4.
- Frame: w0 = slot, w1 = type, **w2 = hop counter (1 when sent)**, w3-w4 = TX words 0x22-0x23, data (FT/S8H: w5-w0x23, DF: w3-w0x1E).
  The last word (FT/S8H w0x24, DF w0x1F) = sum of the low bytes of the previous words & 0xFF **| 0x100** (end of frame).

### 3.3 Periodic routine (Four Trax 0x4A6A, once per VBLANK)
```
if ($2282) $2282--;                                  // link liveness (15 video frames, cf. 3.4)
mine = (mine & 0xFF00) | (1 << grid_no);             // $2112; high byte = other slots that are mine (H: AI)
TX[0x20] = mine >> 8; TX[0x21] = mine & 0xFF;
seen = TX[0x11] << 8 | TX[0x12] & 0xFF;              // $2110
others_minus_1 = TX[0x10] & 0xFF;                    // $2008 = N - 1
for s in 0..15:
    if (mine & bit(s)) write my car data into TX slot s (w5..);       // 0x4ADA
    else if (seen & bit(s)) read car data of slot s from its TX copy;  // 0x4B80, re-read if modified in the meantime
every 2nd call: TX[0x11..0x12] = OR of the last 16 $2150 snapshots;   // slots seen over about 32 video frames
for s in 15..0 with bit(s) in (TX[0x20]<<8 | TX[0x21]):               // 0x4CA6
    f = TX slot s; f[0] = s; f[1] = TYPE; f[2] = 1; f[3..4] = TX[0x22..0x23];
    f[last] = sum8(f[0..last-1]) | 0x100;
    reloc[s] = byte offset 0x1000 + s*0x100;  pending |= bit(s);
    MODE &= 0x0B;                                    // 0x4D04: clears bit 2 = "transmitter free" IRQ request
```

### 3.4 SCI interrupt (Four Trax 0x4D34)
```
st = STATUS;
if (st & 2) {                                         // ---- reception (takes priority)
    p = (RXOFFSET*2 & 0x1FFF) | 0x2000;               // current RX pointer (word address × 2)
    for (k = 0; k < 0x26; k++, p -= 2)                // DF: 0x21
        if (RAM16[p] & 0x100) goto found;             // searches BACKWARDS for the end word (bit 8)
    goto out;
found:
    p -= 2*(LEN-1);  start = p;                       // $2144 (LEN = 0x25, DF 0x20)
    s = w0;  if (w1 != TYPE) goto out;
    w2 += 1 (except the S8H monitor, cf. 3.6); written into the RX buffer;  if (w2 >= 0x10) goto out;
    if (mine & bit(s)) {                              // MY frame has gone round the ring
        TX[0x10] = max(w2 - 2, 0);                    // = N - 1 (N increments, mine included)
        $2282 = 15;  goto out;                        // "LINK ON"
    }
    copy data words to tmp; last = last + 1 | 0x100 (written into the RX buffer);
    if (sum8 != last) goto out;                       // checksum corrected for the increment
    copy data into TX slot s (w5.., DF w3..); TX slot s, high byte of word 0 += 1 ("new frame");
    reloc[s] = start;                                 // RELAY: the frame will be sent again from the RX area
    pending |= bit(s);  seen_now |= bit(s);
} else if (st & 4) {                                  // ---- transmitter free
    for (i = 0; i < 16; i++, rr = (rr+1) & 15)        // round robin $2140
        if (pending & bit(rr)) {
            pending &= ~bit(rr);
            TXOFFSET = reloc[rr] >> 1;                // 0x800+s*0x80 (my frame) or 0x1000+pos (relay)
            TXSIZE   = 0x2C;                          // S8H/S8H2 0x25; DF: START=1; TXSIZE=0x3F; START=0
            break;                                    // ONE frame per IRQ
        }
    START = 0;
}
out:
STATUS = 0;                                           // FT/S8H; DF only writes it in the RX branch
C148.ack_sci();
MODE = 0x09 | (pending ? 0 : 4);                      // FT/S8H. DF: MODE &= 9 if frames remain/RX, MODE &= 0x0D otherwise
```
- Transmission is **interrupt-driven**. When the game has something to send, it clears MODE bit 2.
  The chip must then raise an IRQ with STATUS bit 2 ("transmitter free"). The handler sends one frame, then sets MODE back to 9 if any are left.
  When the queue is empty, it sets MODE to 0x0D, which disables this IRQ.
- Dirt Fox **never** sets MODE bit 2 itself:
  - periodic routine: `MODE &= 0x0B`;
  - IRQ, if frames remain or after a reception: `&= 9`;
  - IRQ, when the queue is empty: `&= 0x0D` (leaves bit 2 as it is).
  **Strong H**: when the chip raises the "transmitter free" IRQ, it sets MODE bit 2 itself, and the software clears it to re-arm it.
  Without that, Dirt Fox would be either flooded with IRQs or stuck (no edge when it clears a bit that is already 0).
  Same logic for bit 1 and the receive IRQ (FL1 rewrites 0x0D, kyukaidk does `bclr #1, MODE`, ridgera2 `&= 9 / &= 0xD`).
- Reception only processes **the most recent frame**: the search starts from RXOFFSET and goes backwards.
  If two frames have arrived before the IRQ, the older one is lost: one frame must be delivered per IRQ.
- TXSIZE 0x2C (FT) and 0x3F (DF) exceed the frame length (0x25, 0x20). **H:** in 9-bit mode, the chip stops at the word that carries bit 8.
  If it sent everything, the extra words would be zeros (end of the TX area) or the rest of the RX buffer, without bit 8. The backward search (window LEN + 1) tolerates them: both models work.

### 3.5 Machine count
- N − 1 = `w2 − 2` of my returned frame → TX word 0x10 → `$2008` (FT 0x4AB8, S8H 0x5884, DF master 0x8A16 → $10A91C).
- Four Trax, attract "UP TO n CAN RACE SIMULTANEOUSLY" (string 0x1176): at 0x122E, character 6 = `'1' + $2008` = N.
  Test mode: "LINK" at 0x178A, digit `0x291 + $2008`, ON/OFF depending on `$2282` (my frame came back within the last 15 video frames).
- Suzuka 8h/8h2: same thing ("UP TO ? CAN RACE", 0x13C0).
  With the "System is Status Monitor" DIP ($460027 bit 6 → $40D410 = 1, 0x2E54), the machine does not play: slot 0, no slot of its own.
  It **relays without incrementing** the hops (`seq d5 ; sub.b d5,d0`, 0x5C02) and counts `popcount(seen & 0xFF) − 1` (0x582E).
- Dirt Fox: the master also reads the number of slots seen, `popcount(TX[0x11..0x12])` → $10AA24 (0x89E0).

### 3.6 Settings, maximum number
- **Slot = GRID NO** (FT byte 8, S8H/S8H2 byte 9 of the EEPROM; default value **0** in tables 0x2E9E (FT), 0x35EA (S8H), 0x34A4 (S8H2)).
  Dirt Fox: **CAR TYPE** in GAME OPTIONS (1-8 "… 7 GREEN, 8 BLUE", variable `a6+0x194` = $1005D4, `& 7` at 0x899E), in the checksum-protected EEPROM (signature "DART").
  Dirt Fox has a default NVRAM in ROM in MAME (`namcos2.cpp` l. 2561).
- Two cabinets with the same slot each take the other's frames for their own. **A different GRID NO / CAR TYPE per cabinet** is required.
- The player masks fit in 8 bits (low byte). The S8H monitor only counts the low byte and DF does `& 7`. **8 player cabinets**, plus slots 8-15 (H: AI vehicles).
  A frame dies at 16 hops.
- The vehicle type ("CAR TYPE" of FT/S8H) is a separate setting. For FT/S8H, H: it is a livery, as in Final Lap.

### 3.7 Notes on the current emulation (mode 0x09)
- `m_reg_f3 & 2` (START = 3 in config) divides TXOFFSET by 2. This contradicts the code: TXOFFSET = `reloc >> 1` is already a word address, 0x800 + s·0x80 or 0x1000 + RX position.
- The emulation never raises the "transmitter free" IRQ: so the game never enters its transmit branch.
  The link holds (H) through a series of coincidences:
  - the tick sends on its own from 0x1000 >> 1 = 0x800, i.e. the slot 0 area;
  - `find_sync_bit` looks there for the first bit 8, which also catches another slot's frame if area 0 is empty;
  - the receiver's backward search only keeps the end, so the extra beginning is ignored;
  - the chip relays with its own counter `m_buffer[0x1ff]`, fed back into w2.
  The game's own relays, for their part, never go out.

## 4. What these games say about the chip (registers)
Sources: the code above, plus the "COMMUNICATION TEST" (HSCI/LSCI) of the System 2 test library (Dirt Fox master 0xE67A/0xE6D0, identical in about twenty games, § 5).
This test does a TX → RX loopback by polling: it writes n words in TX, RXSIZE = TXSIZE = n, waits for TXSIZE.b = 0, then checks three things:
- `RXOFFSET − 0x1000 == TXOFFSET`;
- `RXSIZE.b == 0`;
- the received data (& 0x1FF).

| Register | Expected behaviour |
|---|---|
| STATUS | bit 2 = **transmitter free**, read live: 1 at reset (otherwise "SCI ERROR"), 0 during a transmission. bit 1 = frame received (IRQ cause, tested first by FT/S8H/DF). bit 0 = error (kyukaidk counts it). Writing 0, or `bclr`, clears bits 0-1; bit 2 cannot be modified. |
| MODE | bit 3 always 1. bit 0 set in all the System 2 games that link (bit 8 = end of frame supplied by the game). **bit 1** = RX IRQ locked/masked. **bit 2** = "transmitter free" IRQ locked/masked. 0x0F = everything masked (init); 0x0D = RX only (Final Lap, kyukaidk); 0x09 = RX + TX. **H:** the chip sets the bit when it raises the matching IRQ, and the handler clears it last (cf. 3.4). |
| START | never used by FL (always 0). FT/S8H: 0 after TXSIZE. DF, kyukaidk, ridgera2: 1 before TXOFFSET/TXSIZE, 0 after. **H:** bit 0 = "freeze"; transmission starts when TXSIZE ≠ 0 and START bit 0 = 0. Config value (written under MODE 0x0F) = 3 (FT/S8H/DF), 0x11 (ridgera2). **H:** bit 1 = data rate (the test alternates START 0/2 between "HSCI" and "LSCI"; Metal Hawk and Dirt Fox have "1 MBPS / 2 MBPS" strings). |
| RXSIZE | **down-counter**, −1 per word received (FL resets it to 0 and waits for −0x14; the test loads it with n and waits for 0). |
| TXSIZE | length to transmit, **counts down to 0** as the frame goes out (the test and sws wait for TXSIZE.b = 0). Writing it triggers the transmission (with START bit 0 = 0). |
| RXOFFSET | 13-bit RX pointer, word address (0x1000 = start of the RX area), post-incremented. The write goes to `0x1000 \| (RXOFFSET & 0xFFF)`, wrapping around on 12 bits. FL writes 0 and masks `& 0xFFF`; FT/DF write 0x1000 and read it on 13 bits. |
| TXOFFSET | word address, **13 bits across the whole RAM**: the RX area is used for relaying (0x1000 + position). Post-incremented during transmission (the test reads it back). Never any byte addressing in these games. |
| CONTROL | never written by FL, always 0 in FT/S8H/DF: bit 8 is written directly by the CPU (`+0x100`, `ori #$100`). **H** (sws, ridgera2): with CONTROL = 3, CPU writes set bit 8 ("end word"); with CONTROL = 1, they clear it. |
| RAM | 0x2000 × 9 bits; bit 8 can be read and written from the host. Bits 9-15 are not used (FT does `addq.b #1` on the high byte of a TX word that is never transmitted). |

## 5. Other System 2 games

Twenty games contain the same **loopback communication test** routine (HSCI/LSCI, RXSIZE/TXSIZE/pointer/data test, no IRQ) in their test mode.
It is also on the master of kyukaidk (0x1F872), sws (0x2C29C), sws92 (0x31404) and sws93 (0x3409C), which also have a real link (see below).
For the following sixteen games, the C139 serves no purpose in game:
- assault (0xCEB8);
- bubbletr (0x32EA, "SCI ERROR" strings);
- burnforc (0x2E9C);
- cosmogng (0x2284);
- dsaber (0x31A4);
- finehour (0x2924);
- gollygho (0x32D8);
- marvland (0x303A);
- mirninja (0x1EEAA);
- ordyne (0x21CEA);
- phelios (0x2710E);
- rthun2 (0x15BFA on the master, copy at 0x15BFE in the slave);
- sgunner (0x235D2);
- sgunner2 (0x38DCE);
- valkyrie (0x3E2C6);
- Dirt Fox (0xE67A, master).

The other "accesses" found by `c139scan.py` in these games are data constants (`ori.l #$48xxxx`, `btst …$480400`).

- **assault, burnforc, cosmogng, dsaber, finehour, gollygho, marvland, mirninja, ordyne, phelios, rthun2, sgunner, sgunner2, valkyrie, bubbletr**: no link. Only the COMMUNICATION TEST of the test screen, which requires a TX → RX loopback.
  Without loopback, MAME should display an error; this has no consequence.
- **luckywld**: no access to the C139 (the only one found, 0x22098, is data). No link.
- **metlhawk**: no register access. The C139 RAM only appears in the RAM test table (0x1CBA).
  The strings "SCI OK", "1 MBPS / 2 MBPS", "LSCI FLAME ERROR", "LSCI NOT CONNECT" (0xA80-0xBC0) exist, but no code found drives the chip.
  H: link planned but not used, or driven through addressing that the scan did not see.
- **kyukaidk** (Kyuukai Douchuuki): **real link**, driven by the slave.
  - Init 0x13AE: registers = {0, 0x0F, 0, 0, 0, 0, 0x1000, 0}, SCI level 4, then `bclr #1, MODE` → 0x0D.
  - IRQ 0x16C2: `bclr #1, STATUS`, C148 acknowledge, `bclr #1, MODE`, `bclr #0, STATUS` (errors).
  - Variable-length frames; the last word (bit 8) = machine number 0-14 (0xF = broadcast) and a type.
  - Reads **forwards** from a software pointer up to RXOFFSET.
  - Software relay with a hop counter (0x18BC: TXOFFSET = 0x1000 | position, limit 0xF), START bit 0 set around TXOFFSET/TXSIZE.
  - Use: H, play between linked cabinets. It confirms Final Lap's MODE 0x0D / bit 1 model.
- **sws, sws92, sws93** (Super World Stadium): **real link** on the slave (0x254-0x874, same code in all three).
  - MODE 0xFC (0x0C + high bits), CONTROL 1/3.
  - Fixed frames of 0x48 words, in 16 areas of 0x80 words in TX.
  - Before each transmission, the game waits for `TXSIZE.b == 0`.
  - Software relay: copy into the slot's TX area, then retransmission.
  - Machine number 0-3 (setting), ID drawn from the product of the C116 registers (0x5B6, same pitfall as Final Lap).
  - H: up to 4 cabinets. The end word (bit 8) is set with CONTROL = 3 (0x728: `CONTROL = 3 ; clr.w $8E(a0)`).

## 6. What the C139 must do for these games
1. **Relay nothing, filter nothing, modify nothing.**
   - Every received frame goes into the RX RAM, including the machine's own frames that have gone round the ring.
   - The game relays what it wants by pointing TXOFFSET into the RX area.
   - The "ring ID" (`m_linkid`), the automatic relay, `m_buffer[0x1ff]` and the rewriting of w2 in mode 9 must be removed for these modes.
   - H, to be checked on the S22 games in mode 09 (acedrive, victlap): if they relay by themselves like Four Trax, the same rule applies to them.
2. **Transmission**
   - It starts when TXSIZE ≠ 0 and START bit 0 = 0, evaluated on writes to TXSIZE and START.
   - The chip takes the words `RAM[(TXOFFSET + i) & 0x1FFF]` on 9 bits, as they are, without adding a bit 8.
   - It stops after TXSIZE words, or (H) at the first word that carries bit 8 when MODE bit 0 = 1.
   - At the end of the transmission: TXOFFSET += n, TXSIZE = 0.
   - The data must be **captured at write time** (instant transmission): with deferred transmission, frames get overwritten (§ 2.8).
     If the transmission takes time, STATUS bit 2 must be 0 during that time.
3. **Reception**
   - One packet = one frame. Each word goes to `RAM[0x1000 | (RXOFFSET & 0xFFF)]`; then RXOFFSET++ (bit 12 kept, low 12 bits wrapping around) and RXSIZE−−.
   - Then STATUS |= 2 and RX IRQ if MODE bit 1 = 0 (H: the chip then sets MODE bit 1).
   - **Only one frame per IRQ.** The following frames stay in the network queue, without being lost, until the game re-arms.
   - Re-arming is the write to MODE with bit 1 at 0: it is the last write of all the handlers.
     Do not rely on `STATUS = 0`: Four Trax writes it **before** the C148 acknowledge, so an IRQ raised at that moment would be cleared.
   - Final Lap requires RXSIZE to be exactly −length (low byte) at each IRQ.
   - Optional (H): clear bit 8 of the word at RXOFFSET after each frame, so that a stale end word is not picked up by the backward search of FT/S8H/DF. This word is not part of any frame read by the games studied.
4. **"Transmitter free" IRQ**
   - It is raised (STATUS bit 2) when the transmitter is free and MODE bit 2 = 0, so as soon as the game clears that bit.
   - H: the chip then sets MODE bit 2, to avoid Dirt Fox's IRQ storm.
   - Never in MODE 0x0D: Final Lap's handler takes every IRQ for a reception.
5. **STATUS**
   - Bit 2 true at reset and whenever the transmitter is free (the "SCI ERROR" wait at init).
   - Writing clears bits 0-1 without touching bit 2.
6. **No byte addressing** for START = 3: TXOFFSET and RXOFFSET are word addresses.
   MODE 0x0F = "everything masked" (the config is written under it); it is not a transmission mode.
7. **On the MAME side, setting up the cabinets**
   - Final Lap 1/2/3: a **different, non-zero** PCB number per instance, bytes 0x10-0x11 of the NVRAM file.
     FL2/FL3 can also re-draw it in test; FL1 cannot.
     Another option on the driver side: make C116 registers 6/7 vary from one instance to another. But MAME is deterministic: two identical instances would draw the same number.
   - Four Trax / Suzuka: distinct GRID NOs (GAME OPTIONS, byte 8 or 9 of the NVRAM file).
   - Dirt Fox: distinct CAR TYPEs.
   - Maximum 8 cabinets in all these games.
8. **Data rate** (H): 1 or 2 Mbit/s according to the strings. The games need no realistic timing, as long as no frame is lost or overwritten.

## 7. Open questions
- Exact role of START = 3 in config, and of the high bits of MODE (sws 0xFC, alpinr2b 0xFD).
- In 9-bit mode, does transmission stop at bit 8 (TXSIZE 0x2C/0x3F)? No effect on these games, but to be settled on a real board.
- Does the chip set MODE bit 1/bit 2 when it raises the IRQ? Dirt Fox needs it; to be checked by running Dirt Fox linked with the proposed model.
- Value of the unwired high byte of `c116_r` (0xFF or 0), which gives Final Lap 1's ID 0x0001 or 0xFE01 (no consequence: it is the same everywhere).
- Contents of the default NVRAMs of finalap2/finalap3/dirtfoxj (PCB NO / CAR TYPE), not dumped here.
