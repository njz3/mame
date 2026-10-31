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
| 2 | Namco System 22 / Super 22 | ridgerac, ridgera2, raverace, acedrive, victlap, cybrcycc, dirtdash, alpinerd | OK (ridgerac, dirtdash ×2) | à faire |
| 3 | Konami K001005 | gticlub, midnrun, windheat, jetwave | OK (gticlub, midnrun ×2) | à faire |
| 4 | 3dfx Voodoo 1/2 | sfrush, sfrushrk, calspeed, hyprdriv, vaportrx, roadburn, thrilld, racingj | OK (sfrush ×2 ×4, racingj ×2, roadburn ×2) ; coûteux en CPU | OK (même image que le logiciel) ; au niveau du natif en ×2 sur roadburn |
| 5 | Midway V-Unit | crusnusa, crusnwld, offroadc | 2D OK (crusnusa ×2) ; 3D à vérifier en démo (sommets entiers : bords natifs, textures HD) | à faire |
| 6 | Midway Zeus 2 | crusnexo | OK (crusnexo ×2, mode démo) | à faire |
| 7 | Namco System 21 | winrun, driveyes | OK (winrun, driveyes ×2) | à faire |
| 8 | Taito JC (TC0780FPA) | sidebs, sidebs2, dangcurv | OK (sidebs, sidebs2 ×2) | à faire |
| 9 | Gaelco 3D | speedup, surfplnt, radikalb | OK (speedup, radikalb ×2) | à faire |
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
