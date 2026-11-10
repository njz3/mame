# C139 on Super System 22: dirtdash, tokyowar, alpinr2b, alpinerd, alpines, aquajet, propcycl, timecris, airco22b

Static analysis (capstone M68K) of the `:maincpu` regions dumped by MAME (32-bit words byte-reversed).
Super 22: C139 registers at 0x420000 (+0 STATUS, +2 MODE, +4 CONTROL, +6 START, +8 RXSIZE, +A TXSIZE, +C RXOFFSET, +E TXOFFSET),
RAM at 0x410000 (0x2000 half-words; TX = words 0-0xFFF = 0x410000-0x411FFF, RX = words 0x1000-0x1FFF = 0x412000-0x413FFF).
System controller at 0x700000: +2 = SCI IRQ level (0 = disabled), +6 = SCI acknowledge.
EEPROM 0x460000 (`umask32(0xff00ff00)`): EEPROM byte *n* is at address 0x460000 + 2*n.

Conventions: `a6 = $E08000` in tokyowar (e.g. `-$5FFA(a6)` = 0xE02006). "H:" marks a hypothesis.

**Tool pitfall**: capstone does not show the index scale factor when the "brief" extension word has a non-zero displacement.
`30317402` is shown as `move.w $2(a1, d7.w), d0` whereas it is `move.w 2(a1,d7.w*4),d0` (bits 10-9 of the extension word = 10).
The bytes must be checked whenever a table is indexed (this also applies to the other analyses done with `c139dis.py`).

## 1. Verdict per game

| Game | Real link? | What the main CPU does with the C139 | SCI IRQ |
|---|---|---|---|
| **tokyowar** | **yes, 2 to 4 cabinets**, ring with **software relay** | full driver 0x92F82-0x93820; one frame per VBLANK + relay in the IRQ | level 4 (0x4274), vector 0x70 → 0x932A0 |
| **alpinr2b** | **yes, 2 cabinets** (LEFT/RIGHT), no relay | driver 0x60FC-0x63BE (compiled C); one frame per video frame, called by the main loop (0x5D16/0x5E08/0x5E94 via `d7 = #$6270`) | level 4 (written by the transmit routine 0x63B2), vector 0x70 → 0x60DC |
| dirtdash | **no** | `MODE = 0x0D` only once (0x1384); reset routine 0x2130 never called | **disabled**: `$700002 = 0` (0x12A8); level 2 handler (0x158A) = plain acknowledge |
| alpines | no (dead code) | driver copied from alpinr2b (init 0x6312, transmit 0x64B6) **never referenced**; COMMUNICATION TEST screen (0x9EB2/0x9F3E) that fills the buffer without ever transmitting | never programmed (only the dead routine writes `$700002`) |
| timecris | no | at boot (0x1135C): init 0x128E8 (`STATUS 0, MODE 0x0F, CONTROL 0, START 3`, the rest at 0), then 9-bit RAM test (0x1292A, patterns 0x1FF) | disabled (`$700002 = 0`, 0x12C7C) |
| alpinerd | no | RAM test 0x410000-0x413FFF at boot (0x8C26); no register | level 2, handler 0x8CA2 = acknowledge |
| aquajet | no | RAM test at boot (0x367A); no register | level 2, handler 0x3CDC = acknowledge |
| airco22b | no | RAM test at boot (0x519C/0x51C2); no register | level 3, handler 0x5508 = acknowledge |
| propcycl | no | no access (the "hits" 0xC2BC/0xC306 are data constants) | level 3, handler 0xC1B8 = acknowledge |

Strings: only tokyowar ("LINK", "ID NO.", "TEAM COLOR", "1VS1 … 3VS1 BATTLE") and alpinr2b ("LINK ON 1/2", "LINK OFF", "LINK NG", "ID NO.", "COMMUNICATION TEST")
have link settings. Dirt Dash has neither "LINK" nor a cabinet option (GAME OPTIONS: difficulty, distances, STAGE SELECT, motor, CABINET MOTION, sound, speed).

## 2. Tokyo Wars (tokyowar, TW2 Ver.A)

### 2.1 Init (0x92F82, called once at boot from 0x4250 after the 0x2000 words of C139 RAM are cleared at 0x422E)
```
id = EEPROM[8..9] & 3              // movep.l $10(0x460000) >> 16 ; -$6000(a6) = id ; -$5FFC = id
-$5FEC = -$5FE8 = 0                // "virtual node" masks (AI tanks), see 2.6
MODE     = 0x0F                    // 0x92FB2
CONTROL  = 0
START    = 1
RXSIZE   = 0xFF                    // never read back by the game
TXSIZE   = 0
RXOFFSET = 0x1000                  // start of the RX area
TXOFFSET = 0
STATUS   = 0
MODE     = 0x0D                    // 0x92FEE
START    = 0
// then the counters, the TX queue and the histories are reset
```
Then: `$700002 = 0x34` (SCI IRQ level 4), VBLANK level 2 (vector 0x68 → 0x49E0).

### 2.2 Frame format (fixed: 0x46 = 70 half-words, one byte per half-word)
| word | contents |
|---|---|
| 0 | `0x81 + id` |
| 1 | hop counter (0 on transmission) |
| 2 | `0xA2` (type marker) |
| 3 | `0x73 + id` |
| 4-0x43 | 64 bytes of tank state (`-$4E00(a6) + id*0x40` = 0xE03200 + id*0x40) |
| 0x44 | copy of the hop counter (0 on transmission) |
| 0x45 | `0x100 \| sum8`: **bit 8 = end of frame**; sum8 = low byte of (0x81+id) + 0xA2 + (0x73+id) + Σ data |

Source: built at 0x930CA (own frame at TX word 0) and 0x931B4 (frames of virtual nodes 4-15, table 0x9322A:
node *n* read from `-$4E00 + tbl[n].src` and written at TX word `tbl[n].dst` = 0x46, 0x8C, … 0x348).

### 2.3 Own transmission, at every VBLANK (0x49E0 → `jsr 0x9362E` then `jsr 0x93072`, unless the DSP is busy `$C00000 bit 0`)
```
93072: -$5FFC = id ; -$5FEA = -$5FEC & 0xFFF0
       if (TXSIZE & 0xFF) != 0 && (TXOFFSET & 0x1000) == 0: return   // the C139 is still transmitting OUR frame: skip this VBLANK
       if -$5FF6 != 0xFFFF: -$5FF6++          // frames transmitted since ours last came back
       build_own_frame(TX[0])                 // 2.2
       build_virtual_frames()                 // 0x931B4, for each bit 4..15 of -$5FEA
       MODE = 0x0F                            // 0x9311E: critical section (H: bit 1 = C139 IRQ masked)
       if (TXSIZE & 0xFF) != 0:               // busy relaying → queue everything
           queue_push(0) ; queue_push(virtuals) ; MODE = 0x09 ; return          // 0x93190
       queue_push(virtuals)                   // 0x9326A
       if queue not empty:                    // the end-of-transmission IRQ will be needed
           TXOFFSET = 0 ; START = 1 ; TXSIZE = 0x46 ; MODE = 0x0B ; START = 0 ; MODE &= 0x0D   // → 0x09
       else:
           TXOFFSET = 0 ; TXSIZE = 0x46 ; MODE = 0x0D                                          // no end IRQ
```
The software queue: 32 TXOFFSET entries at `-$5FE0(a6)` (0xE02020), head `-$5FE4`, tail `-$5FE2`.

### 2.4 SCI IRQ handler (level 4, 0x932A0)
```
if $4302(a6) || $4300(a6): $700006 = 0 ; rte          // link suspended: only acknowledge the controller
st = STATUS
if !(st & 2): goto tx_done                             // 0x932E8: anything that is not "RX" is handled as an end of transmission
-$5F9E++ ; STATUS = 0 ; $700006 = 0
// --- frame search: backwards from RXOFFSET-1, at most 0x46 words, first word with bit 8
p = (RXOFFSET & 0xFFF) ; repeat 0x46: p = (p-1) & 0xFFF ; if RX[p] & 0x100: found
if !found: err ; goto next_tx
s = p - 0x45                                           // start of frame (ring of 0x1000 words)
if (RX[s+2] & 0xFF) != 0xA2: err ; goto next_tx
if (RX[s+1] & 0xFF) != (RX[s+0x44] & 0xFF): err ; goto next_tx
hop = RX[s+1] + 1 ; if hop > 4: err ; goto next_tx      // TTL: at most 4 hops
RX[s+1] = RX[s+0x44] = hop                             // the game MODIFIES the frame in place in the RX area
n = RX[s] - 0x81 ; if n != RX[s+3] - 0x73 || n >= 16: err ; goto next_tx
if n == id:                                            // our frame has gone all the way round the ring
    -$5FF6 = 0 ; seen |= 1<<n ; -$5FF8 = hop & 7       // hop = number of machines in the ring
    goto next_tx                                       // removed from the ring (no relay)
if -$5FEA & (1<<n): seen |= 1<<n ; goto next_tx        // one of our virtual nodes: no relay
copy RX[s..s+0x44] (low bytes) to the double buffer of node n (-$5E00 + n*0x8C [+0x46]), sum8 of these 69 bytes
RX[s+0x45] += 2 (low byte)                             // checksum adjusted for the two incremented counters
if (RX[s+0x45] & 0xFF) != sum8: -$5F9A++ ; err ; goto next_tx   // bad checksum: NOT relayed
if n >= 4 && hop >= 3: seen |= 1<<n ; goto next_tx     // virtual node: relay limited to 2 hops
seen |= 1<<n
// --- SOFTWARE RELAY: the frame is sent back as it stands in the RX area
off = 0x1000 + s                                       // TXOFFSET points into the RX area (bit 12)
if (TXSIZE & 0xFF) != 0:  queue_push(off) ; MODE = 0x09            // 0x934D8
elif queue not empty:     TXOFFSET = off ; START = 1 ; TXSIZE = 0x46 ; MODE = 0x09 ; START = 0
else:                     TXOFFSET = off ; TXSIZE = 0x46 ; MODE = 0x0D
rte
next_tx:  MODE = queue not empty ? 0x09 : 0x0D ; rte
tx_done:  // 0x935A6
-$5F9C++ ; $700006 = 0                                 // (STATUS is not cleared here)
if queue empty: -$5F96++ ; MODE = 0x0D ; rte
off = queue_pop()
if queue still not empty: TXOFFSET = off ; START = 1 ; TXSIZE = 0x46 ; MODE = 0x09 ; START = 0
else:                     TXOFFSET = off ; TXSIZE = 0x46 ; MODE = 0x0D
rte
```

### 2.5 Machine count and linked-game decision (0x9362E, at every VBLANK)
```
MODE = 0x0F
-$5FF2 = seen ; seen = 0 ; history of 8 masks (-$5EB0) ; if all 8 are equal: -$5FF0 = stable mask
history of 8 values of -$5FF8 (-$5E90) ; if all 8 are equal: -$5FFA = stable number of machines ; -$5FF8 = 0
-$5FF4 = (-$5FF6 < 8)            // "link alive": our frame came back within the last 8 transmissions
copy of the per-node double buffers to the game structures ; MODE = queue ? 0x09 : 0x0D
```
- The number of machines is **the hop counter of our own frame when it comes back**: each other cabinet increments it once,
  and we do once → value = N. Alone (TX looped back onto RX) → 1. Nothing comes back → 0.
- The game copies `-$5FFA` and `-$5FF0` to 0xE1C2F2 / 0xE1C2F4 at the start of the match (0x1A734) and uses them to form the teams
  (0x1A8EA, 0x1CEB8: 0 is treated as 1). Modes 1VS1, 2VS1, 2VS2, 3VS1.
- **Max 4 cabinets**: 2-bit ID, TTL of 4 hops (with 5 machines our frame is rejected at the 5th hop → count 0).
- Virtual nodes 4-15: AI tanks hosted by a cabinet (table 0x93A80 per ID: 0x0071, 0x0382, 0x1C04, 0xE008 → 3 AI tanks per cabinet when `$1304(a6)` ≠ 0).
  A cabinet can therefore transmit up to 4 frames per VBLANK, plus the relays: up to 16 frames of 70 words per video frame on each link segment with 4 cabinets.

### 2.6 Settings (GAME OPTIONS, strings 0x12A2C)
- **ID NO.**: 0-3, word `$200C(a6)` (0xE0A00C) ↔ EEPROM bytes **8-9** (big-endian word, the ID is byte 9 & 3), saved by `movep.l` at 0x11700, read back at 0x1174C, 0x538C and 0x92F88.
- **LINK**: read-only, shows `-$5FFA` (0x12D06): 0 → "NG", 1 → "OFF", 2-4 → the digit.
- **TEAM COLOR**: GREEN/WHITE, `$201C(a6)` (0x12CF2; EEPROM 0x1008-0x1009 via `movep.l $2010(a0)`, 0x11718).
- Two cabinets must not have the same ID: each would take the other's frame for its own (counted as a return, never copied).
- Debug overlay at 0x94008, never called (DSW `$440001` bit 7): ID, count, masks, RX/TX/error counters.

### 2.7 What tokyowar tells us about the registers
- **MODE is a bit field, not a mode number**: the game keeps switching between 0x0F, 0x0B, 0x09 and 0x0D.
  - bit 2 (0x04) = 1: **end-of-transmission IRQ masked**. 0x09 only when the software queue is not empty (the IRQ is needed to drain it), 0x0D otherwise.
  - bit 1 (0x02) = 1: set during the critical sections (0x9311E, 0x9362E) then cleared. H: global mask of the C139 IRQs (reception continues).
  - bits 0 and 3 always set.
- **TXSIZE**: writing a non-zero word count starts the transmission (with START = 0). The game reads back `TXSIZE & 0xFF` ≠ 0 as "transmitter busy": it must stay non-zero during the transmission and return to 0 at the end.
- **START**: 1 while programming (TXOFFSET/TXSIZE/MODE), then 0. H: bit 0 = "hold the transmission", so as not to miss the end-of-transmission IRQ. The 0x0D path writes TXSIZE with START already at 0.
- **TXOFFSET**: 13-bit address; 0x1000 + RX position to relay. It is read back during a transmission (bit 12 = "I am relaying").
  H: the address must wrap within the selected half (0x1FFF → 0x1000), otherwise relays of frames that straddle the end of the RX ring would be wrong.
- **RXOFFSET**: RX write pointer (next word), 12 bits within the RX area; initialized to 0x1000.
- **STATUS**: bit 1 = frame received; any other IRQ cause = end of transmission; writing 0 acknowledges.
- **RXSIZE**: 0xFF written once, never read back. It must not block reception: a counter that stopped at 0 would cut the link after 3 frames.

## 3. Alpine Racer 2 (alpinr2b, ARS2 Ver.B)

### 3.1 Init (0x60FC, called at boot at 0x5B90)
```
MODE = 0x00FF ; CONTROL = 0 ; START = 0 ; RXOFFSET = 0 ; RXSIZE = 0 ; TXOFFSET = 0 ; TXSIZE = 0
// no STATUS write; MODE stays 0xFF (bit 1 set) until the first transmission
```

### 3.2 Transmission, once per video frame (0x6270, called by the main loop, before or after reception depending on a phase toggle)
```
$700002 = 0                                   // disables the SCI IRQ at the controller during the routine
buf = (toggle == 0) ? 0x200 : 0               // two TX buffers (words 0 and 0x200)
if n = payload_len ($E17E8C) != 0:            // 0x66 = 102 bytes during play (0x2EDA6)
    TX[buf+0] = n + 2                         // length, not transmitted
    TX[buf+1] = 0x0A5                         // header
    TX[buf+2 .. buf+1+n] = payload bytes ($E17D92)
    TX[buf+2+n] = 0x100 | (n + 1)             // end of frame: bit 8 + distance in words back to the header
    toggle ^= 0x200
    if (TXSIZE & 0xFF) == 0:                  // transmitter free; otherwise the frame is lost for this video frame
        TXOFFSET = toggle + 1 ; START = 1 ; TXSIZE = TX[toggle] & 0xFF (= n+2 = 0x68) ; START = 0 ; TX[toggle] = 0
copy of the received frames (list of at most 8) from the RX area to 8 slots of 0xFA bytes at $E17E90
MODE &= 0xFFFD ; $700002 = 4
```
**No relay**: TXOFFSET never points into the RX area.

### 3.3 SCI IRQ handler (0x60DC → 0x616C)
```
st = STATUS ; STATUS = 0 ; MODE = 0xFFFD            // bit 2 always set: never an end-of-transmission IRQ
if st & 2:
    p = RXOFFSET & 0xFFF ; search backwards (p included, at most 32 words) for a word with bit 8
    if not found: $E18660++ ; goto end
    L = RX[p] & 0xFF ; RX[p] = 0                    // clears the end mark
    h = (p - L) & 0xFFF
    if (RX[h] & 0x1FF) != 0xA5: $E18664++ ; goto end
    add (h+1, L) to the list $E1B00C (8 entries; the oldest drops out when the list is full)
end: STATUS = 0 ; MODE &= 0xFFFD ; $700006 = 0
```

### 3.4 Validation and link state (0x2EDB4, once per video frame)
- Only **the last frame received** during the video frame is examined (the highest non-empty slot); no frame → counter `$E1866A`,
  after 60 video frames → type 0x51 (link lost); otherwise type 0x50.
- The payload contains a 32-bit tag at +0x10: `0x173 << 20 | frame_counter << 4 | ID`, set on transmission (0x2EAD6).
  - tag == the last one transmitted, or `(tag & 0xF) == (my ID & 0xF)` → **it is my own frame** → state 7;
  - `tag >> 20 != 0x173` → state 9;
  - 16-bit checksum at +0x62 (and +0x64 for type 0x40) wrong → error counters `$E1866C` / `$E18670`;
  - otherwise state 4 and the other skier's data are taken.
- Display (0xFF7C, GAME OPTIONS) of `$E1867E`: 4 → "LINK ON 2", 1 → "LINK OFF", 7 → "LINK ON 1", 9 → "LINK NG".
  The linked race tests state 4 (0x3B4CA). "LINK ON 1" means "I receive my own frames", i.e. a one-cabinet ring (TX looped back onto RX).
- The first word of the payload is a message type (0x52, 0x50, 0x51, 0x40, 0x10, 0x11, 0x30): a game-specific handshake.

### 3.5 Settings
- **ID NO.**: 0 = LEFT, 1 = RIGHT (toggled with `eori.b #1`, 0xFCC0): setting byte `$E186B8+0x32` = **EEPROM byte 0x32**
  (the first 0x200 EEPROM bytes are copied to $E186B8 at 0x6500).
- **Two cabinets at most.** They must have different IDs, otherwise each one takes the other's frame for its own (permanent "LINK ON 1").
- COMMUNICATION TEST (0xA36A): shows the 8 registers, the TX area, the 8 RX slots and the error counters; it transmits frames of random length (0x1E-0x9D bytes).

## 4. Alpine Surfer (alpines, AF2 Ver.A): same driver, never hooked up
- Init 0x6312 and transmit 0x64B6 identical to alpinr2b, but **no reference** to these addresses (no jsr/bsr, no pointer, no constant).
  The SCI IRQ is never programmed (only the dead routine writes `$700002`). The COMMUNICATION TEST (test table 0x1ACD80: 0x9EB2/0x9F3E)
  fills the buffer (0xA19C) without ever transmitting. H: leftover code from alpinr2b; another revision might link.
- Why it matters: this version uses the **end-of-transmission IRQ**, which confirms the bits:
  - transmission: `TXOFFSET = buf+1 ; START = 1 ; TXSIZE = n ; MODE &= 0xFFFB` (bit 2 clear: end IRQ enabled) `; START = 0`;
    if the transmitter is busy, the frame stays pending (its length word stays non-zero);
  - IRQ (0x635A): `if STATUS & 4` (end of transmission): if a frame is pending → `TXOFFSET = buf+1 ; START = 1 ; TXSIZE = len ; MODE = 0xFFF9 ; START = 0`,
    otherwise `MODE = 0xFFFD` (bit 2 set: no more end IRQ); then `if STATUS & 2`: reception as in alpinr2b.
  - STATUS bit 2 = end of transmission; MODE bit 2 = end-of-transmission IRQ mask. This is consistent with tokyowar and with ridgera2 (C139.md § 4.1).

## 5. Dirt Dash (dirtdash, DT2 Ver.C): no link in the code
- The only effective access is `move.w #$0D, $420002` at 0x1384, at boot, after the copy from 0x1F0000 to 0xE16000. This is where the "mode 0x0D" of the MAME notes comes from.
- 0x2130 (`lea $420000,a4` then 8 writes) has no caller and no pointer. It contains a typo: `38F8 000F` = `move.w $000F.w,(a4)+`
  instead of `#$0F` (MODE would receive 0xF400).
- SCI IRQ disabled (`$700002 = 0`, 0x12A8). The level 2 handler (0x158A) only acknowledges. No TXSIZE, no RX read, no cabinet or link setting.
- The other "hits" in c139_maincpu.asm (0x13EEA4 onwards) are data (geometry).
- H: the clones dirtdasha/dirtdashb/dirtdashj have not been dumped; to be checked, but nothing points to a linkable version.

## 6. Why the current emulation (namco_c139_orig.cpp) gives "a single machine"

**Dirt Dash**: nothing to do with the C139. The game transmits nothing (TXSIZE is never written, so `send_data` never fires),
reads nothing and its SCI IRQ is disabled. On the real cabinet it also runs alone. No change to the emulation will make it link.

**Tokyo Wars** (LINK = 1 / "OFF", whereas it should be 2):
1. Automatic relay in `read_data` (`if (m_buffer[0] != m_linkid) send_frame()`) **on top of** the software relay. With A and B:
   B sends the raw copy of F_A (hop 0) back to A; A recognizes it as its own (`m_linkid`), stores it in RX anyway and the game counts hop 0+1 = **1 machine**.
   Then come the copies relayed by the game (hop 1, 2, 3, 4), which A's C139 relays in turn. `-$5FF8` takes the values 1, 2, 3, 4 in turn within the same VBLANK:
   the filter of 8 equal values keeps whatever the timing produces, 1 in practice according to MAME's TODO note. The traffic is multiplied.
2. MODE handled as an enumeration: when its software queue is not empty, the game writes 0x0B then 0x09. MAME then switches to the "mode 9" behaviour:
   transmission by searching for bit 8 with a 12-bit mask, TXSIZE never reset to 0 (the game thinks the transmitter is busy forever),
   a hack on the link counter. In MODE 0x0B or 0x0F, MAME no longer does anything.
3. No end-of-transmission IRQ. MAME only raises an IRQ on reception (and STATUS = 4 after each acknowledge); the game's handler always sees bit 1
   and never drains its queue: the AI tanks' frames and the delayed relays are lost.
4. `tx_mask = 0x1FFF`: a relay that straddles 0x1FFF wraps around into the TX area.
5. Single instance: nothing comes back, so LINK = "NG" instead of "OFF" (H: on the real cabinet running alone, the ring is closed onto itself).

**Alpine Racer 2** ("LINK ON 1" in attract and in test, cf. MAME's TODO): B automatically relays F_A to A, and A stores its own frame in RX.
Since the game only examines the last frame of the video frame, it often finds its own: state 7, and the other's data are ignored for that video frame.
In play, it "more or less works". With the same ID NO. in both EEPROMs, the link never comes up.

## 7. What the C139 must do for these games

Model deduced from tokyowar, alpinr2b and alpines (and compatible with ridgera2, C139.md § 4.1). "H" = not proven by the code.

1. **No hardware relay, no filtering by origin** in this mode of operation (MODE 0x09/0x0B/0x0D/0x0F, 0xFD/0xFF/0xF9). Each word transmitted
   by a cabinet arrives at the RX of the next one. The software does the relaying (tokyowar) or does not need it (alpinr2b, 2 cabinets).
   A cabinet on its own: H, TX looped back onto its own RX (tokyowar then shows "OFF", alpinr2b "LINK ON 1").
2. **MODE = bits** (4 bits read back as written; bits 4-15 of 0xFF/0xFFFD/0xFFF9 are ignored):
   bit 2 = end-of-transmission IRQ mask; bit 1 = H: global mask of the C139 IRQs (set during init and the critical sections);
   bits 0 and 3 = 1 in all these games (meaning unknown). Writing 0x09/0x0B/0x0F must **not** change the way the chip transmits or receives.
3. **Transmission**: it starts when TXSIZE ≠ 0 and START = 0. It sends TXSIZE 9-bit words from TXOFFSET (13 bits; bit 12 = RX area;
   H: the address wraps within its 4 K half). The words go out as they are: the games set bit 8 on the last one themselves;
   forcing bit 8 on the last word and clearing it elsewhere is still compatible. TXSIZE counts down and is 0 at the end; the games test its low byte.
   At the end: STATUS |= 4, IRQ if MODE bit 2 = 0 (and H: bit 1 = 0).
4. **Reception**: each received word (9 bits) is written at `0x1000 | (RXOFFSET & 0xFFF)`, then RXOFFSET advances, on 12 bits, within the RX area;
   when read back, it points to the next word. On a word with bit 8 (end of frame): STATUS |= 2 and IRQ. The games only process the last frame
   (they walk back from RXOFFSET). So the chip must deliver **one frame per IRQ** and wait for the acknowledge (STATUS = 0) before writing the next one,
   as the `STATUS != 6` condition already does; it should be rewritten as "bit 1 pending".
   tokyowar modifies the frame in place in the RX area before relaying it: the RX RAM must stay readable and writable by the CPU on 9 bits
   (timecris also tests these 9 bits).
5. **STATUS**: bit 1 = frame received, bit 2 = transmission finished (latched event; do not leave it at 1 permanently, otherwise a level-triggered IRQ
   would loop forever as soon as MODE bit 2 = 0); writing 0 clears. tokyowar treats any IRQ without bit 1 as an end of transmission.
6. **RXSIZE / CONTROL**: written only at init (0xFF/0, 0/0). They must not block anything.
7. **Throughput** (H): tokyowar with 4 cabinets can push up to about 16 frames of 70 words per video frame and per link segment (own frames, AI tanks, relays).
   One frame per 800 Hz tick (about 13 per video frame) is not always enough: a higher throughput is needed, or a time computed per word.
8. **Configuration**: nothing to do for dirtdash, timecris, alpinerd, alpines, aquajet, airco22b and propcycl, apart from tolerating the init and the RAM test.
   For tokyowar, the ID NO. (EEPROM 8-9, 0-3) must differ from one cabinet to the next; for alpinr2b, the ID NO. (EEPROM 0x32: 0 = LEFT, 1 = RIGHT) as well.

A lead for Final Lap ("UP TO 1 CAN PLAY", mode 0x0D, System 2), unverified H: if Final Lap counts its machines by the return of its own frame,
like tokyowar, the same duplicated automatic relay would explain the count of 1.
