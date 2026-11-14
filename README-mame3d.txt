MAME 0.289 - njz3 mame3d
========================

Copyright 2026 Benjamin Maurin (njz3)

Version : VERSION_LINE
Source  : https://github.com/njz3/mame/tree/dev/mame3d  (commit COMMIT_LINE)
Build   : Visual Studio 2022, release x64, DATE_LINE. mame.exe only depends
          on Windows DLLs (C++ runtime and zlib are static). Vulkan is loaded
          at run time (graphics card driver); it is not required.

mame3d = MAME njz3 FFB edition (everything described below) + the 3D of
1990-2010 racing games drawn at a higher resolution, on the graphics card
(Vulkan) or in software.

High resolution 3D
- Option -hires3d N (1 to 16; 1 = native, the default): the 3D is drawn at
  N times the native resolution. The 2D layers (texts, counters, sprites)
  stay the game's own, scaled up. The emulation (CPU, DSP, sound, inputs,
  link) does not change. Example: mame vr -hires3d 3
- Option -hires3d_renderer auto|vulkan|software (auto by default): auto
  draws with Vulkan when the graphics card allows it, otherwise in software.
  The software renderer is the reference; it is much slower at x3/x4.
  With -verbose, MAME reports the chosen renderer and the graphics card.
- Supported hardware (software and Vulkan):
    Sega Model 1           : Virtua Racing, Virtua Formula
    Namco System 21        : Winning Run, Driver's Eyes
    Namco System 22 / S22  : Ridge Racer 1/2, Rave Racer, Ace Driver,
                             Victory Lap, Cyber Cycles, Dirt Dash, Alpine Racer
    Konami K001005         : GTI Club, Midnight Run, Winding Heat, Jet Wave
    3dfx Voodoo 1/2        : San Francisco Rush (and The Rock), California
                             Speed, Hyperdrive, Vapor TRX, Road Burners,
                             Racing Jam 1 and 2, Hang Pilot, Thrill Drive
    3dfx Banshee/Voodoo 3  : San Francisco Rush 2049, Cart Fury, Thrill
                             Drive 2, Xtrial Racing, GTI Club 2
    Midway V-Unit          : Cruis'n USA, Cruis'n World, Off Road Challenge
    Midway Zeus 2          : Cruis'n Exotica
    Taito JC               : Side by Side 1/2, Dangerous Curves
    Gaelco 3D              : Speed Up, Surf Planet, Radikal Bikers
    SNK Hyper Neo Geo 64   : Roads Edge, Xtreme Rally
    Namco System 23        : Race On!, 500 GP, Motocross Go! (Vulkan
                             only; not the Gorgon games)
    PlayStation graphics
    processor              : Pocket Racer, Kart Duel and the other games of
                             Namco System 10/11/12, Konami GQ/GV/573, Sony
                             ZN, Taito GNET (Vulkan only, 15 bits)
  Model 2, Model 3, Naomi/Atomiswave and Hikaru are not included (see
  sm2-emu, Supermodel, Flycast, hikaruloader).
- The Vulkan picture is the same as the software one, give or take a few
  pixels on colour blending.
- With -video bgfx, the Vulkan picture stays on the graphics card: it is
  handed to bgfx (Direct3D 11) instead of being read back by the CPU, when
  both use the same card. Option -bgfx_hires3d_share, on by default
  (-nobgfx_hires3d_share goes back to reading back). bgfx then draws on the
  emulation thread.
- If the graphics card fails during play, rendering falls back to software
  or native, with a message, instead of a black screen.
- Tips: x2 or x3 on a modest PC. Without this sharing (other video modules),
  at x4 each frame goes back and forth between the graphics card and memory
  (15 MB), which can limit the speed.
- Driver's Eyes (three screens): the three boards are linked directly, all
  three screens work.

Display
- Option -aspect_ratio (same values as the cabinets' other emulators): the
  frame the screen is shown in, centred with black bars.
    original                  : as before (follows -keepaspect), the default
    4:3, 16:9                 : the screen keeps its shape inside the frame
                                (4:3_letterbox = 4:3)
    4:3_stretch, 16:9_stretch : the screen is stretched over the whole frame
    4:3_crop, 16:9_crop       : the screen covers the frame, the excess is cut
- Option -crop_position -100..100 (0 by default): the part kept by the _crop
  modes (-100 the left or top edge, 0 the middle, 100 the right or bottom
  edge).
- Option -widescreen (16:9 hack): the 3D is drawn in 16:9 with a wider field
  of view, and the screen is shown in 16:9. Only for the chips whose 3D
  projection MAME does, with the Vulkan renderer (also at -hires3d 1): Sega
  Model 1, Namco System 22 / Super 22, Namco System 23, SNK HNG64, Midway
  Zeus 2. Without Vulkan, the picture stays 4:3. The games that project
  their 3D themselves get a wider view another way (below).
  Example: mame ridgerac -hires3d 2 -widescreen
- -widescreen on the games that project their 3D themselves: the game is
  given a wider field of view, or its polygons are drawn narrower, and the
  picture, which keeps its size, is shown stretched to 16:9 (anamorphic).
  The 3D keeps its shape; texts, counters and sprites are stretched
  (-widescreen_2d has no effect), except on Side by Side 1 and 2, GTI
  Club, Midnight Run, Winding Heat, Racing Jam 1 and 2 and Thrill Drive 1:
  there the 2D layer drawn over the 3D keeps its shape in the middle of
  the picture (-widescreen_2d stretch stretches it). Without -hires3d, in
  software and with Vulkan.
    Thrill Drive 2, Thrill Drive 1, Racing Jam 1 and 2, GTI Club, Midnight
    Run, Winding Heat, GTI Club 2, Xtrial Racing, San Francisco Rush, Rush
    2049, Road Burners, California Speed, Side by Side 1 and 2, CART Fury:
    the camera of the game is made 4/3 wider (in memory, in its code, or as
    its 3D processor reads it).
    PlayStation games (Pocket Racer...): the projection of the GTE, the 3D
    unit of the CPU, is made 4/3 narrower.
    Winning Run and Driver's Eyes (System 21), Radikal Bikers and Speed Up
    (Gaelco 3D), Cruis'n USA, Cruis'n World and Off Road Challenge
    (V-Unit): these games send their 3D past the edges of the screen; it is
    drawn 4/3 narrower. Driver's Eyes: each of the three screens gets it,
    for a single (centre) screen; side by side, their edges overlap.
    What the game did not send (objects outside the 4:3 view) is missing at
    the edges, and a screen the game covers with a 4:3 picture (menus,
    transitions, game over of the V-Unit games and Radikal Bikers) shows
    the 3D behind it on the sides.
  Not yet: Hang Pilot (Voodoo), Dangerous Curves (Taito JC, no 3D in MAME).
  Example: mame midnrun -hires3d 2 -widescreen
- Thrill Drive 2 (all versions): the rear view mirror keeps its shape.
  Thrill Drive 1 and Racing Jam: the 3D parts of the dashboard (rev
  counter, speed) are stretched, the rear view mirror keeps its shape. In
  the cockpit view of Thrill Drive 1, the bottom of the cockpit, made for
  4:3, leaves the road visible at the edges.
- Option -widescreen_2d 4:3|stretch (4:3 by default): texts, counters and
  sprites stay in the middle with their shape, or are stretched over the
  whole width. The Zeus 2 draws its 2D as polygons: it stays in the middle.
- Option -widescreen_bg: the 2D backgrounds under the 3D (tilemaps) are also
  drawn on the sides (HNG64: the sky of Roads Edge; System 22 has none).
- Limits of the hack: what the game places off screen becomes visible (a 3D
  sign at the edge of the Roads Edge selection screen, the 3D city behind
  the Xtreme Rally intro); a background that does not extend past the screen
  leaves black on the sides. With -widescreen_bg and -widescreen_2d stretch,
  a background that does not extend past the 4:3 screen (HNG64 menus) is
  stretched with the rest.

Since mame3d 0.289-2336-g705501b
- Thrill Drive 1 and 2: 16:9 with -widescreen (see Display).
- 16:9 with -widescreen also on Racing Jam 1 and 2, GTI Club, Midnight
  Run, Winding Heat, GTI Club 2, Xtrial Racing, San Francisco Rush, Rush
  2049, Road Burners, California Speed, the PlayStation games, Winning
  Run, Driver's Eyes, Radikal Bikers, Speed Up, Cruis'n USA, Cruis'n World,
  Off Road Challenge, Side by Side 1 and 2 and CART Fury (see Display).
- -widescreen: the 2D over the 3D keeps its shape on Side by Side 1 and 2,
  GTI Club, Midnight Run, Winding Heat, Racing Jam 1 and 2 and Thrill
  Drive 1 (see Display).
- HNG64 in software at -hires3d 3, 5...: repeated bands in the 3D. Fixed.
- Thrill Drive 1: the rear view mirror stayed empty (light blue). Racing
  Jam 1 and 2: a grey band at the bottom of the screen. The Voodoo clipped
  the triangles in the wrong place when the game draws from the bottom of
  the screen. Fixed (also with -hires3d, Vulkan or software).
- Race On!, 500 GP, Motocross Go! (Vulkan): the picture was sometimes one
  frame late, for long stretches, depending on the run: the wait for the
  GPU returned at once and the previous frame was read back. Fixed.

Since mame3d 0.289-2323-g2748796
- FFBPlugin folder: FFB Arcade Plugin 2.1.0.6 (commit 09c2657): it finds
  the wheel again when its GUID was saved with another SDL2 version, shows
  its version, and comes with FFBPluginReadme.md (see below).
- California Speed (Voodoo, Vulkan): the race went differently from
  native. When only the GPU draws, the pixel count that sets the chip
  timing seen by the game is now the same as the native rasterizer's.
  Checked against native over a whole race, x2 and x4.
- Road Burners and Xtrial Racing also race differently from one run to the
  next in native MAME: their clock chip reads the PC time. Not a mame3d
  issue.
- Merge of upstream MAME from 2026-11-12 (8 commits).

Since mame3d 0.289-2257-g2826ec5
- Option -output network: when a game starts, MAME broadcasts
  "mame_start = <game>" then "tcp = 8000" on UDP port 8001 (as Supermodel
  njz3 does), so BackForceFeeder and the FFB plugin find the output server
  by themselves. The TCP server on port 8000 accepts several clients.
- Gunbuster link: the receive buffer was shifted the wrong way after each
  message. Fixed (not tested in game).
- LINK.md in the source tree: how to set up every linked racing game
  (NVRAM, DIP switches, test menus, pitfalls). All linked games tested
  again on one PC with this version, native and Vulkan x2/x4.
- FFBPlugin folder: plugin commit a70875b, which reads MAME's -output
  network.
- Merge of upstream MAME from 2026-11-11 (59 commits).

Since mame3d 0.289-2253-g3f418df
- The high resolution 3D and display options can be set in the Video
  Options menu (general settings and per machine), under "High Resolution
  3D Options". They apply from the next game launched.
- Thrill Drive 2, Xtrial Racing: every other frame was black with Vulkan
  (x2 to x4), so the picture flickered. Fixed.

Since mame3d 0.289-2240-g09f9d69
- Sega Model 1: Vulkan picture handed to bgfx without read-back, and
  -widescreen.
- Namco System 23 in high resolution on Vulkan: Race On!, 500 GP,
  Motocross Go!, with -widescreen.
- PlayStation graphics processor in high resolution on Vulkan: Pocket
  Racer, Kart Duel and the family (see the list above).
- Thrill Drive 1 and 2 and Xtrial Racing: partly tested on Vulkan, from x1
  to x4. Thrill Drive 1, Racing Jam 1 and 2 need their memory initialised
  on the first run (Test button).
- Thrill Drive 2, Xtrial Racing: the picture stayed frozen on the title and
  the mirrors were empty with Vulkan. Fixed.
- GTI Club: the wheel check at boot now passes, without the Test button.
  The motor is simulated during this check only.
- If the graphics card fails, rendering falls back to software or native
  with a message, also for System 23, PlayStation and Voodoo.
- Motocross Go!: crash at boot fixed.

Since mame3d 0.289-2224-g169e786
- San Francisco Rush: the game froze after 49.5 s (every version since
  0.289-1946-g8e81ca4, an upstream MAME bug in the MIPS CPU counter).
  Fixed.
- San Francisco Rush: rare crash at boot, seen on an Intel card, when the
  visible area of the screen changed. Fixed.
- Vulkan picture handed to bgfx without read-back for V-Unit and Zeus 2
  too. At x4 in a window: Cruis'n USA at 441 % speed instead of 287 %,
  Cruis'n World 338 % instead of 245 %, Off Road Challenge 442 % instead of
  294 %, Cruis'n Exotica 306 % instead of 223 %. Little or no gain at x2.
- HNG64: only the 2D lines that change are sent to the graphics card. Fewer
  transfers, no measurable speed gain.
- HNG64 in 16:9: menus fixed with -widescreen_bg and -widescreen_2d
  stretch.
- Tested on Intel UHD as well as NVIDIA. AMD not tested.
- Merge of upstream MAME from 2026-11-08 (12 more commits).

Since mame3d 0.289-2168-g8022e78
- Merge of upstream MAME from 2026-11-08 (53 commits). Nothing new in high
  resolution 3D, link or display.

Since mame3d 0.289-2144-g58552fd
- 3dfx Banshee and Voodoo 3 in high resolution (software and Vulkan): San
  Francisco Rush 2049, Cart Fury.
- Vulkan picture handed to bgfx without read-back (-bgfx_hires3d_share, see
  above). At x4 in a window: Roads Edge at 88.8 % speed instead of 64.5 %,
  Ridge Racer at 100 % instead of 98.3 %.
- Display options -aspect_ratio, -crop_position and 16:9 hack -widescreen,
  -widescreen_2d, -widescreen_bg (see Display).
- 3dfx Voodoo 1/2: the San Francisco Rush wheel test screen was black when
  only the graphics card drew. Fixed.
- Taito JC, Gaelco 3D, HNG64, System 21 and Zeus 2: with Vulkan, the
  graphics card now draws the triangles itself and the CPU no longer
  rasterises them (CPU time roughly halved on Speed Up and Cruis'n
  Exotica). The picture stays that of the software renderer, give or take
  texel edges.
- System 22 / Super System 22: the emulation no longer waits for the
  graphics card every frame (Dirt Dash at x4 in a window: 98.8 % speed
  instead of 95.4 %).
- Taito JC, Gaelco 3D, HNG64, System 21: waiting for the graphics card's
  picture happens during speed throttling (Speed Up x4: 0.6 ms per frame
  instead of 3.1 ms).
- Konami Hornet (Gradius IV, NBA Play By Play, Silent Scope, Teraburst): the
  2D texts and counters are shown in high resolution (they were missing).

Since mame3d 0.289-1971-g1cf6ec6
- Merge of upstream MAME from 2026-11-06 (172 commits). Konami's K033906 is
  now a real PCI bridge: Racing Jam and Hang Pilot, tested after the merge,
  keep their high resolution 3D.

Since MAME njz3 FFB edition 0.289-1946-g8e81ca4
- Konami link (K056230): transmit race fixed (the link locked up or sent
  duplicate bytes: NETWORK ERROR on Midnight Run and GTI Club); no more
  endless connection loop when the neighbouring cabinet is unreachable or
  after a warm reset.
- Namco link (C139): same fixes to the connection loop and the queues.
- Capcom link (CPS2): the USART buffer is no longer emptied by the network
  thread.

Based on upstream MAME from 2026-11-11, with:

- Option -write_directory / -writedir <folder> (or the write_directory key
  in mame.ini): everything MAME writes (mame.ini, ui.ini, plugin.ini, game
  ini files, cfg, nvram, sta, snap, inp, diff, plugin data, error.log) goes
  into this folder, for cabinets protected by a write filter (UWF). The
  original locations are still read until a file has been saved in this
  folder.

- Virtua Racing: emulation of the 838-9081 motor board (outputs
  drive:drive_motor1-2 and drive:drive_digit0-3). Needs the ROM
  epr-15111.12 (CRC 45c02763), missing from the official sets: put it in
  vr.zip or in sega_838_9081.zip.

- Link between cabinets (options -comm_localhost, -comm_localport,
  -comm_remotehost, -comm_remoteport; ring: each cabinet listens on
  localport and connects to the next one): SailorSat's PRs not merged
  upstream (#13852, #13853, #13861, #13862, #13863, #13865, #13866), then
  the rewritten Namco C139.
    m1comm, m2comm, s32comm : rewritten with ASIO (non-blocking)
    xbdcomm   : Super Monaco GP, Last Survivor
    ybdcomm   : Power Drift Link Version (set the DIP switches)
    K056230   : GTI Club, Jet Wave, Midnight Run, Winding Heat,
                Polygonet Commanders, Poly-Net Warriors
    C139      : Namco System 2, 21, 22 and Super System 22, rewritten from
                the games' link code. Tested linked with 2 and 4 cabinets:
                Ridge Racer 2, Rave Racer, Ace Driver, Victory Lap, Winning
                Run 91, Winning Run Suzuka GP, Final Lap 1/2/3, Four Trax,
                Suzuka 8 Hours 1/2, Dirt Fox, Cyber Cycles, Armadillo
                Racing; Alpine Racer 2 with 2 cabinets (its maximum). Each
                cabinet keeps its cabinet number/link ID in its NVRAM (the
                game's test menu). Ridge Racer, Dirt Dash, Air Combat and
                Alpine Surfer have no linked play.

- Endprodukt's force feedback outputs (wheel_motor): Cruis'n Exotica,
  Model 2 (including Touring Car and Over Rev), Konami Hornet, Gaelco Speed
  Up, Driver's Edge. Outputs pcboutput0 / mcuoutput0-2 (GTI Club, Midnight
  Run, System 22) and digit0-6 of Suzuka 8 Hours (SailorSat).

- "No-nag": the driver warning screen is no longer shown at start-up (the
  game information screen is still controlled by -skip_gameinfo).

- Midnight Run, Winding Heat (Konami ZR107): no more crash when quitting
  MAME after more than 10 s of play (registers XMM6-XMM15 of the x64
  recompiler preserved).
- Side by Side 1 and 2 (Taito JC): races at 30 frames/s instead of 8-10
  (68040 FPU timings and skipping the DSP busy wait).

Known issues
- pong (and probably other netlist games) crashes with this MSVC build.
- -validate reports about 20,600 badly encoded key labels, on home computer
  keyboards and terminals (cause not found). Racing games are not affected.
- The random 0xC0000005 crashes when quitting MAME (srallyc, scud,
  orunners) came from FFB Arcade Plugin 2.0.0.53 (DINPUT8.dll): see the
  FFBPlugin folder, which holds a fixed version.

FFBPlugin folder (FFB Arcade Plugin, njz3 fork)
Same content as FFBArcadePlugin-2.1.0.6-g09c2657-njz3-windows-x64.zip
(github.com/njz3/FFBArcadePlugin, branch dev/mame-racer, commit 09c2657).
- dinput8.dll: FFB Arcade Plugin 2.1.0.6 (njz3 fork), Release x64:
    - Endprodukt's MAME racing games ported onto the refactored plugin;
    - fix of the nameFFB pointer to a freed string (crash when quitting
      MAME, memory leak);
    - static C/C++ runtime: the Visual C++ redistributable is no longer
      needed;
    - also reads the network outputs (MAME -output network, Supermodel
      -outputs=net, Flycast): TCP port 8000, or the port announced on UDP
      port 8001 (opened shared, so BackForceFeeder can listen too).
      MAMENetworkOutputs=0 in FFBPlugin.ini turns them off;
    - finds the wheel or vJoy again when its DeviceGUID was saved with
      another SDL2 version (SDL 2.32.8 puts a CRC in bytes 2-3 of the
      GUID, so older ini files no longer matched);
    - its version (2.1.0.6-g09c2657) is in the file properties and on the
      first line of FFBlog.txt, with the SDL version.
  Only depends on SDL2.dll and Windows DLLs.
- FFBPluginGUI.exe + MetroFramework*.dll: the configuration program. It
  lists wheels and vJoy again on the MAME, Demul and FFB Test pages, and
  shows its version in its title (and the version of the dinput8.dll next
  to it when it differs).
  Needs .NET Framework 4.7.2 and the Visual C++ 2015-2022 x64
  redistributable (C++/CLI).
- SDL2.dll (SDL 2.32.8), MAME64.dll: as supplied with the plugin.
- FFBPluginReadme.md: the plugin guide (Markdown; it replaces
  FFBPluginReadme.txt), with the list of MAME games: those that work as
  they are, those that need a setting, those that do not work yet. Games
  that need MAME njz3 are marked.
- FFBPlugin.ini: "MAME 64bit Outputs" profile (GameId=22), for a new
  install only.
- On an existing install, replace DINPUT8.dll, SDL2.dll and
  FFBPluginGUI.exe, add FFBPluginReadme.md (FFBPluginReadme.txt can be
  deleted); keep FFBPlugin.ini and the other configuration files in place:
  the saved wheel GUID works again.
- Settings some MAME games need for force feedback (Virtua Racing
  cabinet type, Rave Racer feedback steering in each cabinet's NVRAM,
  Cruis'n Exotica, Over Rev, Super Monaco GP, Super GT 24h): see "Games
  that need a setting" in FFBPlugin\FFBPluginReadme.md.
