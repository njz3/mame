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

Matériel visé, dans l'ordre :

| # | Matériel | Jeux | Rendu logiciel HD | Rendu Vulkan |
|---|---|---|---|---|
| 1 | Sega Model 1 | vr, vformula | OK (vr ×2, ×4) | OK (vr ×2, ×4) ; relecture lente |
| 2 | Namco System 22 / Super 22 | ridgerac, ridgera2, raverace, acedrive, victlap, cybrcycc, dirtdash, alpinerd | OK (ridgerac, dirtdash ×2) | OK (5 jeux ×2) ; plus rapide que le natif |
| 3 | Konami K001005 | gticlub, midnrun, windheat, jetwave | OK (gticlub, midnrun ×2) | OK (midnrun, gticlub) ; proche du natif |
| 4 | 3dfx Voodoo 1/2 | sfrush, sfrushrk, calspeed, hyprdriv, vaportrx, roadburn, thrilld, racingj | OK (sfrush ×2 ×4, racingj ×2, roadburn ×2) ; coûteux en CPU | OK (même image que le logiciel) ; au niveau du natif en ×2 sur roadburn |
| 5 | Midway V-Unit | crusnusa, crusnwld, offroadc | OK (3 jeux ×2 en démo 3D ; sommets entiers : bords natifs, textures HD) | OK (même image que le logiciel) ; ×4 60 s contre 81 s en logiciel |
| 6 | Midway Zeus 2 | crusnexo | OK (crusnexo ×2, ×4, mode démo) | OK (même image que le logiciel, aux arrondis des mélanges près) ; ×4 107 s contre 334 s en logiciel |
| 7 | Namco System 21 | winrun, driveyes | OK (winrun, driveyes ×2) | OK (même image que le logiciel) ; ×2 près du natif |
| 8 | Taito JC (TC0780FPA) | sidebs, sidebs2, dangcurv | OK (sidebs, sidebs2 ×2) | OK (sidebs ×2, même image que le logiciel à 0,07 % près) |
| 9 | Gaelco 3D | speedup, surfplnt, radikalb | OK (speedup, radikalb ×2) | OK (même image que le logiciel) ; ×4 38 s contre 108 s en logiciel |
| 10 | SNK HNG64 | roadedge, xrally | OK (roadedge, xrally ×2) | à faire |
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
