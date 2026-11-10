# Namco System 21: how the games drive the C139

Static disassembly (capstone, regions dumped by MAME, 68000 bytes put back in big-endian order). Tools in `s21tools/`
of the working directory: `d.py` (disassemble a range), `xref.py` (references to an address), `absrange.py`
(absolute accesses to an address range), `vec.py` (vectors), `strs.py` (strings), `lin.py` (linear disassembly).
All addresses are those of the CPU named; `$xxxxxx` = 68000 address.
Anything marked **hypothesis** is not proven by the code.

Reminder of the MAME wiring (the three S21 drivers):
- C139: registers at `$B80000` (STATUS +0, MODE +2, CONTROL +4, START +6, RXSIZE +8, TXSIZE +A, RXOFFSET +C, TXOFFSET +E),
  RAM at `$B00000` (0x2000 half-words; TX = words 0x000-0xFFF = `$B00000-$B01FFF`, RX = words 0x1000-0x1FFF = `$B02000-$B03FFF`).
  The RAM is seen by all the 68000s (common_map).
- The C139's IRQ goes to the C148 of each CPU (`sci_int_w`). Each CPU selects its SCI level by writing `$1CC000`;
  level 0 = no interrupt. The acknowledge is a write to `$1DC000`. **So the CPU that sets an SCI level ≠ 0 is the one that handles the link.**
- 8 KB NVRAM at `$180000-$183FFF`, low byte only (umask 0x00FF): byte n of the nvram file = address `$180000 + 2n + 1`.

## 0. Summary per game

| Game | Use of the C139 | Link CPU (SCI level) | Programmed mode | START in mode 0x0F |
|---|---|---|---|---|
| winrun (1988, Ver.09) | **test only** ("SCI" in the SWITCH TEST, loopback) | master, level 1 (= VBL level!); slave idle | 0x0F during the test, 0 otherwise (reset) | 0 |
| winrungp (Suzuka GP) | **link between cabinets**, ring, up to 8 cabinets | slave, level 4 (`L4 = $1574`) | 0x09 | 3 |
| winrun91 | **link between cabinets**, same code as winrungp, different frame | slave, level 4 (`L4 = $1528`) | 0x09 | 3 |
| cybsled (CY2) | **link between cabinets**, same protocol, 2 cabinets (LEFT / RIGHT) | slave, level 4 (`L4 = $37D8`) | 0x09 | 3 |
| driveyes | **internal link** between the 3 boards (centre → sides), one-way broadcast | slave of each board, level 4 (`L4 = $3DF0`) | 0x0D (the centre switches to 0x09 by itself, see § 3) | 1 |
| aircomb (AC2 US) | chip initialised, IRQ installed, **dead link code** (never called) | master, level 4 (`L4 = $125A`) | 0x0D | 0 |
| starblad | **init only** (mode 0x0F); RX routine never called | none (SCI level 0 on both CPUs) | 0x0F | 0 |
| solvalou | **no access** to the C139 | none (SCI level 0) | — | — |

Key points:
1. The three link games (winrungp, winrun91, cybsled) use **the same ring protocol, and it is the game that relays**:
   it points TXOFFSET into the RX area to retransmit the received frame. The C139 must **not** relay.
2. MODE register bits 1 and 2 act as RX and TX interrupt locks: the chip sets them to 1 when it raises the IRQ, the game clears them back to 0 to re-arm. The code only works with this behaviour (§ 2.4).
3. TXOFFSET is a **13-bit word address** (bit 12 = RX area), not a byte address.
4. Driver's Eyes: the centre transmits, the sides only receive. No board relays in software.

---

## 1. Winning Run (winrun, 1988): test only

- GAME OPTIONS (GPU string `$45BB`): COIN1, COIN2, TIME RANK, SPEED RANK, LAP EASY, LAP TECHNICAL, FREE PLAY, MOTION, HISCORE CLEAR.
  No CPU NO., no LINK.
- The slave does nothing: loop `$1074-$107E` (watchdog), all IRQ vectors at `$1080` (rte).
  The C139 code present at `$28E0` in the slave ROM is not reachable.
- The master sets the SCI level to **1** (`$1086`), the **same as the VBL** (`$108E`), and the L1 handler (`$1928`) is the VBL one
  (it only acknowledges the VBL, `$1DE000`). An IRQ from the C139 would therefore re-run the game loop, and the SCI IRQ would stay
  unacknowledged, hence an interrupt loop. **The C139 must never raise an IRQ here.** During play, MODE is 0 (reset value),
  because the game only writes to the chip in the test.
- SWITCH TEST "SCI" test, master: `$2016` (entry), `$2064` (every video frame), result in `$600430` (1 = OK).
  Same code as winrungp `$29DE` / `$2A2C` (see § 2.8).

## 2. Winning Run Suzuka GP, Winning Run '91, Cyber Sled: ring in mode 9

### 2.1 Addresses

| | winrungp (slave) | winrun91 (slave) | cybsled (slave) |
|---|---|---|---|
| Slave C148 levels | CPU 1, EX 1, POS 1, **SCI 4**, VBL 1 (`$1038-$1058`) | same | SCI 4, VBL 6 (`$1058/$1060`); the VBL sometimes moves to level 5 (`$1BB4`) |
| C139 init (table) | `$142E` (table `$140E`), called at `$1074` | `$13E2` (table `$13C2`), called at `$1074` | `$3692` (table `$3672`), `jsr` at `$10C8/$1166` |
| VBL handler | L1 `$110A` → `$1158`, `$149C`, `$17BC` | L1 `$110A` → `$1156`, `$1450`, `$1770` | L6 `$1682` (or L5 `$1CFA`) → `$3188`, `$3700`, `$3A20` |
| Queueing (MODE &= 0xB) | `$149C` (`$154A`) | `$1450` (`$14FE`) | `$3700` (`$37AE`) |
| SCI handler | `$1574` | `$1528` | `$37D8` |
| Frame type (word 1) | **0x21** | **0x99** | **0x99** |
| Frame length | **0x2A** words | **0x24** words | **0x5E** words |
| TXSIZE written | 0x3F | 0x3F | 0x5E |
| Cabinet number read from | `$900220` (master: NVRAM `$180012`, `$105E`) | `$900228` (master: `$180012 & 7`, `$105E`) | `$900228` (master: `$180024 & 1`, `$1186`/`$1F40`) |
| Number of machines → master | `$90020A` | `$900208` | `$900208` |
| "LINK alive" counter | `$600B10` (reset to 0 when its frame comes back) | `$600734` | `$90020A` |
| Working variables | `$108004-$10805C` | `$10C004-$10C05C` | `$10D14E-$10D1A6` |

The code is the same; only the constants in the table and the location of the variables change. This was checked with a
normalised diff of winrungp `$149C-$17BC` against winrun91 `$1450-$1770`: it differs only in 0x21/0x99, 0x28/0x22, 0x2A/0x24 and 0x25/0x1F.
Cyber Sled reuses the same code with 0x99, 0x5C and 0x59.
winrungp and winrun91 therefore reject each other's frames. winrun91 and cybsled share type 0x99, but their lengths differ.
In what follows, the addresses are those of **winrungp**.

### 2.2 The C139 RAM also serves as a mailbox

Since all the CPUs see the C139 RAM, the slave keeps variables in it (values ≤ 0xFF per half-word):
- TX word 0x10 (`$B00020`): number of **other** machines in the ring (read back by the slave at `$11DA` → `$90020A`);
- TX words 0x11/0x12 (`$B00022/24`): mask of the numbers received during the last 16-VBL window (high byte / low byte);
- TX words 0x20/0x21 (`$B00040/42`): mask of the numbers transmitted by this machine (high byte / low byte);
- TX words **0x800 + id×0x80** (`$B01000 + id×0x100`), id = 0..15: one frame slot per number.
  For its own number, it is the frame to transmit. For another number, it is the last payload received, and word 0 serves as a reception counter.

The RAM must therefore behave like ordinary RAM (at least 9 bits read back unchanged), in both areas.
The game also writes into the RX area (§ 2.5).

### 2.3 Initialisation (slave `$142E`)

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
winrun91 (`$13C2`) and cybsled (`$3672`) have exactly the same table.
RXOFFSET = 0x1000 amounts to RX pointer 0 (the game masks it, § 2.5). The game never writes RXOFFSET again afterwards.

### 2.4 Transmission

**Queueing at every VBL** (`$149C`):
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

**SCI handler** (`$1574`), TX side:
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
- The TX branch **never** writes STATUS. Only the RX branch writes 0.
- `&= 0x0D` (last frame or nothing to transmit) and `&= 0x09` (some left) differ only in bit 2.
  The VBL queueing does `&= 0x0B`, which only touches bit 2. If MODE always read back as 9, these three writes would be identical.
  We would then get either an endless transmitter-free IRQ, or a transmission that never restarts. **So the chip must
  set MODE bit 2 to 1 when it raises the TX IRQ, and raise the IRQ again when the game clears the bit back to 0 while the transmitter is free.**
  Bit 1 plays the same role for RX. These conclusions come from the code. The exact electrical behaviour remains a **hypothesis**.
- The only way to start a transmission is the TX IRQ (STATUS bit 2). No other code writes TXOFFSET, START or TXSIZE.
  After a period with nothing to transmit, it is the VBL's `MODE &= 0x0B` that must trigger the IRQ.
- End of frame: TXSIZE (0x3F) is larger than the frame (0x2A). The game forces bit 8 of every word to 0, except the last one.
  The frame therefore stops at the first word whose bit 8 is 1, that word included (this is the "sync bit").
  Cyber Sled writes TXSIZE = 0x5E, exactly the length. The two readings then agree.

### 2.5 Reception, validation, relay (handler `$1574`, branch `$1630`)

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
What this requires of the chip:
- **RXOFFSET is the RX write pointer**, in words; the game only uses its low 12 bits. The search covers exactly
  LEN+1 positions starting from RXOFFSET: the RXOFFSET position itself, then LEN words backwards. It finds the end of the last complete frame even if a frame is still being received.
  This fits "RXOFFSET = next free location"; the "last word written" reading would also work.
- Frames must arrive **contiguous and with no added word**, keeping **bit 8** of the last word.
  The game locates the start of the frame at `end − (LEN−1)`.
- An interrupt handles **only one frame**, the most recent one. If two frames end before the handler runs, the older one is lost.
- The **relay is done by the game**. The next transmission starts from `txsrc[id] >> 1 = 0x1000 + k`, an address in the RX area,
  and takes the frame as the game modified it in place (hop + 1, checksum + 1). The C139 must read the RAM at the time it transmits.
  An RX frame can straddle the end of the area (0x1FFF → 0x1000), since 0x1000 is not a multiple of 0x2A.
  The transmit pointer must therefore wrap around inside its half, with bit 12 kept (necessary **hypothesis**).
- The sender removes its own frames (mask `TX[0x20..0x21]`). The hop counter ≥ 16 acts as a safeguard.

### 2.6 Counting the machines, "LINK OK n" display

- A frame that has gone round N machines comes back with hop = 1 (start) + (N−1) relays + 1 (handler increment) = N + 1.
  So `TX[0x10] = N − 1`, copied to `$90020A` (`$11E2`).
- winrungp's GAME OPTIONS 1 (GPU string `$6188`, master menu `$3332`/`$3372`/`$3480`, GPU display `$61F2`, pointer table `$62FC`):
  LINK line (y = 0x14):
  - "OK" if `$200B10` (= `$600B10`) < 8, otherwise "NG" (`$6BEE`). The slave increments this counter once per video frame
    (main loop `$1088`, capped at 8) and resets it to 0 when its frame comes back.
    So OK = its own frame came back within the last 8 video frames (≈ 133 ms).
  - then the number `$20014A + 1` = `$90020A + 1` = **N, the number of machines in the ring, itself included** (`$6A1E`, +1 offset).
  A lone cabinet, with an open ring, displays "NG 1". A cabinet looped back on itself would display "OK 1".
  LINK is not a setting: it is a status display.
- winrun91: same thing (GPU pointers `$1D90`); OK/NG from `$200734` (`$1D38` → `$2CD6`), number `$2007A0` + 1 (`$1D62`).
- Cyber Sled: slave GAME OPTIONS, LINK line (`$1E166`): "OK" if `$10D14E` ≠ 0, that is, if the slave received
  at least one foreign number in the previous 16-VBL window; otherwise "ERROR". The number N−1 goes through `$900208`.
  The master tests `$900208 == 1` at `$49D2`, which means "linked to the other cabinet".
- Presence: `TX[0x11..0x12]` (numbers heard within 16 VBL) is read back at every VBL (`$1158` → `$108004`) to know which cars are present.

### 2.7 Timing

- Everything goes through the slave at VBL (60 Hz). Each machine queues its frame or frames (its own number, plus numbers 8-15 for the
  computer-driven cars, see § 2.9), then transmits one frame per "transmitter free" IRQ.
  Relays are queued as soon as they are received and go out at the next TX IRQ, one after the other.
  In a ring of N machines, each one therefore transmits about N frames per VBL (N×0x2A words for winrungp, N×0x5E for cybsled).
- Frames go out back to back. The receiver must be able to run its handler (≈ 0.1-0.2 ms at 12.288 MHz) between two frame ends (§ 2.5).
- Game-side delays: its own frame must come back within 8 video frames; 16-VBL presence window. No timeout at the chip level.
- Cyber Sled: VBL at level 6, above SCI (4). The VBL handler can interrupt the SCI handler, never the reverse.

### 2.8 SWITCH TEST "SCI" test (master, winrungp `$29DE`/`$2A2C`, winrun `$2016`/`$2064`)

Test page table: `$295C` (entry) and `$299C` (every video frame), page 3. The result goes to `$600A70`, displayed by the GPU (string `$64C5`, "SCI").
```
enter:  START = 1 ; RXSIZE = TXSIZE = 0 ; STATUS = 0 ; MODE = 0x0F ; CONTROL = 0 ; START = 0   // START=0 in mode 0x0F
        RXSIZE = TXSIZE = 0 ; RXOFFSET = TXOFFSET = 0 ; TX[k] = k*0x25 (k = 0..15)
frame:  STATUS = 0 ; ok = 0 ; n = RXSIZE ; RXSIZE = 0 ; RXOFFSET = 0
        if (((-n) & 0xFF) == 0x10) ok = 1    // 16 words received since the previous frame: RXSIZE COUNTS DOWN
        TXOFFSET = 0 ; TXSIZE = 0x10          // send 16 words (no bit 8, START = 0): the TXSIZE write starts it
```
- In mode 0x0F, the chip therefore transmits TXSIZE words as soon as TXSIZE is written, with START at 0.
  RXSIZE goes down by 1 per word received. The test is OK if 16 words have arrived since the previous video frame.
- No IRQ (SCI level 0 on the master; in mode 0x0F MODE bits 1 and 2 are 1).
  The test passes if the upstream machine also transmits 16 words per video frame, or with a TX → RX loopback.
  A lone cabinet in an open ring displays NG.

### 2.9 Settings, cabinet number, maximum number of cabinets

- **winrungp** (master `$105E`, GAME OPTIONS 1 page `$3332`, save `$3480`). NVRAM bytes:
  [0] = 0x4D, [1] = 0x01, [2] COIN1 COIN, [3] COIN1 CREDIT, [4] COIN2 COIN, [5] COIN2 CREDIT, [6] LAP, [7] FREE PLAY,
  [8] **CAR TYPE** (0-3: A RED, B WHITE/RED, C YELLOW/BLUE, D BLUE; the colour goes into the frame, `$1530`), [9] **CPU NO.** (0-7, displayed 1-8),
  [0xA] MOTION, [0xB] GAME OPTIONS 2 setting, [0xC] = sum of bytes 0..0xB modulo 256 (checked at `$2B26`).
  The master copies CPU NO. to `$900220`; the slave takes it `& 0xF`.
- **winrun91**: same layout. [0] = 0x63, [1] = 0x00 (`$1DE4`), CPU NO. = [9] (`$105E`, `& 7` → `$900228`), sum at [0xC].
- **cybsled**: [0] = 0x7D, [1] = 0x03, **POSITION** = bit 0 of byte [0x12] (`$180024`, read at `$1186` and `$1F40`; slave menu `$1E138`:
  "1 LEFT" / "2 RIGHT", via `$901B50`), sum of bytes 0..0x15 at [0x16] (`$21AA`).
- **Maximum number of cabinets**:
  - winrungp and winrun91: **8** (3-bit CPU NO.). The protocol has 16 numbers: numbers 0-7 are the cabinets, numbers 8-15 the computer-driven cars.
    Only the cabinet with the lowest active number simulates them (`$B880-$BA06` in the master, masks `$B0DC` = 0x0100…0xFF00 in `$90020E`).
    How many there are depends on N (`$B0EC`: 4, 4, 4, 3, 3, 3, 2, 2 for N = 1..8). The hop < 16 safeguard allows at most 14 machines.
  - Cyber Sled: **2** (LEFT = number 0, RIGHT = number 1). The RIGHT cabinet also transmits number 8 (master table `$18E8`, `$268C`).
  - The cabinet numbers must be **different**. With the same number twice, each cabinet takes the other's frame for its own and discards it.

## 3. Driver's Eyes: internal link between the three boards

The three boards have identical ROMs; the screen comes from DSW 3-4, read by the MCU at `$A00026`, value inverted.
`$A00026 & 0xC` is 0 for the **centre**, 4 for the left, 8 or 0xC for the right. The result is stored in `$900104` (0 = centre), at `$3CE8` in the slave.
The link runs on the **slave** of each board: SCI level 4 (`$1050`), handler `$3DF0`, and CPU IRQ level 5 (`$3F0A`) sent by the master.

**Init** (`$3C92`, table `$3C72`):
```
block 1: STATUS 0, MODE 0x0F, CONTROL 0, START 1, RXSIZE 4, TXSIZE 4, RXOFFSET 0, TXOFFSET 0      // "config" START = 1
RAM[0..0x1FFF] = 0
block 2: STATUS 0, MODE 0x0D, CONTROL 0, START 1, RXSIZE 0, TXSIZE 0, RXOFFSET 0, TXOFFSET 0x1000
START = 1 (again) ; txoff = 0 ; srcoff = 0 ; chunks = 0 ; SCI ack
```
All three boards program **0x0D**. Then:
- the centre does `MODE &= 9` at each transmission, so MODE is **0x09** after the first one;
- the sides only ever write `MODE &= 0xD` from then on, so they stay at **0x0D**.

This is where the MAME note "09 centre / 0D sides" comes from. START stays at 1 permanently on the sides.

**Preparation** (CPU IRQ level 5, `$3F0A`, once per request from the master):
4 chunks of 0x80 bytes at `$102000/$102080/$102100/$102180`.
- Header: chunk number (1..4), sequence number, 2 free bytes, total = 4 (chunk 1 only).
- Data: `$900200`, `$900240`, `$908040…`, `$908240…`.

Then `chunks = 4` and `srcoff = 0`.

**Transmission (centre only)** `$3D2A`. Called by the main loop (`$10EE`, `$19EE`, `$1A5E`, on every pass) and by the SCI handler:
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
**SCI handler** `$3DF0`:
```
st = STATUS ; STATUS = 0 ; SCI ack
if (side) { parse(st) ; MODE &= 0x0D }       // re-arm RX only (TX stays disarmed: sides never transmit)
else      { busy-wait 0x200 dbra (~0.42 ms) ; tx_chunk() }       // centre: next chunk after a TX interrupt
```
**Reception (sides)** `$3E3A`:
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
The master of a side board derives its state from this data; its side code starts at `$13EA`.
In one of the states the master gives it, the slave displays "RECEIVE ERROR" (routine `$1382`, string `$13AA` in the slave).
This is what is seen when nothing arrives from the centre.

What the chip must do for Driver's Eyes:
1. **Centre, transmission** of frames of TXSIZE = 0x80 words starting at TXOFFSET (ring of 32 buffers in the TX area).
   Transmission starts when START goes from 1 to 0, with TXSIZE ≠ 0. **TXSIZE must read back non-zero during transmission and return to 0 at the end**.
   Otherwise the centre stays stuck after the first chunk, because the main loop polls `tst.b $B8000B`.
   The "transmitter free" IRQ at the end (MODE bit 2 cleared to 0 just before) speeds up chunks 2 to 4. It is not essential.
2. **Sides, reception**: write the frame right after the previous one in the RX ring (RXOFFSET advances by 0x80 per frame), keeping bit 8 of the last word,
   then RX IRQ (STATUS bit 1, MODE bit 1 cleared to 0 by the handler), **one IRQ per frame**.
   The handler must be given time to run between two frames. On the real hardware, the centre waits about 0.42 ms after each TX IRQ.
3. **Both sides must receive every frame from the centre.** No board relays in software.
   The sides never transmit: the only call to `$3D2A` that does not depend on the role (`$19EE`) happens at startup, with no chunk queued.
   The centre ignores what it receives (it does not parse it). MAME's current ring (`sci_de_hack`: left → centre → right → left)
   only works because the current emulation relays inside the chip.
   - **Hypothesis A**: in the cabinet, the centre's TX output drives the RX inputs of both sides (broadcast).
   - **Hypothesis B**: a C139 with START bit 0 = 1 (transmitter blocked) repeats what it receives. The sides keep START = 1 permanently;
     winrungp and RR2 only keep START = 1 for a few instructions.
   No proof in the code. The simplest and safest in MAME: connect directly, inside the machine,
   the centre's transmitter to the receivers of both sides, without TCP.
4. The centre has MODE = 9 after its first transmission, so the RX IRQ is armed. If it receives something (sent back by the ring),
   its handler waits 0.42 ms then calls `$3D2A`. This is harmless, but useless.

Number of boards: 3, fixed. There is no link between cabinets.

## 4. Air Combat (aircomb, AC2 US): dead link code

- Master: C148 SCI at level 4 (`$119A`), handler `$125A`.
  It copies the 8 registers to `$1001BE`, then does STATUS = 0, MODE = 0x0D, RXSIZE = 0, RXOFFSET = 0, `$100108 = 1`, ack.
- Init: `$19BE` (called at `$114A`) writes the 8 registers in mode 0x0F, all at 0 (START = 0). Then MODE = 0x0D at `$11B8` (`$145A` in the other path).
- `$19E6` (transmission of a 0x1F-word frame: CONTROL 1, 0x1E words, CONTROL 3, one last word, TXOFFSET 0, TXSIZE 0x1F) and `$1AB8`/`$1AE4`
  (reception based on RXOFFSET & 0xFF, sum of the words) are **called from nowhere**. We searched for `bsr`/`jsr`, absolute addresses,
  PC-relative references and raw words in the program ROM; the data ROM was not checked.
  The `$100108` flag, set to 1 by the handler, is only read in this dead code. The link state `$100006` is only written there, and never read.
  The role (`$7E32(a6) & 3`, an input) is computed, but nothing uses it.
- Slave: SCI level 0 (`$69BE`). Same dead routines (`$11F0`, `$1218`: 4-word frame = 2 data, checksum, CONTROL 3, 0xFF; `$126E`), handler `$6A7C`.
- The "LINKNO." in the slave's menu (`$DEF9`) belongs to the OBJECT TEST: it is the sprite link, not the link between cabinets.
- The chip only has to stay silent. If a frame arrives, the handler rewinds RXOFFSET and does nothing else.

## 5. Starblade, Solvalou

- starblad, master: `$46C8` (called at `$4730`) writes the table `$46DC`:
  STATUS 0, MODE 0x0F, CONTROL 1, START 0, RXSIZE 0xFF, TXSIZE 0, RXOFFSET 0, TXOFFSET 0x1000.
  The routine `$46EC` (test of STATUS bit 1, then MODE 0, MODE 0x0D, RXOFFSET 0) is referenced nowhere.
  SCI level 0 on both CPUs (master `$4778`, slave `$1FD2E`). No link.
- solvalou: no access to the C139 registers or RAM; the few matches are data. SCI level 0 (master `$4B6`, slave `$246A`).

---

## 6. What the C139 must do for these games

Minimal model that runs all the code above. Points 1, 3 and 5 are imposed by the code. The rest is the simplest reading, marked **hypothesis** where it goes further.

**Registers**
1. **MODE.**
   - The games write 0x09, 0x0D or 0x0F, then only `&=` (0x09, 0x0B, 0x0D).
   - **Bit 1 = RX IRQ lock, bit 2 = TX IRQ lock.** The chip sets the bit to 1 when it raises the corresponding IRQ.
     When the game clears the bit back to 0 and the condition is true (transmitter free, or received frame pending), the chip raises the IRQ again at once.
     These are flags internal to the chip, not mode bits: 0x0D = "9 with TX locked" (Driver's Eyes proves it, 0x0D & 9 = 9),
     and 0x0F = "everything locked".
   - **Hypothesis**: bit 3 = chip active. MODE = 0 at reset, and in that case no IRQ.
     This is mandatory for winrun: its SCI IRQ is at the VBL level and never acknowledged.
   - No IRQ in mode 0x0F.
2. **STATUS.**
   - Bit 1 = frame received, bit 2 = transmitter free (TX IRQ).
   - Writing 0 clears it. winrungp only clears it in its RX branch.
3. **TXOFFSET**: 13-bit **word** address; bit 12 = RX area (relay). Do not divide it by 2, and do not look for the sync bit further along.
   The transmit pointer wraps around inside its 4 K-word half (**hypothesis** needed for relays that straddle 0x1FFF).
4. **START** bit 0 = transmitter blocked. A transmission starts when TXSIZE ≠ 0 and START bit 0 is 0, that is:
   - when TXSIZE is written, if START = 0 (SCI test, Air Combat);
   - or when START goes from 1 to 0 (winrungp, cybsled, Driver's Eyes, RR2).

   The value written in mode 0x0F (3, 1 or 0) needs no lasting effect for the S21 games.
   The behaviours that MAME ties to `m_reg_f3 & 2` are wrong here: TXOFFSET in bytes, replacement of the link counter.
5. **TXSIZE**: maximum number of words. The frame stops **after the first word whose bit 8 is 1, or after TXSIZE words**, whichever comes first.
   This rule covers winrungp (bit 8 at word 0x29, TXSIZE 0x3F), cybsled (0x5D / 0x5E), Driver's Eyes (0x7F / 0x80),
   the SCI test (no bit 8, 16 words) and RR2 (0x24).
   TXSIZE reads back ≠ 0 while the transmission is pending or in progress, then **returns to 0 at the end** (Driver's Eyes, RR2).
   At that point, the TX IRQ fires if MODE bit 2 is 0.
6. **RXOFFSET**: 12-bit RX write pointer. Each received word goes to `RAM[0x1000 + (RXOFFSET & 0xFFF)]`, then RXOFFSET goes up by 1.
   It wraps around within the 4 K words. The initial value is the one written by the game (0x1000 or 0).
7. **RXSIZE** goes down by 1 per received word, on 12 bits (SCI test). No S21 game reads it in game mode.
8. **CONTROL**: no effect needed on S21. Driver's Eyes sets it to 1 while filling, then to 0.
9. **RAM**: ordinary 9-bit RAM for the host, in both areas (TX mailbox, patches in the RX area).
   Words read back must have clean bits 9-15, because the game tests `& 0x100`.

**Line and frames**
10. 9-bit words are transmitted: 8 data bits, plus bit 8 on the end word.
    The same words are received, written **one after the other** with no header or padding, bit 8 kept.
    End of frame = word received with bit 8: STATUS |= 2, then RX IRQ if MODE bit 1 is 0, otherwise pending until re-armed.
11. **No relay in the chip**, and no ring number or hop counter added to the frame.
    winrungp, winrun91 and cybsled relay by themselves (TXOFFSET in the RX area), count the hops (word 2) and remove their own frames.
    The current emulation relays and rewrites word 2 (`m_buffer[7] = m_buffer[0x1ff]`). This works by chance, at the cost of many duplicate frames.
12. **Timing**:
    - Deliver at most **one frame per RX IRQ**. Keep the following ones in a queue until the game clears MODE bit 1 back to 0,
      or at least until it clears STATUS. The handlers only process the most recent frame.
      The real hardware would lose some; this queue is safer and has no side effects.
    - Signal "transmitter free" after a delay proportional to the length, for example 5-10 µs per word (**hypothesis** about the bit rate).
      It can also be done on the next tick.
    - About N frames per VBL per machine must be sustained (8 × 0x2A words for winrungp with 8 cabinets).
13. **Transport**: simple ring, each instance's transmitter connected to the next one's receiver.
    The game handles identity: different CPU NO. or POSITION in the NVRAM.
14. **Driver's Eyes**: broadcast each frame from the centre to **both** sides, for example by linking the three C139s in the driver, without a socket.
    Failing that, keep the ring and have the boards that never transmit (START bit 0 = 1) repeat the frames (**hypothesis B**).
    TXSIZE must also return to 0 at the end of transmission.
15. **winrun, Air Combat, Starblade, Solvalou**: no IRQ outside active mode. For winrun, the only requirement is the SCI test in mode 0x0F
    (transmission of 16 words when TXSIZE is written, RXSIZE counting down, no IRQ).
