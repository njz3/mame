# Linked racing games with MAME njz3 — how-to and tips

This document gathers the settings and steps needed to make several MAME instances race together
(several cabinets, or several windows on one PC). It is based on the tests run from 2026-11-02 to 2026-11-12
(0.289 njz3 builds: release g8e81ca4, then g6b9d67c for Konami, then `dev/mame3d` g754eb0adb02).
Details on the Namco chip: `C139_en.md` (French: `C139.md`).

The ready-made NVRAMs (one per cabinet and per game) and the list for the cabinet image (`liste-image.csv`) are in
the sessions' shared notes folder: `link-tests-0289/` (README in French, same information as below).

---

## 1. Basics

- Each instance opens **one incoming TCP connection** and **one outgoing connection to the next cabinet**:
  ```
  mame.exe <game> -comm_localhost <my IP> -comm_localport <my port> -comm_remotehost <next IP> -comm_remoteport <next port>
  ```
  Defaults: `0.0.0.0:15112` locally, `127.0.0.1:15112` remotely. A lone instance with these values connects
  to itself: the ring is looped back, and the game stays playable alone.
- The cabinets form a **ring**: cab1 → cab2 → cab3 → cab4 → cab1. With 2 cabinets: cab1 → cab2 → cab1.
- **`-comm_framesync` is not used**: every test was run without it.
- **Each cabinet has its own NVRAM** (or its own DIP lines in its cfg). That is where the game reads its cabinet
  number, its role (master or slave) and the number of machines. Without it, each cabinet races alone.
- Start order and start offset do not matter: every game links with 0, 4 or 15 s between the cabinets.
  The connection comes up on its own when a cabinet starts late.

### On the cabinets

`RunMAMELinkGame.cmd` calls `set_cab_id.bat`, which sets `CAB_ID` (`_link_cab1` or `_link_cab2`),
then `set_cab_network<CAB_ID>.bat`:

| Cabinet | LOCALIP | LOCALPORT | NEXTIP | NEXTPORT |
|---|---|---|---|---|
| cab1 | 192.168.1.4 | 15112 | 192.168.1.5 | 15113 |
| cab2 | 192.168.1.5 | 15113 | 192.168.1.4 | 15112 |

The write directory is `-writedir C:\Racecab\Saves\mame_link_cabN` (UWF). On the first run, MAME reads the NVRAMs
in place in `nvram\<game>`, which Build-Config copies from `nvram_link_cabN`. After that it only reads the ones in
the writedir. **To change a cabinet's NVRAM, first empty `Saves\mame_link_cabN\nvram\<game>`.**

For 3 or 4 cabinets, `set_cab_network_link_cab3/4.bat` scripts are also needed to close the ring
(cab2 → cab3, cab3 → cab4, cab4 → cab1).

Cabinet buttons: coin = vJoy 1, Test = vJoy 2, Service = vJoy 3, Start = vJoy 4.

### On a single PC (tests)

One loopback address and one port per instance, and one `-writedir` per instance (otherwise the NVRAMs get mixed):
```
mame.exe raverace -writedir cab1 -comm_localhost 127.0.0.4 -comm_localport 15112 -comm_remotehost 127.0.0.5 -comm_remoteport 15113
mame.exe raverace -writedir cab2 -comm_localhost 127.0.0.5 -comm_localport 15113 -comm_remotehost 127.0.0.4 -comm_remoteport 15112
```
Put cabinet N's NVRAM in `cabN\nvram\<game>\` before the first run.
For 4 instances: 127.0.0.4 to .7, ports 15112 to 15115, the last one pointing back to the first.

---

## 2. Per-game settings

### 2.1 Namco (C139): everything is in the NVRAM

With the new C139 (since g8e81ca4), **no port tweak is needed**: the standard ports 15112/15113 work for every game.
Just put the cabinet's NVRAM in place: nothing to do in the menus.

| Game (set) | Cabinets | What tells the cabinets apart |
|---|---|---|
| ridgera2 (Ridge Racer 2) | 2-4 | CPU NO. 1-4 and CAR TYPE 1-4 (EEPROM 0x71C/0x71D) |
| raverace (Rave Racer) | 2-4 | CPU NUMBER 1-4 and CAR TYPE 1-4 (EEPROM 0x2C0/0x2C1) |
| acedrive (Ace Driver) | 2-8 | PCB ID NUMBER 1-4 (EEPROM 0x111) |
| victlap (Ace Driver Victory Lap) | 2-8 | PCB ID NUMBER 1-4 (EEPROM 0x10C) |
| winrun91, winrungp (Winning Run '91, Suzuka GP) | 2-8 | CAR TYPE and CPU NO. (bytes 8-9, checksum at 0xC) |
| finallap, finalap2, finalap3 (Final Lap 1/2/3) | 2-8 | PCB number 0xFE01-0xFE04 (bytes 0x10-0x11) |
| fourtrax (Four Trax) | 2-8 | GRID NO 0-3 (byte 8) |
| suzuka8h, suzuk8h2 (Suzuka 8 Hours 1/2) | 2-8 | GRID NO 0-3 (byte 9) |
| dirtfoxj (Dirt Fox) | 2-8 | CAR TYPE 1-4 (byte 0x47, checksum at 0x41) |
| cybrcycc (Cyber Cycles) | 2-4 | ID NUMBER 0-3 (EEPROM 0x26) |
| adillor (Armadillo Racing) | 2-4 | PCB ID 0-3 (checksummed settings record, see `C139_en.md` § 7) |
| alpinr2b (Alpine Racer 2) | 2 | ID NO. 0 LEFT / 1 RIGHT (EEPROM 0x32) |

To redo a setting by hand:
- **raverace**: Test (toggle). Wheel right to go down, left to go up, accelerator to enter, edit or confirm.
  Go to GAME OPTIONS (2nd line). The cursor lands on NEXT PAGE; 13 steps left lead to CPU NUMBER.
  Press accelerator, wheel right (+1), accelerator. 1 step right leads to CAR TYPE: same moves.
  Go down to EXIT, accelerator, then Test (toggle) to leave. The line under the cursor blinks.
- **acedrive / victlap**: Test (toggle). Lever (Shift Up) to go down, accelerator to enter.
  Go to GAME OPTIONS, onto PCB ID NUMBER. Accelerator, Shift Up (1 → 2), accelerator.
  Shift Up down to EXIT, accelerator, then Test.
- **winrun91 / winrungp**: Test (toggle) opens GAME OPTIONS 1. Wheel to change line, lever Up/Down to change the value.
  Set CAR TYPE A → B and CPU NO. 1 → 2, then Test. The "LINK OK n" line is display only.
- Games without a menu path here (Final Lap, Four Trax, Suzuka, Dirt Fox, Cyber Cycles, Armadillo, Alpine Racer 2):
  use the provided NVRAMs.

### 2.2 Konami (K056230): MAME "Network ID" DIP

midnrun (Midnight Run), windheat (Winding Heat), gticlub (GTI Club). Nothing to set in the game: cab1 keeps the
default ID 1, and the other cabinets get a DIP line in the `<input>` section of their cfg:

| Game | cab2 | cab3 | cab4 |
|---|---|---|---|
| midnrun, windheat | `<port tag=":IN3" type="DIPSWITCH" mask="12" defvalue="12" value="8" />` | value="4" | value="0" |
| gticlub | `<port tag=":IN3" type="DIPSWITCH" mask="3" defvalue="3" value="2" />` | value="1" | value="0" |

**gticlub** redoes its controls setup on every boot. With a blank EEPROM and no wheel, it shows
"DEVICE ERROR (STEERING WHEEL) – PUSH TEST SWITCH TO SKIP" after about 70 s. The provided EEPROMs are already
past it. Otherwise: one short press on Test, then Shift Down once (the menu wraps to GAME MODE), then Start.

### 2.3 Sega: test menus or DIPs

| Game | Board | Setting | Where |
|---|---|---|---|
| **scrossa** (not scross!) | s32comm | cab1 NETWORK / MASTER, cab2 NETWORK / SLAVE | test menu (EEPROM) + `scrossa.cfg` (inputs) |
| orunners (OutRunners) | s32comm | NETWORK ASSIGNMENTS → COMMUNICATION NETWORK: cab1 MASTER / ID 1, cab2 SLAVE / ID 2 | test menu |
| f1lap (F1 Super Lap) | s32comm | GAME ASSIGNMENTS → NETWORK ID NUMBER: cab1 "1 MASTER", cab2 "2" | test menu, **then restart the game** |
| radr (Rad Rally) | s32comm | Game & system setting → Network ID number (change with Start): cab1 "1 Master", cab2 "2" | test menu |
| vr (Virtua Racing), vformula (Virtua Formula) | m1comm | GAME SYSTEM → LINK ID: cab1 MASTER, cab2 SLAVE | test menu, **leave through EXIT, then YES** |
| pdriftl (Power Drift Link) | ybdcomm | Link_SW1: Cabinet ID 1/2, Cabinet Count 2 on both | MAME DIP (cfg) |
| smgp (Super Monaco GP) | xbdcomm | Machine ID 1/2, Number of Machines 2 | MAME DIP (cfg) |

Menu details:
- **scrossa**: Test needs a **very short press** (≈ 40 ms). A longer press selects EXIT, where the cursor sits by
  default, and the game restarts on release. This is the ROM's behaviour; the real cabinet does the same.
  Service moves the cursor, Test confirms. From EXIT, 12 presses of Service lead to COMMUNICATION SETTING; press Test.
  In that menu: 1st line, Test (STANDALONE → NETWORK); 2nd line, Test (MASTER ↔ SLAVE); EXIT, Test;
  then EXIT from the main menu.
- **orunners**: in the menus, Service = Push SW1, Test = Push SW2.
- **vr / vformula**: red button to select, green to enter, blue to change the value
  (NO LINK → LIVE → SLAVE → MASTER). Green only leaves from the EXIT line. Then answer **YES**
  to "SAVE THE CHANGE ?", otherwise nothing is kept. The vformula menu is shorter than vr's:
  after LINK ID come RACE MODE, COUNTRY, ADVERTISE, DIFFICULTY, EXIT. Count the steps, or the cursor
  wraps back to the top.
- **f1lap**: the new ID only takes effect after the game is restarted.

Sega DIP lines (`<input>` section of each cabinet's cfg):
- **pdriftl**: on every cabinet `<port tag=":ybdcomm:Link_SW1" type="DIPSWITCH" mask="240" defvalue="16" value="32" />`
  (number of cabinets: 2 → 32, 3 → 48, 4 → 64). In addition, on cabinets 2 to 4:
  `<port tag=":ybdcomm:Link_SW1" type="DIPSWITCH" mask="15" defvalue="1" value="2" />` (value = cabinet number).
- **smgp**: on every cabinet `<port tag=":mainpcb:IO1PORTD" type="DIPSWITCH" mask="56" defvalue="56" value="48" />`
  (number of machines: 2 → 48, 3 → 40, 4 → 32). In addition, on cabinets 2 to 4:
  `<port tag=":mainpcb:IO1PORTD" type="DIPSWITCH" mask="7" defvalue="7" value="6" />` (cabinet 2 → 6, 3 → 5, 4 → 4).

MAME has no command-line option for DIPs: these lines go into each cabinet's own cfg.

---

## 3. Known pitfalls

- **scross does not link**: its COMMUNICATION SETTING menu is dead code. Run **scrossa**
  ("World, linkable", already inside the merged scross.zip) with `scrossa.cfg`. Without an `<input>` section in
  the cfg, vJoy 1 is not Coin 1 and the game stays at 0 credits.
- **finallap, finalap2, fourtrax, dirtfoxj**: `lh5762.6n` (CRC 90db1bf6) is missing from the NAS zips.
  Put it as a loose file in `MameRoms\<set>\lh5762.6n`, leaving the zip untouched; MAME finds it next to it.
  Its content is the same as `04544191.6r` in finalap3.zip.
- **smgp**: each player must qualify (a lap under 45 s). A player who fails qualifying goes back to
  "ENTRIES WELCOME": this is normal, not a link problem.
- **vr.zip**: never overwrite it (it holds epr-15111.12 and the merged vformula).
- **Changing NVRAM** on a UWF cabinet: empty the writedir subfolder first (see § 1).
- **Rave Racer with 4 cabinets**: 2 times out of 14 runs, two or three cabinets showed the same POSITION
  (once with other MAME instances running on the PC, once in a close race). The 6 runs after that were consistent.
  Worth watching; no regression proven.
- **Old exe 919EFA6F (before g8e81ca4)**: it needed port 15114 on cab2 for acedrive, victlap, winrun91 and winrungp,
  and linked neither ridgera2 nor Final Lap. That tweak is no longer needed (it has no effect if left in).

## 4. Games without link

- **ridgerac**: standard Ridge Racer, no link in the game.
- **ridgeracf** (Full Scale): 3 screens on one machine, texture ROMs never dumped, not playable.
- **driveyes** (Driver's Eyes): one machine with 3 screens, the three boards are wired together in the driver.
  All three screens work.
- **dirtdash** (Dirt Dash): no link code in the game.

## 5. Checking that a link works

- **Attract or selection screen**:
  - ridgera2: "Up to 4 can race at once!";
  - raverace: "PLAY LINKED GAMES…";
  - Final Lap: "UP TO 2/4 CAN RACE SIMULTANEOUSLY";
  - smgp: "ENTRIES WELCOME – LINK SYSTEM".
- **In the race**:
  - the position is shown out of the number of cabinets (1/2, 2/2…);
  - the other players' cars appear, in their colours;
  - vr and vformula show 1ST/2, 2ND/2.
- A cabinet alone shows 1/1, "UP TO 1 CAN PLAY" or "One-player VS computer cars" instead.

## 6. With `dev/mame3d` (HD rendering)

- Link also works with `-hires3d 2` or `4` and the Vulkan renderer (`-hires3d_renderer auto`). Tested on the
  12 games with a 3D chip (System 22, System 21, gticlub, midnrun, windheat), with 0, 4 or 15 s offsets.
- `-verbose` should print "hires3d drawn with Vulkan on …". Checked solo for one game of each family.
- `-output network` broadcasts `mame_start = <game>` over UDP on port 8001 at start-up.
  BackForceFeeder and the njz3 FFB Plugin both listen to it (shared port).
- On exit, every tested game leaves MAME with code 0: all 27 games in native mode, the 12 3D games with Vulkan.
