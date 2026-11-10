# System 22 — C139 in mode 0x08 (ridgera2, raverace) and 0x0C (ridgerac, ridgeracf)

Static analysis (capstone disassembly of the `:maincpu` regions dumped by MAME, put back in big-endian order).
No emulator run. What is read in the code is given as fact, with the address;
what is deduced is marked **hypothesis**.

Conventions:
- Work variables relative to A6 (A6 = 0x10008000): `$181A(A6)` = 0x1000981A.
  The CPU is big-endian: byte `$1104` is the **high** byte of word `$1104` (bits 8-15), byte `$1105` the low byte.
- C139 registers (base 0x20020000): +0 STATUS, +2 MODE, +4 CONTROL, +6 START, +8 RXSIZE, +A TXSIZE, +C RXOFFSET, +E TXOFFSET.
  `TXSIZE.b` = byte 0x2002000B = low byte of TXSIZE.
- C139 RAM: 0x20010000 (word 0x0000) … 0x20013FFF. TX area = words 0x000-0xFFF, RX area = words 0x1000-0x1FFF (0x20012000).
- Interrupts (system registers 0x40000000):
  ridgera2 and raverace program SCI = 0x36 (level 6, vector 30) and VBLANK = 0x35 (level 5, vector 29);
  ridgerac and ridgeracf program SCI = 0x35 (level 5) and VBLANK = 0x34 (level 4). SCI acknowledge: write to 0x40000007.
- Tools (scratchpad `c139re\work\`): `fulldis.py` (full disassembly → `<game>.lst`, A6 annotations), `da.py`, `hx.py`, `xr.py` (cross-references),
  `strs.py`, `sim.py` (two-cabinet simulation of the link code, § 4.6).
  Caution: capstone does not show the scale factor of the `(d8,PC,Dn*2)` addressing mode; the indexed tables (0x1BFDC, 0x26D1A, 0x27670) are indeed ×2.

---

## 1. What ridgera2 and raverace have in common

Both games have the same link driver, apart from the frame sizes and the delays.

| | ridgera2 (RRS2) | raverace (RV2 Ver.B) |
|---|---|---|
| C139 init | 0x1BDA8 (called at boot 0xCEA8) | 0x26B20 (boot 0x4370) |
| SCI IRQ handler | 0x1BE4C | 0x26BBE |
| Transmit scheduler | 0x1BF38 → 0x1BF72 | 0x26C84 (selection + register writes) |
| "Kick" if TXSIZE = 0 | 0x1BD9A (called only by the I/O test: 0x7568, 0x2285C) | 0x26B12 (only by the I/O test: 0x610A) |
| Startup send | `jsr 0x1BF38` at boot (0xCDAE) | **none** |
| RX parsing (in the IRQ) | 0x1BFEC | 0x26D2A |
| Building the own frame | 0x1C588 | 0x26F86 |
| Extra frames (AI opponents) | post 0x1C734, write 0x1C76C | post 0x274CA, write 0x275BE |
| Counting / LINKCT (VBLANK) | 0x1C144 | 0x26EAE (the counting itself at 0x26F44) |
| Ring consumer (VBLANK) | 0x1C60A | 0x27126 |
| VBLANK handler | 0xD116 | 0x44FE |
| Length (TXSIZE) | 0x24 half-words (variable -$7756 = 0x22, +2) | 0x28 (variable $1038 = 0x26, +2) |
| Payload bytes | 0x22 | 0x26 |
| "Own frame back" timeout | 16 VBLANK ($1860) | 8 VBLANK ($1108) |
| "Frame counter frozen" timeout | 16 VBLANK ($181E[]) | 8 VBLANK ($110C[]) |

### 1.1 Frame format (in the C139 RAM, one byte per half-word)

```
word 0 .. L-3 : payload bytes (L-2 bytes), high byte first for each 16-bit word of the packet
word L-2      : length L (0x24 ridgera2, 0x28 raverace)               <- written before CONTROL=3
word L-1      : 16-bit sum of the payload bytes (not the length)       <- written AFTER CONTROL=3
reception     : the last half-word carries bit 8 (end of frame), set by the chip
```
Packet (in work RAM, copied byte by byte):
- ridgera2 (0x1C588): 6 words from `$189C + n*16` ("link" record of CPU n), 10 words from `$1400 + n*64` (car state), 0xFFFF → 0x22 bytes.
- raverace (0x26F86): 6 words from `$11AC + n*16`, 12 words from `$12AC + n*64`, 0xFFFF → 0x26 bytes.

Link record (6 words): word 0 = CPU number (byte 1 of the frame therefore carries the identifier, read with `& 0xF`),
word 1 = GRP, word 2 = flags/step (raverace `$11A4/$11A5`), word 3, byte 8 = status flags (`$1808` / `$11A8`),
byte 9 = **frame counter** (`$1809 = $1FBF` / `$11A9 = $C4D`, incremented at each VBLANK), word 5 (`$180A` / `$11AA`).

Check on reception: `(sum - Σ words 0..L-2 + L) & 0xFF == 0`, which checks both the data and the length.

### 1.2 Register writes

Init (ridgera2 0x1BDA8 with table 0x1BD7A; raverace 0x26B20 with table 0x26AF2, identical):
```
byte 0x40000019 = 1
regs[0..7] = 0000 000F 0000 0011 0004 0004 0000 0000   ; STATUS, MODE=0x0F (config), CONTROL, START=0x11, RXSIZE=4, TXSIZE=4, RXOFF=0, TXOFF=0
C139 RAM[0..0x1FFF] = 0
regs[0..7] = 0000 000C 0000 0011 0000 0000 0000 1000   ; MODE=0x0C, START=0x11 (written AFTER MODE), TXOFFSET=0x1000
(clearing of a work area: 0x10005800 / 0x10002000, 0x400 longs)
START = 0
error counters = 0 ; RX ring ($19BE / $105E, 0x50 words) = 0xFFFF ; current bank = 0 ; length = 0x22 / 0x26
```
At the very start of boot (before the RAM test): CONTROL = 0, START = 1 (ridgera2 0x246D8, raverace 0x2FA68).
The error/restart routines at 0x400/0x404/0x408 write MODE = 0x0F (stop).

Transmission (scheduler, both games):
```
TXOFFSET = off ; START = 1 ; TXSIZE = L ; MODE &= 8 ; START = 0
```
Building a frame: `CONTROL = 1 ; words 0..L-2 ; CONTROL = 3 ; word L-1 (checksum)`.
Masks on MODE, and nothing else (never an OR, never a direct write after the init):

| Where | Mask | Bits cleared |
|---|---|---|
| start of the VBLANK (0xD11E; 0x26EAE) | `&= 9` | 1, 2 |
| after posting the own frame (0x1C16E; 0x26ECC) or the AI frames (0x1C746; 0x274DC) | `&= 0xB` | 2 |
| scheduler, after TXSIZE (0x1BF60; 0x26D08) | `&= 8` | 0, 1, 2 |
| IRQ exit after RX / error / transmission | `&= 9` | 1, 2 |
| IRQ exit "nothing to transmit" | `&= 0xD` | 1 only |

---

## 2. Ridge Racer 2 (ridgera2)

### 2.1 Variables

| Var | Role |
|---|---|
| `$1800` | CPU NO. (0-7, from option 0x1000685C & 7, routine 0x1C110); `$1810 = n*2`, `$1812 = n*16`, `$1814 = n*64` |
| `$1802` | GRP NO.; `$1816` = ALONE; `$1818` = LINKCT (names from the overlay 0x1E36C, strings 0x1E47C) |
| `$181A` | mask of the slots to transmit (bit n = CPU n; bits 8-15 = AI frames) |
| `$181C` | round-robin position of the scheduler |
| -$775A | chosen TXOFFSET; -$7756 = 0x22 (length - 2) |
| `$199C[16]` | TXOFFSET used to retransmit the last frame received from n (= 0x1000 + start in the RX area); 0 for its own |
| `$185E` | "seen" mask (bits 0-15); `$1860` = VBLANKs since the own frame came back |
| `$181E[16]` | VBLANKs since the last change of n's frame counter; `$183F[n*2]` = last value seen |
| `$19BC` | current bank of the ring (0, 0x20, 0x40, 0x60 bytes); `$19BE` = 4 banks × 16 words; `$1A3E` = copy of the consumed bank |
| `$1A5E` | RXOFFSET read on IRQ entry |
| `$2174` | mask of the AI slots transmitted by this cabinet; `$215E` = first AI slot (8 or 12); `$1E60` = count-1; `$2172` = AI master CPU; `$217C` = transfer in progress |
| `$4F4[16]` | +1 at each VBLANK (0xD1A4), -1 at each frame received from n: stays constant if exactly one frame from n per VBLANK (shown by the overlay) |
| -$7750 / -$774A / -$7748 / -$7752 / -$774E / -$774C | errors: STATUS bit 0 / transmit IRQ with TXSIZE ≠ 0 / IRQ with no cause / end of frame not found / bad checksum / ring overflow |

### 2.2 SCI IRQ handler (0x1BE4C, level 6)

```c
d3 = STATUS;  $1A5E = RXOFFSET;  bclr #5,-$77D5;  -$7746 = 0xFFFF;
if (d3 & 1) { -$7750++; goto ack_all; }            // error
if (d3 & 2) { rx_parse(); goto ack_all; }          // frame received (takes priority over bit 2)
if (!(d3 & 4)) { -$7748++; goto ack_idle; }        // IRQ with no cause
// bit 2: transmitter available
if (TXSIZE.b != 0) { -$774A++; goto ack_all; }     // the transmitter has not finished: anomaly
if ($181A == 0) goto ack_idle;                     // nothing to transmit
send_next();                                       // 0x1BF38
START = 0; ack_sys(); MODE &= 9; rte;              // NB: STATUS is not written
ack_all:  STATUS = 0; START = 0; ack_sys(); MODE &= 9; rte;
ack_idle: ack_sys(); MODE &= 0xD; rte;             // MODE bit 2 left as is
```

### 2.3 Scheduler (0x1BF38 / 0x1BF72)

```c
send_next() {
  d4 = $181C; d0 = $181A;
  for (i = 0; i < 16; i++, d4 = (d4+1) & 15)       // round robin
    if (d0 & (1 << d4)) {
      if (d4 == $1800)                       -$775A = 0;                       // own frame: TX 0x000
      else if ($2174 && ($2174 & 1<<d4))     -$775A = tab_1BFDC[d4-8];         // AI: 0x40,0x80,..,0x200
      else                                   -$775A = $199C[d4];               // retransmission: 0x1000+RX pos
      d0 &= ~(1 << d4); d4 = (d4+1) & 15; break;
    }
  -$7756 = 0x22; $181C = d4; $181A = d0;
  TXOFFSET = -$775A; START = 1; TXSIZE = 0x24; MODE &= 8; START = 0;  // even if nothing was chosen
}
```
Only one frame per interrupt; the next one goes out at the next "transmitter available" interrupt.
At boot, `jsr 0x1BF38` (0xCDAE) with `$181A = 0` transmits 0x24 words from TX 0 (still zero): a dummy frame
(bad checksum at the neighbour, counted in -$774E) that serves to start the interrupt chain and takes MODE from 0x0C to 0x08.

### 2.4 RX parsing (0x1BFEC, in the IRQ)

```c
rx_parse() {
  p = $1A5E & 0xFFF;
  for (k = 0; k < 0x21; k++) { p = (p-1) & 0xFFF; if (rx[p] & 0x100) goto found; }
  -$7752++; return;                                  // no end of frame
found:
  $1A5E = p+1;                                       // just after the frame
  s = (p - 0x23) & 0xFFF;                            // start (fixed length 0x24, the length word is not read)
  if ((rx[p] - Σ rx[s..s+0x22] + 0x24) & 0xFF) { -$774E++; return; }
  id = rx[s+1] & 0xF;
  $185E |= 1 << id;  $4F4[id]--;
  // ring: 4 banks, starting from the current bank (the one the next VBLANK will consume)
  for (b = $19BC, k = 0; k < 4; k++, b = (b+0x20) & 0x60) if (ring[b][id] < 0) break;
  if (k == 4) { -$774C++; b = $19BC; }               // overflow: overwrites the oldest entry
  ring[b][id] = $1A5E;                               // the POSITION is kept, not the data
  $199C[id] = 0x1000 + s;
  if (id == $1800) { $1860 = 0; $199C[id] = 0; }     // own frame came back
  else if (!($2174 && ($2174 & 1<<id))) $181A |= 1 << id;   // to be retransmitted (except our own AI)
}
```
An interrupt only processes **the last** frame before RXOFFSET: if two frames arrive before the IRQ is serviced,
the first one is lost.

### 2.5 Consumer (0x1C60A, called by the VBLANK 0xD142, hence 60 times per second)

```c
copy = ring[$19BC]; ring[$19BC] = 0xFFFF…; $19BC = ($19BC + 0x20) & 0x60;   // one bank per VBLANK
for (n = 15; n >= 0; n--) if (copy[n] >= 0) {
  re-reads the frame in the RX RAM backwards from copy[n] (checksum and length re-read but not checked);
  id = word 0; if 0xFFFF, == $1800 or in $2174: ignored;
  otherwise 6 words -> $189C + id*16, 10 words -> $1400 + id*64.
}
```
So: at most 4 pending frames per identifier, one consumed per VBLANK. The frame is re-read from the C139 RAM
up to 4 VBLANKs after it was received: the RX area (0x1000 words, ~113 frames of 0x24) must not have wrapped around in the meantime.

### 2.6 VBLANK (0xD116) and counting

```c
vblank() {                                // level 5, preempted by the SCI (level 6)
  0xD6C4(); MODE &= 9;
  link_tick();                            // 0x1C144
  0xD778();                               // AI: if $1800 != 0 && $1800 == $2172 -> $2174 = mask; if !$217C -> 0x1C734
  0x1C60A();                              // consumer
  0x1C45C();                              // applies the received cars (if not ALONE, etc.)
  counters; $4F4[0..15]++; ack VBLANK (0x40000009)
}
link_tick() {                             // 0x1C144
  build_own_frame();                      // 0x1C588: CONTROL=1, words 0..0x22, CONTROL=3, checksum
  $181A |= 1 << $1800;  MODE &= 0xB;      // posts the own frame
  $1809 = $1FBF;                          // frame counter
  for (n = 15..0) {
    if ($183F[n] != rec[n].byte9) $181E[n] = 0;  $183F[n] = rec[n].byte9;
    if ($185E & 1<<n) if (++$181E[n] >= 16) $185E &= ~(1<<n);   // n frozen for 16 VBLANKs: forgotten
  }
  if (++$1860 >= 16) $185E &= ~(1 << $1800);                    // own frame not back for 16 VBLANKs
  LINKCT = ($185E & 1<<$1800) ? popcount($185E & 0xFF) : 0;    // $1818 (0x1C1E0)
  $180C = highest n (0-7) seen, $180E = *16;
  0x1C370 (group masks), 0x1C3F6 (clears the records and the ring of the unseen n), 0x1C2AE (copies the own record)
}
```

### 2.7 Group, ALONE, AI: what decides a linked or solo race

- Group election (0x1607A → 0x160E4): if nobody has the "host" flag (byte 8 bit 1), the cabinet takes it and
  chooses `GRP = 1 + max(GRP of the records with flags 4/5/6)`; otherwise it takes the host's GRP.
- Decision (0x164DC, when the countdown `$1894` equals 8):
  ```c
  if (LINKCT == 0) ALONE = 0xFFFF;                 // <- requires the own frame to come back
  else { ALONE = 0; if (number of records with GRP == $1802 <= 1) ALONE = 0xFFFF; }
  ```
  **ridgera2 only plays linked if LINKCT ≠ 0**, that is, if its own frame goes around the ring in less than 16 VBLANKs.
  This is why it stayed solo with the original emulation with identical IDs (no return) and links with the WIP model.
- AI (0x11E06): in a linked race with 2-4 cabinets of the same group, the highest-numbered cabinet (`$2176 → $2172`) also transmits
  AI opponents in slots 8.. (or 12.. if 8 is already seen; none if 12 is too), count = table 0x11F00.
  The CPU 0 cabinet cannot be master (test `$1800 != 0` at 0xD77E).

---

## 3. Rave Racer (raverace)

Same code; only the differences are detailed.

### 3.1 Variables

| Var | Role |
|---|---|
| `$11A0` | CPU NUMBER (0x10001060 & 7, routine 0x26E24; can be changed in the menu, 0x2D6DC); `$1158/$115A/$115C` = n*2/16/64 |
| `$11A2` GRP, `$1100` ALONE ("ALN"), `$1102` LINKCT ("LNK") | names from the overlay 0xACC2 / 0xAD8E (strings 0xADD4) |
| `$1034` | mask to transmit; `$1036` round robin; `$103A` chosen TXOFFSET; `$1038` = 0x26 |
| `$103C[16]` | retransmission TXOFFSET |
| `$1104` | "seen" mask; `$1108` VBLANKs since the own frame came back; `$110C[]`, `$112D[n*2]` like `$181E/$183F` |
| `$105C` bank, `$105E` 4×16 words, `$10DE` copy, `$10FE` RXOFFSET on IRQ entry | ring |
| `$1162` mask of the AI transmitted by this cabinet; `$1168` first AI slot (8 or 12); `$4A94` = `$1168*64`; `$4A96` = count-1; `$116A` "handover" mode | AI |
| `$1002` STATUS bit 0, `$1004` transmit IRQ with TXSIZE ≠ 0, `$1006` IRQ with no cause, `$1008` end not found, `$100A` checksum, `$100C` **ring overflow** | errors (IRQ) |
| `$1150` / `$1152` | **frames whose length byte ≠ 0x28 at consumption time** / last value read |
| `$114C` / `$114E` | frames whose CPU word has non-zero bits 4-15 / last value |
| `$1012[16]` | decremented at each frame received from n (no increment found) |
| `$110A` | set to 0xFFFF by each SCI IRQ (used by the I/O test) |

In the I/O test (0x6150): "C139 RA ERR" = `$1008`, "SUM ERR" = `$100A`, "FRAME ERR" = `$1002`, "CNT. OK" = word 0x10002000.

### 3.2 Code differences

- **No startup send.** Outside the I/O test, the scheduler 0x26C84 is only called by the IRQ (0x26C06): raverace
  transmits **nothing** as long as it does not get a "transmitter available" interrupt (STATUS bit 2, TXSIZE.b = 0). Nothing other
  than the clearing of MODE bit 2, just after posting a frame (0x26ECC, 0x274DC) or at the start of the VBLANK (0x26EAE),
  precedes this need; **hypothesis**: it is this clearing that causes the interrupt on the hardware.
- RX (0x26D2A): end search over 0x25 words, frame 0x28, CPU in word `s+1`, delays 8 instead of 16.
- Consumer (0x27126): in addition, `length byte == 0x28` otherwise `$1150++` (`$1152` = value); `CPU word & 0xFFF0 == 0` otherwise `$114C++`.
- VBLANK (0x44FE): `0xEFEE ; if -$7800 != 3 { 0x26EAE ; 0x273EE (AI) ; 0x27126 (consumer) ; 0x2721A ; 0x470E }`.
  -$7800 = 3 is only used by a test submenu (0x2A082).
- 0x26EAE: `MODE &= 9 ; debug copy (if $CD6 == 1) ; build (0x26F86) ; $1034 |= 1<<$11A0 ; MODE &= 0xB ; $11A9 = $C4D ;`
  expiry (8); 0x2708A (forgets the unseen ones); `if (++$1108 >= 8) bclr $11A0, $1105`; LINKCT at 0x26F44; `$115E` = highest seen; 0x27008; 0x2708A; 0x270F2 (own record).

### 3.3 AI frames ("multi-frame routine")

- Setup (0x184E2, at the start of a race): if ALONE ≠ 0 → `$1162 = 0` (nothing is transmitted; in demo or in solo);
  otherwise 0x185E0:
  ```c
  master = highest CPU n (0-7) whose rec[n].GRP == $11A2 ;  $1164 = master;
  if (master != $11A0) { $1162 = $4A94 = $1168 = $4A96 = 0; return; }
  slot = 8; if ($1104 & 1<<8) { slot = 12; if ($1104 & 1<<12) { nothing; return; } }
  $1168 = slot; $4A94 = slot*64; $4A96 = tab_186B4[number of group members]   // 3,3,3,2,1,0,0,0 = count-1
  $1162 = bits slot .. slot+$4A96 (0x18802) ; initialises the AI states
  ```
- Each VBLANK (0x273EE): if `$1162 != 0` and `$116A == 0` → 0x274CA:
  0x275BE writes the `$4A96+1` AI frames into the TX area at half-words `tab_27670[k]/2` = 0x40, 0x80, 0xC0, 0x100
  (slots 8-11) or 0x140 … 0x200 (12-15), each with CONTROL 1 … 3; then `$1034 |= $1162 ; MODE &= 0xB`.
  The scheduler transmits them with TXOFFSET = `tab_26D1A[d4-8]` (same positions).
- If `$116A != 0` (0x27400, handover): no more AI transmission; it waits until each AI slot is no longer seen
  (bits 8-15 of `$1104`) for 8 VBLANKs, then resumes. No direct call to 0x27446 (which arms `$116A`) was found.
- In a 2-cabinet linked race, the master therefore transmits 1 + 4 frames per VBLANK, and the other cabinet must retransmit these 5 frames;
  each cabinet transmits ~6 frames and receives ~6 per VBLANK (360 per second in each direction).

### 3.4 ALONE, GRP and LINKCT in raverace

- ALONE (`$1100`) is decided from the received records (0xFCC0-0xFD38: number of records with the
  flags byte 8 bits 0/1; a single participant → `$2344 = 1` → `ALONE = tab_FDA4[1] = 0xFFFF`), GRP taken from
  the "host" record (byte 8 bit 6). **LINKCT plays no part** in this decision.
- LINKCT is only read by: the display (0xACDC, 0xADAA), 0xC150 (if LINKCT ≠ 0, the animation phase `$2034` is taken from
  the frame counter of the highest CPU seen), 0xE724 ("link" text if > 1), 0xF7AC (**synchronised attract**: if LINKCT > 1,
  the cabinet sets `$11A4` bit 0 and waits until every cabinet in attract has the same bit in its record, then restarts at
  the max step + 1), 0xFAFE (`$46F4` = highest CPU seen + 1 → choice of the panorama attract views, 0xF9EE).
- Consequence: raverace can run a linked race **with LINKCT = 0**, that is, without ever seeing its own frame again.
  This is exactly the situation of the original emulation with identical IDs, the only one where it links in the tests.

---

## 4. Question 2: raverace with the WIP model

### 4.1 What raverace expects (read in the code)

1. A "transmitter available" interrupt (STATUS bit 2 with TXSIZE.b = 0) every time the transmitter is free **and**
   the game has something to transmit; without it, raverace never transmits (no startup send, see 3.2).
2. After a write to TXSIZE, TXSIZE ≠ 0 as long as the frame has not gone out, then 0. A bit 2 interrupt with
   TXSIZE ≠ 0 is counted as an anomaly (`$1004`).
3. One receive interrupt per frame, serviced before the next one arrives (only the last frame before
   RXOFFSET is parsed).
4. At most one frame per identifier and per VBLANK in steady state; up to 4 ahead are absorbed, beyond that `$100C`
   counts and the oldest entry is overwritten (the data stays fresh).
5. The data of a frame stays intact in the RX area for at least 4 VBLANKs (`$1150` otherwise).
6. For LINKCT ≠ 0: its own frame comes back at least once every 8 VBLANKs. For a cabinet to stay "seen":
   its frame counter (byte 9) changes at least once every 8 VBLANKs.
7. Throughput: ~2 frames per VBLANK in each direction in attract, ~6 in a linked race with AI.

### 4.2 Why the original emulation "works" for raverace

With the original emulation in mode 08:
- the IRQ is only raised on reception, with STATUS = 6: bit 1 has priority, the scheduler is **never** called;
- transmission comes from the "raverace hack": at each CONTROL = 3, TXSIZE = (last word written) + 2, then sent at the next tick
  from TXOFFSET & 0xFFF; TXOFFSET is still 0x1000 (2nd init block) → 0 → own frame. That is why the mask
  0x1FFF "broke" raverace (§ 6 of C139.md): it then sent the RX area;
- with identical IDs, the chip relays nothing; each cabinet only receives the neighbour's frame, never its own → **LINKCT = 0
  permanently**, but ALONE (records) allows the linked race;
- when the AI are active, the AI frames are written at 0x40.. with their own CONTROL = 3: the fabricated TXSIZE then covers
  0x000 up to the last AI frame (e.g. 0x128 words) and goes out as **a single** frame; the neighbour only parses the
  last 0x28 words (the last AI). The master's own frame only gets through if the tick falls between its construction and
  that of the AI (simulation: the neighbour only sees slot 11).

With different IDs, the original chip relays: each cabinet sees its frame again, LINKCT = 2… and raverace stays solo (tests of 2026-11-02).

### 4.3 Discrepancies between the WIP model and what the code reads

| # | WIP | Effect |
|---|---|---|
| 1 | "transmitter free" IRQ at **every** tick (800 Hz) as long as TXSIZE = 0 and STATUS ≠ 6 | ~730 useless IRQs per second (`ack_idle` path), with no effect on the logic |
| 2 | transmission at the tick following the TXSIZE write, then IRQ at the tick after that: 2 ticks per frame minimum | throughput ≤ 400 frames/s |
| 3 | `send_data` and the transmit IRQ are blocked as long as STATUS == 6; reception goes first at each tick | as long as there are frames left to read (TCP queue not empty), the cabinet **no longer transmits at all** |
| 4 | STATUS bit 2 stays at 1 after the first acknowledge (write of 0 → 4) | no consequence (bit 1 is tested first) |
| 5 | TXOFFSET 0x1000+pos: `(off + j) & 0x1FFF` | a retransmitted frame that overlaps the end of the RX area reads the TX area: ~1 retransmission in 100 has a bad checksum (`$100A`, simulation: 10 in 20 s, 0 with a wrap within the RX half) |
| 6 | a single action per tick | in a race with AI (360 frames/s in each direction) the WIP caps at ~266 transmissions/s: ~25% of the requests are merged (lost), with no overflow |

None of these discrepancies produces more than one frame per identifier and per VBLANK: they cannot make `$100C` go up.

### 4.4 Simulation (scratchpad `c139re\work\sim.py`)

Model of the routines 0x26BBE, 0x26C84, 0x26D2A, 0x26EAE, 0x27126 (and ridgera2 variant: 0x24, delays 16) on two cabinets,
against three chips: `wip` (namco_c139_wip.cpp), `orig` (identical IDs), `origrelay` (different IDs), `latch` (§ 7).
Atomic VBLANK, lossless network.

| Case | Result |
|---|---|
| wip, attract (no AI) | both cabinets: seen = 0x0006, LINKCT = 2, no overflow |
| wip, race with 4 AI | seen = 0x0F06, LINKCT = 2, ~44 frames/s per identifier instead of 60, ~266 transmissions/s per cabinet (saturation), a few bad checksums (discrepancy 5) |
| orig | seen = the other one only (0x0004 / 0x0002), **LINKCT = 0** |
| orig + 4 AI | the non-master cabinet only sees slot 11 (0x0800) |
| origrelay | LINKCT = 2 (like wip) |
| latch | LINKCT = 2, 1 transmit IRQ per frame, no error, even with AI |
| wip / orig / latch, cab2 5% slower | `$100C` ≈ 2.5 per second on cab2, in **all three** models |
| latency of 100 ms per hop | no effect (only gaps count, not delay) |

### 4.5 What this says about the observation "`$100C` goes up, `$1104` stays at 0 on cab2"

- In the code, `$1104 |= bit` (0x26D9C) precedes `$100C++` (0x26DCC) in the same path, and the expiry only removes a bit
  after 8 VBLANKs without change. **`$100C` cannot go up with `$1104` permanently zero.** To check:
  look at the **word** 0x10009104; the byte 0x10009104 alone (bits 8-15, AI slots) is 0 outside a race.
- `$100C` going up steadily = an identifier received faster than once per cab2 VBLANK. Possible causes outside the chip:
  cab2 slower than real time (debugger open, loaded machine), or catching up on a TCP backlog (cab2 started
  its link after cab1: the queue filled up during the POST). With the WIP, during this catch-up, discrepancy 3 prevents
  cab2 from transmitting (cab1 then loses cab2 and the return of its own frame). **Hypothesis**: raverace has no
  "frames received − VBLANK" counter like ridgera2's `$4F4[]`; `$100C` and the receive queue on the MAME side must be monitored
  (`m_fifo_rx.used()` / 0x200).
- `$1150` (bad lengths at consumption time) can only come from an RX area rewritten less than 4 VBLANKs after
  reception: more than ~100 frames of 0x28 in 4 VBLANKs, or frames of a different size. If this counter goes up, the chip
  writes too much (backlog processed at 800 frames/s: 13 frames per VBLANK) or frames that are too long.
- What the failed tests (orig with different IDs, WIP) have in common is **LINKCT = 2**, which in raverace enables the synchronised
  attract (0xF7AC), the panorama attract (`$46F4`) and the received animation phase (0xC150). **Hypothesis**: the failure
  comes from these paths or from their sensitivity to the frame timing, not from the end-of-transmission signalling itself.
  To settle it: on both cabinets, record `$1102`, `$1104` (word), `$1100`, `$11A2`, `$100A`, `$100C`, `$1150`,
  `$114C`, `$1004`, and the attract step `$232E`/`$11A4-$11A5`; also test the WIP while continuously forcing `$1108 = 8`
  (MAME cheat at every video frame: LINKCT stays at 0) to see whether raverace then links as it does with the original.

### 4.6 End of transmission in the WIP

What both games check is respected: TXSIZE returns to 0 when the frame has gone out, the IRQ arrives with
STATUS bit 2. The defects are elsewhere (discrepancies 2, 3, 5, 6): transmission delayed by one tick, transmission blocked during
receptions, capped throughput, address wrap. The IRQ repeated at 800 Hz is tolerated by both games but probably does not match
the hardware (see § 7).

---

## 5. Question 3: settings, EEPROM and menu

8 KB parallel EEPROM at 0x58000000 (HN58C65), one byte per address.

### 5.1 ridgera2
- Directory: 32-byte block at 0x400 (signature "V146" at +0x12); 16 bytes = block number of each
  record; record r at `0x500 + 16 * dir[r]` (default `dir[r] = 0x10*r`, 8 slots of 0x20 per
  record for wear levelling), 15 words + checksum (`~Σ`); copy in RAM 0x10006800 + r*0x20 (0x1FEEC…0x2002E).
- GAME OPTIONS = record 2 from +0x10 (RAM 0x10006850), ROM defaults 0x2009A:
  bytes 0-4 GAME DIFFICULTY (NOVICE, INTERMEDIATE, ADVANCED, T.T., LINKABLE), 5-9 TIME EXTEND, 0xA SPEED (KM/H, MPH),
  0xB SOUND IN ATTRACT, **0xC CPU NO.** (0x1000685C, 0-7 shown as 1-8), **0xD CAR TYPE** (0x1000685D, colour, sent in the frame).
  In EEPROM (default layout): CPU NO. = **0x71C**, CAR TYPE = 0x71D, record checksum at 0x71E-0x71F.
- **LINK** (last line of GAME OPTIONS, 0x217C4): just a display of the number of cabinets seen
  (`popcount($185E & 0xFF)`, including itself only if its frame comes back). The cursor stops at option 0xD (CAR TYPE):
  LINK is **not adjustable**. "LINKABLE" is the difficulty/time line of the linked-race course.
  Changing CPU NO. in the menu updates `$1800`, `$1810-$1814` immediately (0x21742): this is the setting to change
  so that two cabinets have different numbers.

### 5.2 raverace
- Directory: 0x40 blocks at 0x000 + 0x40*k (signature "V154" at +0x3A), one selection bit per record;
  32-byte record r at `0x200 + r*0x40 + sel*0x20`, checksum `~Σ` over 30 bytes at +0x1E; copy in RAM 0x10001000 + r*0x20.
- GAME OPTIONS (1/2) = record 3 (RAM 0x10001060): byte 0 **CPU NUMBER** (EEPROM **0x2C0** or 0x2E0), byte 1 CAR TYPE (0x2C1 / 0x2E1),
  2-4 DIFFICULTY (LINKED PLAY, SOLO PLAY, TIME TRIALS), 5-0xC LAPS per course (RR SHORT, CITY, MOUNTAIN, MOUNTAIN EX × LINKED/SOLO).
- **LINK** (line 6 of the menu, 0x2D798): shows ON if `popcount($1104 & 0xFF) != 0`, OFF otherwise. Read only.
- Debug overlay: 0xAC98 / 0xAD8E (CPU, GRP, ALN, LNK).

---

## 6. Question 4: ridgerac and ridgeracf

### 6.1 ridgerac (RR2 Ver.B)
- Same small driver as ridgeracf (init 0x14CDE, table 0x14CBE: 0x0F/START 1/RXSIZE 4/TXSIZE 4 then MODE 0x0C, START 1, TXOFFSET 0x1000).
- Transmission 0x14D56, called by the VBLANK (0x9FD8, level 4) **every video frame** and by the I/O test (0x5416, 0x1A704):
  ```c
  if (len == 0 || TXSIZE.b != 0) return;
  START = 1; TXOFFSET = pos; pos = (pos + len + 2) & 0xFFF;    // transmit ring in the TX area
  CONTROL = 1; writes len bytes from 0x10006000, length, checksum;  // no CONTROL = 3
  TXSIZE = len + 2; MODE &= 0xC; START = 0; len = 0x20;
  ```
- SCI IRQ level 5 (0x14DF0): STATUS read then reset to 0, acknowledge, bit 1 → copy of the frame to 0x10005800 and
  checksum check (`-$7760` = SUM ERR), otherwise counter; exit `MODE &= 0xC`. No handling of bit 2.
- 0x10005800 is only read by the I/O test. **No linked race**: the chip is used for the test ("C139 RA ERR / SUM ERR")
  and to transmit a dummy frame (0x22 half-words, data from 0x10006000) every video frame.

### 6.2 ridgeracf (Full Scale, 3 boards)
- Role of the board: 0x50000001 bit 1 = 0 → centre (master), bit 1 = 1 → side (bit 0: left/right, angle `-$7554`).
- The centre, at each VBLANK (0xA05E, level 4), calls 0x15396: 0x18820 fills 0x10006000 (video frame counter `$19A4`,
  player position/orientation, game states in nibbles, camera, 15 cars × 7 words), then
  `TXOFFSET = pos (ring) ; CONTROL = 1 ; 0xF0 bytes ; length 0xF2 ; CONTROL = 3 ; checksum ; START = 1 ; TXSIZE = 0xF2 ; MODE &= 0xC ; START = 0`.
- All the boards have the SCI IRQ (level 5, 0x15432), which copies the last frame to 0x10005800 (length read from the frame).
  The sides never transmit; their loop (0xA89C) unpacks 0x10005800 (0x18734) and they render their view;
  if the received counter is 0, MODE = 0x0C (0x18744).
- **Hypothesis**: since the sides retransmit nothing in software, either the C139 relays on its own in mode 0x0C (ring
  centre → left → right → centre), or the Full Scale wiring broadcasts the centre's output to both sides.
- MAME's "not working" comes mostly from the absence of the two other boards (three complete S22s to run),
  not from an exotic use of the C139.

---

## 7. What the C139 must do for these games

Proposed semantics (what is marked *deduced* cannot be read directly but explains all the masks above;
it satisfies ridgera2 and raverace in the simulation, `latch` model).

**RAM and frames**
- 0x2000 9-bit half-words. On transmission, only the low 8 bits of each word go out; the chip sets bit 8 on the
  last half-word received (end of frame).
- Transmit address: TXOFFSET on 13 bits; a frame started at 0x1000+ (retransmission from the RX area) must wrap
  **within the RX half**: `address(j) = (TXOFFSET & 0x1000) | ((TXOFFSET + j) & 0xFFF)`.
- Reception: write at `0x1000 + (RXOFFSET & 0xFFF)`, RXOFFSET advances (12 bits), STATUS bit 1, interrupt.

**Transmission**
- Writing TXSIZE ≠ 0 starts the transmission of TXSIZE half-words from TXOFFSET (modes 08 and 0C). CONTROL = 1 / 3
  only brackets the CPU's writing of the packet (the checksum is written after CONTROL = 3); START 1 → 0 brackets the
  loading of TXOFFSET/TXSIZE. Neither of them must trigger a send.
- TXSIZE reads ≠ 0 during transmission, 0 when it is finished (the games test `TXSIZE.b`).
- Transmission and reception are independent (bidirectional link): a pending reception must neither block nor
  delay transmission.

**Interrupts** (*deduced*)
- STATUS bit 2 = "transmitter free": a state, at 1 when nothing is in progress; writing 0 to STATUS does not clear it
  (same convention as the "write of 0 → 4" of the current emulation). STATUS bits 0 and 1 are cleared by writing 0.
- MODE bit 2 = transmit interrupt latch: the chip raises the "transmitter free" IRQ when the transmitter is free and
  this bit is 0, and sets it to 1 when raising it. The CPU resets it to 0 to re-arm:
  - after starting a frame (`&= 8`, `&= 9`) → IRQ at the end of that frame;
  - after posting a frame while the transmitter is free (`&= 0xB`) → immediate IRQ;
  - in the "nothing to transmit" path the game leaves the bit at 1 (`&= 0xD`) → no more IRQs until the next re-arm.
  In mode 0x0C (ridgerac, ridgeracf) the bit stays at 1 (`&= 0xC`): no transmit IRQ, consistent with their handlers.
  MODE is 0x0C after the init: no transmit IRQ before the first re-arm (ridgera2: boot send; raverace: VBLANK).
- MODE bit 1 = same latch for reception (cleared at each IRQ exit). A frame that arrives while the bit is at 1
  is written to RAM; the IRQ is raised when the CPU clears the bit if STATUS bit 1 is still at 1 (*deduced*). The handler
  clears STATUS before MODE: a frame that arrived during the handler is lost, which the game tolerates (it only reads the last one).
- MODE bit 0: cleared only just after TXSIZE; unknown role (never read).
- Variant tolerated by both games (this is the WIP): "transmitter free" IRQ repeated as long as the transmitter is free.

**Relay**
- Mode 08: **no automatic relay**; the games retransmit the foreign frames themselves and stop at their own.
  A hardware relay would make every frame go out twice.
- Mode 0C (ridgeracf): relay needed if the wiring is a ring (*hypothesis*, see 6.2).

**Timing and throughput**
- One receive IRQ per frame, serviced before the next one moves RXOFFSET (no batches).
- Do not inject more than one frame per identifier and per VBLANK in steady state; do not process a backlog at full
  speed (it overflows the ring of 4 and, beyond ~100 frames in 4 VBLANKs, rewrites the RX area before it is consumed).
- Useful throughput: ≥ 6 frames of 0x28 per VBLANK in each direction (raverace linked race with AI), i.e. ≥ 360 frames/s
  in transmission **and** reception simultaneously. An 800 Hz tick that does only one thing at a time is not enough.

**Minimal changes to the WIP that follow from this**
1. Transmit as soon as TXSIZE is written (or in the same tick), and raise "transmitter free" at the end of that transmission.
2. No longer make `send_data` and the transmit IRQ conditional on `STATUS != 6`.
3. Wrap the 0x1000+ transmit addresses within the RX half.
4. (Option) "transmitter free" IRQ once per end of transmission or per clearing of MODE bit 2, instead of at every tick.
5. Faster tick, or reception as the data arrives, while keeping one IRQ per frame.
6. For raverace, check the LINKCT = 2 lead in parallel (§ 4.5): it is the regime that the real hardware produces, and it is
   also the one that fails in both emulations where it appears.
