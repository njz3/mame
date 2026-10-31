# mame3d — suivi d'avancement

Branche `dev/mame3d` (partie de `dev/ffdedition`). But : rendre la 3D des jeux de
course 1990-2010 à n'importe quelle résolution, d'abord en logiciel puis sur la
carte graphique (Vulkan), en gardant l'émulation MAME de tout le reste : CPU,
DSP, géométrie, son, I/O, link et 2D.

## Périmètre

Exclus, car déjà rendus nativement en 3D par d'autres émulateurs :

- Sega Model 2 : sm2-emu
- Sega Model 3 : Supermodel
- Naomi, Naomi 2, Atomiswave (PowerVR2) : Flycast
- Hikaru : hikaruloader

Matériel visé, dans l'ordre :

| # | Matériel | Jeux | Rendu logiciel HD | Rendu Vulkan |
|---|---|---|---|---|
| 1 | Sega Model 1 | vr, vformula | OK (vr ×2, ×4) | OK (vr ×2, ×4) ; relecture lente |
| 2 | Namco System 22 / Super 22 | ridgerac, ridgera2, raverace, acedrive, victlap, cybrcycc, dirtdash, alpinerd | OK (ridgerac, dirtdash ×2) | à faire |
| 3 | Konami K001005 | gticlub, midnrun, windheat, jetwave | OK (gticlub, midnrun ×2) | à faire |
| 4 | 3dfx Voodoo 1/2 | sfrush, sfrushrk, calspeed, hyprdriv, vaportrx, roadburn, thrilld, racingj | à faire | à faire |
| 5 | Midway V-Unit | crusnusa, crusnwld, offroadc | 2D OK (crusnusa ×2) ; 3D à vérifier en démo (sommets entiers : bords natifs, textures HD) | à faire |
| 6 | Midway Zeus 2 | crusnexo | à faire | à faire |
| 7 | Namco System 21 | winrun, driveyes | à faire | à faire |
| 8 | Taito JC (TC0780FPA) | sidebs, sidebs2, dangcurv | à faire | à faire |
| 9 | Gaelco 3D | speedup, surfplnt, radikalb | à faire | à faire |
| 10 | SNK HNG64 | roadedge, xrally | à faire | à faire |
| — | Namco System 23 | motoxgo, raceon, finfurl | quand l'émulation de base marchera | |

## Architecture

1. **Sortie haute résolution** : `src/emu/hires3d.{h,cpp}`, avec l'option
   `-hires3d N` (1 à 16, 1 = natif).
   - Le driver dessine l'image dans `frame_bitmap()` (taille native × N), puis
     appelle `frame_done()`.
   - `screen_device::video_output_update()` donne alors cette image au moteur
     d'affichage à la place de l'image native.
   - L'émulation, la synchronisation et les mises à jour partielles ne changent
     pas.
   - `screen.h` n'est pas modifié, ce qui évite une recompilation complète.
2. **2D** : chaque driver dessine ses tilemaps et sprites à la résolution
   native. `upscale()` les agrandit au plus proche dans l'image HD, en sautant
   les pixels transparents (alpha 0) pour les couches au-dessus de la 3D.
3. **3D logicielle HD** : le rasteriseur de chaque puce dessine directement dans
   l'image HD, avec des sommets mis à l'échelle à partir de coordonnées
   flottantes conservées avant l'arrondi au pixel natif.
4. **3D Vulkan** : `src/emu/vk3d.{h,cpp}`, avec l'option
   `-hires3d_renderer auto|vulkan|software` (défaut : auto).
   - Rendu hors écran au format de pixel de `bitmap_rgb32` (B8G8R8A8), avec
     tampon de profondeur D32. Le driver enregistre des lots de triangles en
     pixels HD, puis `end_frame()` rend l'image, la relit dans l'image HD et
     suit le même chemin que le rendu logiciel. Ça marche avec tous les moteurs
     d'affichage, les captures et l'enregistrement vidéo.
   - `vulkan-1.dll` est chargée au lancement par `osd::dynamic_module`. Sans
     Vulkan, `hires3d_output::vulkan()` renvoie nullptr et le driver dessine en
     logiciel. Un échec en cours de partie fait aussi revenir au logiciel.
   - Pipelines fournis : `PIPE_COLOR`, `PIPE_COLOR_STIPPLE` (damier, taille de
     case dans `p`), `PIPE_TEXTURE_KEY` (texture, alpha 0 transparent). Un
     driver peut ajouter les siens avec `add_pipeline()` (SPIR-V, mélange,
     profondeur).
   - Les en-têtes Vulkan viennent de `3rdparty/bgfx/3rdparty/khronos/vulkan-local`.
   - Shaders : `src/emu/vk3d_shaders/*.vert|frag`, compilés en SPIR-V dans des
     fichiers `.spv.inc`, qui sont versionnés :

     ```
     glslc --target-env=vulkan1.0 -O -mfmt=num -o X.spv.inc X
     ```

     (`C:\VulkanSDK\1.4.363.0\Bin\glslc.exe`)
   - Plus tard : partage direct de la texture avec bgfx, sans relecture.

## Compilation

- Build réduit pour itérer vite (dossier séparé `build3d`, exe `mame3d.exe`) :

  ```
  make vs2022 BUILDDIR=build3d SUBTARGET=3d SOURCES=src/mame/sega/model1.cpp,...  PTR64=1 NOWERROR=1 PYTHON_EXECUTABLE=<python Windows>
  msbuild build3d\projects\windows\mame3d\vs2022\mame3d.sln /p:Configuration=Release /p:Platform=x64 /m:2 /p:CL_MPCount=1 /p:VcpkgAutoLink=false
  ```

  `SOURCES` = model1, namcos21, namcos21_de, namcos22, namcos23, gticlub,
  zr107, nwk-tr, seattle, vegas, midvunit, midzeus, taitojc, gaelco3d, hng64.
- Le build complet (`build\...\mame.sln`) reste celui des releases.

## ROMs

On copie seulement ce qui est nécessaire, au fur et à mesure, depuis
`\\192.168.1.100\toshiba2t\MAME ROMs (merged)` (et `MAME CHDs (merged)`) vers
`C:\Users\benja\Desktop\Perso\Racecab\roms`, sur ce PC uniquement, jamais sur
les bornes. Le `vr.zip` déjà présent contient vformula et epr-15111.12 : ne pas
l'écraser.

## Journal

- 2026-10-31 : branche `dev/mame3d` créée. Inventaire des puces 3D fait. Ajout
  de l'infrastructure `hires3d` et de l'option `-hires3d`. Model 1 :
  - rendu logiciel HD : positions projetées gardées en flottants, découpage du
    viewport mis à l'échelle, fils de fer d'un pixel natif d'épaisseur, HUD
    agrandi et masque HUD calculé sur l'image HD ;
  - rendu Vulkan : fond et HUD en textures, quadrilatères en paires de
    triangles dans l'ordre trié, damier en shader, passe « au-dessus du HUD »
    en logiciel sur l'image relue.
- 2026-10-31 : rendu logiciel HD écrit (compilé, pas encore testé) pour trois
  matériels.
  - **Namco System 22 / Super 22** : sommets et découpage mis à l'échelle pour
    les polygones et les sprites. Ils sont dessinés avec leur propre carte de
    priorité HD. Le texte reste natif : ses marques de priorité sont copiées
    dans la carte HD, puis le mélange (ombre, spot, fondu, gamma) se fait par
    pixel HD.
  - **Konami K001005** : tampons du K001005 agrandis × N (option lue au
    démarrage du device). `draw(bitmap, clip, scale)` sous-échantillonne pour
    l'écran natif ou copie 1:1 en HD. Côté drivers (GTI Club, Midnight Run,
    Winding Heat, Jet Wave), la tilemap de fond est agrandie, la 3D copiée en
    HD, la tilemap de devant agrandie avec transparence.
  - **Midway V-Unit** : la mémoire vidéo native est gardée pour l'émulation.
    Une copie HD des deux pages reçoit les mêmes quads (centres de pixels natifs
    → centres de blocs HD) et les écritures CPU agrandies en blocs. L'affichage
    HD convertit cette copie par la palette. Les sommets fournis par le jeu
    sont entiers : les bords restent ceux du natif, mais les textures et le
    tramage sont calculés par pixel HD.
- ROMs copiées : ridgerac, raverace, acedrive, dirtdash, alpinerd, namcoc71,
  namcoc74, gticlub, midnrun, windheat, crusnusa, crusnwld, tms320c31.
- 2026-10-31 : premiers tests avec `3d.exe` (build `build3d`).
  - **Model 1 (vr)** : ×4 logiciel et ×4 Vulkan donnent la même image en
    1984×1536.
    - Défaut corrigé : en Vulkan, les petits quads lointains réduits à deux
      points en natif étaient dessinés comme des fils de fer. Le test se fait
      maintenant sur les positions HD, comme en logiciel.
    - Le masque HUD (« au-dessus du HUD ») n'est calculé que si la liste
      d'affichage contient des objets 0x41.
    - `upscale()` agrandit une ligne puis la recopie.
    - Temps pour 25 s émulées (`-nothrottle`, RTX 3050 portable) : natif
      environ 19,5 s, logiciel ×4 25,5 s. En Vulkan ×4, l'attente du GPU et le
      transfert prennent 6,3 ms par image, la copie 1,9 ms : il faut rendre le
      GPU asynchrone, ou passer l'image à l'affichage sans relecture.
  - **System 22** : ridgerac et dirtdash corrects en ×2.
  - **K001005** : gticlub et midnrun corrects en ×2.
  - **V-Unit** : il y avait deux défauts.
    - Le jeu fait ses fondus par la palette seule. Le chemin HD convertit donc
      la page à chaque mise à jour et signale toujours un changement.
    - Sur un écran indexé, `palette.pens()` renvoie des numéros de couleur, pas
      des couleurs : il faut lire `palette()->entry_list_adjusted()`. Ce point
      vaudra aussi pour System 21, Taito JC et HNG64.
    - Cruis'n USA démarre sur la calibration et la NVRAM est vide ; il faudra
      une NVRAM calibrée pour vérifier la 3D en démo.
  - Bug sans lien avec mame3d, présent aussi dans le `mame.exe` de
    `dev/ffdedition` : midnrun et windheat (driver zr107) plantaient à la
    sortie après plus de 10 s. **Corrigé** par le commit 60f3007783b sur
    `dev/ffdedition`, fusionné dans `dev/mame3d`.
    - Cause : sous Windows, le DRC x64 range ses registres flottants UML dans
      XMM6-XMM15 mais ne les sauvegardait pas au point d'entrée.
    - MSVC garde une constante dans XMM6 pendant `machine.run()` ; elle
      revenait corrompue, d'où une chaîne vide invalide dans
      `set_system_name("")`.
    - Le bug existe aussi dans MAME upstream.

## Prochaines étapes

1. Vulkan : deux images en vol (fence, commandes, relecture et sommets doublés),
   puis passage direct de l'image à bgfx.
2. Zeus 2 (crusnexo) : copie HD du tampon couleur et profondeur, avec
   l'adressage `frame_addr_from_xy`, les remplissages et `frame_write`.
3. Voodoo 1/2 : sommets 12.4, recalcul des gradients ; relectures LFB par le
   CPU sur le tampon natif.
4. System 21, Taito JC, Gaelco 3D, HNG64.
5. Rendu Vulkan de System 22 et K001005 (shaders propres à chaque puce).
