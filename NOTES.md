# mame3d — suivi d'avancement

Branche `dev/mame3d` (partie de `dev/ffdedition`). But : rendre la 3D des jeux de
course 1990-2010 à n'importe quelle résolution, d'abord en logiciel puis sur la
carte graphique (Vulkan), en gardant l'émulation MAME de tout le reste : CPU,
DSP, géométrie, son, I/O, link et 2D.

Le rendu Vulkan n'est pas un bonus : le but est aussi de faire travailler la
carte graphique pour soulager le CPU de la machine qui fait tourner MAME. Pour
chaque puce, on traduit donc ses commandes 3D (triangles, états de mélange,
profondeur, textures) en appels Vulkan. Le rendu logiciel HD sert de référence
pour vérifier l'image, et de repli sans Vulkan ou pour les cas non traduits.

## Périmètre

Exclus, car déjà rendus nativement en 3D par d'autres émulateurs :

- Sega Model 2 : sm2-emu
- Sega Model 3 : Supermodel
- Naomi, Naomi 2, Atomiswave (PowerVR2) : Flycast
- Hikaru : hikaruloader

Sega Model 1 (vr, vformula, ...) est de nouveau suivi (décision du propriétaire,
2026-11-10) : il a son rendu Vulkan en haute résolution ; restent le partage
avec bgfx et le 16:9, MAME y faisant la projection 3D.

Matériel visé, dans l'ordre :

| # | Matériel | Jeux | Rendu logiciel HD | Rendu Vulkan |
|---|---|---|---|---|
| 1 | Namco System 22 / Super 22 | ridgerac, ridgera2, raverace, acedrive, victlap, cybrcycc, dirtdash, alpinerd | OK (ridgerac, dirtdash ×2) | OK (5 jeux ×2) ; plus rapide que le natif |
| 2 | Konami K001005 | gticlub, midnrun, windheat, jetwave | OK (gticlub, midnrun ×2) | OK (midnrun, gticlub) ; proche du natif |
| 3 | 3dfx Voodoo 1/2 | sfrush, sfrushrk, calspeed, hyprdriv, vaportrx, roadburn, thrilld, racingj, racingj2 | OK (sfrush ×2 ×4, racingj ×2, roadburn ×2) ; coûteux en CPU | OK (même image que le logiciel) ; au niveau du natif en ×2 sur roadburn |
| 3b | 3dfx Banshee / Voodoo 3 | sf2049, sf2049se, cartfury (et viper, nbashowt… non essayés) | OK (sf2049, cartfury ×2) | OK (même image que le logiciel ; ×4 sf2049 130 s en 65 s) |
| 4 | Midway V-Unit | crusnusa, crusnwld, offroadc | OK (3 jeux ×2 en démo 3D ; sommets entiers : bords natifs, textures HD) | OK (même image que le logiciel) ; ×4 60 s contre 81 s en logiciel |
| 5 | Midway Zeus 2 | crusnexo | OK (crusnexo ×2, ×4, mode démo) | OK, vrais triangles, plus de raster CPU ; temps CPU ×2 52 s contre 92 s en natif |
| 6 | Namco System 21 | winrun, driveyes | OK (winrun, driveyes ×2) | OK, vrais quads, plus de raster CPU ; winrun, temps CPU ×4 14,5 s contre 23 s en natif |
| 7 | Taito JC (TC0780FPA) | sidebs, sidebs2, dangcurv | OK (sidebs, sidebs2 ×2) | OK, vrais triangles et quads, plus de raster CPU ; temps CPU ×4 84,5 s contre 86 s en natif |
| 8 | Gaelco 3D | speedup, surfplnt, radikalb | OK (speedup, radikalb ×2) | OK, vrais triangles, plus de raster CPU ; temps CPU ×2 18 s contre 52 s en natif |
| 9 | SNK HNG64 | roadedge, xrally | OK (roadedge, xrally ×2) | OK, vrais triangles, plus de raster CPU, 2D au-dessus mélangée sur le GPU ; temps CPU ×4 75 s contre 88 s en natif |
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
   - Avec bgfx sur Direct3D 11, l'image n'est plus relue : voir le point 7.
5. **Contexte Vulkan commun** : `src/emu/vk3d_context.{h,cpp}` charge Vulkan,
   crée le périphérique (avec les options demandées que le GPU possède) et
   fournit les aides tampons, images et shaders. `vk3d_renderer` et les rendus
   propres à une puce en héritent.
6. **Rendus GPU propres à une puce** : quand le modèle « lots de triangles »
   de `vk3d_renderer` ne suffit pas, la puce a son propre rendu qui traduit
   ses commandes. Premier cas : `src/devices/video/voodoo_vk.{h,cpp}` (voir le
   journal Voodoo). Ses shaders sont dans `src/devices/video/voodoo_vk_shaders`
   et se compilent avec `-I .` pour `layout.h`, partagé avec le C++ :

   ```
   glslc --target-env=vulkan1.0 -O -mfmt=num -I . -o X.spv.inc X
   ```

7. **Image partagée avec bgfx** (`-bgfx_hires3d_share`, actif par défaut) :
   l'image finie reste sur le GPU.
   - `src/osd/modules/render/bgfx/sharedtexture.{h,cpp}` crée des textures
     Direct3D 11 partagées par handle NT et les substitue aux textures bgfx
     (`overrideInternal`), comme texture d'écran des chaînes ou du quad simple.
   - `src/emu/vk3d_share.{h,cpp}` les importe dans le périphérique Vulkan de
     chaque puce (`VK_KHR_external_memory_win32`, Vulkan 1.1, même adaptateur
     vérifié par le LUID) et y copie l'image finie au lieu de la relire.
   - `hires3d_output::use_frame()` reçoit la texture ; bgfx la dessine, et la
     relecture ne se fait plus que pour les captures et l'enregistrement
     (`read_gpu_textures()` dans `video_manager::create_snapshot_bitmap`).
   - bgfx tourne alors sur le thread de l'émulation (`bgfx::renderFrame()`
     avant `bgfx::init()`) : son périphérique D3D11 est créé mono-thread, et
     les textures sont créées et suivies depuis l'émulation. Avec le thread de
     rendu de bgfx, le pilote NVIDIA plantait ou bloquait.
   - Synchronisation par le CPU : la puce attend son lot avant que bgfx dessine
     (rappel `ready`), et ne réécrit une texture qu'une fois les dessins qui la
     lisent finis (anneau de requêtes D3D11 ; sinon l'image est relue).
   - Branché sur System 22, System 21, Taito JC, HNG64, Gaelco 3D, K001005 et
     Voodoo (1/2, Banshee, Voodoo 3), V-Unit et Zeus 2 (2026-11-09). Pas
     encore : Model 1.
8. **Options d'affichage**, comme dans les autres émulateurs des bornes
   (`render.cpp`, `place_view()`) :
   - `-aspect_ratio original|4:3|16:9|4:3_stretch|16:9_stretch|4:3_crop|16:9_crop`
     (`4:3_letterbox` = `4:3`) : le cadre où l'écran est montré, centré avec des
     bandes noires ; l'écran y garde sa forme, le remplit, ou le couvre en
     étant coupé. `-crop_position -100..100` choisit la partie gardée.
   - `-widescreen` : image 16:9 pour les puces dont MAME fait la projection
     3D (System 22, HNG64, Zeus 2), sur le rendu Vulkan seulement, même en
     `-hires3d 1`. `hires3d_output` ajoute `pad()` colonnes HD de chaque côté
     de l'écran natif, et l'écran est montré en 16:9 (`view_widening()`).
   - `-widescreen_2d 4:3|stretch` : les couches 2D au milieu à leur forme, ou
     étirées sur toute la largeur.
   - `-widescreen_bg` : les tilemaps sous la 3D dessinées aussi sur les côtés
     (HNG64 ; System 22 n'en a pas, son fond est la couleur de fond ou la 3D).

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

Toutes les puces de la liste ont un rendu logiciel HD et un rendu Vulkan
(2026-11-02). Reste :

1. Essayer le partage avec bgfx sur une carte AMD ou Intel. Mesures sur la
   RTX 3050 du portable, en ×4, fenêtre sans limite de vitesse : crusnusa
   441 % contre 287 % en relecture, crusnwld 338 contre 245, offroadc 442
   contre 294, crusnexo 306 contre 223. Le gain est faible ou nul en ×2.
   Intel UHD de ce PC (2026-11-09, Vulkan et bgfx forcés dessus) : crusnexo,
   crusnwld, xrally, ridgerac, midnrun, sf2049, winrun, radikalb et sidebs
   tournent, images identiques à NVIDIA (0,13 % au pire sur sf2049). Plantage
   rare au démarrage de sfrush corrigé : l'adresse fautive était 184 pixels
   avant une image relue, l'écart du `xoffs` natif. Si la zone visible change
   entre la préparation et l'affichage, `update_texture` calculait la zone
   actuelle ; il prend maintenant celle de l'image. Le cas n'est pas réapparu
   sur 42 lancements, sans plantage, donc le correctif n'a pas été exercé
   directement. `frames_freed` copie aussi une image montrée dont la mémoire
   est libérée. Captures noires isolées de ridgerac et radikalb, pas d'image
   noire dans les captures du jeu. AMD non testé.
2. HNG64 : les lignes 2D sont gardées sur le GPU, seules celles qui changent
   partent (fait, 2026-11-09 : 23 à 33 % des lignes, temps d'envoi divisé par
   2,4, attente -15 %, images identiques ; pas de gain de vitesse mesurable).
   Course xrally en ×2, sans affichage : 5,1 ms d'attente par image avant ce
   changement (relecture 47 %, envois 30 %, dessin 22 %). Le K001005 (midnrun)
   n'attend que 0,5 ms par image : rien à faire.
   `-widescreen_bg` avec `-widescreen_2d stretch` : les couches sous la 3D sont
   étirées quand elles ne dépassent pas de l'écran 4:3 (menus de roadedge et
   xrally), et restent en place sinon (course).
   **Correctif MIPS** : sfrush se figeait après 49,5 s (toutes nos
   versions depuis g8e81ca4). Le commit upstream fde5b471bfc arme le compare
   sur une écriture de Count ; Compare reste à 0xffffffff, le jeu n'y écrit
   jamais et IP7 reste levée quand Count boucle. L'armement est retiré
   (`mips3.cpp`, `mips3drc.cpp`). sfrush continue après 50 s, en démo et avec
   pièce et accélérateur ; xrally est identique sur 45 s et continue après
   50 s, roadedge et cartfury aussi ; sf2049 continue (sa démo n'est pas
   déterministe, même sur l'ancienne version). dev/ffbedition a le même défaut.
3. Banshee/Voodoo 3 : sf2049se essayé jusqu'en course, correct (2026-11-09).
   Restent les jeux non-course (Viper, nbashowt) ; les blits 2D entre tampons
   perdent la HD (aucun blit lisant un tampon sur 100 s de sf2049 et cartfury).
   Voodoo : le rastériseur natif tourne encore pendant le jeu, relancé par les
   lectures du CPU (sur 100 s : sfrush fbiPixelsIn ×2151 et tampon ×4122, soit
   2,4 cœurs ; sf2049 les 5 registres de stats ×528, 1 cœur ; cartfury tampon
   ×945, 0,4 cœur). Pour l'arrêter : compter les pixels sur le GPU et relire
   les zones lues. Pas nécessaire pour la vitesse (177 à 376 % en ×2).
   Zeus 2 : plus de reprise logicielle mesurable (crusnexo).
   À supporter (demande du propriétaire, 2026-11-10) : thrilld (Hornet, Voodoo)
   et, sur Viper (Voodoo 3), thrild2 et xtrial. Tous « not working » dans MAME : voir
   d'abord ce qui les bloque ; ROM pas encore copiées, Viper jamais essayé.
4. System 23, Taito Type Zero et Konami Cobra, quand leur émulation marchera.
   Demandé par le propriétaire (2026-11-10) : le GPU PS1 (`psxgpu`), pour les
   systèmes dérivés de la PlayStation (Namco System 11 : pocketrc, System 12 :
   kartduel, ...). Sommets entiers : plus de résolution, mais les polygones
   tremblent toujours.
5. HNG64 en logiciel aux échelles qui ne sont pas des puissances de 2 (×3,
   ×5…) : remplacer les masques `taille - 1` des rastériseurs et de
   `blit_3d` (le chemin GPU est déjà corrigé).
- 2026-10-31 : **Namco System 21** en logiciel HD.
  - `namcos21_3d` : tampons × N, sommets (entiers) mis à l'échelle, ce qui
    affine les arêtes. Une copie native de la profondeur visible reste
    disponible pour le mélange des sprites de Cyber Sled.
  - Winning Run : mélange en index à la résolution HD. `bitmap_draw` lit la
    2D native pour chaque pixel HD, ombres comprises, puis la conversion passe
    par la palette ajustée (`hires3d_output::indexed_to_frame`).
  - Driver's Eyes : sprites et mélange en natif, 3D copiée en HD. Seuls les
    pixels que les sprites de priorité 15 modifient (ombres sur la 3D) sont
    reportés en blocs.
  - ROMs copiées : winrun, driveyes, namcoc65, namcoc68.
- 2026-10-31 : **Taito JC** en logiciel HD : tampons du TC0780FPA × N, sommets
  (entiers) mis à l'échelle. Mélange en index : objets du fond agrandis, 3D
  HD, objets du dessus et texte agrandis avec la valeur sentinelle 0xffff
  comme transparence.
- 2026-10-31 : **vitesse de Side by Side**, corrigée sur `dev/ffdedition`
  (efc1acdba1e, ba474f170be) puis fusionnée ici.
  - Avant : la course de démo tournait à 8-10 images 3D/s.
  - Durées FPU propres au 68040 dans le cœur 68k : environ 20 images/s.
  - Horloge du 68040 × 1,5 : 30 images/s stables (une image sur deux).
  - Idle skip sur la boucle d'attente du 68040, qui scrute le mot d'état du
    DSP : vitesse non limitée de 102 % à 144 % (sidebs) sur le portable.
- 2026-10-31 : **Gaelco 3D** en logiciel HD. Le DSP fournit des équations de
  plan par pixel (u/z, v/z, 1/z) : leurs gradients sont divisés par N, les
  sommets multipliés par N, le tampon image et le z-buffer sont agrandis × N.
  Le tampon est rangé tête en bas, il est retourné en copiant vers l'image HD
  et échantillonné pour l'écran natif.
- 2026-10-31 : **SNK HNG64** en logiciel HD.
  - Tampons 3D (index, lumière, bit de mélange) de 512 × N de côté ; sommets
    et découpage mis à l'échelle.
  - `blit_3d()` sert pour l'écran natif (échantillonné) et pour l'image HD
    (étirement vertical et défilement x mis à l'échelle, mélange 50 %).
  - Couches du fond agrandies. Couches du dessus mélangées en natif ; seuls
    les pixels qu'elles changent sont reportés en blocs.
  - Les images sans 3D (écran coupé, transitions) sont publiées agrandies.
  - `hng64.zip` (BIOS) remplacé par celui du partage, qui ajoute trois PROM
    de configuration (mêmes CRC pour le reste).
- 2026-10-31 : **Midway Zeus 2** en logiciel HD.
  - Copie HD du tampon couleur et profondeur, avec la même structure de lignes
    (512 ou 1024 pixels selon `m_yScale`) × N.
  - Le rastériseur écrit par pointeur de début de fenêtre et longueur de
    ligne, ce qui sert au tampon natif comme au tampon HD.
  - Polygones dessinés dans les deux, sommets et découpage × N.
  - Remplissage rapide et `frame_write` recopiés en blocs ; l'affichage HD lit
    la copie.
  - Test : Cruis'n Exotica, calibration passée par script (5 appuis sur
    « Service Mode »), mode démo 3D correct en ×2.
  - ROMs copiées : crusnexo, tms320c32.
- 2026-10-31 : **3dfx Voodoo 1/2** en logiciel HD (Banshee et Voodoo 3 non
  concernés).
  - Copie HD de toute la mémoire image : une ligne de `rowpixels` pixels
    natifs devient N lignes de `rowpixels` × N pixels, avec une marge de chaque
    côté. Les tampons couleur et profondeur y gardent leur adresse.
  - Longueur de ligne et origine Y passées par polygone (`poly_data`) au lieu
    de l'état global du rastériseur, pour dessiner chaque triangle une seconde
    fois dans la copie HD.
  - Triangle HD : sommets et découpage × N, gradients ÷ N, valeurs de départ
    recentrées sur le pixel HD quand l'ajustement sous-pixel est actif. Un
    triangle qui sortirait de la copie HD n'y est pas dessiné.
  - Remplissages rapides refaits en HD. Écritures LFB (brutes ou par le
    pipeline) et remplissage par blit du Voodoo 2 recopiés en blocs.
  - Affichage HD lu dans la copie du tampon avant. `hires()` permet au driver
    de reporter les pixels que sa 2D change : NWK-TR et Hang Pilot (K001604)
    le font, avec la nouvelle `hires3d_output::magnify_changes()`.
  - Tests : sfrush ×2 et ×4, racingj ×2 (texte K001604 par-dessus), roadburn
    ×2 (Voodoo 2) : images correctes.
  - Coût CPU élevé : 110 s émulées de racingj en 109 s en natif et 210 s en
    ×2, 80 s de roadburn en 85 s en natif et 232 s en ×2. D'où la priorité au
    rendu Vulkan pour cette puce.
  - ROMs copiées : sfrush (+ CHD), racingj, roadburn (+ CHD).
- 2026-10-31 : **3dfx Voodoo 1/2 sur Vulkan** (`voodoo_vk`), choisi par
  défaut quand Vulkan est disponible (`-hires3d_renderer software` garde le
  rendu logiciel HD).
  - Le pipeline de pixels du Voodoo tourne dans un fragment shader
    (`tri.frag`). Il refait les valeurs itérées de chaque pixel avec la même
    arithmétique entière que le rastériseur logiciel (entiers 64 bits pour W
    et les textures), et lit les texels directement dans la mémoire des TMU,
    copiée sur le GPU page par page quand elle change. Il n'y a donc pas de
    cache de textures. Les tables de texels, palettes et tables de brouillard
    sont dans un tampon de consultation. Test de profondeur, mélange
    (« couleur avant brouillard » par double source) et ordre des pixels
    passent par les étages fixes du GPU.
  - Une image par tampon couleur et une image de profondeur, × N.
    Remplissages rapides et blits en effacements, écritures LFB en blocs.
  - Lots envoyés au GPU sans attendre (tous les 256 triangles), sur un
    anneau de 3. L'affichage attend la fin du lot courant et relit le tampon
    avant. La table de couleurs (CLUT) est appliquée en relisant, sauf si
    elle est linéaire.
  - Quand Vulkan dessine, le rastériseur natif est sauté, sauf pendant
    2 secondes après une lecture du tampon d'image par le CPU (sfrush en fait
    au démarrage, roadburn aussi). Le temps émulé du Voodoo vient alors de
    l'aire des triangles : la démo peut se décaler d'un peu par rapport au
    rendu natif, mais l'image d'une scène donnée est la même.
  - Un Voodoo qui n'est pas affiché ne dessine pas sa copie HD (Racing Jam
    en a deux).
  - Les paquets CMDFIFO de type 5 qui écrivent directement dans la mémoire
    d'image du Voodoo 2 sont recopiés en HD (logiciel et Vulkan).
  - Contrôle : écart moyen avec le logiciel HD de 0 à 1,6 sur 255, aucun
    pixel au-delà de 24 (0,01 % sur roadburn). Les différences viennent du
    tramage 565 que le GPU ne fait pas.
  - Temps sur le portable (RTX 3050), ×2, très variables d'un essai à
    l'autre : racingj 110 s émulées en 127 s (logiciel 194 s, natif 109 s),
    roadburn 30 s en 21,7 s (natif 14 s), sfrush 60 s en 22,7 s (natif 18 s).
    L'attente du GPU suit le nombre de pixels (3,4 ms par image en ×2,
    10,6 ms en ×4 sur roadburn) : c'est surtout la relecture de l'image et sa
    copie dans le bitmap.
  - Suite : relecture sans attente et image directe (entrée suivante).
- 2026-10-31 : **Voodoo sur Vulkan, relecture sans attente et image directe.**
  - MAME affiche à la vblank le tampon devenu avant à l'échange précédent.
    Juste après chaque échange, la relecture du nouveau tampon avant part donc
    sur le GPU sans attendre, et elle a une image entière pour arriver. Pas
    d'image de retard : l'image montrée est la même qu'avant.
  - Les relectures vont dans un anneau de 3 tampons. L'affichage montre la
    mémoire relue directement, sans copie, grâce à la nouvelle
    `hires3d_output::use_frame()`. La relecture montrée n'est pas réécrite
    avant l'image suivante.
  - Les drivers qui dessinent leur 2D par-dessus (NWK-TR, Hang Pilot, via
    `hires()`) gardent une copie, pour ne pas laisser d'ancien texte dans la
    mémoire relue.
  - La CLUT du DAC, quand elle n'est pas linéaire (roadburn), est appliquée
    sur le GPU par une passe plein écran vers une image d'affichage, qui est
    relue à la place du tampon couleur.
  - Quand seul le GPU dessine, le tampon natif n'est plus converti pour
    l'écran natif (il n'est ni dessiné ni montré). Le nombre de pixels estimé
    pour la chronologie du Voodoo tient compte du rectangle de découpage :
    sans lui, les grands triangles hors écran faisaient paraître la puce plus
    occupée et le CPU tournait davantage sur son état.
  - roadburn, 30 s émulées, meilleur de 3 essais alternés : avant cette étape
    16,5 s en natif contre 20,3 s en Vulkan ×2 ; après, 19,7 s contre 19,8 s
    (le portable chauffe, d'où les écarts). Attente GPU : de 3,5 s à 0,02 s,
    toutes les images montrées sans copie.
  - Reste : partager l'image avec l'affichage (bgfx) pour supprimer la
    relecture elle-même.
- 2026-10-31 : **Namco System 22 / Super 22 sur Vulkan** (`namcos22_vk`), même
  méthode que le Voodoo.
  - Le parcours de la scène triée envoie chaque quad (éventail de 3 à 6
    sommets après découpage en z) et chaque sprite au GPU, avec ce que les
    fonctions de rendu par ligne lisent de leurs données : palette, mode de
    couleur, banque de textures, ombrage, brouillard, fondus, alpha,
    priorité. Le GPU les dessine dans l'ordre du tri, sans tampon de
    profondeur, comme le matériel.
  - `scene.frag` reprend ces fonctions (une pour le System 22, une pour le
    Super 22, une pour les sprites). Il lit les textures (tilemap de
    textures, attributs, tuiles) et les sprites dans les ROM, envoyées une
    seule fois sur le GPU. Le mélange avec la destination passe par l'étage
    fixe. Une seconde sortie (image de priorité R8) marque les pixels
    couverts et le bit « au-dessus du texte » du dernier polygone.
  - `mix.frag` reprend `namcos22_mix_text_layer` (ombres, fondu, gamma des
    PROM) et `namcos22s_mix_text_layer` (spot, alpha du texte, fondu, gamma
    du mixeur), avec la couche de texte et sa carte de priorité natives,
    envoyées à chaque image. Sur le Super 22, le texte est aussi mélangé
    au fond avant la scène, comme dans le rendu logiciel.
  - Image relue et montrée sans copie (anneau de 3), comme le Voodoo. Le
    rendu se fait au moment de l'affichage et on attend le GPU (pas
    d'échange de tampons à anticiper sur ce matériel).
  - Il faut `independentBlend` (couleur mélangée, priorité écrite telle quelle).
  - Contrôle contre le logiciel HD, même image : ridgerac 2,5 % de pixels
    au-delà de 24, dirtdash 0,14 %, raverace 3,9 %, acedrive 1,2 %,
    alpinerd 1,6 %. Les écarts sont des bords de texels décalés d'un pixel
    (interpolation en float sur le GPU, en double sur le CPU).
  - Temps, ×2 : ridgerac 40 s émulées en 12,2 s (logiciel 42,9 s, natif
    14,8 s) ; dirtdash 60 s en 25,4 s (logiciel 84,3 s, natif 34,3 s) ;
    raverace 29,2 s (logiciel 101 s) ; acedrive 34,5 s (75 s) ; alpinerd
    31 s (100 s). Le GPU fait mieux que le rendu natif, qui rastérise sur
    le CPU.
- 2026-10-31 : **Konami K001005 sur Vulkan** (`k001005_vk`).
  - Le K001005 envoie ses triangles au GPU au lieu de les rastériser. Le
    shader `tri.frag` reprend `draw_scanline_generic` : texels de la ROM du
    K001006 (envoyée une fois) et sa palette (envoyée avec chaque lot),
    miroir, bilinéaire, lumière diffuse et ambiante, brouillard, couleur par
    sommet ou par polygone.
  - Le z-buffer flottant du matériel devient la profondeur du GPU : le Z est
    multiplié par 2^-40, ce qui ne change pas son ordre en flottant, avec
    `depthClamp` puisque la puce ne coupe pas en Z.
  - Les mélanges avec l'image (texture bilinéaire semi-transparente, puis
    brouillard, puis mélange du polygone) sont tous linéaires en la couleur
    de destination. Le shader calcule donc « A + K × destination » et la
    double source du GPU fait l'addition.
  - Deux pages et un z-buffer partagé ; l'échange de pages devient un
    effacement. Les lots partent sans attendre (tous les 1024 triangles et à
    chaque échange).
  - À l'affichage HD, GTI Club, Jet Wave et Midnight Run / Winding Heat
    font composer l'écran par le GPU : couche 2D du fond (native), pixels
    opaques de la page 3D, couche 2D du dessus (native). L'image relue est
    montrée sans copie. La composition n'envoie que sa propre passe, pour
    ne pas attendre les triangles de la page en cours. Quand rien n'a changé
    (même page, mêmes couches), l'image précédente est remontrée.
  - Contrôle : midnrun identique au logiciel HD (0,02 % des pixels
    au-delà de 24) ; gticlub identique sur ses écrans 2D, avec le décalage
    d'écran (40, 28). Sa démo 3D n'a pas été atteinte : l'appui sur Test
    pour passer « DEVICE ERROR (STEERING WHEEL) » ouvre le menu de test.
  - Temps, midnrun 60 s émulées en ×2 : 14,3 à 17,1 s en Vulkan, 12,7 s en
    natif, 27,5 s en logiciel. Il reste environ 1 ms d'attente par image
    composée (composition et relecture), sur environ 1500 images composées.
- 2026-11-02 : **Midway V-Unit sur Vulkan** (`midvunit_vk`).
  - La copie HD des deux pages de mémoire vidéo est sur le GPU (images R16
    de 512·N de côté). Les quads de la file DMA y sont dessinés par un port
    des rastériseurs par ligne (aplat, texture, texture transparente,
    texture masquée). Comme `poly_manager::render_polygon<4>` interpole par
    ligne le long des arêtes gauche et droite (quads non plans), le shader
    de fragments réévalue ces arêtes à sa ligne. Il reprend aussi les
    arrondis, le clip appliqué avant le calcul des paramètres, les
    coordonnées de texture entières et le tramage.
  - Les écritures CPU dans la mémoire vidéo sont groupées en segments de
    pixels voisins, dessinés en blocs N×N. Une écriture dans la mémoire de
    textures n'envoie le lot en cours que si elle touche la fenêtre de
    textures qu'il utilise.
  - Au changement de page, la page visible est convertie tout de suite par
    la palette dans une image d'affichage, relue sans attendre (3 tampons).
    À la mise à jour de l'écran, cette image est montrée sans copie si la
    page et la palette n'ont pas changé ; sinon les lignes sont converties
    dans l'ordre du dessin, ce qui garde les fondus par la palette.
  - Après un chargement d'état, les deux pages sont refaites depuis la
    mémoire native ; si Vulkan échoue, le rendu repasse en logiciel HD.
  - NVRAM calibrées pour crusnusa et crusnwld (Enter, c'est-à-dire Test,
    aux écrans « CALIBRATE CONTROLS ») : les deux jeux atteignent la démo
    3D. ROM copiée : offroadc.
  - Contrôle en ×2, 120 s de démo : crusnwld et offroadc identiques au
    logiciel sur 12 captures ; crusnusa identique sur 7 captures, à 4
    pixels près sur une ligne (arrondi flottant).
  - Temps pour 120 s émulées (portable, mesures assez dispersées) :
    crusnusa ×2 43,5 à 58,9 s en Vulkan, 53,2 s en logiciel, 38,8 à 52 s en
    natif ; crusnusa ×4 60,2 s en Vulkan, 81,1 s en logiciel, natif 50,3 et
    52,2 s mesurés juste avant et après ; crusnwld ×2 59,2 s en Vulkan,
    79,4 s en logiciel, 58,7 s en natif ; offroadc ×2 46,3 s en Vulkan,
    57,8 s en logiciel. Le rendu coûte environ 3 s de CPU sur 120 s, dont
    environ 1 s d'attente du GPU, surtout pour les images converties à la
    mise à jour au lieu de l'être au changement de page.
- 2026-11-02 : **Midway Zeus 2 sur Vulkan** (`zeus2_vk`).
  - La copie HD du tampon image (couleur et profondeur, avec la structure
    de lignes native × N) est sur le GPU, et la mémoire d'ondes (16 Mo,
    textures) y est recopiée par pages de 16 Ko écrites depuis le lot
    précédent.
  - Chaque triangle de `render_triangle_fan` est préparé sur le CPU comme
    le fait `render_triangle` (tri en Y, lignes couvertes, pentes, plans des
    paramètres). Le shader de fragments en déduit l'étendue de sa ligne, la
    profondeur entière (départ tronqué puis pas entiers), u/z, v/z et 1/z,
    puis applique `render_poly_8bit` : plancher de profondeur, couleur
    pleine, RGB555, texels avec alpha, texels par palette avec le filtre
    bilinéaire de `rgbaint_t` (version SSE) et la couleur transparente.
  - La profondeur 24 bits devient celle du GPU (D32, valeur / 2^24, exacte).
    Le mélange « source × a + destination × b » passe par la double source
    du GPU : seul l'arrondi de la part de la destination peut différer d'un
    niveau.
  - La palette est copiée dans le lot quand elle change ; une écriture dans
    la mémoire d'ondes envoie d'abord les triangles en attente.
  - Remplissages et écritures de l'hôte deviennent des segments de pixels
    natifs, dessinés en blocs N×N.
  - Quand l'adresse d'affichage change (registre 0x38), les lignes affichées
    sont relues sans attendre ; la mise à jour de l'écran les montre si
    rien n'a été dessiné dedans depuis.
  - Défaut corrigé au passage : le mode logiciel ne doit pas recopier le
    tampon natif au démarrage, `m_yScale` n'y est pas encore fixé (plantage
    en ×4).
  - Contrôle, démo de crusnexo sur 120 s, 8 captures : écart moyen de 0 à
    0,33 niveau, au plus 0,05 % des pixels au-delà de 24 (halos mélangés).
  - Temps pour 120 s émulées : ×2 89 à 105 s en Vulkan, 164 à 169 s en
    logiciel, 82 s en natif ; ×4 107 à 111 s en Vulkan, 334 s en logiciel,
    78 à 102 s en natif (portable qui chauffe). Environ 1600 triangles et
    moins de 2 lots par image ; 3 s d'attente du GPU sur 120 s.
- 2026-11-02 : **Namco System 21 sur Vulkan** (`namcos21_vk`).
  - Les deux tampons image (numéros de couleur R16 et profondeur) sont sur
    le GPU à N fois la taille native ; les tampons du CPU restent natifs,
    pour l'écran natif et la profondeur que lisent les sprites de Driver's
    Eyes.
  - Pour chaque triangle, le CPU fait avancer les arêtes en double comme
    `rendertri` et garde l'étendue de chaque ligne (un mot par ligne). Le
    GPU remplit ces étendues avec le test de profondeur (zsort constant par
    quad, exact en D32). Le CPU ne fait donc plus qu'un travail par ligne,
    et l'image est exactement celle du logiciel.
  - L'écran est mélangé sur le GPU puis relu : Winning Run y a sa couche 2D
    (mémoire de 512 Ko recopiée par pages écrites, `bitmap_draw` porté avec
    les trois priorités) ; Driver's Eyes reçoit à chaque image l'écran natif
    sous la 3D et les pixels changés par les sprites du dessus. La palette
    ajustée est envoyée avec le mélange.
  - `hires3d_output::use_frame()` accepte un rappel « image prête », appelé
    juste avant que l'affichage prenne l'image. L'attente de la relecture
    est faite là : les trois cartes de Driver's Eyes travaillent en même
    temps sur le GPU au lieu de s'attendre l'une l'autre (attente totale de
    25 s à 6 s sur 60 s).
  - Contrôle, 6 captures sur 60 s : Winning Run identique au logiciel ;
    Driver's Eyes identique sur l'écran central. Les écrans de côté
    dépendent du link entre les trois cartes, qui échoue parfois au
    démarrage (« RECEIVE ERROR »), en natif aussi : la partie diverge d'une
    exécution à l'autre.
  - Temps pour 60 s émulées : winrun natif 29 à 31 s, ×2 33 s en Vulkan
    contre 41 à 55 s en logiciel, ×4 44 s contre 87 s ; driveyes natif
    65 s, ×2 88 s contre 109 à 182 s, ×4 112 s contre 197 s.
- 2026-11-02 : **Taito JC (TC0780FPA) sur Vulkan** (`tc0780fpa_vk`).
  - Même organisation que System 21 : deux tampons de numéros de couleur et
    une profondeur 16 bits (exacte en D32) sur le GPU, effacés à l'échange ;
    les tampons du CPU restent natifs pour l'écran natif.
  - Triangles (`render_triangle`, plans des paramètres) et quads
    (`render_polygon<4>`, paramètres le long des arêtes) préparés sur le
    CPU, puis le shader reprend l'étendue de la ligne et les trois
    rastériseurs (plein, ombré, texturé avec bouclage). Les paramètres
    avancent en `départ + k × pas` au lieu d'additions successives, d'où
    de rares pixels différents.
  - La mémoire de textures (4 Mo) est recopiée par pages écrites ; l'écran
    mélange sur le GPU les couches natives du dessous, la 3D et les couches
    du dessus (objets et texte), puis la palette, avec l'attente reportée au
    moment où l'affichage prend l'image.
  - Contrôle, Side by Side sur 90 s, 9 captures : au plus 0,07 % des pixels
    au-delà de 24. Dangerous Curves n'atteint pas la 3D en 90 s (le mélange
    est identique).
  - Temps pour 90 s émulées : sidebs natif 86 s, ×2 104 s en Vulkan contre
    147 s en logiciel ; dangcurv natif 34 s, ×2 47 s contre 54 s. En ×4,
    la relecture de l'image (13 Mo) coûte 3,3 ms d'attente par image : il
    faudra passer l'image du GPU à l'affichage sans la relire. (Mesures ×4
    faites pendant que deux autres MAME tournaient sur le portable.)
- 2026-11-02 : **Gaelco 3D sur Vulkan** (`gaelco3d_vk`).
  - Le tampon image (jamais effacé par le matériel) et la profondeur 16 bits
    sont sur le GPU, à l'envers comme l'écran les montre ; les tampons du
    CPU restent natifs.
  - Chaque triangle de l'éventail est préparé comme `render_triangle` ; le
    shader reprend l'étendue de la ligne puis les trois rastériseurs : plans
    de u/z, v/z et 1/z, masque de texture, filtre bilinéaire COLOR3D, mélange
    à 50 % (double source, la moitié de la destination arrondie par le GPU).
  - Le test de profondeur compare la valeur signée et écrit sa valeur
    absolue : le signe dépend de z0, fixe par polygone, d'où une variante
    de pipeline (comparaison ou non) par polygone.
  - La texture et son masque (en bits) sont envoyés une fois ; la palette
    est copiée dans le lot quand elle change ; la relecture est attendue
    quand l'affichage prend l'image.
  - Contrôle sur 60 s : speedup identique au logiciel sur 7 captures,
    radikalb à 0,07 % des pixels près.
  - Temps pour 60 s émulées : speedup natif 24 à 27 s, ×2 29 s en Vulkan
    contre 33 s en logiciel, ×4 38 s contre 90 à 108 s ; radikalb natif 33 s,
    ×2 36 s contre 41 à 53 s.
- 2026-11-02 : **SNK HNG64 sur Vulkan** (`hng64_vk`).
  - Le tampon 3D (numéros de couleur avec lumière et bit de mélange) et sa
    profondeur flottante sont sur le GPU à N fois 512 × 512 ; la profondeur
    est stockée en Z / 128, ce qui garde l'ordre et la valeur 100 de
    l'effacement exactes. Les tampons du CPU restent natifs.
  - Triangles de `drawShaded` préparés comme `render_triangle` (plans de Z,
    W, lumière, S et T) ; le shader reprend l'étendue de la ligne puis les
    rastériseurs plein et texturé (division de perspective, défilement de
    texture, petites pages avec `fmod` du C, texels 4 ou 8 bits). La ROM de
    textures (16 Mo) part avec le premier lot.
  - L'écran est mélangé sur le GPU : couches natives sous la 3D, `blit_3d`
    porté (défilement, étirement vertical, palette 3D à 16 luminosités,
    mélange à 50 % calculé exactement dans le shader), puis les pixels
    changés par les couches du dessus.
  - Contrôle sur 90 s, 9 captures par jeu : au plus 0,54 % des pixels
    au-delà de 24 (texture de la route de xrally).
  - Temps pour 90 s émulées : roadedge natif 136 s, ×2 144 s en Vulkan
    contre 168 s en logiciel ; xrally natif 126 s, ×2 132 s contre 138 s.
    L'émulation elle-même est lourde, le gain est donc faible. xrally attend
    le GPU 28 s (5 ms par image) : à examiner.
- 2026-11-07 : **Vraie 3D sur le GPU pour cinq puces** (Taito JC, Gaelco 3D,
  HNG64, System 21, Zeus 2). Avant, le CPU rastérisait toujours en natif et le
  GPU redessinait chaque triangle dans une boîte, le shader recalculant
  l'étendue de chaque ligne.
  - Le GPU rastérise maintenant les primitives elles-mêmes, et le raster CPU
    est coupé. Le V-Unit reste tel quel : sommets entiers, mémoire vidéo lue
    par le CPU, et rendu de toute façon peu coûteux.
  - Taito JC : les triangles sont interpolés par le GPU. Les quads restent
    évalués le long de leurs arêtes dans le shader, car deux triangles feraient
    un pli sur un quad qui n'est pas un parallélogramme. La profondeur reste
    calculée par pixel (arrondi du matériel), mais une borne inférieure donnée
    au GPU lui permet de rejeter les pixels cachés avant le shader.
  - HNG64 : paramètres et profondeur interpolés par le GPU, plus d'écriture de
    profondeur dans le shader. Les couches 2D au-dessus de la 3D peuvent s'y
    mélanger (alpha, addition) : dessinées sur une marque, elles portent dans
    leur alpha ce qu'il faut garder du dessous, et le GPU fait le mélange.
  - System 21 : les quads sont dessinés avec un décalage d'un demi-pixel, qui
    reprend les pixels remplis par `rendertri` (arêtes prises en haut de ligne).
    Les stylos 0, 1 et 0xff des sprites du dessus de Driver's Eyes dépendent de
    ce qu'ils couvrent : ils sont notés dans le stylo et appliqués par le GPU.
    `namcos21_c67` et `gal3` n'affichent pas d'image HD : `set_hires_supported`
    les laisse en natif (Cyber Sled vérifié).
  - Zeus 2 : le CPU peut lire le tampon image (534 000 lectures au démarrage de
    crusnexo). Le raster natif reprend donc 2 secondes après chaque lecture,
    comme pour le Voodoo. Sinon, plus rien n'est mis en file pour le rendu
    logiciel : `object_data().next()` sans `wait()` devenait très lent (611 s
    au lieu de 50 s). Un scissor limite les triangles à la fenêtre de rendu.
  - Contrôle contre le logiciel HD, pixels au-delà de 24, pire capture : sidebs
    0,05 %, dangcurv 0,00 %, speedup 0,00 %, radikalb 0,05 %, roadedge 0,09 %,
    xrally 0,02 %, winrun 0,35 %, driveyes 0,26 %, crusnexo 0,01 %. Repli
    logiciel relancé sur les cinq puces. Sauvegarde puis rechargement d'état
    sans plantage sur les cinq puces, mais crusnexo y restait sur l'écran de
    calibration, sans 3D.
  - Temps CPU total (tous les threads), avant puis après, durée émulée fixe,
    portable, un seul essai par mesure :

    | Jeu | Natif | ×2 avant | ×2 après | ×4 avant | ×4 après |
    |---|---|---|---|---|---|
    | sidebs (90 s) | 86,0 | 89,2 | 85,1 | 90,5 | 84,5 |
    | speedup (60 s) | 52,1 | 44,0 | 18,1 | 51,7 | 23,9 |
    | roadedge (60 s) | 88,1 | 92,2 | 88,6 | 85,8 | 74,7 |
    | winrun (60 s) | 23,2 | 21,2 | 19,0 | 23,0 | 14,5 |
    | driveyes (60 s) | 64,7 | 71,2 | 57,3 | 70,2 | 63,8 |
    | crusnexo (120 s) | 91,8 | 102,5 | 52,2 | 103,9 | 51,5 |

    Le gain dépend de la part du raster dans le coût total : sidebs et roadedge
    passent surtout leur temps dans les processeurs émulés et la 2D.
    Le temps réel varie trop d'un essai à l'autre pour conclure : sidebs ×4 a mis
    77 s puis 101 s avec le même exécutable.
  - Parties automatisées (pièces, Start, accélérateur, mêmes entrées aux mêmes
    images) sur le `mame.exe` complet, logiciel contre GPU, de 10 à 15 captures
    par jeu, pixels au-delà de 24, pire capture : speedup 0,00 %, crusnexo
    0,01 %, xrally 0,03 %, roadedge 0,04 %, sidebs 0,07 %, driveyes 0,40 %,
    winrun 0,46 %. Vraies courses sur tous, sauf xrally (écrans 3D d'avant la
    course). Essai de 25 s dans une fenêtre bgfx : sidebs, winrun et crusnusa
    à 100 % de vitesse, sans erreur.
- 2026-11-07 (suite) : **contrôles en fenêtre, attentes du GPU, Hornet**.
  - Captures de l'image réellement affichée par bgfx (`PrintWindow`) : Speed
    Up, Roads Edge et Cruis'n Exotica en ×2, Winning Run et Side by Side en ×4,
    3D et 2D correctes. Le gros chiffre du départ de Roads Edge (un sprite 2D
    avec son ombre noire) est le même en logiciel. L'essai en fenêtre précédent
    couvrait crusnusa, un V-Unit qui n'avait pas changé.
  - Hornet : la couche K037122 manquait en HD (texte, HUD). Elle est
    maintenant agrandie comme sur nwk-tr. Sur Gradius IV, le texte s'affiche et
    le logiciel égale le GPU. Le jeu lui-même n'a pas été atteint : il reste dans
    ses menus de réglage de l'horloge.
  - L'attente de la relecture (Taito JC, Gaelco 3D, HNG64, System 21) se fait
    maintenant quand le rendu lit l'image : `render_texture::get_scaled` appelle
    `hires3d_output::complete_reads()`, après la régulation de vitesse. Sur Speed
    Up ×4 en fenêtre : 1,4 s d'attente sur 40 s au lieu de 6,9 s, mêmes images.
  - System 22 : `end_frame` attendait le GPU à chaque image, ce qui prenait 35 %
    du thread principal sur Ridge Racer. Il garde maintenant deux images en vol,
    avec une barrière en tête de chaque image, et attend via `use_frame`. Sans
    régulation en ×2, Ridge Racer passe de 341 % à 407 % et Dirt Dash de 263 % à
    286 %. En fenêtre en ×4, Dirt Dash tient 98,8 % au lieu de 95,4 %. Images
    identiques à l'ancien chemin.
  - HNG64, horodatages GPU, xrally ×2, par image : 1,75 ms pour envoyer la 2D
    (1,8 Mo), 0,76 ms pour dessiner et mélanger, 2,17 ms pour relire l'image
    (3,7 Mo). L'attente vient donc des transferts PCIe, pas du dessin.
    L'émulation elle-même reste sous la vitesse réelle en course sur ce
    portable : 56 % sur GPU et 49 % en logiciel ×2.
  - Cruis'n Exotica : après une sauvegarde en pleine course et un rechargement
    300 images plus tard, les images sont identiques au pixel près à la course
    de référence (décalées de 300 images). L'écran de calibration du premier
    essai venait d'une sauvegarde faite avant la calibration. Les 534 423
    lectures du tampon image par le CPU ont toutes lieu au démarrage, aucune en
    course.
  - Géométrie, profil du thread principal : sur crusnexo, Zeus 2 traite ses
    modèles en 12 % environ, contre 58 % pour le TMS320C31 émulé. Sur ridgerac,
    System 22 dessine sa scène en 9 % environ. HNG64 n'apparaît pas. Passer la
    géométrie sur le GPU rapporterait peu : l'étape est retirée.
  - Autres puces, d'après une lecture du code (rien n'a été testé) :
    - Banshee et Voodoo 3 : seul candidat faisable avec des jeux de course qui
      marchent (sf2049, sf2049se, cartfury).
    - En attente de leur émulation : System 23, Type Zero (batlgear), Cobra
      (racjamdx), Quicksilver II.
    - GPU PS1 (technodr, kartduel, glpracr…) : sommets entiers, donc une HD sans
      sous-pixel, pour un gros travail.
    - Zeus 1 et Konami M2 : faisables, mais sans jeu de course.
    - Hard Drivin' et Polygonet : la 3D est calculée par le CPU ou le DSP émulé,
      impossible sans HLE.
- 2026-11-07 (soir) : **Banshee et Voodoo 3 en HD, image partagée avec bgfx**.
  - Banshee/Voodoo 3 : tampons couleur et profondeur n'importe où dans la
    mémoire partagée avec les textures, à des adresses que le jeu change. Les
    copies HD (logiciel et Vulkan) sont tenues par adresse de tampon, et toute
    écriture dans la mémoire (FIFO, écritures directes et LFB, port de
    textures, blits 2D) va aux copies qu'elle touche et à la mémoire de
    textures du GPU ; CLUT de 512 entrées. sf2049 et cartfury tournent en
    double tampon à deux adresses, sans blit.
  - Les lectures des registres de statistiques de pixels relancent le
    rastériseur natif (seul lui compte les pixels qui passent les tests) :
    sf2049 les lit toutes les 5 images, et sa démo divergeait quand seul le GPU
    dessinait. Elle suit maintenant le logiciel ; racingj aussi les lit, et
    dessine donc en natif en plus du GPU.
  - Voodoo 1/2 : les écritures LFB avec pipeline n'étaient recopiées en HD que
    si elles changeaient le pixel natif ; en mode GPU seul, l'écran de test du
    volant de sfrush était noir. Corrigé. La démo de sfrush diverge toujours
    en mode GPU seul (temps d'exécution estimé), comme avant.
  - Partage avec bgfx : sur ce portable, 16 essais sur 16 sans erreur en
    fenêtre (8 jeux, 2 fois). En ×4 en fenêtre, vitesse régulée, avec partage
    puis sans : roadedge 88,8 % contre 64,5 % ; ridgerac 100 % contre 98,3 % ;
    attente du GPU sur 29 s : sf2049 0,07 s contre 6,0 s, sidebs 0,19 s contre
    0,82 s, speedup 0,09 s contre 0,94 s, winrun 0,02 s contre 0,71 s.
  - Pièges trouvés en route : bgfx rend sur son propre thread sous Windows
    (`BGFX_CONFIG_MULTITHREADED` n'est à 0 que sur macOS) avec un périphérique
    D3D11 mono-thread. Tant que bgfx gardait ce thread, les appels D3D11 depuis
    l'émulation faisaient planter le pilote, les requêtes ne finissaient pas, et
    un keyed mutex donnait une image noire (le dessin avait lieu après la
    libération du verrou).
- 2026-11-07 (nuit) : **options d'affichage et hack 16:9**.
  - `-aspect_ratio` et `-crop_position`, avec les mêmes valeurs que les autres
    émulateurs des bornes (voir le point 8 de l'architecture). Vérifié dans
    une fenêtre de 640x480 : en 16:9, cadre de 640x360 avec l'image 4:3
    entre deux bandes ; `_stretch` remplit, `_crop` couvre, et
    `-crop_position -100` garde le haut. Le rendu GPU suit la taille de la
    fenêtre comme le rendu logiciel : bgfx étire la texture de l'image.
  - `-widescreen` (rendu Vulkan, aussi en `-hires3d 1`), pour les puces dont
    MAME fait la projection :
    - System 22 : les polygones projetés sont décalés de `pad`, et les
      fenêtres 3D qui touchent un bord de l'écran vont jusqu'au bord de
      l'image ; les sprites, les polygones directs et la couche texte restent
      au milieu ou sont étirés. ridgerac : la piste continue sur les côtés,
      sans couture, et le point de fuite ne bouge pas.
    - HNG64 : tampons 3D plus larges de `pad` de chaque côté, frustum élargi
      d'autant (x divisé avant le clip puis remultiplié), mélange avec les
      couches du dessus au milieu ou étirées. `-widescreen_bg` dessine aussi
      les tilemaps sous la 3D au-delà des bords de l'écran (bitmaps avec
      marge), pas les sprites. Le ciel de roadedge continue sans couture ;
      la course de xrally s'élargit aussi.
    - Zeus 2 : chaque ligne de l'image du tampon a `pad` colonnes de plus de
      chaque côté ; les polygones dont le clip touche un bord de l'écran
      continuent, et les remplissages de lignes entières effacent aussi les
      côtés. Le Zeus 2 dessine sa 2D en polygones : le HUD reste au milieu,
      `-widescreen_2d stretch` n'y change rien. crusnexo : la 3D s'élargit,
      le menu et le tableau de bord restent au milieu avec du noir autour.
  - Limites propres au hack : ce que le jeu met hors de l'écran devient
    visible (un panneau 3D au bord de l'écran de choix de roadedge, la ville
    3D derrière l'intro 2D de xrally) ; une tilemap qui ne déborde pas laisse
    du noir ; avec `-widescreen_bg` et `-widescreen_2d stretch`, les couches
    2D sous la 3D ne sont pas étirées et se décalent de celles du dessus sur
    les écrans de menus.
  - Corrigé le 2026-11-09. HNG64 : quand les tilemaps sous la 3D ne dépassent
    pas de l'écran 4:3 (menus), ces couches sont étirées avec le reste en mode
    `stretch`, et les côtés restent noirs sinon, sans 3D ni tilemap. Le test
    regarde les côtés dans tous les modes `-widescreen` ; la course ne change
    pas. La bande de roadedge était une tilemap, pas de la 3D. Zeus 2 : en mode
    `stretch`, les polygones sans test de profondeur ou plats devant la scène
    (z constant sous 12,5) sont étirés : menus et textes de crusnexo. Le HUD
    d'une vraie course n'a pas été vu (démo seulement). Essai rejeté : les
    sprites HNG64 prolongés sur les côtés dupliquent les menus.
  - Le crash à la sortie en fenêtre venait de `view_widening()`, appelé
    pendant la destruction de la fenêtre alors que les périphériques étaient
    déjà arrêtés : les écrans arrêtés sont ignorés.
  - Vu en route : le rendu logiciel du HNG64 en ×3 (taille de tampon qui
    n'est pas une puissance de 2) masque ses coordonnées avec `taille - 1`
    et montre des bandes répétées ; le chemin GPU passe maintenant par un
    modulo et est correct en ×3. Le chemin logiciel n'est pas corrigé.

## Bilan du 2026-11-10 : rendu GPU étendu et essais complets

Mandat : tous les points de la liste sauf 1, 4, 8, 9, puis essais de tous les
jeux en ×1 à ×4, avec et sans `-widescreen`. Pas de push, pas de release.

- **Model 1** (bf63fc36f50) : partage bgfx et 16:9.
- **System 23** (84f118c06f9) : rendu Vulkan (`namcos23_vk`), hors Gorgon. Race
  On! : course complète en ×2 (65 % de la vitesse, limité par le CPU émulé).
- **PlayStation** (ce5c31da416) : `psxgpu_vk`. Le rendu logiciel continue sur la
  VRAM native (textures, lectures du jeu) ; les primitives sont redessinées
  dans une copie de la VRAM à haute résolution, et la zone affichée en est
  tirée. Le GPU rejette comme le matériel les triangles de plus de 1023
  pixels (sinon blocs colorés en bas à gauche dans kartduel). 15 bits
  seulement : le mode 24 bits et l'écran éteint restent en natif. Pas de 16:9
  (projection faite par le jeu).
- **Viper et Thrill Drive** : thrild2, xtrial (Viper) et thrilld (Hornet)
  démarrent et jouent leur démo sans modification, malgré « not working ».
  thrilld, racingj et racingj2 demandent d'initialiser leur mémoire (bouton Test).
- **Point 3** : en course sur le GPU : roadburn ×3, windheat ×2, racingj ×2,
  racingj2 ×2, calspeed ×2. Le rectangle gris de Racing Jam (rétroviseur,
  aperçu de voiture) existe aussi en natif et en logiciel.

### Matrice

40 jeux, ×1 à ×4, avec et sans `-widescreen` : 320 essais, tous avec le code
de sortie 0, sans `-video` (image relue du GPU), 60 à 230 s émulées, images
comparées d'une configuration à l'autre (dernière capture de chaque essai).
Jeux joués par script (130 s) car bloqués à 60 s sur calibrage ou démarrage :
crusnexo, crusnusa, hangplt, raceon, motoxgo, kartduel ; sf2049 et sf2049se
vont plus loin sur 130 s sans script. Les jeux à disque dur (CHD) ne peuvent
pas tourner à plusieurs instances : un par un.

- Pas essayés : netmerc et wingwar, ROM `hd44780` absente du NAS.
- gticlub : erreur de volant (point 4, exclu) ; essayé jusqu'au menu de test
  seulement, sa 3D n'est pas vue dans la matrice.
- 500gp : écran noir persistant en fin d'essai en ×2 et ×3, seulement avec plus
  de 5 instances en même temps sur le GPU de ce portable (3 essais sur 8 de la
  première passe, puis ×2 dans une passe chargée). Jamais vu seul ni sur la
  moitié des passes à 5 instances. Hypothèse non vérifiée : soumission Vulkan
  qui échoue (`end_frame` rend false sans rien dire, le bitmap natif n'est pas
  dessiné). Une seule instance tourne sur une borne.
- Le 16:9 du menu de raceon laisse des côtés noirs en haut et en bas (décor 2D).

### En fenêtre bgfx

Partage sans relecture CPU, vu sur l'écran réel (60 captures du bureau de
suite, aucune noire) : vr ×1 et ×2, vr ×2 16:9, raceon ×2 et ×2 16:9,
pocketrc ×3. Le `PrintWindow` de `wincap.ps1` donne des images noires sur
ces jeux : défaut de la capture, pas de l'affichage.

### Suite du 2026-11-10 : volant, repli logiciel, rétroviseurs

- **GTI Club** : « DEVICE ERROR (STEERING WHEEL) » venait du contrôle au
  démarrage, à l'adresse ff80fc0c : couple croissant d'un côté (0x91 à 0x9f),
  de l'autre (0x81 à 0x8f), puis de nouveau le premier côté. Le volant doit
  bouger de 0x60 à un couple donné, aller de butée à butée et être centré. Un
  petit modèle dans `gticlub.cpp` donne la position lue pendant ce contrôle,
  du premier 0x91 à l'arrêt du moteur (0x00) ; ensuite le port du volant
  reprend la main, sur les bornes aussi. thunderh et slrasslt ne sont pas
  concernés. La démo 3D est vue de ×1 à ×4, avec et sans `-widescreen`.
- **Repli logiciel** : System 23, PS1 et Voodoo restaient noirs ou muets si le
  GPU échouait. Ils passent en natif avec un message. Essayé en provoquant la
  panne (submit qui échoue après 400 appels, patch temporaire non commité) :
  500gp, pocketrc, vr, ridgerac, sfrush, sidebs, roadedge, speedup, crusnusa,
  winrun, gticlub2 reviennent à l'image. Le moteur `-video none` n'a pas pu
  reproduire l'écran noir de 500gp vu sous forte charge : sa cause reste
  supposée.
- **Thrill Drive 2 et Xtrial Racing** : le jeu change de largeur de tampon à
  chaque image pour son rétroviseur, ce qui détruisait les images GPU
  (`hires_layout`) : le titre restait affiché pendant la course. Une cible
  plus étroite garde maintenant les images. Elle est aussi dessinée par le
  rasteriseur natif, car le rétroviseur est relu comme texture depuis la
  mémoire native (`tmu_written` au retour à la largeur normale). Courses
  complètes sur GPU : thrilld, thrild2, xtrial, de ×1 à ×4.
- **Motocross Go!** : les pneus ont bien la couleur du sol sur une vraie borne
  (vidéo vue par le propriétaire). Aucun défaut.
- Vérification finale, Voodoo et Hang Pilot, 96 essais : thrild2, xtrial,
  thrilld, gticlub2, sf2049se, sfrush, calspeed, cartfury, roadburn, racingj,
  racingj2, hangplt, ×1 à ×4, avec et sans `-widescreen`, sans erreur.

### Suite du 2026-11-11 : menu vidéo, image noire de Thrill Drive 2

- **Menu** : « Video Options » (réglages généraux et par jeu) a une section
  « High Resolution 3D Options » : `-hires3d`, `-hires3d_renderer`,
  `-bgfx_hires3d_share`, `-aspect_ratio`, `-crop_position`, `-widescreen`,
  `-widescreen_2d`, `-widescreen_bg`. Pris en compte au lancement suivant,
  pas à la volée (choix du propriétaire).
- **Thrill Drive 2, Xtrial Racing** : une image sur deux du jeu sortait noire
  avec Vulkan en ×2 à ×4 (le propriétaire voyait l'image clignoter). Le
  rétroviseur, dessiné en natif sur la cible étroite, prenait quand même une
  des trois copies haute résolution des tampons de couleur et chassait celle
  d'un tampon à afficher ; ce tampon était alors montré depuis sa version
  native, vide quand seul le GPU dessine. La cible étroite ne passe plus par
  la copie haute résolution avec Vulkan. Les essais précédents (une capture
  toutes les 300 images) tombaient toujours sur une bonne image : vérifié
  cette fois image par image, en course, thrild2 ×2 à ×4, xtrial ×3 et ×4,
  écarts entre images égaux au natif ; gticlub2 et sf2049se inchangés.

### Suite du 2026-11-11 : fusion upstream, annonce UDP, chasse au clignotement

- **Fusion** de MAME upstream (59 commits, 754eb0adb02). Seul conflit : le
  compteur MIPS, qu'upstream a corrigé comme nous (adb51e24801) ; les
  fichiers mips3 sont repris d'upstream. Deux nouveaux `.lay` demandaient
  de régénérer le projet (`make vs2022`) avant la compilation.
- **Sorties réseau** (208804e82b9) : au lancement d'un jeu, `-output network`
  envoie en broadcast UDP sur le port 8001 `mame_start = <jeu>\r` puis
  `tcp = 8000\r`, comme Supermodel njz3. BackForceFeeder et le FFB Plugin
  njz3 écoutent ce port, maintenant tous deux en partagé (SO_REUSEADDR).
  Vu avec un récepteur pour vr et ridgerac ; rien sans `-output network`.
  Le serveur TCP 8000 accepte plusieurs clients à la fois.
- **Chasse au clignotement**, sur le build fusionné : 40 jeux, natif puis
  Vulkan ×2, ×3, ×4 et ×2 `-widescreen`, une capture par image dans deux
  fenêtres de 40 images (démo, puis partie avec pièce, Start et
  accélérateur), chaque image comparée à l'image native de même numéro, et
  les images noires comptées. Le banc retrouve les 21 images noires de
  thrild2 avec l'exe d'avant le correctif. Résultat : aucune image noire,
  aucun pic. Les écarts restants ne sont pas des défauts d'image :
  - sfrush : la dernière capture, prise à la sortie de `-seconds_to_run`
    (reste d'image native, déjà connu) ;
  - raceon : l'image Vulkan est décalée d'une image entière, régulièrement,
    selon la fenêtre (rythme de l'émulation, pas vérifié plus loin) ;
  - roadburn, xtrial, calspeed : la course part autrement qu'en natif
    (place, classement), le chrono égal ; images justes, rétroviseurs
    compris. Sans doute le nombre de pixels estimé quand seul le GPU
    dessine, relu par le jeu (pas vérifié) ;
  - pocketrc : scène immobile, seule la netteté en ×4 diffère.
  Limites : 80 images par essai, toujours la même conduite ; seulement la
  carte NVIDIA de ce PC.

### Suite du 2026-11-12 : fusion upstream, cas bizarres du banc

- **Fusion** de MAME upstream (8 commits, 9a52a4806ab), sans conflit.
  `diexec.h` change (`signal_external_write`) : presque tout recompile.
- **Méthode** : en plus des captures, une empreinte de la RAM du CPU
  principal (8 Mo, un mot sur seize, toutes les 10 images, mêmes entrées
  que le banc) dit si l'émulation elle-même s'écarte du natif, et deux
  lancements natifs disent si le jeu est reproductible.
- **California Speed** : la course partait autrement en Vulkan. Cause :
  quand seul le GPU dessine, le nombre de pixels d'un triangle (qui donne
  la durée de l'opération vue par le jeu) était l'aire découpée par le
  rectangle de clip ; le rasterizer natif (`POLY_FLAG_NO_CLIPPING`) compte
  les étendues de chaque ligne sans découpe. `estimated_pixels` refait
  maintenant exactement son calcul, et le remplissage rapide suit
  `render_tile`. Après correction, RAM identique au natif en ×2 et ×4 sur
  toute la partie.
- **Road Burners, Xtrial Racing** : deux lancements natifs diffèrent déjà
  (roadburn dès l'image 430, au démarrage). Les horloges M48T37 (Vegas)
  et M48T58 (Viper) lisent l'heure du PC ; ce n'est pas mame3d.
- **Race On!** : RAM identique au natif en ×2 et ×4 ; l'image ×4 est
  parfois en retard d'une image (pas à chaque lancement), l'affichage
  seulement. Cause non trouvée (le rendu Vulkan du System 23 relit son
  image tout de suite).
- **Pocket Racer** : natif reproductible, logiciel identique, Vulkan ne
  diffère que par la netteté.

### Suite du 2026-11-12 (soir) : Thrill Drive 2 en 16:9, Race On!

- **Thrill Drive 2, `-widescreen`** (viper.cpp, toutes les versions) : le jeu
  projette lui-même sa 3D, le hack passe donc par son code. La structure
  caméra (viewport 512x384, near 0,2, far 280, FOV 50°, tanH 1,1918, tanV
  0,8938 en 0x5729a0) est remplie une fois par une fonction (0x040b40 dans
  les quatre versions) : `tan(fov)`, tanH en +0x34/+0x38, tanV = tanH × forme
  en +0x3c, puis le frustum (gauche, droite, haut, bas) passé à la fonction
  qui construit les plans de découpe. Le driver cherche cette fin de fonction
  par signature, y met un saut vers 12 instructions écrites en 0x3600 (RAM vide
  entre les vecteurs et le programme) qui multiplient tanH et la droite du
  frustum par 4/3 sans toucher au haut ni à tanV : projection et culling
  s'élargissent ensemble. L'écran garde sa taille et est montré étiré en 16:9
  (`hires3d_output::allow_anamorphic`). Toutes les caméras sont élargies, le
  rétroviseur garde donc sa forme ; textes et compteurs sont étirés.
  - Essais : modifier tanH à chaud élargit la vue (le frustum seul n'a aucun
    effet, l'angle non plus). Les watchpoints du débogueur ne voient rien :
    la RAM est en fastram DRC. Code hors RAM de travail (0x02000000,
    0xfe100000) : le jeu reste sur son écran bleu de démarrage (cause non
    cherchée ; peut-être la traduction d'adresses du PowerPC).
  - Vérifié : thrild2, thrild2j, thrild2a, thrild2c patchés à la même
    adresse, course correcte sans `-hires3d` et en Vulkan ×2 et ×4 ; sans
    `-widescreen`, image identique à avant au pixel près.
- **Race On!, image en retard** (vk3d_context.cpp) : l'image Vulkan avait
  une image de retard sur le natif, sur des fenêtres entières, selon le
  lancement et plus souvent en ×3 et ×4. Les listes de rendu et les sommets
  envoyés au GPU étaient identiques au natif image par image (empreintes),
  l'image relue juste après la fence était celle d'avant, et redevenait juste
  après `vkDeviceWaitIdle` (pas avec une invalidation seule, ni avec des
  barrières complètes). Cause : `submit_and_wait()`, qui envoie les textures
  au démarrage, laissait la fence du contexte signalée ; le rendu du System 23
  envoie chaque image avec cette fence, la croit libre et l'attend puis la
  remet à zéro. Envoyée signalée, chaque attente rendait la main tout de
  suite, et la relecture prenait l'image précédente tant que le GPU restait
  en retard sur le CPU. La fence est maintenant remise à zéro après
  `submit_and_wait()` (les autres puces ont leurs propres fences, ou
  remettent celle-ci à zéro avant d'envoyer). Après correction : ×2, ×3, ×4,
  deux lancements, aucun décalage, ×3 identique au natif.
- **Thrill Drive 1 en 16:9** (nwk-tr.cpp, `-widescreen`) : thrilld et ses
  versions sont sur le pilote NWK-TR (pas Hornet : seuls les jeux thrilldg*
  y sont, marqués non fonctionnels). Le PowerPC 403 n'a pas de FPU et ne
  garde pas de flottants de caméra : en perturbant ~6500 flottants de sa
  RAM et ~2750 du bloc 1 du DSP, rien. C'est le DSP SHARC qui projette.
  - Trouvé en traçant les écritures du DSP vers la Voodoo (tap Lua sur
    0x2400000-0x27fffff : fvertexAx..Cy en 0x2480022-27, ~460 triangles par
    image), puis en remontant l'interpréteur de commandes du DSP (chaque
    gestionnaire lit ses mots par `DM(I4, M1)` dans la mémoire partagée
    0x400000 et saute au suivant par la table 0x28283).
  - Commande 3 = viewport, 6 mots de 16 bits : centre (256, 192), demi-taille
    (256, 192), focale horizontale puis verticale en pixels, négatives. Le
    DSP en tire la projection (demi-largeur / |focale|) et les plans de
    clipping (focale × 16). 7 viewports par image : le monde à −215
    (256/215 = tan 50°, comme la tangente de Thrill Drive 2), le tableau de
    bord 3D (aiguille du compte-tours, chiffres de vitesse) à −443, et une
    lecture à +128 au même PC sans effet visible.
  - Le patch : un tap de lecture sur la mémoire partagée des deux DSP ; quand
    le DSP est au PC qui lit la focale horizontale (0x20130, retrouvé par
    une signature de 8 instructions, vérifiée à chaque image, cherchée une
    fois par seconde tant qu'elle n'y est pas), la valeur est divisée par
    l'élargissement (4/3), sauf −443 (le tableau de bord reste comme la 2D,
    étiré) et les valeurs positives. Image étirée en 16:9
    (`allow_anamorphic`), comme Thrill Drive 2.
  - Piège : un premier essai (écrire −332 dans la RAM du PowerPC en
    0x3eb2d4) semblait élargir la vue à l'œil, mais les captures étaient
    identiques au pixel : toujours comparer les pixels.
  - Vérifié sur thrilld (seule version en local) : sans `-widescreen`, image
    identique au pixel ; avec, identique au test Lua ; en fenêtre bgfx avec
    `-hires3d 2`, image 16:9, voitures aux bonnes proportions. En vue
    cockpit, le bas de l'habitacle, modélisé pour du 4:3, laisse voir la
    route sur les bords. Seul le DSP 1 a le programme (le site n'est pas
    trouvé sur le DSP 2).
- **Thrill Drive 1, rétroviseur vide** (voodoo_render.cpp, voodoo.cpp,
  voodoo_vk.cpp) : le cadre du rétro (en haut au centre) restait mauve uni,
  avec ou sans `-widescreen`. Bug de la Voodoo de MAME, pas du 16:9.
  - Fausse piste : la 2e carte vidéo (TWIN/SLAVE). Son DSP n'a jamais de
    programme dans MAME, mais le PowerPC ne s'adresse qu'à la carte 0
    (l'id 2 seulement au démarrage) ; en copiant les écritures vers la
    carte 1, elle dessine la même image que la carte 0, le DSP n'ayant pas
    d'identité à lui (pas de lecture de drapeau ou de cavalier).
  - Le rétro est dessiné par la carte 0 : une commande viewport de plus,
    centre (256, 344), demi-taille (128, 32), focales +128/−128 (positive en
    horizontal : l'image est retournée), découpage x 128-384, y 312-376
    avec l'origine Y en bas (fbzMode bit 17) ; un fast fill de la zone en
    0xaab4ff (le mauve), puis ~60 triangles avec un test alpha et une
    couleur clé.
  - Cause : le fast fill compare le rectangle de découpage aux Y avant
    retournement (il remplit la bonne zone), les triangles aux Y après
    retournement : avec un rectangle qui n'est pas plein écran, tous les
    triangles du rétro (y 312-376 → lignes 7-71) étaient rejetés. Les
    triangles sont maintenant découpés comme le fast fill, dans les trois
    chemins (logiciel, -hires3d logiciel, Vulkan). Les écritures LFB à
    travers le pipeline gardent l'ancienne comparaison (non vu utilisé).
  - Vérifié au pixel avant/après en attract (70 s) : GTI Club 1 et 2, Hang
    Pilot, Thrill Drive 2, Xtrial Racing, San Francisco Rush, California
    Speed, Gradius 4, Teraburst identiques ; Rush 2049 et 2049 SE : seule
    l'horloge affichée diffère ; Racing Jam 1 et 2 : la bande grise du bas
    de l'écran disparaît, la 3D descend jusqu'en bas (même cause, plus
    juste). Thrill Drive : rétro en natif, Vulkan ×2, logiciel ×2.
  - Avec `-widescreen`, la caméra du rétro est élargie aussi (seule −443,
    le tableau de bord, est laissée), pour qu'il garde ses proportions une
    fois étiré.
