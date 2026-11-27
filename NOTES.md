# mame3d — progress log

Branch `dev/mame3d` (part of `dev/ffbedition`). Goal: render the 3D of
1990-2010 racing games at any resolution, first in software, then on the
graphics card (Vulkan), while keeping MAME's emulation of everything else: CPU,
DSP, geometry, sound, I/O, link and 2D.

The Vulkan renderer is not a bonus: the goal is also to put the graphics card
to work and relieve the CPU of the machine that runs MAME. So for each chip we
translate its 3D commands (triangles, blend states, depth, textures) into
Vulkan calls. The HD software renderer is the reference for checking the
image, and the fallback without Vulkan or for untranslated cases.

## Scope

Excluded, because other emulators already render them natively in 3D:

- Sega Model 2: sm2-emu
- Sega Model 3: Supermodel
- Naomi, Naomi 2, Atomiswave (PowerVR2): Flycast
- Hikaru: hikaruloader

Sega Model 1 (vr, vformula, ...) is tracked again (owner's decision,
2026-11-10): it has its high-resolution Vulkan renderer; sharing with bgfx and
16:9 remain, MAME doing the 3D projection there.

Target hardware, in order:

| # | Hardware | Games | HD software renderer | Vulkan renderer |
|---|---|---|---|---|
| 1 | Namco System 22 / Super 22 | ridgerac, ridgera2, raverace, acedrive, victlap, cybrcycc, dirtdash, alpinerd | OK (ridgerac, dirtdash ×2) | OK (5 games ×2); faster than native |
| 2 | Konami K001005 | gticlub, midnrun, windheat, jetwave | OK (gticlub, midnrun ×2) | OK (midnrun, gticlub); close to native |
| 3 | 3dfx Voodoo 1/2 | sfrush, sfrushrk, calspeed, hyprdriv, vaportrx, roadburn, thrilld, racingj, racingj2 | OK (sfrush ×2 ×4, racingj ×2, roadburn ×2); CPU-heavy | OK (same image as software); on par with native at ×2 on roadburn |
| 3b | 3dfx Banshee / Voodoo 3 | sf2049, sf2049se, cartfury (and viper, nbashowt… not tried) | OK (sf2049, cartfury ×2) | OK (same image as software; ×4 sf2049 130 s in 65 s) |
| 4 | Midway V-Unit | crusnusa, crusnwld, offroadc | OK (3 games ×2 in 3D demo; integer vertices: native edges, HD textures) | OK (same image as software); ×4 60 s vs 81 s in software |
| 5 | Midway Zeus 2 | crusnexo | OK (crusnexo ×2, ×4, demo mode) | OK, real triangles, no more CPU raster; CPU time ×2 52 s vs 92 s native |
| 6 | Namco System 21 | winrun, driveyes | OK (winrun, driveyes ×2) | OK, real quads, no more CPU raster; winrun, CPU time ×4 14.5 s vs 23 s native |
| 7 | Taito JC (TC0780FPA) | sidebs, sidebs2, dangcurv | OK (sidebs, sidebs2 ×2) | OK, real triangles and quads, no more CPU raster; CPU time ×4 84.5 s vs 86 s native |
| 8 | Gaelco 3D | speedup, surfplnt, radikalb | OK (speedup, radikalb ×2) | OK, real triangles, no more CPU raster; CPU time ×2 18 s vs 52 s native |
| 9 | SNK HNG64 | roadedge, xrally | OK (roadedge, xrally ×2) | OK, real triangles, no more CPU raster, 2D on top blended on the GPU; CPU time ×4 75 s vs 88 s native |
| — | Namco System 23 | motoxgo, raceon, finfurl | when the base emulation works | |

## Architecture

1. **High-resolution output**: `src/emu/hires3d.{h,cpp}`, with the option
   `-hires3d N` (1 to 16, 1 = native).
   - The driver draws the image into `frame_bitmap()` (native size × N), then
     calls `frame_done()`.
   - `screen_device::video_output_update()` then hands this image to the
     display backend instead of the native image.
   - Emulation, synchronisation and partial updates do not change.
   - `screen.h` is not modified, which avoids a full rebuild.
2. **2D**: each driver draws its tilemaps and sprites at native resolution.
   `upscale()` enlarges them (nearest neighbour) into the HD image, skipping
   transparent pixels (alpha 0) for layers above the 3D.
3. **HD software 3D**: each chip's rasterizer draws directly into the HD image,
   with vertices scaled from floating-point coordinates kept before rounding to
   the native pixel.
4. **Vulkan 3D**: `src/emu/vk3d.{h,cpp}`, with the option
   `-hires3d_renderer auto|vulkan|software` (default: auto).
   - Offscreen rendering in the `bitmap_rgb32` pixel format (B8G8R8A8), with a
     D32 depth buffer. The driver records batches of triangles in HD pixels,
     then `end_frame()` renders the image, reads it back into the HD image and
     follows the same path as the software renderer. This works with all
     display backends, snapshots and video recording.
   - `vulkan-1.dll` is loaded at startup by `osd::dynamic_module`. Without
     Vulkan, `hires3d_output::vulkan()` returns nullptr and the driver draws in
     software. A failure during a game also falls back to software.
   - Pipelines provided: `PIPE_COLOR`, `PIPE_COLOR_STIPPLE` (checkerboard,
     cell size in `p`), `PIPE_TEXTURE_KEY` (texture, alpha 0 transparent). A
     driver can add its own with `add_pipeline()` (SPIR-V, blending, depth).
   - The Vulkan headers come from
     `3rdparty/bgfx/3rdparty/khronos/vulkan-local`.
   - Shaders: `src/emu/vk3d_shaders/*.vert|frag`, compiled to SPIR-V into
     `.spv.inc` files, which are versioned:

     ```
     glslc --target-env=vulkan1.0 -O -mfmt=num -o X.spv.inc X
     ```

     (`C:\VulkanSDK\1.4.363.0\Bin\glslc.exe`)
   - With bgfx on Direct3D 11, the image is no longer read back: see item 7.
5. **Common Vulkan context**: `src/emu/vk3d_context.{h,cpp}` loads Vulkan,
   creates the device (with the requested features the GPU has) and provides
   the buffer, image and shader helpers. `vk3d_renderer` and the chip-specific
   renderers inherit from it.
6. **Chip-specific GPU renderers**: when the "triangle batches" model of
   `vk3d_renderer` is not enough, the chip has its own renderer that
   translates its commands. First case: `src/devices/video/voodoo_vk.{h,cpp}`
   (see the Voodoo log). Its shaders are in `src/devices/video/voodoo_vk_shaders`
   and are compiled with `-I .` for `layout.h`, shared with the C++:

   ```
   glslc --target-env=vulkan1.0 -O -mfmt=num -I . -o X.spv.inc X
   ```

7. **Image shared with bgfx** (`-bgfx_hires3d_share`, on by default): the
   finished image stays on the GPU.
   - `src/osd/modules/render/bgfx/sharedtexture.{h,cpp}` creates Direct3D 11
     textures shared by NT handle and substitutes them for the bgfx textures
     (`overrideInternal`), as the screen texture of the chains or of the
     simple quad.
   - `src/emu/vk3d_share.{h,cpp}` imports them into each chip's Vulkan device
     (`VK_KHR_external_memory_win32`, Vulkan 1.1, same adapter checked by the
     LUID) and copies the finished image into them instead of reading it back.
   - `hires3d_output::use_frame()` receives the texture; bgfx draws it, and
     read-back is now only done for snapshots and recording
     (`read_gpu_textures()` in `video_manager::create_snapshot_bitmap`).
   - bgfx then runs on the emulation thread (`bgfx::renderFrame()` before
     `bgfx::init()`): its D3D11 device is created single-threaded, and the
     textures are created and tracked from the emulation. With the bgfx render
     thread, the NVIDIA driver crashed or hung.
   - Synchronisation by the CPU: the chip waits for its batch before bgfx
     draws (`ready` callback), and only rewrites a texture once the draws that
     read it are finished (ring of D3D11 queries; otherwise the image is read
     back).
   - Wired up on System 22, System 21, Taito JC, HNG64, Gaelco 3D, K001005 and
     Voodoo (1/2, Banshee, Voodoo 3), V-Unit and Zeus 2 (2026-11-09). Not yet:
     Model 1.
8. **Display options**, as in the other emulators on the cabinets
   (`render.cpp`, `place_view()`):
   - `-aspect_ratio original|4:3|16:9|4:3_stretch|16:9_stretch|4:3_crop|16:9_crop`
     (`4:3_letterbox` = `4:3`): the frame the screen is shown in, centred with
     black bars; the screen keeps its shape in it, fills it, or covers it and
     is cropped. `-crop_position -100..100` picks the part that is kept.
   - `-widescreen`: 16:9 image for the chips where MAME does the 3D projection
     (System 22, HNG64, Zeus 2), on the Vulkan renderer only, even at
     `-hires3d 1`. `hires3d_output` adds `pad()` HD columns on each side of the
     native screen, and the screen is shown in 16:9 (`view_widening()`).
   - `-widescreen_2d 4:3|stretch`: the 2D layers in the middle at their shape,
     or stretched over the full width.
   - `-widescreen_bg`: the tilemaps below the 3D also drawn on the sides
     (HNG64; System 22 has none, its background is the background colour or
     the 3D).

## Build

- Reduced build to iterate fast (separate `build3d` folder, exe `mame3d.exe`):

  ```
  make vs2022 BUILDDIR=build3d SUBTARGET=3d SOURCES=src/mame/sega/model1.cpp,...  PTR64=1 NOWERROR=1 PYTHON_EXECUTABLE=<python Windows>
  msbuild build3d\projects\windows\mame3d\vs2022\mame3d.sln /p:Configuration=Release /p:Platform=x64 /m:2 /p:CL_MPCount=1 /p:VcpkgAutoLink=false
  ```

  `SOURCES` = model1, namcos21, namcos21_de, namcos22, namcos23, gticlub,
  zr107, nwk-tr, seattle, vegas, midvunit, midzeus, taitojc, gaelco3d, hng64.
- The full build (`build\...\mame.sln`) remains the one for releases.

## ROMs

Copy only what is needed, as we go, from
`\\192.168.1.100\toshiba2t\MAME ROMs (merged)` (and `MAME CHDs (merged)`) to
`C:\Users\benja\Desktop\Perso\Racecab\roms`, on this PC only, never on the
cabinets. The `vr.zip` already there contains vformula and epr-15111.12: do
not overwrite it.

## Log

- 2026-10-31: branch `dev/mame3d` created. Inventory of 3D chips done. Added
  the `hires3d` infrastructure and the `-hires3d` option. Model 1:
  - HD software renderer: projected positions kept as floats, viewport
    clipping scaled, wireframes one native pixel thick, HUD enlarged and HUD
    mask computed on the HD image;
  - Vulkan renderer: background and HUD as textures, quads as triangle pairs
    in sorted order, checkerboard in a shader, "above the HUD" pass in
    software on the read-back image.
- 2026-10-31: HD software renderer written (compiled, not yet tested) for
  three hardware platforms.
  - **Namco System 22 / Super 22**: vertices and clipping scaled for polygons
    and sprites. They are drawn with their own HD priority map. The text stays
    native: its priority marks are copied into the HD map, then blending
    (shadow, spot, fade, gamma) is done per HD pixel.
  - **Konami K001005**: K001005 buffers enlarged × N (option read at device
    startup). `draw(bitmap, clip, scale)` downsamples for the native screen or
    copies 1:1 in HD. On the driver side (GTI Club, Midnight Run, Winding
    Heat, Jet Wave), the background tilemap is enlarged, the 3D copied in HD,
    the front tilemap enlarged with transparency.
  - **Midway V-Unit**: the native video memory is kept for the emulation. An
    HD copy of the two pages receives the same quads (native pixel centres →
    HD block centres) and the CPU writes enlarged into blocks. The HD display
    converts this copy through the palette. The vertices supplied by the game
    are integers: the edges stay native, but textures and dithering are
    computed per HD pixel.
- ROMs copied: ridgerac, raverace, acedrive, dirtdash, alpinerd, namcoc71,
  namcoc74, gticlub, midnrun, windheat, crusnusa, crusnwld, tms320c31.
- 2026-10-31: first tests with `3d.exe` (build `build3d`).
  - **Model 1 (vr)**: ×4 software and ×4 Vulkan give the same image at
    1984×1536.
    - Bug fixed: in Vulkan, small distant quads reduced to two points in
      native were drawn as wireframes. The test is now done on the HD
      positions, as in software.
    - The HUD mask ("above the HUD") is only computed if the display list
      contains 0x41 objects.
    - `upscale()` enlarges one line then copies it.
    - Time for 25 emulated s (`-nothrottle`, laptop RTX 3050): native about
      19.5 s, software ×4 25.5 s. In Vulkan ×4, waiting for the GPU and the
      transfer take 6.3 ms per frame, the copy 1.9 ms: the GPU must be made
      asynchronous, or the image passed to the display without read-back.
  - **System 22**: ridgerac and dirtdash correct at ×2.
  - **K001005**: gticlub and midnrun correct at ×2.
  - **V-Unit**: there were two bugs.
    - The game does its fades through the palette only. So the HD path
      converts the page at each update and always reports a change.
    - On an indexed screen, `palette.pens()` returns colour numbers, not
      colours: `palette()->entry_list_adjusted()` must be read. This also
      applies to System 21, Taito JC and HNG64.
    - Cruis'n USA starts on the calibration and the NVRAM is empty; a
      calibrated NVRAM will be needed to check the 3D in demo.
  - Bug unrelated to mame3d, also present in the `mame.exe` of
    `dev/ffbedition`: midnrun and windheat (zr107 driver) crashed on exit
    after more than 10 s. **Fixed** by commit 60f3007783b on `dev/ffbedition`,
    merged into `dev/mame3d`.
    - Cause: on Windows, the x64 DRC keeps its UML float registers in
      XMM6-XMM15 but did not save them at the entry point.
    - MSVC keeps a constant in XMM6 during `machine.run()`; it came back
      corrupted, hence an invalid empty string in `set_system_name("")`.
    - The bug also exists in upstream MAME.

## Next steps

All chips in the list have an HD software renderer and a Vulkan renderer
(2026-11-02). Remaining:

1. Try sharing with bgfx on an AMD or Intel card. Measurements on the laptop
   RTX 3050, at ×4, window without speed limit: crusnusa 441 % vs 287 % with
   read-back, crusnwld 338 vs 245, offroadc 442 vs 294, crusnexo 306 vs 223.
   The gain is small or nil at ×2. Intel UHD of this PC (2026-11-09, Vulkan
   and bgfx forced on it): crusnexo, crusnwld, xrally, ridgerac, midnrun,
   sf2049, winrun, radikalb and sidebs run, images identical to NVIDIA
   (0.13 % at worst on sf2049). Rare crash at sfrush startup fixed: the
   faulting address was 184 pixels before a read-back image, the offset of
   the native `xoffs`. If the visible area changes between preparation and
   display, `update_texture` computed the current area; it now takes the
   image's. The case has not reappeared over 42 launches, with no crash, so
   the fix has not been exercised directly. `frames_freed` also copies a
   shown image whose memory is freed. Isolated black snapshots of ridgerac
   and radikalb, no black image in the in-game snapshots. AMD not tested.
2. HNG64: the 2D lines are kept on the GPU, only those that change are sent
   (done, 2026-11-09: 23 to 33 % of the lines, upload time divided by 2.4,
   wait -15 %, identical images; no measurable speed gain). xrally race at
   ×2, without display: 5.1 ms wait per frame before this change (read-back
   47 %, uploads 30 %, drawing 22 %). The K001005 (midnrun) only waits 0.5 ms
   per frame: nothing to do.
   `-widescreen_bg` with `-widescreen_2d stretch`: the layers below the 3D are
   stretched when they do not extend beyond the 4:3 screen (roadedge and
   xrally menus), and stay in place otherwise (race).
   **MIPS fix**: sfrush froze after 49.5 s (all our versions since g8e81ca4).
   Upstream commit fde5b471bfc arms the compare on a write to Count; Compare
   stays at 0xffffffff, the game never writes to it and IP7 stays raised when
   Count wraps. The arming is removed (`mips3.cpp`, `mips3drc.cpp`). sfrush
   continues past 50 s, in demo and with coin and accelerator; xrally is
   identical over 45 s and continues past 50 s, roadedge and cartfury too;
   sf2049 continues (its demo is not deterministic, even on the old version).
   dev/ffbedition has the same bug.
3. Banshee/Voodoo 3: sf2049se tried up to the race, correct (2026-11-09).
   Non-racing games remain (Viper, nbashowt); 2D blits between buffers lose
   the HD (no blit reading a buffer over 100 s of sf2049 and cartfury).
   Voodoo: the native rasterizer still runs during the game, restarted by CPU
   reads (over 100 s: sfrush fbiPixelsIn ×2151 and buffer ×4122, i.e.
   2.4 cores; sf2049 the 5 stats registers ×528, 1 core; cartfury buffer
   ×945, 0.4 core). To stop it: count the pixels on the GPU and read back the
   areas that are read. Not needed for speed (177 to 376 % at ×2).
   Zeus 2: no more measurable software fallback (crusnexo).
   To support (owner's request, 2026-11-10): thrilld (Hornet, Voodoo) and, on
   Viper (Voodoo 3), thrild2 and xtrial. All "not working" in MAME: first see
   what blocks them; ROMs not yet copied, Viper never tried.
4. System 23, Taito Type Zero and Konami Cobra, when their emulation works.
   Requested by the owner (2026-11-10): the PS1 GPU (`psxgpu`), for the
   PlayStation-derived systems (Namco System 11: pocketrc, System 12:
   kartduel, ...). Integer vertices: more resolution, but the polygons still
   wobble.
5. HNG64 in software at scales that are not powers of 2 (×3, ×5…): replace
   the `size - 1` masks of the rasterizers and of `blit_3d` (the GPU path is
   already fixed).
- 2026-10-31: **Namco System 21** in HD software.
  - `namcos21_3d`: buffers × N, (integer) vertices scaled, which sharpens the
    edges. A native copy of the visible depth remains available for Cyber
    Sled's sprite blending.
  - Winning Run: indexed blending at HD resolution. `bitmap_draw` reads the
    native 2D for each HD pixel, shadows included, then the conversion goes
    through the adjusted palette (`hires3d_output::indexed_to_frame`).
  - Driver's Eyes: sprites and blending in native, 3D copied in HD. Only the
    pixels that priority-15 sprites modify (shadows on the 3D) are carried
    over in blocks.
  - ROMs copied: winrun, driveyes, namcoc65, namcoc68.
- 2026-10-31: **Taito JC** in HD software: TC0780FPA buffers × N, (integer)
  vertices scaled. Indexed blending: background objects enlarged, HD 3D,
  foreground objects and text enlarged with the sentinel value 0xffff as
  transparency.
- 2026-10-31: **Side by Side speed**, fixed on `dev/ffbedition` (efc1acdba1e,
  ba474f170be) then merged here.
  - Before: the demo race ran at 8-10 3D frames/s.
  - 68040-specific FPU timings in the 68k core: about 20 frames/s.
  - 68040 clock × 1.5: stable 30 frames/s (one frame in two).
  - Idle skip on the 68040 wait loop, which polls the DSP status word:
    unthrottled speed from 102 % to 144 % (sidebs) on the laptop.
- 2026-10-31: **Gaelco 3D** in HD software. The DSP supplies per-pixel plane
  equations (u/z, v/z, 1/z): their gradients are divided by N, the vertices
  multiplied by N, the frame buffer and the z-buffer are enlarged × N. The
  buffer is stored upside down; it is flipped when copying to the HD image
  and sampled for the native screen.
- 2026-10-31: **SNK HNG64** in HD software.
  - 3D buffers (index, light, blend bit) of 512 × N per side; vertices and
    clipping scaled.
  - `blit_3d()` is used for the native screen (sampled) and for the HD image
    (vertical stretch and x scroll scaled, 50 % blending).
  - Background layers enlarged. Foreground layers blended in native; only the
    pixels they change are carried over in blocks.
  - Frames without 3D (screen off, transitions) are published enlarged.
  - `hng64.zip` (BIOS) replaced by the one from the share, which adds three
    configuration PROMs (same CRCs for the rest).
- 2026-10-31: **Midway Zeus 2** in HD software.
  - HD copy of the colour and depth buffer, with the same line structure (512
    or 1024 pixels depending on `m_yScale`) × N.
  - The rasterizer writes through a window start pointer and a line length,
    which serves the native buffer as well as the HD buffer.
  - Polygons drawn in both, vertices and clipping × N.
  - Fast fill and `frame_write` copied in blocks; the HD display reads the
    copy.
  - Test: Cruis'n Exotica, calibration passed by script (5 presses of
    "Service Mode"), 3D demo mode correct at ×2.
  - ROMs copied: crusnexo, tms320c32.
- 2026-10-31: **3dfx Voodoo 1/2** in HD software (Banshee and Voodoo 3 not
  concerned).
  - HD copy of the whole frame memory: a line of `rowpixels` native pixels
    becomes N lines of `rowpixels` × N pixels, with a margin on each side. The
    colour and depth buffers keep their address in it.
  - Line length and Y origin passed per polygon (`poly_data`) instead of the
    rasterizer's global state, to draw each triangle a second time in the HD
    copy.
  - HD triangle: vertices and clipping × N, gradients ÷ N, start values
    recentred on the HD pixel when sub-pixel correction is active. A triangle
    that would go outside the HD copy is not drawn in it.
  - Fast fills redone in HD. LFB writes (raw or through the pipeline) and the
    Voodoo 2 blit fill copied in blocks.
  - HD display read from the copy of the front buffer. `hires()` lets the
    driver carry over the pixels its 2D changes: NWK-TR and Hang Pilot
    (K001604) do it, with the new `hires3d_output::magnify_changes()`.
  - Tests: sfrush ×2 and ×4, racingj ×2 (K001604 text on top), roadburn ×2
    (Voodoo 2): correct images.
  - High CPU cost: 110 emulated s of racingj in 109 s native and 210 s at ×2,
    80 s of roadburn in 85 s native and 232 s at ×2. Hence the priority on the
    Vulkan renderer for this chip.
  - ROMs copied: sfrush (+ CHD), racingj, roadburn (+ CHD).
- 2026-10-31: **3dfx Voodoo 1/2 on Vulkan** (`voodoo_vk`), chosen by default
  when Vulkan is available (`-hires3d_renderer software` keeps the HD software
  renderer).
  - The Voodoo pixel pipeline runs in a fragment shader (`tri.frag`). It
    recomputes each pixel's iterated values with the same integer arithmetic
    as the software rasterizer (64-bit integers for W and textures), and reads
    the texels directly from the TMU memory, copied to the GPU page by page
    when it changes. So there is no texture cache. The texel tables, palettes
    and fog tables are in a lookup buffer. Depth test, blending ("colour
    before fog" through dual source) and pixel order go through the GPU's
    fixed-function stages.
  - One image per colour buffer and one depth image, × N. Fast fills and
    blits as clears, LFB writes in blocks.
  - Batches sent to the GPU without waiting (every 256 triangles), on a ring
    of 3. The display waits for the current batch to finish and reads back the
    front buffer. The colour table (CLUT) is applied during read-back, unless
    it is linear.
  - When Vulkan draws, the native rasterizer is skipped, except for 2 seconds
    after a CPU read of the frame buffer (sfrush does some at startup,
    roadburn too). The Voodoo's emulated time then comes from the triangle
    area: the demo may drift a little compared to native rendering, but the
    image of a given scene is the same.
  - A Voodoo that is not displayed does not draw its HD copy (Racing Jam has
    two).
  - Type 5 CMDFIFO packets that write directly into the Voodoo 2 frame memory
    are copied in HD (software and Vulkan).
  - Check: mean difference from HD software of 0 to 1.6 out of 255, no pixel
    beyond 24 (0.01 % on roadburn). The differences come from the 565
    dithering that the GPU does not do.
  - Times on the laptop (RTX 3050), ×2, very variable from one run to the
    next: racingj 110 emulated s in 127 s (software 194 s, native 109 s),
    roadburn 30 s in 21.7 s (native 14 s), sfrush 60 s in 22.7 s (native
    18 s). The GPU wait follows the pixel count (3.4 ms per frame at ×2,
    10.6 ms at ×4 on roadburn): it is mostly the image read-back and its copy
    into the bitmap.
  - Next: read-back without waiting and direct image (next entry).
- 2026-10-31: **Voodoo on Vulkan, read-back without waiting and direct image.**
  - At vblank, MAME shows the buffer that became the front buffer at the
    previous swap. So right after each swap, the read-back of the new front
    buffer starts on the GPU without waiting, and it has a whole frame to
    arrive. No frame of delay: the image shown is the same as before.
  - Read-backs go into a ring of 3 buffers. The display shows the read-back
    memory directly, without a copy, thanks to the new
    `hires3d_output::use_frame()`. The read-back being shown is not
    overwritten before the next frame.
  - Drivers that draw their 2D on top (NWK-TR, Hang Pilot, via `hires()`)
    keep a copy, so no old text is left in the read-back memory.
  - The DAC CLUT, when it is not linear (roadburn), is applied on the GPU by
    a full-screen pass into a display image, which is read back instead of
    the color buffer.
  - When only the GPU draws, the native buffer is no longer converted for the
    native screen (it is neither drawn nor shown). The estimated pixel count
    for the Voodoo timing takes the clip rectangle into account: without it,
    large off-screen triangles made the chip look busier and the CPU spun
    more on its state.
  - roadburn, 30 s emulated, best of 3 alternating runs: before this step
    16.5 s native vs 20.3 s Vulkan ×2; after, 19.7 s vs 19.8 s (the laptop
    heats up, hence the spread). GPU wait: from 3.5 s to 0.02 s, all frames
    shown without a copy.
  - Left: share the image with the display (bgfx) to remove the read-back
    itself.
- 2026-10-31: **Namco System 22 / Super 22 on Vulkan** (`namcos22_vk`), same
  method as the Voodoo.
  - The walk over the sorted scene sends each quad (fan of 3 to 6 vertices
    after z clipping) and each sprite to the GPU, with what the per-line
    render functions read from their data: palette, color mode, texture
    bank, shading, fog, fades, alpha, priority. The GPU draws them in sort
    order, without a depth buffer, like the hardware.
  - `scene.frag` ports these functions (one for System 22, one for Super 22,
    one for sprites). It reads textures (texture tilemap, attributes, tiles)
    and sprites from the ROMs, uploaded to the GPU only once. Blending with
    the destination goes through the fixed-function stage. A second output
    (R8 priority image) marks covered pixels and the "above text" bit of the
    last polygon.
  - `mix.frag` ports `namcos22_mix_text_layer` (shadows, fade, PROM gamma)
    and `namcos22s_mix_text_layer` (spot, text alpha, fade, mixer gamma),
    with the native text layer and its priority map, uploaded every frame.
    On Super 22, the text is also mixed into the background before the
    scene, as in the software renderer.
  - Image read back and shown without a copy (ring of 3), like the Voodoo.
    Rendering happens at display time and we wait for the GPU (no buffer
    swap to anticipate on this hardware).
  - `independentBlend` is required (color blended, priority written as is).
  - Check against software HD, same frame: ridgerac 2.5% of pixels above 24,
    dirtdash 0.14%, raverace 3.9%, acedrive 1.2%, alpinerd 1.6%. The
    differences are texel edges shifted by one pixel (float interpolation on
    the GPU, double on the CPU).
  - Times, ×2: ridgerac 40 s emulated in 12.2 s (software 42.9 s, native
    14.8 s); dirtdash 60 s in 25.4 s (software 84.3 s, native 34.3 s);
    raverace 29.2 s (software 101 s); acedrive 34.5 s (75 s); alpinerd 31 s
    (100 s). The GPU beats the native renderer, which rasterizes on the CPU.
- 2026-10-31: **Konami K001005 on Vulkan** (`k001005_vk`).
  - The K001005 sends its triangles to the GPU instead of rasterizing them.
    The `tri.frag` shader ports `draw_scanline_generic`: texels from the
    K001006 ROM (uploaded once) and its palette (sent with each batch),
    mirroring, bilinear, diffuse and ambient light, fog, per-vertex or
    per-polygon color.
  - The hardware's float z-buffer becomes the GPU depth: Z is multiplied by
    2^-40, which does not change its order as a float, with `depthClamp`
    since the chip does not clip in Z.
  - The blends with the image (semi-transparent bilinear texture, then fog,
    then polygon blend) are all linear in the destination color. So the
    shader computes "A + K × destination" and the GPU dual source does the
    addition.
  - Two pages and a shared z-buffer; the page swap becomes a clear. Batches
    are sent without waiting (every 1024 triangles and at each swap).
  - On HD display, GTI Club, Jet Wave and Midnight Run / Winding Heat have
    the GPU compose the screen: background 2D layer (native), opaque pixels
    of the 3D page, top 2D layer (native). The read-back image is shown
    without a copy. Composition submits only its own pass, so it does not
    wait for the triangles of the current page. When nothing has changed
    (same page, same layers), the previous image is shown again.
  - Check: midnrun identical to software HD (0.02% of pixels above 24);
    gticlub identical on its 2D screens, with the screen offset (40, 28).
    Its 3D demo was not reached: pressing Test to get past "DEVICE ERROR
    (STEERING WHEEL)" opens the test menu.
  - Times, midnrun 60 s emulated at ×2: 14.3 to 17.1 s in Vulkan, 12.7 s
    native, 27.5 s software. About 1 ms of wait remains per composed frame
    (composition and read-back), over about 1500 composed frames.
- 2026-11-02: **Midway V-Unit on Vulkan** (`midvunit_vk`).
  - The HD copy of the two video memory pages is on the GPU (R16 images of
    512·N per side). The quads from the DMA queue are drawn into it by a port
    of the per-line rasterizers (flat, textured, transparent textured, masked
    textured). Since `poly_manager::render_polygon<4>` interpolates per line
    along the left and right edges (non-planar quads), the fragment shader
    re-evaluates these edges at its line. It also ports the rounding, the
    clip applied before computing the parameters, the integer texture
    coordinates and the dithering.
  - CPU writes to video memory are grouped into segments of neighboring
    pixels, drawn as N×N blocks. A write to texture memory only submits the
    current batch if it touches the texture window that batch uses.
  - On page change, the visible page is converted right away through the
    palette into a display image, read back without waiting (3 buffers). On
    screen update, this image is shown without a copy if the page and the
    palette have not changed; otherwise the lines are converted in drawing
    order, which keeps the palette fades.
  - After a state load, both pages are rebuilt from native memory; if Vulkan
    fails, rendering falls back to software HD.
  - Calibrated NVRAMs for crusnusa and crusnwld (Enter, i.e. Test, at the
    "CALIBRATE CONTROLS" screens): both games reach the 3D demo. ROM copied:
    offroadc.
  - Check at ×2, 120 s of demo: crusnwld and offroadc identical to software
    on 12 captures; crusnusa identical on 7 captures, within 4 pixels on one
    line (float rounding).
  - Times for 120 s emulated (laptop, fairly scattered measurements):
    crusnusa ×2 43.5 to 58.9 s in Vulkan, 53.2 s software, 38.8 to 52 s
    native; crusnusa ×4 60.2 s in Vulkan, 81.1 s software, native 50.3 and
    52.2 s measured just before and after; crusnwld ×2 59.2 s in Vulkan,
    79.4 s software, 58.7 s native; offroadc ×2 46.3 s in Vulkan, 57.8 s
    software. Rendering costs about 3 s of CPU over 120 s, of which about
    1 s is GPU wait, mostly for images converted at update instead of at
    page change.
- 2026-11-02: **Midway Zeus 2 on Vulkan** (`zeus2_vk`).
  - The HD copy of the frame buffer (color and depth, with the native line
    structure × N) is on the GPU, and wave memory (16 MB, textures) is copied
    there in 16 KB pages written since the previous batch.
  - Each triangle from `render_triangle_fan` is prepared on the CPU the way
    `render_triangle` does it (Y sort, covered lines, slopes, parameter
    planes). The fragment shader derives from it the extent of its line, the
    integer depth (truncated start then integer steps), u/z, v/z and 1/z,
    then applies `render_poly_8bit`: depth floor, solid color, RGB555, texels
    with alpha, palette texels with the bilinear filter of `rgbaint_t` (SSE
    version) and the transparent color.
  - The 24-bit depth becomes the GPU depth (D32, value / 2^24, exact). The
    "source × a + destination × b" blend goes through the GPU dual source:
    only the rounding of the destination part can differ by one level.
  - The palette is copied into the batch when it changes; a write to wave
    memory first submits the pending triangles.
  - Fills and host writes become native pixel segments, drawn as N×N blocks.
  - When the display address changes (register 0x38), the displayed lines
    are read back without waiting; the screen update shows them if nothing
    has been drawn into them since.
  - Bug fixed along the way: software mode must not copy the native buffer
    at startup, `m_yScale` is not set yet at that point (crash at ×4).
  - Check, crusnexo demo over 120 s, 8 captures: mean difference of 0 to
    0.33 level, at most 0.05% of pixels above 24 (blended halos).
  - Times for 120 s emulated: ×2 89 to 105 s in Vulkan, 164 to 169 s
    software, 82 s native; ×4 107 to 111 s in Vulkan, 334 s software, 78 to
    102 s native (laptop heating up). About 1600 triangles and fewer than 2
    batches per frame; 3 s of GPU wait over 120 s.
- 2026-11-02: **Namco System 21 on Vulkan** (`namcos21_vk`).
  - The two frame buffers (R16 color numbers and depth) are on the GPU at N
    times the native size; the CPU buffers stay native, for the native screen
    and for the depth read by the Driver's Eyes sprites.
  - For each triangle, the CPU steps the edges in double like `rendertri`
    and keeps the extent of each line (one word per line). The GPU fills
    these extents with the depth test (zsort constant per quad, exact in
    D32). So the CPU only does per-line work, and the image is exactly the
    software one.
  - The screen is mixed on the GPU then read back: Winning Run has its 2D
    layer there (512 KB memory copied by written pages, `bitmap_draw` ported
    with the three priorities); Driver's Eyes receives every frame the native
    screen under the 3D and the pixels changed by the top sprites. The
    adjusted palette is sent with the mix.
  - `hires3d_output::use_frame()` accepts an "image ready" callback, called
    just before the display takes the image. The read-back wait is done
    there: the three Driver's Eyes boards work on the GPU at the same time
    instead of waiting for each other (total wait from 25 s to 6 s over
    60 s).
  - Check, 6 captures over 60 s: Winning Run identical to software; Driver's
    Eyes identical on the center screen. The side screens depend on the link
    between the three boards, which sometimes fails at startup ("RECEIVE
    ERROR"), in native too: the game diverges from one run to the next.
  - Times for 60 s emulated: winrun native 29 to 31 s, ×2 33 s in Vulkan vs
    41 to 55 s software, ×4 44 s vs 87 s; driveyes native 65 s, ×2 88 s vs
    109 to 182 s, ×4 112 s vs 197 s.
- 2026-11-02: **Taito JC (TC0780FPA) on Vulkan** (`tc0780fpa_vk`).
  - Same layout as System 21: two color number buffers and a 16-bit depth
    (exact in D32) on the GPU, cleared at swap; the CPU buffers stay native
    for the native screen.
  - Triangles (`render_triangle`, parameter planes) and quads
    (`render_polygon<4>`, parameters along the edges) prepared on the CPU,
    then the shader ports the line extent and the three rasterizers (solid,
    shaded, textured with wrapping). Parameters advance as
    `start + k × step` instead of successive additions, hence rare differing
    pixels.
  - Texture memory (4 MB) is copied by written pages; the screen mixes on
    the GPU the native layers below, the 3D and the layers above (objects
    and text), then the palette, with the wait deferred to when the display
    takes the image.
  - Check, Side by Side over 90 s, 9 captures: at most 0.07% of pixels above
    24. Dangerous Curves does not reach 3D in 90 s (the mix is identical).
  - Times for 90 s emulated: sidebs native 86 s, ×2 104 s in Vulkan vs
    147 s software; dangcurv native 34 s, ×2 47 s vs 54 s. At ×4, the image
    read-back (13 MB) costs 3.3 ms of wait per frame: the GPU image will
    have to be passed to the display without reading it back. (×4
    measurements taken while two other MAME instances ran on the laptop.)
- 2026-11-02: **Gaelco 3D on Vulkan** (`gaelco3d_vk`).
  - The frame buffer (never cleared by the hardware) and the 16-bit depth
    are on the GPU, upside down as the screen shows them; the CPU buffers
    stay native.
  - Each triangle of the fan is prepared like `render_triangle`; the shader
    ports the line extent then the three rasterizers: u/z, v/z and 1/z
    planes, texture mask, COLOR3D bilinear filter, 50% blend (dual source,
    half of the destination rounded by the GPU).
  - The depth test compares the signed value and writes its absolute value:
    the sign depends on z0, fixed per polygon, hence a pipeline variant
    (compare or not) per polygon.
  - The texture and its mask (as bits) are uploaded once; the palette is
    copied into the batch when it changes; the read-back is waited for when
    the display takes the image.
  - Check over 60 s: speedup identical to software on 7 captures, radikalb
    within 0.07% of pixels.
  - Times for 60 s emulated: speedup native 24 to 27 s, ×2 29 s in Vulkan vs
    33 s software, ×4 38 s vs 90 to 108 s; radikalb native 33 s, ×2 36 s vs
    41 to 53 s.
- 2026-11-02: **SNK HNG64 on Vulkan** (`hng64_vk`).
  - The 3D buffer (colour numbers with light and blend bit) and its float
    depth are on the GPU at N times 512 × 512; depth is stored as Z / 128,
    which keeps the order and the clear value of 100 exact. The CPU buffers
    stay native.
  - `drawShaded` triangles are prepared like `render_triangle` (Z, W, light,
    S and T planes); the shader takes over the line extent, then the flat and
    textured rasterizers (perspective divide, texture scrolling, small pages
    with C `fmod`, 4- or 8-bit texels). The texture ROM (16 MB) goes with the
    first batch.
  - The screen is mixed on the GPU: native layers under the 3D, `blit_3d`
    ported (scrolling, vertical stretch, 3D palette with 16 brightness
    levels, 50% blend computed exactly in the shader), then the pixels
    changed by the layers on top.
  - Check over 90 s, 9 captures per game: at most 0.54% of pixels above 24
    (xrally road texture).
  - Time for 90 emulated seconds: roadedge native 136 s, ×2 144 s on Vulkan
    versus 168 s in software; xrally native 126 s, ×2 132 s versus 138 s.
    The emulation itself is heavy, so the gain is small. xrally waits 28 s
    for the GPU (5 ms per frame): to be looked at.
- 2026-11-07: **Real 3D on the GPU for five chips** (Taito JC, Gaelco 3D,
  HNG64, System 21, Zeus 2). Before, the CPU still rasterized at native
  resolution and the GPU redrew each triangle in a box, with the shader
  recomputing the extent of each line.
  - The GPU now rasterizes the primitives themselves, and the CPU raster is
    turned off. The V-Unit stays as it is: integer vertices, video memory read
    by the CPU, and rendering cheap anyway.
  - Taito JC: triangles are interpolated by the GPU. Quads are still
    evaluated along their edges in the shader, because two triangles would
    make a fold on a quad that is not a parallelogram. Depth is still computed
    per pixel (hardware rounding), but a lower bound given to the GPU lets it
    reject hidden pixels before the shader.
  - HNG64: parameters and depth interpolated by the GPU, no more depth writes
    in the shader. The 2D layers above the 3D can blend into it (alpha,
    additive): drawn over a marker, they carry in their alpha what must be
    kept from below, and the GPU does the blend.
  - System 21: quads are drawn with a half-pixel offset, which matches the
    pixels filled by `rendertri` (edges taken at the top of the line). Pens 0,
    1 and 0xff of the top sprites in Driver's Eyes depend on what they cover:
    they are flagged in the pen and applied by the GPU. `namcos21_c67` and
    `gal3` do not show an HD image: `set_hires_supported` leaves them native
    (Cyber Sled checked).
  - Zeus 2: the CPU can read the frame buffer (534,000 reads at crusnexo
    startup). So the native raster resumes for 2 seconds after each read, as
    for the Voodoo. Otherwise, nothing is queued for software rendering any
    more: `object_data().next()` without `wait()` had become very slow (611 s
    instead of 50 s). A scissor limits triangles to the render window.
  - Check against HD software, pixels above 24, worst capture: sidebs 0.05%,
    dangcurv 0.00%, speedup 0.00%, radikalb 0.05%, roadedge 0.09%, xrally
    0.02%, winrun 0.35%, driveyes 0.26%, crusnexo 0.01%. Software fallback
    rerun on all five chips. Save state then load state without a crash on all
    five chips, but crusnexo stayed on the calibration screen there, with no
    3D.
  - Total CPU time (all threads), before then after, fixed emulated duration,
    laptop, a single run per measurement:

    | Game | Native | ×2 before | ×2 after | ×4 before | ×4 after |
    |---|---|---|---|---|---|
    | sidebs (90 s) | 86.0 | 89.2 | 85.1 | 90.5 | 84.5 |
    | speedup (60 s) | 52.1 | 44.0 | 18.1 | 51.7 | 23.9 |
    | roadedge (60 s) | 88.1 | 92.2 | 88.6 | 85.8 | 74.7 |
    | winrun (60 s) | 23.2 | 21.2 | 19.0 | 23.0 | 14.5 |
    | driveyes (60 s) | 64.7 | 71.2 | 57.3 | 70.2 | 63.8 |
    | crusnexo (120 s) | 91.8 | 102.5 | 52.2 | 103.9 | 51.5 |

    The gain depends on the raster's share of the total cost: sidebs and
    roadedge spend most of their time in the emulated CPUs and the 2D.
    Wall-clock time varies too much from one run to the next to conclude:
    sidebs ×4 took 77 s then 101 s with the same executable.
  - Automated games (coins, Start, accelerator, same inputs on the same
    frames) on the full `mame.exe`, software versus GPU, 10 to 15 captures
    per game, pixels above 24, worst capture: speedup 0.00%, crusnexo 0.01%,
    xrally 0.03%, roadedge 0.04%, sidebs 0.07%, driveyes 0.40%, winrun
    0.46%. Real races on all of them, except xrally (pre-race 3D screens).
    25 s run in a bgfx window: sidebs, winrun and crusnusa at 100% speed,
    no errors.
- 2026-11-07 (continued): **windowed checks, GPU waits, Hornet**.
  - Captures of the image actually displayed by bgfx (`PrintWindow`): Speed
    Up, Roads Edge and Cruis'n Exotica at ×2, Winning Run and Side by Side at
    ×4, 3D and 2D correct. The big digit at the Roads Edge start (a 2D sprite
    with its black shadow) is the same in software. The earlier windowed run
    covered crusnusa, a V-Unit that had not changed.
  - Hornet: the K037122 layer was missing in HD (text, HUD). It is now scaled
    up as on nwk-tr. On Gradius IV, the text shows and software matches the
    GPU. The game itself was not reached: it stays in its clock setup menus.
  - The read-back wait (Taito JC, Gaelco 3D, HNG64, System 21) now happens
    when the renderer reads the image: `render_texture::get_scaled` calls
    `hires3d_output::complete_reads()`, after speed throttling. On Speed Up ×4
    windowed: 1.4 s of waiting over 40 s instead of 6.9 s, same images.
  - System 22: `end_frame` waited for the GPU on every frame, which took 35%
    of the main thread on Ridge Racer. It now keeps two frames in flight, with
    a barrier at the head of each frame, and waits via `use_frame`. Unthrottled
    at ×2, Ridge Racer goes from 341% to 407% and Dirt Dash from 263% to 286%.
    Windowed at ×4, Dirt Dash holds 98.8% instead of 95.4%. Images identical
    to the old path.
  - HNG64, GPU timestamps, xrally ×2, per frame: 1.75 ms to upload the 2D
    (1.8 MB), 0.76 ms to draw and blend, 2.17 ms to read back the image
    (3.7 MB). So the wait comes from PCIe transfers, not from drawing.
    The emulation itself stays below real speed in races on this laptop: 56%
    on GPU and 49% in software ×2.
  - Cruis'n Exotica: after a save mid-race and a load 300 frames later, the
    images are pixel-identical to the reference race (shifted by 300 frames).
    The calibration screen of the first run came from a save made before
    calibration. The 534,423 CPU reads of the frame buffer all happen at
    startup, none during the race.
  - Geometry, main thread profile: on crusnexo, Zeus 2 processes its models
    in about 12%, against 58% for the emulated TMS320C31. On ridgerac, System
    22 draws its scene in about 9%. HNG64 does not show up. Moving geometry to
    the GPU would bring little: the step is dropped.
  - Other chips, from a code read (nothing was tested):
    - Banshee and Voodoo 3: the only feasible candidate with racing games that
      work (sf2049, sf2049se, cartfury).
    - Waiting for their emulation: System 23, Type Zero (batlgear), Cobra
      (racjamdx), Quicksilver II.
    - PS1 GPU (technodr, kartduel, glpracr…): integer vertices, so HD without
      sub-pixel precision, for a lot of work.
    - Zeus 1 and Konami M2: feasible, but no racing game.
    - Hard Drivin' and Polygonet: the 3D is computed by the emulated CPU or
      DSP, impossible without HLE.
- 2026-11-07 (evening): **Banshee and Voodoo 3 in HD, image shared with
  bgfx**.
  - Banshee/Voodoo 3: colour and depth buffers anywhere in the memory shared
    with textures, at addresses the game changes. The HD copies (software and
    Vulkan) are kept per buffer address, and every write to memory (FIFO,
    direct and LFB writes, texture port, 2D blits) goes to the copies it
    touches and to the GPU texture memory; 512-entry CLUT. sf2049 and cartfury
    run double-buffered at two addresses, without blits.
  - Reads of the pixel statistics registers restart the native rasterizer
    (only it counts the pixels that pass the tests): sf2049 reads them every
    5 frames, and its demo diverged when only the GPU was drawing. It now
    follows the software path; racingj also reads them, and so draws native
    in addition to the GPU.
  - Voodoo 1/2: pipelined LFB writes were only copied to HD if they changed
    the native pixel; in GPU-only mode, the sfrush wheel test screen was
    black. Fixed. The sfrush demo still diverges in GPU-only mode (estimated
    execution time), as before.
  - Sharing with bgfx: on this laptop, 16 runs out of 16 without errors in a
    window (8 games, twice). At ×4 windowed, throttled, with sharing then
    without: roadedge 88.8% versus 64.5%; ridgerac 100% versus 98.3%; GPU wait
    over 29 s: sf2049 0.07 s versus 6.0 s, sidebs 0.19 s versus 0.82 s,
    speedup 0.09 s versus 0.94 s, winrun 0.02 s versus 0.71 s.
  - Pitfalls found along the way: bgfx renders on its own thread on Windows
    (`BGFX_CONFIG_MULTITHREADED` is 0 only on macOS) with a single-threaded
    D3D11 device. As long as bgfx kept that thread, D3D11 calls from the
    emulation crashed the driver, queries never finished, and a keyed mutex
    gave a black image (drawing happened after the lock was released).
- 2026-11-07 (night): **display options and 16:9 hack**.
  - `-aspect_ratio` and `-crop_position`, with the same values as the other
    emulators on the cabinets (see item 8 of the architecture). Checked in a
    640x480 window: in 16:9, a 640x360 frame with the 4:3 image between two
    bars; `_stretch` fills, `_crop` covers, and `-crop_position -100` keeps
    the top. GPU rendering follows the window size like software rendering:
    bgfx stretches the image texture.
  - `-widescreen` (Vulkan renderer, also with `-hires3d 1`), for the chips
    where MAME does the projection:
    - System 22: projected polygons are shifted by `pad`, and 3D windows that
      touch a screen edge extend to the edge of the image; sprites, direct
      polygons and the text layer stay in the middle or are stretched.
      ridgerac: the track continues on the sides, without a seam, and the
      vanishing point does not move.
    - HNG64: 3D buffers wider by `pad` on each side, frustum widened by the
      same amount (x divided before the clip then multiplied back), blending
      with the layers on top in the middle or stretched. `-widescreen_bg` also
      draws the tilemaps under the 3D beyond the screen edges (bitmaps with a
      margin), not the sprites. The roadedge sky continues without a seam;
      the xrally race widens too.
    - Zeus 2: each line of the buffer image has `pad` extra columns on each
      side; polygons whose clip touches a screen edge continue, and full-line
      fills also clear the sides. Zeus 2 draws its 2D as polygons: the HUD
      stays in the middle, `-widescreen_2d stretch` changes nothing there.
      crusnexo: the 3D widens, the menu and the dashboard stay in the middle
      with black around.
  - Limits specific to the hack: what the game puts off screen becomes
    visible (a 3D panel at the edge of the roadedge selection screen, the 3D
    city behind the xrally 2D intro); a tilemap that does not overflow leaves
    black; with `-widescreen_bg` and `-widescreen_2d stretch`, the 2D layers
    under the 3D are not stretched and shift relative to the ones on top on
    the menu screens.
  - Fixed on 2026-11-09. HNG64: when the tilemaps under the 3D do not extend
    beyond the 4:3 screen (menus), those layers are stretched with the rest in
    `stretch` mode, and the sides stay black otherwise, with no 3D or tilemap.
    The test looks at the sides in all `-widescreen` modes; the race does not
    change. The roadedge bar was a tilemap, not 3D. Zeus 2: in `stretch`
    mode, polygons with no depth test or flat in front of the scene (constant
    z below 12.5) are stretched: crusnexo menus and texts. The HUD of a real
    race was not seen (demo only). Rejected attempt: HNG64 sprites extended
    on the sides duplicate the menus.
  - The crash on exit in a window came from `view_widening()`, called during
    window destruction while the devices were already stopped: stopped
    screens are ignored.
  - Seen along the way: HNG64 software rendering at ×3 (buffer size that is
    not a power of 2) masks its coordinates with `size - 1` and shows
    repeated bands; the GPU path now uses a modulo and is correct at ×3. The
    software path is not fixed.

## Summary, 2026-11-10: extended GPU rendering and full runs

Brief: all items on the list except 1, 4, 8, 9, then runs of all games at ×1
to ×4, with and without `-widescreen`. No push, no release.

- **Model 1** (bf63fc36f50): bgfx sharing and 16:9.
- **System 23** (84f118c06f9): Vulkan renderer (`namcos23_vk`), except
  Gorgon. Race On!: full race at ×2 (65% speed, limited by the emulated CPU).
- **PlayStation** (ce5c31da416): `psxgpu_vk`. Software rendering continues on
  the native VRAM (textures, game reads); primitives are redrawn into a
  high-resolution copy of the VRAM, and the displayed area is taken from it.
  The GPU rejects triangles larger than 1023 pixels like the hardware
  (otherwise coloured blocks at the bottom left in kartduel). 15-bit only:
  24-bit mode and the blanked screen stay native. No 16:9 (projection done
  by the game).
- **Viper and Thrill Drive**: thrild2, xtrial (Viper) and thrilld (Hornet)
  boot and play their demo without changes, despite "not working". thrilld,
  racingj and racingj2 require their memory to be initialised (Test button).
- **Item 3**: racing on the GPU: roadburn ×3, windheat ×2, racingj ×2,
  racingj2 ×2, calspeed ×2. The grey rectangle in Racing Jam (rear-view
  mirror, car preview) also exists in native and in software.

### Matrix

40 games, ×1 to ×4, with and without `-widescreen`: 320 runs, all with exit
code 0, without `-video` (image read back from the GPU), 60 to 230 emulated
seconds, images compared from one configuration to another (last capture of
each run). Games played by script (130 s) because they were stuck at 60 s on
calibration or startup: crusnexo, crusnusa, hangplt, raceon, motoxgo,
kartduel; sf2049 and sf2049se get further in 130 s without a script.
Hard-disk games (CHD) cannot run as several instances: one at a time.

- Not tried: netmerc and wingwar, `hd44780` ROM missing from the NAS.
- gticlub: wheel error (item 4, excluded); tried up to the test menu only,
  its 3D is not seen in the matrix.
- 500gp: persistent black screen at the end of the run at ×2 and ×3, only
  with more than 5 instances at the same time on this laptop's GPU (3 runs out
  of 8 in the first pass, then ×2 in a loaded pass). Never seen alone or on
  half of the 5-instance passes. Unverified hypothesis: a failing Vulkan
  submit (`end_frame` returns false silently, the native bitmap is not
  drawn). Only one instance runs on a cabinet.
- The 16:9 of the raceon menu leaves black sides at the top and bottom (2D
  background).

### In a bgfx window

Sharing without CPU read-back, seen on the real screen (60 consecutive
desktop captures, none black): vr ×1 and ×2, vr ×2 16:9, raceon ×2 and ×2
16:9, pocketrc ×3. The `PrintWindow` in `wincap.ps1` gives black images on
these games: a capture defect, not a display one.

### Continued, 2026-11-10: steering wheel, software fallback, rear-view mirrors

- **GTI Club**: "DEVICE ERROR (STEERING WHEEL)" came from the startup check,
  at address ff80fc0c: increasing torque to one side (0x91 to 0x9f), to the
  other (0x81 to 0x8f), then to the first side again. The wheel must move
  from 0x60 at a given torque, go from end stop to end stop and be centred. A
  small model in `gticlub.cpp` gives the position read during this check,
  from the first 0x91 to the motor stop (0x00); after that the wheel port
  takes over, on the cabinets too. thunderh and slrasslt are not affected.
  The 3D demo is seen from ×1 to ×4, with and without `-widescreen`.
- **Software fallback**: System 23, PS1 and Voodoo stayed black or silent if
  the GPU failed. They switch to native with a message. Tested by forcing the
  failure (submit that fails after 400 calls, temporary uncommitted patch):
  500gp, pocketrc, vr, ridgerac, sfrush, sidebs, roadedge, speedup,
  crusnusa, winrun, gticlub2 get their image back. The `-video none` engine
  could not reproduce the 500gp black screen seen under heavy load: its cause
  remains a guess.
- **Thrill Drive 2 and Xtrial Racing**: the game changes buffer width every
  frame for its rear-view mirror, which destroyed the GPU images
  (`hires_layout`): the title stayed on screen during the race. A narrower
  target now keeps the images. It is also drawn by the native rasterizer,
  because the mirror is read back as a texture from native memory
  (`tmu_written` on return to the normal width). Full races on GPU: thrilld,
  thrild2, xtrial, from ×1 to ×4.
- **Motocross Go!**: the tyres really do have the ground colour on a real
  cabinet (video seen by the owner). No defect.
- Final check, Voodoo and Hang Pilot, 96 runs: thrild2, xtrial, thrilld,
  gticlub2, sf2049se, sfrush, calspeed, cartfury, roadburn, racingj,
  racingj2, hangplt, ×1 to ×4, with and without `-widescreen`, no errors.

### Continued, 2026-11-11: video menu, Thrill Drive 2 black frame

- **Menu**: "Video Options" (general and per-game settings) has a "High
  Resolution 3D Options" section: `-hires3d`, `-hires3d_renderer`,
  `-bgfx_hires3d_share`, `-aspect_ratio`, `-crop_position`, `-widescreen`,
  `-widescreen_2d`, `-widescreen_bg`. Applied at the next launch, not on the
  fly (owner's choice).
- **Thrill Drive 2, Xtrial Racing**: every other game frame came out black
  with Vulkan at ×2 to ×4 (the owner saw the image flicker). The rear-view
  mirror, drawn native on the narrow target, still took one of the three
  high-resolution copies of the colour buffers and evicted the one of a
  buffer to be displayed; that buffer was then shown from its native version,
  empty when only the GPU draws. The narrow target no longer goes through the
  high-resolution copy with Vulkan. The earlier runs (one capture every 300
  frames) always landed on a good frame: checked this time frame by frame,
  in a race, thrild2 ×2 to ×4, xtrial ×3 and ×4, frame-to-frame differences
  equal to native; gticlub2 and sf2049se unchanged.

### Continued, 2026-11-11: upstream merge, UDP announce, flicker hunt

- **Merge** of upstream MAME (59 commits, 754eb0adb02). Only conflict: the
  MIPS counter, which upstream fixed the same way we did (adb51e24801); the
  mips3 files are taken from upstream. Two new `.lay` files required
  regenerating the project (`make vs2022`) before compiling.
- **Network outputs** (208804e82b9): when a game starts, `-output network`
  sends a UDP broadcast on port 8001 `mame_start = <game>\r` then
  `tcp = 8000\r`, like Supermodel njz3. BackForceFeeder and the njz3 FFB
  Plugin listen on this port, now both in shared mode (SO_REUSEADDR). Seen
  with a receiver for vr and ridgerac; nothing without `-output network`.
  The TCP 8000 server accepts several clients at once.
- **Flicker hunt**, on the merged build: 40 games, native then Vulkan ×2,
  ×3, ×4 and ×2 `-widescreen`, one capture per frame in two 40-frame windows
  (demo, then a game with coin, Start and accelerator), each frame compared
  to the native frame with the same number, and black frames counted. The
  bench finds the 21 black frames of thrild2 with the pre-fix exe. Result:
  no black frames, no spikes. The remaining differences are not image
  defects:
  - sfrush: the last capture, taken on exit from `-seconds_to_run` (leftover
    native image, already known);
  - raceon: the Vulkan image is shifted by a whole frame, regularly,
    depending on the window (emulation timing, not checked further);
  - roadburn, xtrial, calspeed: the race starts differently from native
    (position, ranking), same timer; images correct, rear-view mirrors
    included. Probably the estimated pixel count when only the GPU draws,
    read back by the game (not checked);
  - pocketrc: still scene, only the sharpness at ×4 differs.
  Limits: 80 frames per run, always the same driving; only this PC's NVIDIA
  card.

### Continued, 2026-11-12: upstream merge, odd bench cases

- **Merge** of upstream MAME (8 commits, 9a52a4806ab), no conflicts.
  `diexec.h` changes (`signal_external_write`): almost everything recompiles.
- **Method**: in addition to the captures, a fingerprint of the main CPU RAM
  (8 MB, one word in sixteen, every 10 frames, same inputs as the bench)
  tells whether the emulation itself deviates from native, and two native
  launches tell whether the game is reproducible.
- **California Speed**: the race started differently on Vulkan. Cause: when
  only the GPU draws, the pixel count of a triangle (which gives the
  duration of the operation as seen by the game) was the area clipped by the
  clip rectangle; the native rasterizer (`POLY_FLAG_NO_CLIPPING`) counts the
  extents of each line without clipping. `estimated_pixels` now redoes its
  calculation exactly, and the fast fill follows `render_tile`. After the
  fix, RAM identical to native at ×2 and ×4 over the whole game.
- **Road Burners, Xtrial Racing**: two native launches already differ
  (roadburn from frame 430, at startup). The M48T37 (Vegas) and M48T58
  (Viper) clocks read the PC time; this is not mame3d.
- **Race On!**: RAM identical to native at ×2 and ×4; the ×4 image is
  sometimes one frame late (not on every launch), display only. Cause not
  found (the System 23 Vulkan renderer reads back its image right away).
- **Pocket Racer**: native reproducible, software identical, Vulkan differs
  only in sharpness.

### Continued, 2026-11-12 (evening): Thrill Drive 2 in 16:9, Race On!

- **Thrill Drive 2, `-widescreen`** (viper.cpp, all versions): the game does
  its own 3D projection, so the hack goes through its code. The camera
  structure (viewport 512x384, near 0.2, far 280, FOV 50°, tanH 1.1918, tanV
  0.8938 at 0x5729a0) is filled once by a function (0x040b40 in all four
  versions): `tan(fov)`, tanH at +0x34/+0x38, tanV = tanH × aspect at +0x3c,
  then the frustum (left, right, top, bottom) passed to the function that
  builds the clipping planes. The driver looks for the end of this function
  by signature, puts a jump there to 12 instructions written at 0x3600 (empty
  RAM between the vectors and the program) that multiply tanH and the right
  of the frustum by 4/3 without touching the top or tanV: projection and
  culling widen together. The screen keeps its size and is shown stretched to
  16:9 (`hires3d_output::allow_anamorphic`). All cameras are widened, so the
  rear-view mirror keeps its shape; texts and gauges are stretched.
  - Tests: changing tanH on the fly widens the view (the frustum alone has no
    effect, nor does the angle). The debugger watchpoints see nothing: the
    RAM is DRC fastram. Code outside work RAM (0x02000000, 0xfe100000): the
    game stays on its blue boot screen (cause not investigated; maybe
    PowerPC address translation).
  - Checked: thrild2, thrild2j, thrild2a, thrild2c patched at the same
    address, correct race without `-hires3d` and on Vulkan ×2 and ×4; without
    `-widescreen`, image pixel-identical to before.
- **Race On!, late frame** (vk3d_context.cpp): the Vulkan image was one frame
  behind native, over whole windows, depending on the launch and more often
  at ×3 and ×4. The render lists and vertices sent to the GPU were identical
  to native frame by frame (fingerprints), the image read back right after
  the fence was the previous one, and became correct again right after
  `vkDeviceWaitIdle` (not with an invalidation alone, nor with full
  barriers). Cause: `submit_and_wait()`, which uploads the textures at
  startup, left the context fence signalled; the System 23 renderer submits
  each frame with this fence, believes it free, waits on it then resets it.
  Submitted signalled, each wait returned immediately, and the read-back
  took the previous image as long as the GPU lagged behind the CPU. The
  fence is now reset after `submit_and_wait()` (the other chips have their
  own fences, or reset this one before submitting). After the fix: ×2, ×3,
  ×4, two launches, no shift, ×3 identical to native.
- **Thrill Drive 1 in 16:9** (nwk-tr.cpp, `-widescreen`): thrilld and its
  versions are on the NWK-TR driver (not Hornet: only the thrilldg* games are
  there, marked not working). The PowerPC 403 has no FPU and keeps no camera
  floats: perturbing ~6500 floats in its RAM and ~2750 in DSP block 1 did
  nothing. It is the SHARC DSP that does the projection.
  - Found by tracing the DSP writes to the Voodoo (Lua tap on
    0x2400000-0x27fffff: fvertexAx..Cy at 0x2480022-27, ~460 triangles per
    frame), then by walking back up the DSP command interpreter (each handler
    reads its words with `DM(I4, M1)` in shared memory 0x400000 and jumps to
    the next one through table 0x28283).
  - Command 3 = viewport, 6 16-bit words: centre (256, 192), half-size
    (256, 192), horizontal then vertical focal length in pixels, negative.
    The DSP derives the projection from it (half-width / |focal|) and the
    clipping planes (focal × 16). 7 viewports per frame: the world at −215
    (256/215 = tan 50°, like the Thrill Drive 2 tangent), the 3D dashboard
    (rev counter needle, speed digits) at −443, and a read of +128 at the
    same PC with no visible effect.
  - The patch: a read tap on the shared memory of both DSPs; when the DSP is
    at the PC that reads the horizontal focal length (0x20130, found by an
    8-instruction signature, checked every frame, searched once per second
    while it is not there), the value is divided by the widening (4/3),
    except −443 (the dashboard stays like the 2D, stretched) and positive
    values. Image stretched to 16:9 (`allow_anamorphic`), like Thrill Drive
    2.
  - Pitfall: a first attempt (writing −332 to PowerPC RAM at 0x3eb2d4)
    seemed to widen the view by eye, but the captures were pixel-identical:
    always compare pixels.
  - Checked on thrilld (only version available locally): without
    `-widescreen`, pixel-identical image; with it, identical to the Lua test;
    in a bgfx window with `-hires3d 2`, 16:9 image, cars with the right
    proportions. In cockpit view, the bottom of the cabin, modelled for 4:3,
    lets the road show at the edges. Only DSP 1 has the program (the site is
    not found on DSP 2).
- **Thrill Drive 1, empty rear-view mirror** (voodoo_render.cpp, voodoo.cpp,
  voodoo_vk.cpp): the mirror frame (top centre) stayed plain mauve, with or
  without `-widescreen`. A bug in MAME's Voodoo, not in the 16:9 hack.
  - False lead: the 2nd video board (TWIN/SLAVE). Its DSP never has a
    program in MAME, but the PowerPC only addresses board 0 (id 2 only at
    startup); when copying the writes to board 1, it draws the same image as
    board 0, since the DSP has no identity of its own (no flag or jumper
    read).
  - The mirror is drawn by board 0: one more viewport command, centre
    (256, 344), half-size (128, 32), focal lengths +128/−128 (positive
    horizontally: the image is flipped), clipping x 128-384, y 312-376 with
    the Y origin at the bottom (fbzMode bit 17); a fast fill of the area with
    0xaab4ff (the mauve), then ~60 triangles with an alpha test and a colour
    key.
  - Cause: the fast fill compares the clip rectangle with the Y values before
    the flip (it fills the right area), the triangles with the Y values after
    the flip: with a rectangle that is not full screen, all the mirror
    triangles (y 312-376 → lines 7-71) were rejected. Triangles are now
    clipped like the fast fill, in all three paths (software, -hires3d
    software, Vulkan). LFB writes through the pipeline keep the old
    comparison (not seen in use).
  - Checked pixel by pixel before/after in attract mode (70 s): GTI Club 1
    and 2, Hang Pilot, Thrill Drive 2, Xtrial Racing, San Francisco Rush,
    California Speed, Gradius 4, Teraburst identical; Rush 2049 and 2049 SE:
    only the displayed clock differs; Racing Jam 1 and 2: the grey band at
    the bottom of the screen disappears, the 3D goes down to the bottom (same
    cause, more accurate). Thrill Drive: mirror in native, Vulkan ×2,
    software ×2.
  - With `-widescreen`, the mirror camera is widened too (only −443, the
    dashboard, is left alone), so that it keeps its proportions once
    stretched.

### Continued, 2026-11-14: 16:9 for games that do their own projection, known defects

- **Voodoo LFB writes**: nothing to fix. The pipeline call already passes the
  raw Y (`pixel_pipeline(..., x, y, ...)`), like the fast fill and now the
  triangles.
- **500gp, black screen under load**: not reproduced, 6 instances in Vulkan
  ×2 then 8 in ×3 (130 s, demo), no black frame, no fallback, identical
  final frames. Probably the fence left signalled (7e1b998871a, Race On!):
  under load, the read-back took a frame that was not drawn.
- **HNG64 in software ×3, ×5** (hng64.h, hng64_3d.ipp, hng64_v.cpp): the
  `size - 1` masks of the rasterizers and of `blit_3d` become `wrap()`, the
  same mask for powers of 2, a modulo otherwise. Roads Edge in race ×3:
  software = Vulkan (0.03 mean difference, 26 before).
- **Race On!, menu in 16:9**: the background is a wall of 3D tiles placed by
  the game over the 4:3 width; you see past it in 16:9. Game content, not
  fixed.
- **Banshee 2D blits**: Rush 2049, 2049 SE and Cart Fury do no
  screen-to-screen blit (200 s of demo and race, temporary counter). Only
  Thrill Drive 2 and Xtrial Racing (Voodoo 3) do: the rear-view mirror,
  copied line by line from the buffer to texture memory; a texture stays
  native on the real hardware, and the native rasterizer is re-run when a
  blit reads a buffer. Nothing to do. The other 2D commands (except host to
  screen, already upscaled in blocks) stop MAME (`fatalerror`): no game uses
  them.
- **"Anamorphic" 16:9** on games that project their 3D themselves: the
  image keeps its size, the view is 4/3 wider, the screen is shown in 16:9
  (`hires3d_output::allow_anamorphic`). Without `-widescreen`, 16 games
  checked pixel-identical to the previous exe (70 s of demo, native, and
  Vulkan ×2 for 4 of them).
  - Method: a temporary counter of vertex X positions tells whether the game
    sends geometry outside the 4:3 screen. Clipped at the edge: K001005
    (exactly ±256 over 19 million vertices), Voodoo (Rush, California Speed,
    Rush 2049, Road Burners, Cart Fury, GTI Club 2, Hang Pilot, Xtrial:
    < 0.5 %), Taito JC (Side by Side: 0). Beyond the edge: System 21
    (Winning Run 18 %, Driver's Eyes 59 %), Gaelco 3D (3 to 11 %), V-Unit
    (Cruis'n USA 7 %, World 11 %, Off Road Challenge 5 %).
  - **Racing Jam 1 and 2** (nwk-tr.cpp): same viewport command as Thrill
    Drive 1 (signature found at 0x20129 and 0x2012c). Focal lengths: world
    −247 (−233, −211, −210 in Racing Jam 2), mirror +79 (+85); −154, −110
    and −332 draw the dashboard (rev counter, digits) and are left alone,
    like −443 on TD1.
  - **GTI Club, Midnight Run, Winding Heat** (konppc.cpp, gticlub.cpp,
    zr107.cpp): the DSP runs in the recompiler; a Lua tap set after
    compilation sees nothing, and the PC read from Lua is wrong. Taps must
    be set before the first execution (machine_start or video_start). The
    DSP reads its command list as 16-bit words in shared memory: `0x49`
    clipping slopes (32 bits, 2.14, half-width / focal length: 20337 and
    15252), `0x46` viewport (centre 0,0, half-size 256×192), `0x47` focal
    lengths (32 bits, 16.16, −206.24). The focal length alone narrows the
    image but keeps the 4:3 clipping (black bars); the viewport half-width
    does nothing; the x slope of 0x49 multiplied by 4/3 with the x focal
    length divided gives the full width. The konppc tap recognises the
    sequence 0x49…0x46…0x47 and changes the 4 words when the DSP reads
    them. Seen in demo and in race, native and Vulkan ×2; ~5 % more
    vertices (the wider view).
  - **PlayStation** (gte.cpp, psx.cpp): the X projected by RTPS/RTPT is
    multiplied by 3/4 around OFX (like the DuckStation hack). Pocket Racer
    in race: wider view, car keeps its shape, 2D stretched. Kart Duel did
    not start a race with the script (title only).
  - **San Francisco Rush** (seattle.cpp): cameras in RAM (structure found
    from float pairs at a 4:3 ratio): size 512×384, angles 60° and 46.8°,
    tangents 0.57735 and 0.43301, inverses, cosines, 1, ~2^24, centre
    256,192, focal lengths 443.41. The x focal length alone narrows with
    black bars, the x tangent alone changes nothing; both together give the
    wide view. 4 cameras found by their shape and patched every frame when
    the game resets the tangent of the angle.
  - **Winning Run, Gaelco 3D, V-Unit** (namcos21_3d, gaelco3d_v,
    midvunit_v): polygons are drawn 4/3 narrower around the centre (Gaelco:
    the x gradients of the plane equations multiplied by the same; its 2D
    polygons without depth are not narrowed). What the game does not send
    is missing at the edges, and the screens the game covers with a 4:3
    image (V-Unit menus and transitions, Radikal Bikers game over, Speed Up
    texts parked off screen) show the 3D on the sides.
  - **Not done**: California Speed, Rush 2049, Road Burners, Cart Fury (no
    camera structure found by shape), Xtrial Racing and GTI Club 2 (Viper:
    not the TD2 camera function; the viewport structures at 0x2700 have no
    effect), Hang Pilot (command 3 like TD1, different code), Side by Side
    (TMS320C51 DSP), Driver's Eyes (three screens).
  - Tools (scratchpad): `sig/sigscan.lua` (DSP code signature),
    `sig/seq.lua` (command stream read by the DSP), `sig/f47.lua`,
    `sig/pkm.lua` (multiply floats every frame), `sig/ratio.lua` (pairs at
    a 4:3 ratio), `sig/pert.lua` (one float at a time from a save state:
    0.3 to 0.5 s per candidate, too slow for the whole RAM), `pkrun.sh`
    (EEPROM copied at each launch: otherwise the demo changes).

### Continued, 2026-11-14 (afternoon): 16:9 for the remaining games

Six more games in 16:9 with `-widescreen` (anamorphic, like the previous
ones). Cart Fury, Side by Side and Hang Pilot remain.

- **Driver's Eyes** (namcos21_de.cpp): each board stack projects beyond its
  screen, towards the neighbouring screens (59 % of vertices outside the 4:3
  screen): same narrowing as Winning Run, on all three screens. Done for a
  single screen (the centre); side by side, the screen edges overlap (the
  "DRIVER'S EYES" title shows doubled letters at the joins). In MAME, the
  side screens do not show the race in demo (already noted in the driver).
- **California Speed** (seattle.cpp): the projection is recomputed every
  frame by function 0x80024a10 (`set_projection(tan h, ...)`), written at
  0x6dd538: tan h, tan v = tan h × h / w, 1/(tan h² + 1), 1/(tan v² + 1),
  two values passed as is, focal h = w / 2 / tan h, focal v, centre x,
  centre y. A patch at vblank is overwritten before use: a write tap
  (Seattle has no fastram, the MIPS recompiler's writes go through the
  handlers) multiplies the tangent, divides the focal length and recomputes
  1/(tan² + 1) when the game writes them. The structure is found by its
  shape (the two 1/(tan² + 1) relations, width = 2 × focal × tan between
  256 and 1024); the two focal lengths are equal as long as it is not
  patched (square pixels).
- **Rush 2049, 2049 SE, Road Burners** (vegas.cpp): 4 cameras, same fields
  as Rush 1's in a different order (focal lengths, centre, 1, size, angles,
  tangents, inverses, cosines), 0xe4 bytes apart: 0x132e530 (sf2049),
  0x11aab50 (sf2049se), 0xbfabd0 (roadburn). Camera 0 (90° horizontal) is
  the race camera; cameras 1 to 3 (45°) are only written at startup. My
  first tests only patched those (nothing changed): trace who reads and
  writes before concluding. Patch at vblank like Rush 1 (tangent, inverse,
  cosine, focal length; the angle is kept and serves as the "not yet
  patched" marker); tangent and focal length were enough. Direct search in
  RAM (`get_read_ptr` in 1 MB blocks; the continuity check must compare an
  aligned address, 0xffffc, otherwise all blocks were rejected).
- **GTI Club 2** (viper.cpp): the camera has the fields of Thrill Drive 2's
  (tangent at +0x34 and +0x38, vertical +0x3c, h/w shape +0x40, base
  0x7f63d0), but the game recomputes the tangents every frame (not the
  shape): a data patch does not hold. The projection goes through an
  OpenGL-style library (`glMatrixMode(0x1701)`, `glLoadIdentity`,
  `glFrustum(-r, r, -t, t, near, far)` at 0x39f5c) called at 0x3fdcc with
  r = near × tan, t = r × shape. Patch: `fmr f2, f29` (0x3fe20) becomes a
  branch to the code cave at 0x3600, which multiplies r by 4/3 after t is
  computed. The structures at 0x2700 from the previous note are the copy of
  this frustum (l, r, b, t, near, far) and the 2n/(r-l) matrix at 0x26dc:
  written by the library, hence no effect.
- **Xtrial Racing** (viper.cpp): same library (glFrustum at 0x3a148), but
  the cameras are objects with their frustum in doubles (+0x80 to +0xa8)
  applied by a generic function (2 copies: 0x760e4 and 0x7e5c4). The first
  caller found (0x45e48, tan × aspect) was not used in race:
  pixel-identical image. Patch of both calls: left and right × 4/3 before
  `glFrustum`, one 0x40-byte code cave per site.
- Checked: Vulkan ×2 and native on California Speed, GTI Club 2, Xtrial,
  Rush 2049; Road Burners and 2049 SE in native. Without `-widescreen`,
  pixel-identical to the previous exe (Driver's Eyes, California Speed,
  Rush 1, GTI Club 2, Thrill Drive 2, Hang Pilot; Xtrial: 3×3 pixels
  differ, as between two launches of the old exe). Thrill Drive 2 with
  `-widescreen`: identical to the previous exe (viper code reworked for
  several sites).
- **Not done**:
  - Cart Fury (Vegas, Voodoo 3): done the following night, see below.
  - Side by Side (Taito JC): done the same evening, see the next section.
  - Hang Pilot: the word 0x00034f7b (3.31 in 16.16) read by both DSPs at
    0x400005-8 is not the focal length (changing it breaks the 3D).
- Pitfalls and tools (scratchpad):
  - Vegas (vrc5074) gives the RAM to the MIPS recompiler as fastram: Lua
    taps see nothing there. Test build with `add_fastram` disabled
    (environment variable, not committed). The RAM is installed by the BIOS
    after the first frame: a tap set before is erased (set it around frame
    600). With `-debug -debugger none`, the PC read from Lua is correct.
  - Rush 2049, Road Burners, Cart Fury: the demo changes from one launch to
    the next (game clock taken from the PC time). Compare within one
    launch: `sig/pkt.lua` captures, applies the patch for a few frames,
    captures again and restores (`PM_ONCE=1`: a single write, to see
    whether the game rewrites the value).
  - Two launches of the same CHD game at the same time: "DIFF CHD ERROR".
    Two different CHD games: possible.
  - In Python heredocs passed through the Bash tool, `\n` becomes a real
    newline: write `chr(92) + 'n'`.
  - Scripts: `sig/cscan.lua` (screen size or centre pairs as floats),
    `sig/vfind.lua` (given values), `sig/mtap.lua` (reads and writes of a
    range by PC, with `-debug`), `sig/codedump.lua` + `sfdis.py` (MIPS
    disassembly), `sig/ramdump.lua` + `ppcd.py` (PowerPC), `sig/calw.lua`
    (prototype of the California Speed tap), `sig/focal.lua`,
    `sig/pmat.lua`, `sig/sbtap.lua` (Taito JC compute unit),
    `sig/hptap.lua`, `reg2.sh` (regression against `mame_clip2.exe`).

### Continued, 2026-11-14 (evening): 16:9 for Side by Side

- **Side by Side 1 and 2** (taitojc_v.cpp): with `-widescreen`, the word
  the 68040 sends to the DSP at 0x0314 is divided by 4/3 (write tap on the
  DSP data space). Approach:
  - The DSP copies each view matrix (13 words) from the command list to its
    internal RAM at 0x0326 (PC 0x42ae): a status word then 3 rows of 4
    words (3 terms in 2.14, 16384 = 1, and the translation). Row 0 (0x0327)
    is the screen X, row 1 the Y (tested row by row).
  - Scaling row 0 by 3/4 at copy time does widen the view, but distorts and
    punches holes in nearby walls: it is not the only X scaling. The number
    of objects processed by the DSP does not change.
  - The 68040 has a field-of-view function (0x080787ae, ~25 calls: angle in
    tenths of a degree, aspect ratio, 1, distance). It sends the DSP
    0x0314 = 4096 / aspect and 0x0315 = 4096 × tan(angle / 2) (command
    0xc002 at 0x080779d0), and keeps 1 / (tan × aspect) at 0x8038040 and
    1 / tan at 0x8038044 for itself.
  - The DSP multiplies its data by 0x0314 and 0x0315 (0x4434 in Side by
    Side, 0x43ca in Side by Side 2) before clipping and sorting the objects.
    0x0314 × 3/4: image narrowed from 0.74 to 0.76 in width, 1.00 in
    height, no holes. 0x8038040 or 0x8038044 × 3/4: no visible effect;
    0x0315 × 4/3: also narrows the height.
  - Side by Side writes 0x0314 at each scene change (3303 = 4096 / 1.24),
    Side by Side 2 every other frame (3200 = 4096 / 1.28).
- Checked: 7 captures from 2400 to 6300 on Side by Side, 6 on Side by
  Side 2, in native; Side by Side also in Vulkan ×2 (same image, slight
  time offset). Without `-widescreen`, pixel-identical to the previous exe
  on both games. With `-widescreen`, the Side by Side 2 demo drifts a little
  (the DSP no longer has the same work, so not the same timing).
- Remaining:
  - On one Side by Side 2 capture (cherry trees), a small piece of road
    edge is missing near the rail. Also widening the 68040 sort (0x8038040)
    changes nothing in the image; cause not found.
  - 2D texts and gauges stretched, as on the other anamorphic games.
  - Dangerous Curves (same board) is neither handled nor tried: its demo
    does not reach the 3D in 90 s (noted above).
- Tools (scratchpad `sig/`): `jcdasm.lua` (DSP disassembly), `sbwide.lua`
  (scaling of one matrix row), `sbread.lua` (DSP internal RAM accesses by
  PC), `sbcount.lua` (matrices, vertices, FIFO per frame), `sbpc.lua`
  (68040 writes to the DSP), `sbfov.lua` (factors on 0x0314/0x0315 and
  0x8038040/44), `orth.py` (rotation matrices in a RAM dump), `hscale.py`
  (horizontal and vertical scale between two captures).

### Continued, 2026-11-14 (night): Cart Fury, 2D in 16:9, Dangerous Curves

- **Cart Fury** (vegas.cpp, vrc5074): done.
  - Vertices go to the Voodoo 3 through the CMDFIFO (PCI memory
    0x12e70000-0x12e8ffff, `sig/pcitap.lua` and `sig/pcival.lua` on the
    `memory_space` space of `:pci:00.0`), already projected: X, Y in
    pixels, 1/W, S/W, T/W.
  - Triangle submission at 0x80189f8c (projected vertices at +0x10 X,
    +0x14 Y, +0x18 1/W); projection at 0x80151630: 1/W = 1 / z,
    X = cx + x × 1/W, Y = cy + y × 1/W, outcodes on [0, 512] × [0, 384],
    six unrolled copies. The view matrix is in globals: X row 0x80619478,
    0x80619550, 0x806196e4; Y 0x8061954c, 0x806196e0, 0x806197ac; Z
    0x806196dc, 0x806197a8, 0x80619844 (not scaled); X and Y = camera
    rotation × focal length [0x805cc720] (about 550). Written by three
    camera functions (0x8015c36c, 0x8015c918, 0x8015d388) just before
    drawing: patching it at vblank is useless.
  - Fix: with `-widescreen`, page 0x00619000 is excluded from the MIPS
    recompiler's fastram (`vrc5074_device::set_fastram_hole`, the fastram
    is split in two around the hole; 3 entries at most, ROM included) and
    a write tap divides the three terms of the X row by 4/3. The code that
    writes it is checked first (3 words at 0x0015d398, 0x0015d3a8,
    0x0015d3e4; only one version of the game).
  - Checked: 6 captures from 2400 to 5400 in native (wider view, cars at
    the right proportions, team presentation screen too) and 3 in Vulkan
    ×2. The demo changes at each launch: no pixel comparison; without
    `-widescreen` the hole is not set, nothing changes.
  - The 2D (banners, credits) is drawn by the Voodoo: stretched.
- **2D in 16:9** (item 6): `hires3d_output::narrow_2d()`, `narrow()`,
  `upscale_2d()`, `draw_2d()` and `set_narrow_area()`. With `-widescreen`
  on an anamorphic game, the top 2D layer is narrowed by 3/4 around the
  middle (it gets its shape back once stretched); `-widescreen_2d stretch`
  keeps the old behaviour.
  - Done: Side by Side 1 and 2 (priority objects and text layer; the black
    masks of columns 0-16 and 496-511 stay at the edges), GTI Club,
    Midnight Run, Winding Heat, Racing Jam 1 and 2, Thrill Drive 1 (front
    layer K001604 or K056832). Native, software HD (narrowed during
    upscaling, lossless), Vulkan (narrowed at native resolution before
    mixing: one column in four is dropped, the text is a little less
    sharp).
  - Checked: Side by Side in native, software ×2 and Vulkan ×2; without
    `-widescreen`, pixel-identical. Konami: startup texts and CREDIT
    re-centred; Midnight Run in race; GTI Club and Racing Jam did not reach
    the race in these runs (nvram).
  - Not simply doable: Voodoo games (Rush, Rush 2049, California Speed,
    Road Burners, Cart Fury, Thrill Drive 2, GTI Club 2, Xtrial): the 2D is
    made of Voodoo triangles, mixed with the 3D; Winning Run (2D/3D mixing
    in the Vulkan shader, to be changed too); PlayStation (GPU sprites in
    the same buffer); V-Unit, Gaelco 3D, Driver's Eyes not looked at.
- **Dangerous Curves** (item 4): still no 3D.
  - At startup, the DSP tests whether there is RAM at data 0x1400 (writes
    0x1234 and reads back, at 0x7f0a) and picks BMAR = 0x0800 if so,
    0x4000 otherwise; it writes the result at 0x7800/0x7801 for the 68040.
    In MAME, nothing at 0x1400: the 68040 then sends a command that the DSP
    table (0x2025, in the C51 SARAM) does not know, and the DSP spins in
    its trap (`b 2065`).
  - When simulating RAM at 0x0c00-0x2bff (Lua taps, `sig/dcram.lua`), the
    game goes through its menus and starts a real race (countdown, timer,
    dashboard), but without 3D: the DSP waits for its commands at 0x7ff0
    (code relocated to 0x0800) and the 68040 sends it none.
  - TMS320C50 (9K SARAM at the same addresses in program and data) then
    TMS320C53 tried in place of the C51: the DSP crashes in 8 s (the code
    is made for the C51 SARAM at program 0x2000). Reverted.
  - Lead: external RAM at data 0x0c00-0x5fff on the Dangerous Curves
    board, then understand why the 68040 sends nothing.
- **Side by Side 2 hole** (item 5): not fixed.
  - In 16:9, the road edge strip along the left rail is missing in places
    (cherry tree scene, several frames in a row).
  - The 68040 sends as many objects in 4:3 as in 16:9; its field-of-view
    values (0x8038040, 0x8038044, set by 0x080787ae, ~25 calls) change
    nothing, in either direction.
  - In the DSP, the 0x0314 scale is applied to the polygon vertices after
    their transformation (loop at 0x03c5 in Side by Side 2), before their
    outcodes (0x0754: |X| compared to Z): consistent. The bounding box test
    (0x0505, 8 corners, outcodes before the scale, so in 4:3) is only used
    for about ten objects every 4 frames.
  - Cause not found; one of the missing pieces would have to be followed
    in the DSP.
- Tools (scratchpad `sig/`): `play.lua` (coin, start, accelerator),
  `dcram.lua`, `dcmon.lua`, `dspc.lua`, `jcdasm2.lua`, `pcitap.lua`,
  `pcival.lua`, `cfmat.lua`, `cfwide.lua`, `snap.lua`.

### Continued, 2026-11-14 (late night): 2D in 16:9, checks and System 21

- **In-game check of the Konami 2D** (`sig/kplay.lua`: coins, Start every
  2 s in the menus, accelerator at maximum, Test button): Thrill Drive 1
  runs a real race, the HUD (time, position, speedometer) keeps its shape;
  GTI Club stays in demo, but its texts (SHORTCUT, TAG RACE, CRASH, CREDIT)
  are narrowed; Racing Jam goes into its test menu (backup error): the
  menu text is narrowed, the race is not seen (same driver as Thrill
  Drive 1).
- **Winning Run** (namcos21.cpp): in race (priority 2), the 2D layer above
  the 3D is narrowed: `bitmap_draw` (native and software HD, through
  `hires3d_output::narrow_column`) and the Vulkan mixing shader
  (`mix.frag`, 4 more constants: middle, factor, columns, through
  `narrow_params`; `tri.vert` recompiled for the new size). The title
  (priority 5) and test mode (0) keep the full layer. The cockpit is in
  3D. Checked in native, software ×2 and Vulkan ×2; without `-widescreen`,
  pixel-identical.
- **Driver's Eyes** (namcos21_de.cpp): priority 15 sprites (above the 3D)
  are narrowed, but almost all the 2D (menus, speed) is in priority 14,
  under the 3D, with the scenery: it stays stretched, no visible change.
  Without `-widescreen`, pixel-identical.

### Continued, 2026-11-18: upstream merge, GPU bench, NWK-TR crash

- **Merge** of upstream MAME (172 commits, afacf58514f). Only conflict:
  f2mc16.cpp, where hap's interrupt fix (71e966694cc) supersedes ours
  (1272353226d); upstream's file is taken as is. Upstream also loads the
  System 22 point ROM as a 32-bit region (`m_pointrom` is now a region
  pointer); our code only goes through `point_read`. Built in one go
  (mame0289-2535-gafacf58514f).
- **Bench** on the merged build, native then Vulkan ×2, 46 games (the 40 of
  the matrix plus ridgera2, victlap, cybrcycc, alpinerd, wingwar, netmerc,
  never tried before; wingwar and netmerc with `hd44780_a00.zip` copied as
  `hd44780.zip` in an extra rompath): 43 with exit code 0, no black frame.
  sfrush: the known last-capture spike. xtrial: race starts differently
  than native (known).
- **racingj, racingj2, thrilld crashed on the first frame** (native too):
  nwk-tr.cpp drew the front 2D through `m_widescreen`, which only exists
  with anamorphic `-widescreen` (26ee852cdcb, in release g0777442). Fixed in
  ba1757c914a; native, ×2 and ×2 `-widescreen` run with exit code 0.
- README-mame3d.txt: list of the games tested with GPU acceleration.

### Continued, 2026-11-19: what Battle Gear 1 and 2 (Taito Type Zero) lack

Not mame3d scope yet (MAME marks them not working); looked at on the owner's
request. ROMs copied locally: taitotz, batlgear (+ batlgear.chd), batlgr2
(+ bg2_204j.chd). Only e68-01.ic7 is missing (sound CPU internal ROM, no dump
known).
- **Boot**: both load from the CHD and stop on "RANKING DATA LOST, PUSH
  TEST-SW". Test initialises the backup data and deletes the replay files on
  the disk (a few minutes emulated).
- **batlgr2 after that**: full attract mode with its 3D (intro, title, demo
  race, car showcase, rankings) at about 200 % speed unthrottled. Test menu
  works; the switch test sees coin, service, shift up/down and view. Shift
  up moves the menu cursor down, Start selects.
- **Blocker 1, disk loading**: the PPC asks the TLCS-900 for each sector
  (command 0x1010 in the common RAM) and polls for the answer
  (0x40101f04-0x40101f28). The handshake goes through the 120 Hz scheduler
  quantum: about one sector per frame. After a coin the game stays on the
  ranking screens for minutes; in a 560 s run it never left the last ranking
  page (no credit shown, no race). An experiment with `perfect_quantum` on
  each handshake broke the boot ("DUMMY PROGRAM IS RUNNING"), reverted. A fix
  needs the real TLCS/PPC synchronisation (or a faster HDD path).
- **Blocker 2, sound**: MN1020819DA sound CPU (internal ROM undumped) with
  ZOOM ZSG-2 / ZFX-2; the BIOS is patched to skip the sound load timeout.
- **Also missing**: the IEEE1394 LAN (link play); the renderer has fixed
  lighting (white diffuse/specular, one light), additive blending off and
  the texture switch bits unknown.
- batlgear (BIOS 1.11a): only the boot and the warning screen were seen.

### Continued, 2026-11-22: validity box, vr 16:9 frame, upstream merge

- **vr in a 16:9 frame on a 4:3 screen** (cabinet clone at 640x480):
  upstream's vr.lay only has a "Wide (16:9)" view, picked by `view auto`.
  Not a mame3d issue; on 4:3 screens use `view  Screen 0 Standard (4:3)`
  (vr.ini, set by Build-Config on the cabinets' 4:3 profiles).
- **Validity box at exit** (game picked from MAME's system list): the
  startup check of the shared source failed on the SWTPC 8212 keypad names
  with arrows. MSVC with `/Zc:u8EscapeEncoding` (genie.lua since
  a511e3a4e81) stores `↑` in a u8 literal as the single byte 0x91;
  hex escapes are code units with that flag. Fixed in swtpc8212.cpp
  (5f94ff07b1b): no racing driver fails `-validate` any more (2395 failing
  drivers down to 950, the rest are home computer keyboards with the same
  cause).
- **Merge** of upstream (43 commits, 113eb15ff5b). Conflict in namcos22.h:
  upstream moved the Super System 22 sprites to `namcos22s_state` (VICS
  copies to sprite RAM at vblank, 16-bit sprite RAM); our declarations kept
  only the text layer mix signature and `sci_int_w`. Dangerous Curves now
  runs with its 3D upstream (DSP routines patched in, clip flag registers):
  race reached, Vulkan x2 equal to native. Bench native/x2: dangcurv,
  dirtdash, alpinerd, cybrcycc, raverace, ridgerac, ridgera2, acedrive,
  victlap, sidebs, vr, sf2049 all exit 0, no black frame.

### Continued, 2026-11-23: upstream merge (Taito JC cracks, Voodoo chroma range)

- **Merge** of upstream (21 commits, d07b1397619), no conflict.
  - fc1e9763dd0 (taitojc.cpp): the DSP's line intersection read now floors
    the offset so a shared edge gives the same point both ways. Side by
    Side 2 `-widescreen` demo, 71 captures old vs new exe: the thin blue
    cracks across walls (cherry tree scene) are gone; the other differences
    are edge pixels of polygons cut at the screen sides. Vulkan x2 still
    equal to native (median 2.8). Whether this is the "road edge strip"
    hole of 2026-11-14 is not confirmed.
  - 6e95bdc80b6 (voodoo): the Banshee chromaRange register kept 28 bits,
    dropping the enable bit 28; now 29 and latched per triangle. Our Vulkan
    path already reads the register per triangle and the shader handles the
    range. Old vs new native: sf2049, sf2049se, cartfury, thrild2, gticlub2
    identical (xtrial varies per run, RTC), so no visible case found yet.
- Bench native/x2 on the new exe: sf2049, sf2049se, cartfury, thrild2,
  xtrial, gticlub2, dangcurv, sidebs exit 0, no black frame.

### Continued, 2026-11-23 (evening): no-nag lost and restored

- The no-nag change (`show_warnings = false` in
  `mame_ui_manager::display_startup_screens`, Endprodukt 67b9f588841) was
  lost in the upstream merge afacf58514f (2026-11-18): upstream set the same
  line by mistake in 868c260f62b (merged on 2026-11-12, so it became the
  merge base) and reverted it in 7d2d1bc35c3; the merge then took the
  revert. Releases gf7e5713, gead95f7 and g7c5dda3 show the warning screen
  again; thrild2 and raceon wait on it for a key (no frame emulated in 25 s
  in a d3d window). Restored, with a comment; thrild2, raceon and vr reach
  frame 60 at once. After each upstream merge, check
  `grep "bool show_warnings" src/frontend/mame/ui/ui.cpp`.

### Continued, 2026-11-27: netlist crash, bgfx crash, Road Burners speed

- Netlist games (rallyx, nrallyx, 280zzzap, speedfrk, monacogp, pong)
  crashed at reset in `nld_solver::fb_step` with MSVC: plib's INTERNAL_MSC
  member function pointer decoding gave a bad object. `PPMF_FORCE_TYPE 0`
  for MSVC in `src/lib/netlist/plib/ppmf.h` (e54b9875650).
- calspeed (and at times roadburn) crashed with `-video bgfx -hires3d 3`:
  `bgfx_util::mame_texture_data_to_bgfx_texture_data` subtracted the width
  margin and copied rowpixels x height in one block, i.e. from column 0,
  while our direct frames (bitmaps over the Vulkan read-back memory, every
  `*_vk.cpp`) start at the first visible column: it read 276 pixels before
  the buffer. Now copied row by row from the margin (840e091f80f). d3d
  copies row by row and never crashed.
- roadburn at 90.7% on Racecab2 (others 97-100%). Findings:
  - No idle loop worth a hotspot: the vblank wait polls the Voodoo status
    (`800BB704` -> `800BB6E4`, bit 6) only about 165 times a frame, about
    4% of the emulated time (each read eats 1000 cycles).
  - The MIPS DRC options (strict verify, extra instruction check, none)
    make no measurable difference.
  - The Vulkan Voodoo restarts the native rasterizer once, at start-up.
  - Stack samples (cdb) of the main thread: about 14% ADSP-2115 (DCS2),
    15% PCI accesses to the Voodoo, and the CHD: `chd_file::read_hunk`
    and LZMA decoding. Counted inside MAME: 250,000 hunk decompressions
    for 26,000 distinct hunks in 120 emulated seconds, 12.7 s of host
    time (about 18%). calspeed and sf2049 read mostly new hunks.
  - Fix: `chd_file::set_hunk_cache` (CLOCK cache of decompressed hunks,
    slots allocated 1024 at a time), set to 256 MB by `hard_disk_file`.
    roadburn: 47,700 decompressions instead of 234,000, 3 s instead of
    12.7 s. Checked by decompressing every cache hit again and comparing:
    no difference. `write_hunk` drops the cached copy.
  - The laptop's speed varies too much (92 to 150% for the same exe) to
    measure the gain here; to be measured on a cabinet.
  - The emulation is not exactly repeatable: two runs of the same exe
    read slightly different hunks (host timing, e.g. the Voodoo threads).
